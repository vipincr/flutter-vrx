// SPDX-License-Identifier: MIT
/*
 * vrx camera plugin (vrx plan 0018 D8): shows camerad's viewfinder frames as
 * a Flutter external texture.
 *
 * Why this lives in C and not in Dart: Dart's ResourceHandle offers no raw
 * descriptor number, so a Dart-side Open cannot hand a dmabuf to EGL. The
 * plugin runs in the applet's own process, so vrxhost, which knows a caller
 * by its cgroup, sees the applet's identity on the Open call.
 *
 * Flow of "open":
 *   1. (platform thread) create the Flutter texture and start a worker.
 *   2. (worker) Varlink io.vrx.Host.Open("camerad", "frames") on
 *      $VRX_HOST_SOCKET or /run/vrx/io.vrx.Host; the reply carries the
 *      stream's end as an SCM_RIGHTS fd. A refusal is mapped to a short code.
 *   3. (worker) read the BufferOffer line and its N descriptors.
 *   4. (worker) import test: every buffer becomes an EGLImage and a GL
 *      texture once (EGL_LINUX_DMA_BUF_EXT). If any import is refused, or
 *      VRX_CAMERA_FORCE_COPY is set, the plugin selects the counted copy
 *      path instead and says so in the log.
 *   5. (platform thread) answer Dart with the texture id and the geometry.
 *   6. (worker) per filled record: push a texture frame; when the engine is
 *      done with it (its destroy callback) the buffer index is given back.
 *
 * Copy path: each buffer is mapped, converted XRGB8888 -> RGBA8888 (alpha
 * forced opaque) into a staging buffer, and uploaded with glTexSubImage2D
 * into a ring of GL textures; the buffer goes back at once. Every copied
 * frame and byte is counted, and the totals are logged at close.
 *
 * Lifetime: a pushed frame may outlive "close" (the engine holds the last
 * one), so the session is refcounted: each pushed frame holds a reference,
 * and the descriptors, images and textures are released on the last unref.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <linux/dma-buf.h>

#include "egl_dmabuf.h"
#include "flutter-pi.h"
#include "gl_renderer.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "plugins/vrx_camera/wire.h"
#include "texture_registry.h"
#include "util/logging.h"

#define CHANNEL "vrx/camera"
#define DEFAULT_HOST_SOCKET "/run/vrx/io.vrx.Host"
#define OFFER_TIMEOUT_MS 10000

enum mode { MODE_DMABUF, MODE_COPY };

struct session {
    atomic_int refs;

    int64_t texture_id;
    struct texture *texture;
    struct frame_interface *interface;

    pthread_t thread;
    bool thread_started;
    int wake_fd;  // eventfd: a write ends the worker

    // response to the pending "open" (answered on the platform thread)
    const FlutterPlatformMessageResponseHandle *open_response;

    pthread_mutex_t stream_lock;
    int stream_fd;  // -1 once closed
    atomic_bool closed_by_dart;
    atomic_bool end_reported;

    struct vrx_camera_offer offer;
    int fds[VRX_CAMERA_MAX_BUFFERS];
    int n_fds;
    enum mode mode;
    const char *copy_reason;

    // dmabuf mode: one image + texture per buffer
    struct egl_dmabuf_image images[VRX_CAMERA_MAX_BUFFERS];
    int n_images;

    // copy mode
    void *maps[VRX_CAMERA_MAX_BUFFERS];
    uint8_t *staging;
    GLuint ring[VRX_CAMERA_MAX_BUFFERS];
    atomic_bool ring_busy[VRX_CAMERA_MAX_BUFFERS];
    bool ring_created;

    // counters
    atomic_uint_fast64_t frames, dropped, copied_frames, copied_bytes;
};

struct frame_token {
    struct session *s;
    uint32_t index;   // buffer index (dmabuf) to give back, or UINT32_MAX
    int ring_slot;    // copy mode texture slot to free, or -1
};

static struct {
    pthread_mutex_t lock;
    struct flutterpi *flutterpi;
    struct session *current;
} plugin = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ---------------------------------------------------------------- session */

static void session_ref(struct session *s) {
    atomic_fetch_add(&s->refs, 1);
}

static void session_free(struct session *s) {
    LOG_DEBUG(
        "vrx_camera: session %" PRId64 " released (%" PRIu64 " frames, %" PRIu64 " dropped, %" PRIu64 " copied frames, %" PRIu64
        " bytes copied)\n",
        s->texture_id,
        (uint64_t) atomic_load(&s->frames),
        (uint64_t) atomic_load(&s->dropped),
        (uint64_t) atomic_load(&s->copied_frames),
        (uint64_t) atomic_load(&s->copied_bytes)
    );

    for (int i = 0; i < s->n_images; i++) {
        egl_dmabuf_release(s->interface, &s->images[i]);
    }
    if (s->ring_created) {
        if (frame_interface_make_current(s->interface)) {
            glDeleteTextures(s->offer.buffers, s->ring);
            frame_interface_clear_current(s->interface);
        }
    }
    for (int i = 0; i < s->n_fds; i++) {
        if (s->maps[i] != NULL && s->maps[i] != MAP_FAILED) {
            munmap(s->maps[i], s->offer.size);
        }
        close(s->fds[i]);
    }
    free(s->staging);
    if (s->interface != NULL) {
        frame_interface_unref(s->interface);
    }
    if (s->wake_fd >= 0) {
        close(s->wake_fd);
    }
    if (s->stream_fd >= 0) {
        close(s->stream_fd);
    }
    pthread_mutex_destroy(&s->stream_lock);
    free(s);
}

static void session_unref(struct session *s) {
    if (atomic_fetch_sub(&s->refs, 1) == 1) {
        session_free(s);
    }
}

static void stream_send_return(struct session *s, uint32_t index) {
    char line[64];
    size_t n = vrx_camera_format_return(index, line, sizeof line);

    pthread_mutex_lock(&s->stream_lock);
    if (s->stream_fd >= 0 && n > 0) {
        // a full socket buffer drops the return, which the producer sees
        // as a buffer that never comes back; one line is 12 bytes
        (void) send(s->stream_fd, line, n, MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    pthread_mutex_unlock(&s->stream_lock);
}

static void stream_close(struct session *s) {
    pthread_mutex_lock(&s->stream_lock);
    if (s->stream_fd >= 0) {
        close(s->stream_fd);
        s->stream_fd = -1;
    }
    pthread_mutex_unlock(&s->stream_lock);
}

/* ------------------------------------------------- platform thread tasks */

struct reply_task {
    struct session *s;
    const FlutterPlatformMessageResponseHandle *handle;
    bool ok;
    char code[24];
    char message[96];
};

static int task_reply_open(void *userdata) {
    struct reply_task *t = userdata;
    struct session *s = t->s;

    if (t->ok) {
        platch_respond_success_std(
            t->handle,
            &STDMAP7(
                STDSTRING("textureId"),
                STDINT64(s->texture_id),
                STDSTRING("width"),
                STDINT64(s->offer.format.width),
                STDSTRING("height"),
                STDINT64(s->offer.format.height),
                STDSTRING("fourcc"),
                STDINT64(s->offer.format.fourcc),
                STDSTRING("buffers"),
                STDINT64(s->offer.buffers),
                STDSTRING("mode"),
                STDSTRING(s->mode == MODE_DMABUF ? "dmabuf" : "copy"),
                STDSTRING("copyReason"),
                s->copy_reason != NULL ? STDSTRING((char *) s->copy_reason) : STDNULL
            )
        );
    } else {
        platch_respond_error_std(t->handle, t->code, t->message, NULL);
        // nothing was handed to Dart: tear the session down here
        pthread_mutex_lock(&plugin.lock);
        if (plugin.current == s) {
            plugin.current = NULL;
        }
        pthread_mutex_unlock(&plugin.lock);
        if (s->thread_started) {
            pthread_join(s->thread, NULL);
        }
        texture_destroy(s->texture);
        session_unref(s);  // the plugin's reference
    }
    session_unref(s);  // the task's reference
    free(t);
    return 0;
}

static void post_open_reply(struct session *s, bool ok, const char *code, const char *message) {
    struct reply_task *t = calloc(1, sizeof *t);
    if (t == NULL) {
        return;
    }
    session_ref(s);
    t->s = s;
    t->handle = s->open_response;
    t->ok = ok;
    snprintf(t->code, sizeof t->code, "%s", code ? code : "");
    snprintf(t->message, sizeof t->message, "%s", message ? message : "");
    flutterpi_post_platform_task(task_reply_open, t);
}

struct ended_task {
    struct session *s;
    char reason[24];
};

static int task_report_end(void *userdata) {
    struct ended_task *t = userdata;
    static char channel[] = CHANNEL;
    static char method[] = "closed";

    if (!atomic_load(&t->s->closed_by_dart)) {
        platch_call_std(channel, method, &STDMAP2(STDSTRING("textureId"), STDINT64(t->s->texture_id), STDSTRING("reason"), STDSTRING(t->reason)), NULL, NULL);
    }
    session_unref(t->s);
    free(t);
    return 0;
}

static void post_end(struct session *s, const char *reason) {
    if (atomic_exchange(&s->end_reported, true)) {
        return;
    }
    struct ended_task *t = calloc(1, sizeof *t);
    if (t == NULL) {
        return;
    }
    session_ref(s);
    t->s = s;
    snprintf(t->reason, sizeof t->reason, "%s", reason);
    flutterpi_post_platform_task(task_report_end, t);
}

/* ---------------------------------------------------------------- Varlink */

// Returns the stream fd, or -1 with `code` set.
static int varlink_open_frames(char *code, size_t code_cap, char *message, size_t message_cap) {
    const char *path = getenv("VRX_HOST_SOCKET");
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    char call[256];
    int fd;

    if (path == NULL || path[0] == 0) {
        path = DEFAULT_HOST_SOCKET;
    }
    if (strlen(path) >= sizeof addr.sun_path) {
        snprintf(code, code_cap, "failed");
        snprintf(message, message_cap, "VRX_HOST_SOCKET is too long");
        return -1;
    }
    strcpy(addr.sun_path, path);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *) &addr, sizeof addr) != 0) {
        snprintf(code, code_cap, "unavailable");
        snprintf(message, message_cap, "cannot reach vrxhost at %s: %s", path, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }

    size_t n = vrx_camera_format_open_call("camerad", "frames", call, sizeof call);
    if (n == 0 || send(fd, call, n, MSG_NOSIGNAL) != (ssize_t) n) {
        snprintf(code, code_cap, "failed");
        snprintf(message, message_cap, "could not send Open to vrxhost");
        close(fd);
        return -1;
    }

    // the reply: one NUL-terminated JSON message, fd on it
    struct vrx_camera_lines lines;
    vrx_camera_lines_init(&lines, 0);
    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, OFFER_TIMEOUT_MS) <= 0) {
            snprintf(code, code_cap, "unavailable");
            snprintf(message, message_cap, "vrxhost did not answer Open");
            close(fd);
            return -1;
        }

        char data[1024];
        char control[CMSG_SPACE(sizeof(int) * 4)];
        struct iovec iov = { .iov_base = data, .iov_len = sizeof data };
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control };
        ssize_t got = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC);
        if (got <= 0) {
            snprintf(code, code_cap, "unavailable");
            snprintf(message, message_cap, "vrxhost closed the connection");
            close(fd);
            return -1;
        }

        int stream = -1;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm != NULL; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                int nfds = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                int *fds = (int *) CMSG_DATA(cm);
                for (int i = 0; i < nfds; i++) {
                    if (stream < 0) {
                        stream = fds[i];
                    } else {
                        close(fds[i]);
                    }
                }
            }
        }

        if (!vrx_camera_lines_feed(&lines, data, got)) {
            if (stream >= 0) {
                close(stream);
            }
            snprintf(code, code_cap, "failed");
            snprintf(message, message_cap, "vrxhost reply too long");
            close(fd);
            return -1;
        }

        char msg[1024];
        size_t msg_len;
        if (vrx_camera_lines_next(&lines, msg, sizeof msg, &msg_len)) {
            char err[96];
            close(fd);
            if (vrx_camera_varlink_error(msg, msg_len, err, sizeof err)) {
                if (stream >= 0) {
                    close(stream);
                }
                snprintf(code, code_cap, "%s", vrx_camera_error_code(err));
                snprintf(message, message_cap, "%s", err);
                return -1;
            }
            if (stream < 0) {
                snprintf(code, code_cap, "unavailable");
                snprintf(message, message_cap, "Open answered without a stream");
                return -1;
            }
            return stream;
        }
        if (stream >= 0) {
            // the descriptor arrived ahead of the message end; keep reading
            // for the message but remember the fd by closing nothing: the
            // vrxhost reply is one write, so this branch is not expected
            close(stream);
            snprintf(code, code_cap, "failed");
            snprintf(message, message_cap, "split Open reply");
            close(fd);
            return -1;
        }
    }
}

/* ----------------------------------------------------------------- offer */

static bool read_offer(struct session *s, struct vrx_camera_lines *lines, char *code, size_t code_cap, char *message, size_t message_cap) {
    for (;;) {
        char rec[1024];
        size_t rec_len;
        if (vrx_camera_lines_next(lines, rec, sizeof rec, &rec_len)) {
            if (vrx_camera_parse_offer(rec, rec_len, &s->offer) != VRX_CAMERA_PARSE_OK) {
                snprintf(code, code_cap, "bad-offer");
                snprintf(message, message_cap, "camerad's first record is not a buffer offer");
                return false;
            }
            return true;
        }

        struct pollfd p[2] = { { .fd = s->stream_fd, .events = POLLIN }, { .fd = s->wake_fd, .events = POLLIN } };
        if (poll(p, 2, OFFER_TIMEOUT_MS) <= 0 || (p[1].revents & POLLIN)) {
            snprintf(code, code_cap, "failed");
            snprintf(message, message_cap, "no buffer offer from camerad");
            return false;
        }

        char data[2048];
        char control[CMSG_SPACE(sizeof(int) * VRX_CAMERA_MAX_BUFFERS)];
        struct iovec iov = { .iov_base = data, .iov_len = sizeof data };
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control };
        ssize_t got = recvmsg(s->stream_fd, &mh, MSG_CMSG_CLOEXEC);
        if (got <= 0) {
            snprintf(code, code_cap, "failed");
            snprintf(message, message_cap, "camerad closed before its offer");
            return false;
        }
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm != NULL; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                int nfds = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                int *fds = (int *) CMSG_DATA(cm);
                for (int i = 0; i < nfds; i++) {
                    if (s->n_fds < VRX_CAMERA_MAX_BUFFERS) {
                        s->fds[s->n_fds++] = fds[i];
                    } else {
                        close(fds[i]);
                    }
                }
            }
        }
        if (!vrx_camera_lines_feed(lines, data, got)) {
            snprintf(code, code_cap, "bad-offer");
            snprintf(message, message_cap, "record too long");
            return false;
        }
    }
}

/* ------------------------------------------------------- dmabuf and copy */

static bool setup_dmabuf(struct session *s) {
    const struct vrx_camera_format *f = &s->offer.format;
    bool external_only = false;
    bool has_modifier = f->modifier != 0 && f->modifier != DRM_FORMAT_MOD_INVALID;

    // When EGL lists importable (format, modifier) pairs the buffer's must
    // be among them; when it lists none (no modifiers extension) the import
    // below is the test.
    if (frame_interface_get_n_formats(s->interface) > 0) {
        if (!egl_dmabuf_find_format(s->interface, f->fourcc, has_modifier ? f->modifier : DRM_FORMAT_MOD_LINEAR, &external_only)) {
            s->copy_reason = "EGL lists no import for this format and modifier";
            return false;
        }
    }

    for (uint32_t i = 0; i < s->offer.buffers; i++) {
        struct egl_dmabuf_desc desc = {
            .width = f->width,
            .height = f->height,
            .drm_format = f->fourcc,
            .n_planes = 1,
            .color_space = EGL_NONE,
            .sample_range_hint = EGL_NONE,
            .horizontal_chroma_siting = EGL_NONE,
            .vertical_chroma_siting = EGL_NONE,
            .external_only = external_only,
        };
        desc.planes[0] = (struct egl_dmabuf_plane){
            .fd = s->fds[i],
            .offset = 0,
            .pitch = f->stride,
            .has_modifier = has_modifier,
            .modifier = f->modifier,
        };
        if (egl_dmabuf_import(s->interface, &desc, &s->images[i]) != 0) {
            for (int k = 0; k < s->n_images; k++) {
                egl_dmabuf_release(s->interface, &s->images[k]);
            }
            s->n_images = 0;
            s->copy_reason = "EGL refused to import a camerad buffer";
            return false;
        }
        s->n_images++;
    }
    return true;
}

static bool setup_copy(struct session *s) {
    const struct vrx_camera_format *f = &s->offer.format;

    for (uint32_t i = 0; i < s->offer.buffers; i++) {
        s->maps[i] = mmap(NULL, s->offer.size, PROT_READ, MAP_SHARED, s->fds[i], 0);
        if (s->maps[i] == MAP_FAILED) {
            LOG_ERROR("vrx_camera: cannot map buffer %u: %s\n", i, strerror(errno));
            return false;
        }
    }
    s->staging = malloc((size_t) f->width * f->height * 4);
    if (s->staging == NULL) {
        return false;
    }

    if (!frame_interface_make_current(s->interface)) {
        return false;
    }
    glGenTextures(s->offer.buffers, s->ring);
    for (uint32_t i = 0; i < s->offer.buffers; i++) {
        glBindTexture(GL_TEXTURE_2D, s->ring[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f->width, f->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        atomic_store(&s->ring_busy[i], false);
    }
    GLenum err = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    frame_interface_clear_current(s->interface);
    s->ring_created = true;
    if (err != GL_NO_ERROR) {
        LOG_ERROR("vrx_camera: cannot allocate the copy textures: GL error %u\n", err);
        return false;
    }
    return true;
}

static void on_frame_destroy(const struct texture_frame *frame, void *userdata) {
    struct frame_token *t = userdata;
    (void) frame;

    if (t->ring_slot >= 0) {
        atomic_store(&t->s->ring_busy[t->ring_slot], false);
    }
    if (t->index != UINT32_MAX) {
        stream_send_return(t->s, t->index);
    }
    session_unref(t->s);
    free(t);
}

static void push(struct session *s, struct gl_texture_frame gl, uint32_t give_back_index, int ring_slot) {
    struct frame_token *t = malloc(sizeof *t);
    if (t == NULL) {
        stream_send_return(s, give_back_index);
        return;
    }
    session_ref(s);
    t->s = s;
    t->index = give_back_index;
    t->ring_slot = ring_slot;
    if (texture_push_frame(s->texture, &(struct texture_frame){ .gl = gl, .destroy = on_frame_destroy, .userdata = t }) != 0) {
        on_frame_destroy(NULL, t);
    }
}

static void handle_filled(struct session *s, const struct vrx_camera_filled *f) {
    const struct vrx_camera_format *fmt = &s->offer.format;

    if (f->index >= s->offer.buffers) {
        LOG_ERROR("vrx_camera: camerad filled buffer %u of %u\n", f->index, s->offer.buffers);
        return;
    }
    atomic_fetch_add(&s->frames, 1);

    if (s->mode == MODE_DMABUF) {
        push(
            s,
            (struct gl_texture_frame){
                .target = s->images[f->index].target,
                .name = s->images[f->index].texture,
                .format = GL_RGBA8_OES,
                .width = fmt->width,
                .height = fmt->height,
            },
            f->index,
            -1
        );
        return;
    }

    // copy path
    int slot = -1;
    for (uint32_t i = 0; i < s->offer.buffers; i++) {
        if (!atomic_load(&s->ring_busy[i])) {
            slot = (int) i;
            break;
        }
    }
    if (slot < 0) {
        atomic_fetch_add(&s->dropped, 1);
        stream_send_return(s, f->index);
        return;
    }

    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    (void) ioctl(s->fds[f->index], DMA_BUF_IOCTL_SYNC, &sync);  // ENOTTY on a memfd, which is fine
    const uint8_t *src = s->maps[f->index];
    for (uint32_t y = 0; y < fmt->height; y++) {
        const uint8_t *in = src + (size_t) y * fmt->stride;
        uint8_t *out = s->staging + (size_t) y * fmt->width * 4;
        for (uint32_t x = 0; x < fmt->width; x++, in += 4, out += 4) {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out[3] = 0xFF;
        }
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    (void) ioctl(s->fds[f->index], DMA_BUF_IOCTL_SYNC, &sync);

    if (!frame_interface_make_current(s->interface)) {
        atomic_fetch_add(&s->dropped, 1);
        stream_send_return(s, f->index);
        return;
    }
    glBindTexture(GL_TEXTURE_2D, s->ring[slot]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fmt->width, fmt->height, GL_RGBA, GL_UNSIGNED_BYTE, s->staging);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFinish();
    frame_interface_clear_current(s->interface);

    uint64_t n = atomic_fetch_add(&s->copied_frames, 1) + 1;
    atomic_fetch_add(&s->copied_bytes, (uint64_t) fmt->width * fmt->height * 4);
    if (n % 300 == 0) {
        LOG_DEBUG("vrx_camera: copy path: %" PRIu64 " frames, %" PRIu64 " bytes uploaded\n", n, (uint64_t) atomic_load(&s->copied_bytes));
    }

    stream_send_return(s, f->index);  // the data is copied, the buffer is free
    atomic_store(&s->ring_busy[slot], true);
    push(
        s,
        (struct gl_texture_frame){ .target = GL_TEXTURE_2D, .name = s->ring[slot], .format = GL_RGBA8_OES, .width = fmt->width, .height = fmt->height },
        UINT32_MAX,
        slot
    );
}

/* ---------------------------------------------------------------- worker */

static void *worker(void *arg) {
    struct session *s = arg;
    char code[24] = "failed", message[96] = "";
    struct vrx_camera_lines lines;
    const char *force_copy = getenv("VRX_CAMERA_FORCE_COPY");
    bool forced = force_copy != NULL && force_copy[0] != 0 && strcmp(force_copy, "0") != 0;

    s->stream_fd = varlink_open_frames(code, sizeof code, message, sizeof message);
    if (s->stream_fd < 0) {
        goto fail;
    }

    vrx_camera_lines_init(&lines, '\n');
    if (!read_offer(s, &lines, code, sizeof code, message, sizeof message)) {
        goto fail;
    }

    const struct vrx_camera_format *f = &s->offer.format;
    if (!s->offer.has_format) {
        snprintf(code, sizeof code, "no-format");
        snprintf(message, sizeof message, "camerad's offer carries no pixel format");
        goto fail;
    }
    if ((uint32_t) s->n_fds != s->offer.buffers) {
        snprintf(code, sizeof code, "bad-offer");
        snprintf(message, sizeof message, "the offer names %u buffers, %d descriptors came", s->offer.buffers, s->n_fds);
        goto fail;
    }
    if (f->fourcc != DRM_FORMAT_XRGB8888 && f->fourcc != DRM_FORMAT_ARGB8888) {
        snprintf(code, sizeof code, "bad-format");
        snprintf(message, sizeof message, "pixel format %08x is not XRGB8888", f->fourcc);
        goto fail;
    }
    if ((uint64_t) f->stride * f->height > s->offer.size || f->stride < f->width * 4) {
        snprintf(code, sizeof code, "bad-offer");
        snprintf(message, sizeof message, "stride and height exceed the buffer size");
        goto fail;
    }

    if (forced) {
        s->copy_reason = "VRX_CAMERA_FORCE_COPY is set";
    }
    if (forced || !setup_dmabuf(s)) {
        LOG_ERROR("vrx_camera: using the copy path: %s\n", s->copy_reason);
        s->mode = MODE_COPY;
        if (!setup_copy(s)) {
            snprintf(code, sizeof code, "failed");
            snprintf(message, sizeof message, "the copy path could not be set up");
            goto fail;
        }
    } else {
        s->mode = MODE_DMABUF;
        LOG_DEBUG("vrx_camera: %u dmabufs imported as EGL images (zero copy)\n", s->offer.buffers);
    }

    post_open_reply(s, true, NULL, NULL);

    for (;;) {
        char rec[512];
        size_t rec_len;
        bool progressed = false;
        while (vrx_camera_lines_next(&lines, rec, sizeof rec, &rec_len)) {
            struct vrx_camera_filled filled;
            if (vrx_camera_parse_filled(rec, rec_len, &filled) == VRX_CAMERA_PARSE_OK) {
                handle_filled(s, &filled);
            }
            progressed = true;
        }
        (void) progressed;

        struct pollfd p[2] = { { .fd = s->stream_fd, .events = POLLIN }, { .fd = s->wake_fd, .events = POLLIN } };
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (p[1].revents & POLLIN) {
            break;  // closed by Dart
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char data[2048];
            ssize_t got = recv(s->stream_fd, data, sizeof data, 0);
            if (got <= 0) {
                post_end(s, "producer-closed");
                break;
            }
            if (!vrx_camera_lines_feed(&lines, data, got)) {
                post_end(s, "error");
                break;
            }
        }
    }

    stream_close(s);
    session_unref(s);  // the worker's reference
    return NULL;

fail:
    stream_close(s);
    post_open_reply(s, false, code, message);
    session_unref(s);  // the worker's reference
    return NULL;
}

/* -------------------------------------------------------- platform channel */

static int respond_error(const FlutterPlatformMessageResponseHandle *h, char *code, char *message) {
    return platch_respond_error_std(h, code, message, NULL);
}

static int on_open(struct platch_obj *object, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct session *s;
    struct frame_interface *interface;
    (void) object;

    pthread_mutex_lock(&plugin.lock);
    if (plugin.current != NULL) {
        pthread_mutex_unlock(&plugin.lock);
        return respond_error(responsehandle, "busy", "this app already has the camera open");
    }

    if (!flutterpi_has_gl_renderer(plugin.flutterpi)) {
        pthread_mutex_unlock(&plugin.lock);
        return respond_error(responsehandle, "failed", "the camera view needs the OpenGL renderer");
    }

    interface = frame_interface_new(flutterpi_get_gl_renderer(plugin.flutterpi));
    if (interface == NULL) {
        pthread_mutex_unlock(&plugin.lock);
        return respond_error(responsehandle, "failed", "EGL cannot import dmabufs here");
    }

    s = calloc(1, sizeof *s);
    if (s == NULL) {
        frame_interface_unref(interface);
        pthread_mutex_unlock(&plugin.lock);
        return respond_error(responsehandle, "failed", "out of memory");
    }
    atomic_init(&s->refs, 2);  // the plugin's, and the worker's
    s->interface = interface;
    s->stream_fd = -1;
    s->wake_fd = eventfd(0, EFD_CLOEXEC);
    pthread_mutex_init(&s->stream_lock, NULL);
    s->open_response = responsehandle;
    s->texture = flutterpi_create_texture(plugin.flutterpi);
    if (s->wake_fd < 0 || s->texture == NULL) {
        s->n_fds = 0;
        session_unref(s);
        session_unref(s);
        pthread_mutex_unlock(&plugin.lock);
        return respond_error(responsehandle, "failed", "cannot create the texture");
    }
    s->texture_id = texture_get_id(s->texture);

    if (pthread_create(&s->thread, NULL, worker, s) != 0) {
        texture_destroy(s->texture);
        session_unref(s);
        session_unref(s);
        pthread_mutex_unlock(&plugin.lock);
        return respond_error(responsehandle, "failed", "cannot start the camera worker");
    }
    s->thread_started = true;
    plugin.current = s;
    pthread_mutex_unlock(&plugin.lock);
    return 0;  // answered by the worker through the platform thread
}

static int on_close(struct platch_obj *object, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct std_value *id = stdmap_get_str(&object->std_arg, "textureId");
    struct session *s = NULL;

    pthread_mutex_lock(&plugin.lock);
    if (plugin.current != NULL && id != NULL && STDVALUE_IS_INT(*id) && STDVALUE_AS_INT(*id) == plugin.current->texture_id) {
        s = plugin.current;
        plugin.current = NULL;
    }
    pthread_mutex_unlock(&plugin.lock);

    if (s != NULL) {
        atomic_store(&s->closed_by_dart, true);
        uint64_t one = 1;
        (void) write(s->wake_fd, &one, sizeof one);
        pthread_join(s->thread, NULL);  // the worker ends within one poll
        texture_destroy(s->texture);
        LOG_DEBUG(
            "vrx_camera: closed: %" PRIu64 " frames, %" PRIu64 " dropped, %" PRIu64 " frames and %" PRIu64 " bytes copied\n",
            (uint64_t) atomic_load(&s->frames),
            (uint64_t) atomic_load(&s->dropped),
            (uint64_t) atomic_load(&s->copied_frames),
            (uint64_t) atomic_load(&s->copied_bytes)
        );
        session_unref(s);  // the plugin's; the engine's last frame may still hold more
    }
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_stats(struct platch_obj *object, const FlutterPlatformMessageResponseHandle *responsehandle) {
    struct session *s;
    (void) object;

    pthread_mutex_lock(&plugin.lock);
    s = plugin.current;
    if (s == NULL) {
        pthread_mutex_unlock(&plugin.lock);
        return platch_respond_success_std(responsehandle, NULL);
    }
    int r = platch_respond_success_std(
        responsehandle,
        &STDMAP4(
            STDSTRING("frames"),
            STDINT64((int64_t) atomic_load(&s->frames)),
            STDSTRING("dropped"),
            STDINT64((int64_t) atomic_load(&s->dropped)),
            STDSTRING("copiedBytes"),
            STDINT64((int64_t) atomic_load(&s->copied_bytes)),
            STDSTRING("mode"),
            STDSTRING(s->mode == MODE_DMABUF ? "dmabuf" : "copy")
        )
    );
    pthread_mutex_unlock(&plugin.lock);
    return r;
}

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) channel;
    if (strcmp(object->method, "open") == 0) {
        return on_open(object, responsehandle);
    } else if (strcmp(object->method, "close") == 0) {
        return on_close(object, responsehandle);
    } else if (strcmp(object->method, "stats") == 0) {
        return on_stats(object, responsehandle);
    }
    return platch_respond_not_implemented(responsehandle);
}

enum plugin_init_result vrx_camera_init(struct flutterpi *flutterpi, void **userdata_out) {
    plugin.flutterpi = flutterpi;
    if (plugin_registry_set_receiver_locked(CHANNEL, kStandardMethodCall, on_receive) != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }
    *userdata_out = NULL;
    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void vrx_camera_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) userdata;
    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), CHANNEL);
}

FLUTTERPI_PLUGIN("vrx camera", vrx_camera, vrx_camera_init, vrx_camera_deinit)
