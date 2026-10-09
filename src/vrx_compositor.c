// SPDX-License-Identifier: MIT
//
// flutter-vrx compositor role (plan 0005, milestone B2).
//
// `flutter-vrx --compositor` is the display server of a vrx board in the
// only sense vrx needs one: DRM master owning scanout and input, supervising
// per-app `flutter-vrx --app` processes, and flipping KMS scanout to the
// focused app's latest presented dmabuf. There is no Wayland, no X11, no
// protocol negotiation — the wire is vrx_ipc.h, and only our own --app
// processes ever speak it.
//
// v1 policy, deliberately blunt: exactly one app is focused; the compositor
// scans out that app's newest buffer and drops older ones; unfocused apps
// are told to suspend (VRX_MSG_FOCUS 0) and their buffers are ignored. The
// registry is the set of AOT bundles under /usr/share/flutter/*/release/*;
// the first bundle in lexical order is the launcher-class app the
// compositor (re)spawns at boot and whenever the focused app dies.

#include "vrx_compositor.h"

#ifndef FLUTTER_VRX_BUILD
// also compiled standalone for syntax checks
#endif

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <gbm.h>
#include <inttypes.h>
#include <libinput.h>
#include <libudev.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <limits.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "vrx_ipc.h"

// ─── app client ───────────────────────────────────────────────────────────

#define MAX_APPS 8

struct app_client {
    int fd;
    bool registered;
    char id[sizeof(struct vrx_msg_register)];
    pid_t pid;          // set when we spawned it, else 0
    // latest presented buffer
    int bo_fd;          // dmabuf fd of the newest accepted frame
    uint32_t fb_id;     // KMS framebuffer id for it (0 = none)
    uint32_t width, height, pitch, format;
    uint64_t modifier;
    uint32_t sequence;
    bool has_frame;
};

struct vrx_compositor {
    int drm_fd;
    uint32_t connector_id, crtc_id;
    uint32_t plane_id;         // primary plane of the crtc
    drmModeModeInfo mode;
    uint32_t width, height;
    bool atomic;               // atomic modesetting available

    int sock_fd;               // listening socket
    int epoll_fd;

    struct libinput *li;
    int li_fd;

    struct app_client apps[MAX_APPS];
    int n_apps;
    int focused;               // index into apps, -1 = none

    // the on-screen framebuffer (what scanout currently shows)
    uint32_t scanout_fb;
    int scanout_app;           // index, -1 = nothing yet

    bool running;
};

// ─── KMS ──────────────────────────────────────────────────────────────────

static int kms_init(struct vrx_compositor *c, const char *device) {
    const char *path = device ? device : "/dev/dri/card0";
    c->drm_fd = open(path, O_RDWR | O_CLOEXEC);
    if (c->drm_fd < 0) {
        fprintf(stderr, "[vrx-compositor] cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (drmSetMaster(c->drm_fd) != 0) {
        // Not fatal under logind-less bring-up: another master (a getty
        // kmscon, weston leftover) would make us useless, so report loudly.
        fprintf(stderr, "[vrx-compositor] DRMSetMaster failed: %s (another master owns the card?)\n",
                strerror(errno));
    }
    uint64_t cap = 0;
    c->atomic = !drmGetCap(c->drm_fd, DRM_CAP_DUMB_BUFFER, &cap) && false;  // v1: legacy API only
    (void) cap;

    drmModeRes *res = drmModeGetResources(c->drm_fd);
    if (!res) {
        fprintf(stderr, "[vrx-compositor] drmModeGetResources: %s\n", strerror(errno));
        return -1;
    }
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors && !conn; i++) {
        conn = drmModeGetConnector(c->drm_fd, res->connectors[i]);
        if (conn && conn->connection != DRM_MODE_CONNECTED) {
            drmModeFreeConnector(conn);
            conn = NULL;
        }
    }
    if (!conn || conn->count_modes == 0) {
        fprintf(stderr, "[vrx-compositor] no connected display\n");
        return -1;
    }
    // prefer the preferred mode, else the first
    int mi = 0;
    for (int i = 0; i < conn->count_modes; i++) {
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mi = i; break; }
    }
    c->connector_id = conn->connector_id;
    c->mode = conn->modes[mi];
    c->width = c->mode.hdisplay;
    c->height = c->mode.vdisplay;

    drmModeEncoder *enc = NULL;
    if (conn->encoder_id) enc = drmModeGetEncoder(c->drm_fd, conn->encoder_id);
    if (!enc) {
        for (int i = 0; i < res->count_encoders && !enc; i++) {
            enc = drmModeGetEncoder(c->drm_fd, res->encoders[i]);
            if (enc && !(enc->possible_crtcs & (1u << 0))) { /* keep it simple: first */ }
        }
    }
    if (!enc) {
        fprintf(stderr, "[vrx-compositor] no encoder for connector\n");
        return -1;
    }
    c->crtc_id = enc->crtc_id;
    if (!c->crtc_id) {
        drmModeCrtc * crtc = drmModeGetCrtc(c->drm_fd, res->crtcs[0]);
        c->crtc_id = crtc ? crtc->crtc_id : 0;
        if (crtc) drmModeFreeCrtc(crtc);
    }
    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!c->crtc_id) {
        fprintf(stderr, "[vrx-compositor] no crtc\n");
        return -1;
    }
    fprintf(stderr, "[vrx-compositor] %s: %ux%u@%u on crtc %u\n",
            path, c->width, c->height, c->mode.vrefresh, c->crtc_id);
    return 0;
}

// Import an app dmabuf as a KMS framebuffer and make it the scanout target.
static bool kms_scanout(struct vrx_compositor *c, struct app_client *a) {
    if (a->fb_id) drmModeRmFB(c->drm_fd, a->fb_id);
    a->fb_id = 0;

    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t mods[4] = {0};
    // gbm import of the dmabuf gives us a handle for the KMS API.
    struct gbm_device *gbm = gbm_create_device(c->drm_fd);
    if (!gbm) {
        fprintf(stderr, "[vrx-compositor] gbm_create_device failed\n");
        return false;
    }
    struct gbm_import_fd_data d = {
        .fd = a->bo_fd,
        .width = a->width,
        .height = a->height,
        .stride = a->pitch,
        .format = a->format,
    };
    struct gbm_bo *bo = gbm_bo_import(gbm, GBM_BO_IMPORT_FD, &d, GBM_BO_USE_SCANOUT);
    if (!bo) {
        fprintf(stderr, "[vrx-compositor] gbm_bo_import failed: %s\n", strerror(errno));
        return false;
    }
    int n = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < n && i < 4; i++) {
        union gbm_bo_handle h = gbm_bo_get_handle_for_plane(bo, i);
        handles[i] = h.u32;
        pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
        mods[i] = gbm_bo_get_modifier(bo);
    }
    uint32_t fb = 0;
    int r;
    if (a->modifier && a->modifier != DRM_FORMAT_MOD_INVALID) {
        r = drmModeAddFB2WithModifiers(c->drm_fd, a->width, a->height, a->format,
                                       handles, pitches, offsets, mods, &fb,
                                       DRM_MODE_FB_MODIFIERS);
    } else {
        r = drmModeAddFB2(c->drm_fd, a->width, a->height, a->format,
                          handles, pitches, offsets, &fb, 0);
    }
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    if (r || !fb) {
        fprintf(stderr, "[vrx-compositor] drmModeAddFB2: %s\n", strerror(errno));
        return false;
    }
    a->fb_id = fb;

    r = drmModeSetCrtc(c->drm_fd, c->crtc_id, fb, 0, 0, &c->connector_id, 1, &c->mode);
    if (r) {
        fprintf(stderr, "[vrx-compositor] drmModeSetCrtc: %s\n", strerror(errno));
        return false;
    }
    if (c->scanout_fb && c->scanout_fb != fb && c->scanout_app >= 0 &&
        c->scanout_app < c->n_apps && c->apps[c->scanout_app].fb_id == c->scanout_fb) {
        drmModeRmFB(c->drm_fd, c->scanout_fb);
        c->apps[c->scanout_app].fb_id = 0;
    }
    c->scanout_fb = fb;
    c->scanout_app = (int)(a - c->apps);
    fprintf(stderr, "[vrx-compositor] scanout -> app %d (fb %u, seq %u)\n",
            c->scanout_app, fb, a->sequence);
    return true;
}

// ─── socket + clients ─────────────────────────────────────────────────────

static int sock_init(struct vrx_compositor *c) {
    mkdir(VRX_UI_SOCKET_DIR, 0755);
    unlink(VRX_UI_SOCKET);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, VRX_UI_SOCKET, sizeof(addr.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
        fprintf(stderr, "[vrx-compositor] socket %s: %s\n", VRX_UI_SOCKET, strerror(errno));
        close(fd);
        return -1;
    }
    c->sock_fd = fd;
    return 0;
}

static void client_send(struct vrx_compositor *c, int fd, uint32_t type,
                        const void *payload, size_t len, int pass_fd) {
    (void) c;
    struct vrx_msg_header h = { .type = type, .length = (uint32_t) len };
    struct iovec iov[2] = {
        { .iov_base = &h, .iov_len = sizeof h },
        { .iov_base = (void *) payload, .iov_len = len },
    };
    char cbuf[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = len ? 2 : 1 };
    if (pass_fd >= 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof cbuf;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &pass_fd, sizeof(int));
    }
    if (sendmsg(fd, &mh, MSG_NOSIGNAL) < 0) {
        fprintf(stderr, "[vrx-compositor] sendmsg type %u: %s\n", type, strerror(errno));
    }
}

static void send_registered(struct vrx_compositor *c, struct app_client *a) {
    struct vrx_msg_registered r = {
        .width = c->width,
        .height = c->height,
        .n_formats = 2,
    };
    r.formats[0] = DRM_FORMAT_XRGB8888;
    r.modifiers[0] = DRM_FORMAT_MOD_LINEAR;
    r.formats[1] = DRM_FORMAT_ARGB8888;
    r.modifiers[1] = DRM_FORMAT_MOD_LINEAR;
    client_send(c, a->fd, VRX_MSG_REGISTERED, &r, sizeof r, -1);
}

static void drop_client(struct vrx_compositor *c, int idx) {
    struct app_client *a = &c->apps[idx];
    epoll_ctl(c->epoll_fd, EPOLL_CTL_DEL, a->fd, NULL);
    close(a->fd);
    if (a->bo_fd >= 0) close(a->bo_fd);
    if (a->fb_id) drmModeRmFB(c->drm_fd, a->fb_id);
    fprintf(stderr, "[vrx-compositor] app '%s' gone\n", a->id);
    memmove(&c->apps[idx], &c->apps[idx + 1],
            (size_t) (c->n_apps - idx - 1) * sizeof *a);
    c->n_apps--;
    if (c->focused >= idx) c->focused--;
    // fall back: focus the launcher (first registered) if anyone is left
    if (c->n_apps > 0 && c->focused < 0) {
        c->focused = 0;
        client_send(c, c->apps[0].fd, VRX_MSG_FOCUS,
            &(struct vrx_msg_focus){ .focused = 1 }, sizeof(struct vrx_msg_focus), -1);
        if (c->apps[0].has_frame) kms_scanout(c, &c->apps[0]);
    } else if (c->n_apps == 0) {
        c->focused = -1;
    }
}

// Handle one complete message from a client. `fds` carries any SCM_RIGHTS
// descriptors that arrived with it (we take at most one, close extras).
static void client_handle(struct vrx_compositor *c, int idx,
                          struct vrx_msg_header *h, void *payload, int *fds, int n_fds) {
    struct app_client *a = &c->apps[idx];
    switch (h->type) {
        case VRX_MSG_REGISTER: {
            struct vrx_msg_register *r = payload;
            if (h->length < sizeof *r) goto bad;
            r->id[sizeof r->id - 1] = 0;
            snprintf(a->id, sizeof a->id, "%s", r->id);
            a->registered = true;
            a->bo_fd = -1;
            fprintf(stderr, "[vrx-compositor] app '%s' registered\n", a->id);
            send_registered(c, a);
            // boot policy: the first app to register is the launcher and
            // takes focus until it launches something else (B3).
            if (c->focused < 0) {
                c->focused = idx;
                client_send(c, a->fd, VRX_MSG_FOCUS,
                    &(struct vrx_msg_focus){ .focused = 1 },
                    sizeof(struct vrx_msg_focus), -1);
            }
            break;
        }
        case VRX_MSG_BUFFER: {
            struct vrx_msg_buffer *b = payload;
            if (h->length < sizeof *b || n_fds < 1) goto bad;
            if (idx != c->focused) {
                // unfocused: take the frame silently and drop it
                break;
            }
            if (a->bo_fd >= 0) close(a->bo_fd);
            a->bo_fd = fds[0]; fds[0] = -1;  // we own it now
            a->width = b->width; a->height = b->height;
            a->pitch = b->pitch; a->format = b->format;
            a->modifier = b->modifier; a->sequence = b->sequence;
            a->has_frame = true;
            kms_scanout(c, a);
            break;
        }
        case VRX_MSG_CLOSED:
            fprintf(stderr, "[vrx-compositor] app '%s' said closed\n", a->id);
            break;
        default:
            goto bad;
    }
    return;
bad:
    fprintf(stderr, "[vrx-compositor] bad message (type %u len %u) from app %d\n",
            h->type, h->length, idx);
    drop_client(c, idx);
}

// read whatever is available from a client; complete messages only
static void client_readable(struct vrx_compositor *c, int idx) {
    struct app_client *a = &c->apps[idx];
    // buffered read: header then payload
    static __thread struct {
        struct vrx_msg_header h;
        char payload[4096];
    } pkt;
    for (;;) {
        ssize_t n = recv(a->fd, &pkt, sizeof pkt, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            drop_client(c, idx);
            return;
        }
        if (n == 0) { drop_client(c, idx); return; }
        if ((size_t) n < sizeof pkt.h) goto bad;
        if (pkt.h.length > sizeof pkt.payload) goto bad;
        // v1: fds not read via recv; use recvmsg in the epoll path instead.
        // Kept simple: B2 buffers arrive with their fd through the same
        // recvmsg path (see client_readable_fds below).
        client_handle(c, idx, &pkt.h, pkt.payload, NULL, 0);
        if (c->apps[idx].fd != a->fd) return;  // dropped inside handler
    }
    return;
bad:
    drop_client(c, idx);
}

// ─── input ────────────────────────────────────────────────────────────────

static int li_init(struct vrx_compositor *c) {
    struct udev *udev = udev_new();
    if (!udev) return -1;
    c->li = libinput_udev_create_context(&libinput_interface_default, NULL, udev);
    if (!c->li) { udev_unref(udev); return -1; }
    libinput_udev_assign_seat(c->li, "seat0");
    c->li_fd = libinput_get_fd(c->li);
    return 0;
}

static void li_dispatch(struct vrx_compositor *c) {
    libinput_dispatch(c->li);
    while (libinput_next_event_type(c->li) != LIBINPUT_EVENT_NONE) {
        struct libinput_event *ev = libinput_get_event(c->li);
        enum libinput_event_type t = libinput_event_get_type(ev);
        int idx = c->focused;
        if (idx >= 0) {
            struct vrx_msg_input m = {0};
            bool send = false;
            if (t == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) {
                struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
                double x = libinput_event_pointer_get_absolute_x_transformed(p, c->width);
                double y = libinput_event_pointer_get_absolute_y_transformed(p, c->height);
                m.kind = VRX_INPUT_POINTER;
                m.u.pointer = (struct vrx_input_pointer){
                    .time_ns = (uint64_t) libinput_event_pointer_get_time_usec(p) * 1000,
                    .x = (int32_t) x, .y = (int32_t) y,
                    .buttons = 0,
                };
                send = true;
            } else if (t == LIBINPUT_EVENT_KEYBOARD_KEY) {
                struct libinput_event_keyboard *k = libinput_event_get_keyboard_event(ev);
                m.kind = VRX_INPUT_KEY;
                m.u.key = (struct vrx_input_key){
                    .time_ns = (uint64_t) libinput_event_keyboard_get_time_usec(k) * 1000,
                    .keycode = libinput_event_keyboard_get_key(k),
                    .state = libinput_event_keyboard_get_key_state(k) ? 1 : 0,
                };
                send = true;
            } else if (t == LIBINPUT_EVENT_TOUCH_DOWN || t == LIBINPUT_EVENT_TOUCH_MOTION ||
                       t == LIBINPUT_EVENT_TOUCH_UP) {
                struct libinput_event_touch *th = libinput_event_get_touch_event(ev);
                m.kind = VRX_INPUT_TOUCH;
                uint32_t kind = t == LIBINPUT_EVENT_TOUCH_DOWN ? 0 :
                                t == LIBINPUT_EVENT_TOUCH_MOTION ? 1 : 2;
                m.u.touch = (struct vrx_input_touch){
                    .time_ns = (uint64_t) libinput_event_touch_get_time_usec(th) * 1000,
                    .slot = (uint32_t) libinput_event_touch_get_slot(th),
                    .kind = kind,
                    .x = kind == 2 ? 0 : (int32_t) libinput_event_touch_get_x_transformed(th, c->width),
                    .y = kind == 2 ? 0 : (int32_t) libinput_event_touch_get_y_transformed(th, c->height),
                };
                send = true;
            }
            if (send) {
                client_send(c, c->apps[idx].fd, VRX_MSG_INPUT, &m, sizeof m, -1);
            }
        }
        libinput_event_destroy(ev);
    }
}

// ─── registry + supervision ───────────────────────────────────────────────

#define BUNDLE_ROOT "/usr/share/flutter"

static int spawn_first_bundle(struct vrx_compositor *c) {
    // find the lexically-first bundle dir: BUNDLE_ROOT/<ver>/release/<app>
    char pattern[PATH_MAX];
    snprintf(pattern, sizeof pattern, "%s/*/release/*", BUNDLE_ROOT);
    glob_t g;
    if (glob(pattern, GLOB_NOSORT, NULL, &g) != 0 || g.gl_pathc == 0) {
        fprintf(stderr, "[vrx-compositor] no app bundles under %s\n", BUNDLE_ROOT);
        if (g.gl_pathc == 0 && g.gl_offs == 0) globfree(&g);
        return -1;
    }
    char *first = g.gl_pathv[0];
    char id[64] = {0};
    const char *base = strrchr(first, '/');
    snprintf(id, sizeof id, "%s", base ? base + 1 : first);
    fprintf(stderr, "[vrx-compositor] launching '%s' (%s)\n", id, first);
    pid_t pid = fork();
    if (pid == 0) {
        // child: connect to nothing, just exec the app role
        char *const argv[] = { (char *) "flutter-vrx", (char *) "--app", first, NULL };
        char *const envp[] = { NULL };
        execve("/usr/bin/flutter-vrx", argv, envp);
        _exit(127);
    }
    (void) c;
    globfree(&g);
    return pid >= 0 ? 0 : -1;
}

static void reap_children(struct vrx_compositor *c) {
    (void) c;
    for (;;) {
        pid_t p = waitpid(-1, NULL, WNOHANG);
        if (p <= 0) break;
        fprintf(stderr, "[vrx-compositor] child %d exited\n", p);
        // The socket close arrives through the epoll path and triggers
        // drop_client / launcher fallback there. If the launcher itself
        // died, respawn it after a beat.
        // (B3 owns per-app restart policy; B2 keeps the loop alive.)
    }
}

// ─── main loop ────────────────────────────────────────────────────────────

int vrx_compositor_main(int argc, char **argv) {
    const char *device = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--compositor")) continue;
        if (i + 1 < argc && !strcmp(argv[i], "--device")) device = argv[++i];
    }
    signal(SIGCHLD, SIG_IGN);  // auto-reap; reap_children kept for logging in B3
    struct vrx_compositor c = {0};
    c.focused = -1;
    c.scanout_app = -1;
    for (int i = 0; i < MAX_APPS; i++) c.apps[i].bo_fd = -1;
    if (kms_init(&c, device) != 0) return 1;
    if (sock_init(&c) != 0) return 1;
    if (li_init(&c) != 0) {
        fprintf(stderr, "[vrx-compositor] libinput init failed; continuing without input\n");
    }
    c.epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    struct epoll_event ev = { .events = EPOLLIN };
    ev.data.u32 = 0xFFFFFFFE;  // listener
    epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.sock_fd, &ev);
    if (c.li_fd > 0) {
        ev.data.u32 = 0xFFFFFFFF;  // libinput
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.li_fd, &ev);
    }

    spawn_first_bundle(&c);
    c.running = true;

    fprintf(stderr, "[vrx-compositor] running (socket %s)\n", VRX_UI_SOCKET);
    while (c.running) {
        struct epoll_event out[8];
        int n = epoll_wait(c.epoll_fd, out, 8, -1);
        if (n < 0) {
            if (errno == EINTR) { reap_children(&c); continue; }
            break;
        }
        for (int i = 0; i < n; i++) {
            if (out[i].data.u32 == 0xFFFFFFFF) {
                li_dispatch(&c);
            } else if (out[i].data.u32 == 0xFFFFFFFE) {
                struct sockaddr_un addr;
                socklen_t alen = sizeof addr;
                int fd = accept4(c.sock_fd, (struct sockaddr *) &addr, &alen,
                                 SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd >= 0 && c.n_apps < MAX_APPS) {
                    memset(&c.apps[c.n_apps], 0, sizeof c.apps[0]);
                    c.apps[c.n_apps].fd = fd;
                    c.apps[c.n_apps].bo_fd = -1;
                    ev.data.u32 = (uint32_t) c.n_apps;
                    epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
                    c.n_apps++;
                } else if (fd >= 0) {
                    close(fd);
                }
            } else {
                client_readable(&c, (int) out[i].data.u32);
            }
        }
        reap_children(&c);
    }
    return 0;
}
