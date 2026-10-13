// SPDX-License-Identifier: MIT
//
// flutter-vrx compositor role (plan 0005, milestones B2 and B3).
//
// `flutter-vrx --compositor` is the display server of a vrx board in the
// only sense vrx needs one: DRM master owning scanout and input, and
// flipping KMS scanout to the focused app's latest presented dmabuf. There
// is no Wayland, no X11, no protocol negotiation: the wire is vrx_ipc.h,
// and only our own --app processes and vrxhost ever speak it.
//
// The compositor launches nothing and decides no focus. vrxhost (plan 0007
// amendment C) spawns the apps, connects here as the one controller
// (VRX_MSG_CONTROL, root only), and names the focused app by pid; the
// compositor reports each app's arrival and exit back to it. Until the
// controller names one, no app is focused and the screen stays dark.
//
// Exactly one app is focused and the compositor scans out its newest
// buffer. Scanout is one modeset, then page flips: a frame waits for the
// flip slot (newer frames replace a waiting one), and every buffer goes
// back to its app with VRX_MSG_RELEASE once it is off screen or
// superseded, so an app never renders into the frame being displayed.
// Unfocused apps' buffers are released on arrival.

#define _GNU_SOURCE

#include "vrx_compositor.h"

#ifndef FLUTTER_VRX_BUILD
// also compiled standalone for syntax checks
#endif

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <libinput.h>
#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "vrx_ipc.h"

// ─── app client ───────────────────────────────────────────────────────────

// connections: the apps plus the controller
#define MAX_APPS 8

struct app_client {
    bool in_use;        // slot occupied; slots never move (epoll holds them)
    int fd;
    pid_t pid;          // SO_PEERCRED at accept: who vrxhost spawned
    uid_t uid;
    bool registered;
    bool controller;    // this connection is vrxhost, not an app
    char id[sizeof(struct vrx_msg_register)];
};

// One presented buffer on its way to (or on) the screen.
struct frame {
    bool valid;
    int app;            // slot of the app that presented it, -1 once gone
    int app_fd;         // that app's socket when presented (slot reuse guard)
    uint32_t sequence;  // the app's sequence, echoed in VRX_MSG_RELEASE
    int dmabuf_fd;
    uint32_t handle;    // GEM handle in the KMS device (from the dmabuf)
    uint32_t fb_id;
};

struct vrx_compositor {
    int drm_fd;
    bool logged_format;        // first frame's format/modifier logged
    uint32_t connector_id, crtc_id;
    drmModeModeInfo mode;
    uint32_t width, height;
    uint32_t width_mm, height_mm;  // the connector's physical size, 0 = unknown
    bool mode_set;             // first frame did the modeset

    int sock_fd;               // listening socket
    int epoll_fd;

    struct libinput *li;
    int li_fd;

    struct app_client apps[MAX_APPS];
    int focused;               // slot, -1 = none
    int controller;            // slot of vrxhost's connection, -1 = none
    pid_t want_focus;          // the controller's choice, 0 = none; may name
                               // an app that has not registered yet

    // scanout pipeline: what is on screen, what a requested flip will show,
    // and the newest frame waiting for the flip slot
    struct frame on_screen, pending, queued;

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
    c->width_mm = conn->mmWidth;
    c->height_mm = conn->mmHeight;

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
    fprintf(stderr, "[vrx-compositor] %s: %ux%u@%u on crtc %u, %ux%umm\n",
            path, c->width, c->height, c->mode.vrefresh, c->crtc_id, c->width_mm, c->height_mm);
    return 0;
}

static void client_send(struct vrx_compositor *c, int fd, uint32_t type,
                        const void *payload, size_t len, int pass_fd);

// Give a frame back: tell its app the buffer is free (if the app is still
// the one that presented it), then drop our framebuffer, import and fd.
static void frame_release(struct vrx_compositor *c, struct frame *f) {
    if (!f->valid) return;
    if (f->app >= 0 && f->app < MAX_APPS && c->apps[f->app].in_use &&
        c->apps[f->app].fd == f->app_fd) {
        struct vrx_msg_release r = { .sequence = f->sequence };
        client_send(c, f->app_fd, VRX_MSG_RELEASE, &r, sizeof r, -1);
    }
    if (f->fb_id) drmModeRmFB(c->drm_fd, f->fb_id);
    if (f->handle) drmCloseBufferHandle(c->drm_fd, f->handle);
    if (f->dmabuf_fd >= 0) close(f->dmabuf_fd);
    memset(f, 0, sizeof *f);
    f->dmabuf_fd = -1;
    f->app = -1;
}

// Import a presented dmabuf as a KMS framebuffer. The dmabuf goes straight
// into the KMS device (drmPrimeFDToHandle): on split display/render
// hardware (the CM4: vc4 scans out, v3d renders) a GBM device opened on
// the KMS node is Mesa's kmsro shim, whose bo handles belong to the render
// driver, and drmModeAddFB2 rejects them with ENOENT (bench, 2026-10-13).
static bool frame_import(struct vrx_compositor *c, struct frame *f,
                         const struct vrx_msg_buffer *b) {
    if (!c->logged_format) {
        fprintf(stderr, "[vrx-compositor] first frame: %ux%u pitch %u format %.4s modifier 0x%" PRIx64 "\n",
                b->width, b->height, b->pitch, (const char *) &b->format, b->modifier);
        c->logged_format = true;
    }
    if (drmPrimeFDToHandle(c->drm_fd, f->dmabuf_fd, &f->handle) != 0) {
        fprintf(stderr, "[vrx-compositor] drmPrimeFDToHandle: %s\n", strerror(errno));
        return false;
    }
    uint32_t handles[4] = { f->handle }, pitches[4] = { b->pitch }, offsets[4] = { 0 };
    uint64_t mods[4] = { b->modifier };
    int r;
    if (b->modifier != DRM_FORMAT_MOD_INVALID && b->modifier != DRM_FORMAT_MOD_LINEAR) {
        r = drmModeAddFB2WithModifiers(c->drm_fd, b->width, b->height, b->format,
                                       handles, pitches, offsets, mods, &f->fb_id,
                                       DRM_MODE_FB_MODIFIERS);
    } else {
        r = drmModeAddFB2(c->drm_fd, b->width, b->height, b->format,
                          handles, pitches, offsets, &f->fb_id, 0);
    }
    if (r || !f->fb_id) {
        fprintf(stderr, "[vrx-compositor] drmModeAddFB2: %s\n", strerror(errno));
        return false;
    }
    return true;
}

// Ask for the queued frame to be flipped in, if the flip slot is free.
static void flip_queued(struct vrx_compositor *c) {
    if (!c->queued.valid || c->pending.valid) return;
    if (!c->mode_set) {
        // the very first frame: one modeset, after which everything flips
        if (drmModeSetCrtc(c->drm_fd, c->crtc_id, c->queued.fb_id, 0, 0,
                           &c->connector_id, 1, &c->mode) != 0) {
            fprintf(stderr, "[vrx-compositor] drmModeSetCrtc: %s\n", strerror(errno));
            frame_release(c, &c->queued);
            return;
        }
        c->mode_set = true;
        c->on_screen = c->queued;
        memset(&c->queued, 0, sizeof c->queued);
        c->queued.dmabuf_fd = -1;
        c->queued.app = -1;
        fprintf(stderr, "[vrx-compositor] modeset: first frame from app %d on screen\n",
                c->on_screen.app);
        return;
    }
    if (drmModePageFlip(c->drm_fd, c->crtc_id, c->queued.fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, c) != 0) {
        fprintf(stderr, "[vrx-compositor] drmModePageFlip: %s\n", strerror(errno));
        frame_release(c, &c->queued);
        return;
    }
    c->pending = c->queued;
    memset(&c->queued, 0, sizeof c->queued);
    c->queued.dmabuf_fd = -1;
    c->queued.app = -1;
}

// DRM says the requested flip happened: the old screen buffer is free.
static void on_page_flip(int fd, unsigned int seq, unsigned int sec,
                         unsigned int usec, void *data) {
    (void) fd; (void) seq; (void) sec; (void) usec;
    struct vrx_compositor *c = data;
    if (c->on_screen.app_fd != c->pending.app_fd) {
        fprintf(stderr, "[vrx-compositor] screen switched to slot %d ('%s')\n", c->pending.app,
                c->pending.app >= 0 ? c->apps[c->pending.app].id : "?");
    }
    frame_release(c, &c->on_screen);
    c->on_screen = c->pending;
    memset(&c->pending, 0, sizeof c->pending);
    c->pending.dmabuf_fd = -1;
    c->pending.app = -1;
    flip_queued(c);
}

static void drm_readable(struct vrx_compositor *c) {
    drmEventContext ev = {
        .version = 2,
        .page_flip_handler = on_page_flip,
    };
    drmHandleEvent(c->drm_fd, &ev);
}

// ─── socket + clients ─────────────────────────────────────────────────────

// The socket is root:vrx 0660: apps run as the unprivileged vrx user
// (plan 0007 amendment B) and must be able to connect; nobody else can.
// No vrx group means vrxhost is not installed, and then nothing would ever
// launch an app or pick a focus, so that is a startup failure, said once.
static int sock_init(struct vrx_compositor *c) {
    struct group *gr = getgrnam(VRX_UI_SOCKET_GROUP);
    if (!gr) {
        fprintf(stderr, "[vrx-compositor] no '%s' group: vrxhost (vrx-linux) is not installed, "
                        "and it is what launches apps and picks the focus\n", VRX_UI_SOCKET_GROUP);
        return -1;
    }
    mkdir(VRX_UI_SOCKET_DIR, 0755);
    unlink(VRX_UI_SOCKET);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, VRX_UI_SOCKET, sizeof(addr.sun_path) - 1);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    mode_t old = umask(0177);  // born 0600: no window where others connect
    int r = bind(fd, (struct sockaddr *) &addr, sizeof(addr));
    umask(old);
    if (r != 0 || chown(VRX_UI_SOCKET, 0, gr->gr_gid) != 0 ||
        chmod(VRX_UI_SOCKET, 0660) != 0 || listen(fd, 8) != 0) {
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
        .width_mm = c->width_mm,
        .height_mm = c->height_mm,
    };
    r.formats[0] = DRM_FORMAT_XRGB8888;
    r.modifiers[0] = DRM_FORMAT_MOD_LINEAR;
    r.formats[1] = DRM_FORMAT_ARGB8888;
    r.modifiers[1] = DRM_FORMAT_MOD_LINEAR;
    client_send(c, a->fd, VRX_MSG_REGISTERED, &r, sizeof r, -1);
}

// Tell vrxhost an app arrived (registered) or left. Silent without a
// controller: vrxhost learns the current set when it connects.
static void report_app(struct vrx_compositor *c, const struct app_client *a, bool alive) {
    if (c->controller < 0) return;
    struct vrx_msg_app_state m = { .pid = a->pid, .alive = alive ? 1 : 0 };
    snprintf(m.id, sizeof m.id, "%s", a->id);
    client_send(c, c->apps[c->controller].fd, VRX_MSG_APP_STATE, &m, sizeof m, -1);
}

static void focus_slot(struct vrx_compositor *c, int slot) {
    if (slot == c->focused) return;
    int old = c->focused;
    c->focused = slot;
    if (old >= 0 && c->apps[old].in_use) {
        client_send(c, c->apps[old].fd, VRX_MSG_FOCUS,
                    &(struct vrx_msg_focus){ .focused = 0 }, sizeof(struct vrx_msg_focus), -1);
    }
    // a frame from the old focus still waiting for the flip slot never shows
    frame_release(c, &c->queued);
    if (slot < 0) {
        fprintf(stderr, "[vrx-compositor] focus -> none\n");
        return;
    }
    fprintf(stderr, "[vrx-compositor] focus -> slot %d ('%s', pid %d)\n", slot,
            c->apps[slot].id, (int) c->apps[slot].pid);
    // focus gain makes the app re-present its current frame (an idle app
    // has nothing new to draw), which is what flips the screen to it
    client_send(c, c->apps[slot].fd, VRX_MSG_FOCUS,
                &(struct vrx_msg_focus){ .focused = 1 }, sizeof(struct vrx_msg_focus), -1);
}

// Apply the controller's choice if that app is here; if it has not
// registered yet, its REGISTER applies it.
static void apply_want_focus(struct vrx_compositor *c) {
    int slot = -1;
    for (int i = 0; c->want_focus > 0 && i < MAX_APPS; i++) {
        struct app_client *a = &c->apps[i];
        if (a->in_use && a->registered && a->pid == c->want_focus) { slot = i; break; }
    }
    focus_slot(c, slot);
}

static void drop_client(struct vrx_compositor *c, int slot) {
    struct app_client *a = &c->apps[slot];
    if (!a->in_use) return;
    epoll_ctl(c->epoll_fd, EPOLL_CTL_DEL, a->fd, NULL);
    close(a->fd);
    if (a->controller) {
        // the focus stays where vrxhost left it; a restarted vrxhost
        // reconnects and re-sends it
        fprintf(stderr, "[vrx-compositor] controller (pid %d) gone\n", (int) a->pid);
        c->controller = -1;
        memset(a, 0, sizeof *a);
        a->fd = -1;
        return;
    }
    fprintf(stderr, "[vrx-compositor] app '%s' (pid %d) gone\n", a->id, (int) a->pid);
    if (a->registered) report_app(c, a, false);
    // Frames it presented stay valid (we hold our own fd and import): the
    // screen keeps its last frame until the next focused app's frame
    // replaces it. Only the RELEASE goes nowhere, which frame_release checks.
    struct frame *frames[] = { &c->on_screen, &c->pending, &c->queued };
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        if (frames[i]->valid && frames[i]->app == slot) frames[i]->app = -1;
    }
    memset(a, 0, sizeof *a);
    a->fd = -1;
    // what comes next on screen is vrxhost's decision (it sees the exit
    // both here and as its child's status)
    if (c->focused == slot) c->focused = -1;
}

// Handle one message. `fd` is the descriptor that rode it, or -1; this
// function takes ownership of it.
static void client_handle(struct vrx_compositor *c, int slot,
                          const struct vrx_msg_header *h, void *payload, int fd) {
    struct app_client *a = &c->apps[slot];
    // a connection is an app (REGISTER) or the controller (CONTROL), once
    bool app_msg = h->type == VRX_MSG_BUFFER || h->type == VRX_MSG_CLOSED;
    if ((app_msg && !a->registered) || (h->type == VRX_MSG_SET_FOCUS && !a->controller) ||
        ((h->type == VRX_MSG_REGISTER || h->type == VRX_MSG_CONTROL) &&
         (a->registered || a->controller))) {
        goto bad;
    }
    switch (h->type) {
        case VRX_MSG_REGISTER: {
            struct vrx_msg_register *r = payload;
            if (h->length < sizeof *r) goto bad;
            r->id[sizeof r->id - 1] = 0;
            snprintf(a->id, sizeof a->id, "%s", r->id);
            a->registered = true;
            fprintf(stderr, "[vrx-compositor] app '%s' registered (slot %d, pid %d)\n", a->id,
                    slot, (int) a->pid);
            send_registered(c, a);
            report_app(c, a, true);
            if (c->want_focus > 0 && a->pid == c->want_focus) focus_slot(c, slot);
            break;
        }
        case VRX_MSG_CONTROL: {
            // root only: an app (the vrx user) must never steer focus
            if (a->uid != 0 || c->controller >= 0) {
                fprintf(stderr, "[vrx-compositor] refusing controller (pid %d uid %d): %s\n",
                        (int) a->pid, (int) a->uid,
                        a->uid != 0 ? "not root" : "one is connected");
                goto bad;
            }
            a->controller = true;
            c->controller = slot;
            fprintf(stderr, "[vrx-compositor] controller connected (pid %d)\n", (int) a->pid);
            for (int i = 0; i < MAX_APPS; i++) {
                if (c->apps[i].in_use && c->apps[i].registered) report_app(c, &c->apps[i], true);
            }
            break;
        }
        case VRX_MSG_SET_FOCUS: {
            struct vrx_msg_set_focus *m = payload;
            if (h->length < sizeof *m) goto bad;
            c->want_focus = m->pid;
            apply_want_focus(c);
            break;
        }
        case VRX_MSG_BUFFER: {
            struct vrx_msg_buffer *b = payload;
            if (h->length < sizeof *b || fd < 0) goto bad;
            struct frame f = {
                .valid = true, .app = slot, .app_fd = a->fd,
                .sequence = b->sequence, .dmabuf_fd = fd,
            };
            fd = -1;  // the frame owns it now
            if (slot != c->focused || !frame_import(c, &f, b)) {
                // unfocused (or unimportable): straight back to the app
                frame_release(c, &f);
                break;
            }
            // newest wins: a frame still waiting for the flip slot is
            // superseded and returned
            frame_release(c, &c->queued);
            c->queued = f;
            flip_queued(c);
            break;
        }
        case VRX_MSG_CLOSED:
            fprintf(stderr, "[vrx-compositor] app '%s' said closed\n", a->id);
            break;
        default:
            goto bad;
    }
    if (fd >= 0) close(fd);
    return;
bad:
    if (fd >= 0) close(fd);
    fprintf(stderr, "[vrx-compositor] bad message (type %u len %u) from slot %d\n",
            h->type, h->length, slot);
    drop_client(c, slot);
}

// One message per recvmsg on a seqpacket socket; drain what is ready.
static void client_readable(struct vrx_compositor *c, int slot) {
    struct {
        struct vrx_msg_header h;
        unsigned char payload[4096];
    } pkt;
    for (;;) {
        if (!c->apps[slot].in_use) return;
        char cbuf[CMSG_SPACE(sizeof(int) * 4)];
        struct iovec iov = { .iov_base = &pkt, .iov_len = sizeof pkt };
        struct msghdr mh = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = cbuf, .msg_controllen = sizeof cbuf,
        };
        ssize_t n = recvmsg(c->apps[slot].fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) { drop_client(c, slot); return; }

        int fds[4] = { -1, -1, -1, -1 }, n_fds = 0;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                n_fds = (int) ((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                if (n_fds > 4) n_fds = 4;
                memcpy(fds, CMSG_DATA(cm), (size_t) n_fds * sizeof(int));
            }
        }
        for (int i = 1; i < n_fds; i++) close(fds[i]);  // at most one is ours

        bool truncated = (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
        if (truncated || (size_t) n < sizeof pkt.h ||
            (size_t) n < sizeof pkt.h + pkt.h.length) {
            if (n_fds > 0) close(fds[0]);
            fprintf(stderr, "[vrx-compositor] malformed message from slot %d\n", slot);
            drop_client(c, slot);
            return;
        }
        client_handle(c, slot, &pkt.h, pkt.payload, n_fds > 0 ? fds[0] : -1);
    }
}

// ─── input ────────────────────────────────────────────────────────────────

static int li_open_restricted(const char *path, int flags, void *user_data) {
    (void) user_data;
    return open(path, flags | O_CLOEXEC);
}

static void li_close_restricted(int fd, void *user_data) {
    (void) user_data;
    close(fd);
}

static int li_init(struct vrx_compositor *c) {
    static const struct libinput_interface li_if = {
        .open_restricted = li_open_restricted,
        .close_restricted = li_close_restricted,
    };
    struct udev *udev = udev_new();
    if (!udev) return -1;
    c->li = libinput_udev_create_context(&li_if, NULL, udev);
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

// ─── main loop ────────────────────────────────────────────────────────────

// epoll tags: the high bits mark the kind, a client tag also carries its
// socket fd so an event queued for a slot that was dropped (and maybe
// reused) in the same batch is recognised and ignored.
#define TAG_LISTENER  0xFFFFFFFFFFFFFFFEull
#define TAG_LIBINPUT  0xFFFFFFFFFFFFFFFDull
#define TAG_DRM       0xFFFFFFFFFFFFFFFCull
#define CLIENT_TAG(slot, fd) (((uint64_t) (uint32_t) (fd) << 32) | (uint32_t) (slot))

static void accept_client(struct vrx_compositor *c) {
    int fd = accept4(c->sock_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    int slot = -1;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!c->apps[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        fprintf(stderr, "[vrx-compositor] %d apps already; refusing another\n", MAX_APPS);
        close(fd);
        return;
    }
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        fprintf(stderr, "[vrx-compositor] SO_PEERCRED: %s; refusing\n", strerror(errno));
        close(fd);
        return;
    }
    memset(&c->apps[slot], 0, sizeof c->apps[slot]);
    c->apps[slot].in_use = true;
    c->apps[slot].fd = fd;
    c->apps[slot].pid = cred.pid;
    c->apps[slot].uid = cred.uid;
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = CLIENT_TAG(slot, fd) };
    epoll_ctl(c->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

int vrx_compositor_main(int argc, char **argv) {
    const char *device = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--compositor")) continue;
        if (i + 1 < argc && !strcmp(argv[i], "--device")) device = argv[++i];
    }
    struct vrx_compositor c = {0};
    c.focused = -1;
    c.controller = -1;
    struct frame *frames[] = { &c.on_screen, &c.pending, &c.queued };
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        frames[i]->dmabuf_fd = -1;
        frames[i]->app = -1;
    }
    for (int i = 0; i < MAX_APPS; i++) c.apps[i].fd = -1;
    if (kms_init(&c, device) != 0) return 1;
    if (sock_init(&c) != 0) return 1;
    if (li_init(&c) != 0) {
        fprintf(stderr, "[vrx-compositor] libinput init failed; continuing without input\n");
    }
    c.epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = TAG_LISTENER };
    epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.sock_fd, &ev);
    ev.data.u64 = TAG_DRM;
    epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.drm_fd, &ev);
    if (c.li_fd > 0) {
        ev.data.u64 = TAG_LIBINPUT;
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.li_fd, &ev);
    }

    c.running = true;

    fprintf(stderr, "[vrx-compositor] running (socket %s)\n", VRX_UI_SOCKET);
    while (c.running) {
        struct epoll_event out[8];
        int n = epoll_wait(c.epoll_fd, out, 8, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            uint64_t tag = out[i].data.u64;
            if (tag == TAG_LISTENER) {
                accept_client(&c);
            } else if (tag == TAG_LIBINPUT) {
                li_dispatch(&c);
            } else if (tag == TAG_DRM) {
                drm_readable(&c);
            } else {
                int slot = (int) (uint32_t) tag;
                int fd = (int) (tag >> 32);
                if (slot >= 0 && slot < MAX_APPS && c.apps[slot].in_use &&
                    c.apps[slot].fd == fd) {
                    client_readable(&c, slot);
                }
            }
        }
    }
    return 0;
}
