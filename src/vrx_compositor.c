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
#include <linux/input-event-codes.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "vrx_ipc.h"
#include "vrx_kms.h"

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
    // VRX_TRACE_LATENCY=1: log input-to-screen latency per touch/key, the
    // number a person feels as responsiveness (input sent → the app's next
    // buffer arrives → that buffer is flipped onto the panel)
    bool trace;
    uint64_t trace_input_ns;   // the untraced input being answered, 0 = none
    uint64_t trace_buffer_ns;  // when the answering buffer arrived
    bool logged_format;        // first frame's format/modifier logged
    uint32_t connector_id, crtc_id;
    char output[32];           // the connector's name, HDMI-A-1
    drmModeModeInfo mode;
    uint32_t width, height;
    uint32_t width_mm, height_mm;  // the connector's physical size, 0 = unknown
    bool mode_set;             // first frame did the modeset

    int sock_fd;               // listening socket
    int epoll_fd;

    struct libinput *li;
    int li_fd;

    // the mouse: where the pointer is, which buttons are down, and the
    // cursor drawn there on the KMS cursor plane (shown on the first
    // mouse move, hidden on a touch)
    double px, py;
    uint32_t buttons;
    uint32_t cursor_handle;    // 64x64 ARGB dumb buffer, 0 = none
    bool cursor_wanted, cursor_shown;

    // key repeat: libinput sends none, so the held key repeats from here
    int repeat_fd;
    uint32_t repeat_key;       // 0 = none held

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

static void cursor_update(struct vrx_compositor *c);
static void repeat_arm(struct vrx_compositor *c, uint32_t key);

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

// ─── KMS ──────────────────────────────────────────────────────────────────

// "HDMI-A-1", "DPI-1": the kernel's connector name, as /sys/class/drm
// and modetest print it
static void connector_name(const drmModeConnector *conn, char *out, size_t cap) {
    if (!conn) {
        snprintf(out, cap, "?");
        return;
    }
    const char *type = drmModeGetConnectorTypeName(conn->connector_type);
    snprintf(out, cap, "%s-%u", type ? type : "Unknown", conn->connector_type_id);
}

// The output asked for: VRX_OUTPUT, else `output=` in /etc/vrx/display.conf
// (the display settings file plan 0008 Part C shares), else none.
static void output_wanted(char *out, size_t cap) {
    out[0] = 0;
    const char *env = getenv("VRX_OUTPUT");
    if (env && env[0]) {
        snprintf(out, cap, "%s", env);
        return;
    }
    FILE *f = fopen("/etc/vrx/display.conf", "re");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *v = line;
        while (*v == ' ' || *v == '\t') v++;
        if (strncmp(v, "output", 6) != 0) continue;
        v += 6;
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '=') continue;
        v++;
        while (*v == ' ' || *v == '\t') v++;
        v[strcspn(v, " \t\r\n#")] = 0;
        snprintf(out, cap, "%s", v);
    }
    fclose(f);
}

// ─── overlay planes (plan 0008 Part C, step 1) ────────────────────────────
//
// The shell is meant to sit on a second KMS plane above the focused app,
// so no app can hide or fake the status strip. Before building on that,
// the bench proves the plane: every plane this CRTC can use is logged
// (type, formats, alpha and blend properties), and VRX_PROBE_OVERLAY=1
// puts a translucent bar on the first overlay plane.

static const char *plane_type_name(uint64_t t) {
    return t == DRM_PLANE_TYPE_PRIMARY ? "primary" : t == DRM_PLANE_TYPE_CURSOR ? "cursor" : "overlay";
}

static uint64_t plane_prop(int fd, uint32_t plane, const char *name, bool *found) {
    *found = false;
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
    if (!props) return 0;
    uint64_t value = 0;
    for (uint32_t i = 0; i < props->count_props && !*found; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p && strcmp(p->name, name) == 0) {
            value = props->prop_values[i];
            *found = true;
        }
        if (p) drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return value;
}

// The first overlay plane the CRTC can use that takes ARGB8888, 0 if none.
static uint32_t plane_probe(struct vrx_compositor *c, bool log) {
    drmSetClientCap(c->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModeRes *res = drmModeGetResources(c->drm_fd);
    drmModePlaneRes *planes = drmModeGetPlaneResources(c->drm_fd);
    uint32_t overlay = 0;
    if (!res || !planes) goto out;
    int crtc_index = -1;
    for (int k = 0; k < res->count_crtcs; k++) {
        if (res->crtcs[k] == c->crtc_id) crtc_index = k;
    }
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(c->drm_fd, planes->planes[i]);
        if (!p) continue;
        if (crtc_index >= 0 && (p->possible_crtcs & (1u << crtc_index))) {
            bool has_type, has_alpha, has_blend, has_zpos;
            uint64_t type = plane_prop(c->drm_fd, p->plane_id, "type", &has_type);
            uint64_t alpha = plane_prop(c->drm_fd, p->plane_id, "alpha", &has_alpha);
            plane_prop(c->drm_fd, p->plane_id, "pixel blend mode", &has_blend);
            uint64_t zpos = plane_prop(c->drm_fd, p->plane_id, "zpos", &has_zpos);
            bool argb = false;
            for (uint32_t f = 0; f < p->count_formats; f++) {
                if (p->formats[f] == DRM_FORMAT_ARGB8888) argb = true;
            }
            if (log) {
                fprintf(stderr, "[vrx-compositor] plane %u: %s, %u formats%s, alpha %s, blend %s, zpos %s%" PRIu64 "\n",
                        p->plane_id, plane_type_name(type), p->count_formats, argb ? " (ARGB8888)" : "",
                        has_alpha ? "yes" : "no", has_blend ? "yes" : "no", has_zpos ? "" : "none ", has_zpos ? zpos : 0);
                (void) alpha;
            }
            if (!overlay && has_type && type == DRM_PLANE_TYPE_OVERLAY && argb) overlay = p->plane_id;
        }
        drmModeFreePlane(p);
    }
out:
    if (planes) drmModeFreePlaneResources(planes);
    if (res) drmModeFreeResources(res);
    return overlay;
}

// VRX_PROBE_OVERLAY=1: a bar across the top, a sixth of the screen tall,
// translucent blue fading to clear, on the overlay plane. The bench looks
// at the panel: blended over the app, the shell can live on a plane.
static void plane_probe_bar(struct vrx_compositor *c) {
    uint32_t plane = plane_probe(c, false);
    if (!plane) {
        fprintf(stderr, "[vrx-compositor] probe: no ARGB8888 overlay plane on crtc %u\n", c->crtc_id);
        return;
    }
    uint32_t w = c->width, h = c->height / 6;
    struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) return;
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) != 0) return;
    uint32_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, c->drm_fd, md.offset);
    if (px == MAP_FAILED) return;
    for (uint32_t y = 0; y < h; y++) {
        // premultiplied: alpha falls from 0xc0 at the top to 0 at the foot
        uint32_t a = 0xc0 * (h - y) / h;
        uint32_t b = a;  // blue at full intensity, premultiplied
        for (uint32_t x = 0; x < w; x++) px[y * (cd.pitch / 4) + x] = (a << 24) | (b * 0x40 / 0xff << 16) | (b * 0x80 / 0xff << 8) | b;
    }
    munmap(px, cd.size);
    uint32_t handles[4] = { cd.handle }, pitches[4] = { cd.pitch }, offsets[4] = { 0 }, fb = 0;
    if (drmModeAddFB2(c->drm_fd, w, h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &fb, 0) != 0) {
        fprintf(stderr, "[vrx-compositor] probe: AddFB2 %s\n", strerror(errno));
        return;
    }
    if (drmModeSetPlane(c->drm_fd, plane, c->crtc_id, fb, 0, 0, 0, w, h, 0, 0, w << 16, h << 16) != 0) {
        fprintf(stderr, "[vrx-compositor] probe: SetPlane %u: %s\n", plane, strerror(errno));
        return;
    }
    fprintf(stderr, "[vrx-compositor] probe: bar on overlay plane %u (%ux%u ARGB8888)\n", plane, w, h);
}

static int kms_init(struct vrx_compositor *c, const char *device) {
    char found[64];
    const char *path = device;
    if (path) {
        c->drm_fd = open(path, O_RDWR | O_CLOEXEC);
    } else {
        // the node with connectors and CRTCs, whatever its number this boot
        c->drm_fd = vrx_open_kms_node(found, sizeof found);
        path = found;
    }
    if (c->drm_fd < 0) {
        fprintf(stderr, "[vrx-compositor] cannot open %s: %s\n",
                device ? device : "a display (no DRM node has a connector)", strerror(errno));
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
    // The output: the one named (VRX_OUTPUT, else display.conf's
    // output=, e.g. HDMI-A-1) when it is connected, else the first
    // connected. A board with only an HDMI monitor needs no naming; one
    // with a panel and a monitor names the monitor to use it.
    char want[32] = "";
    output_wanted(want, sizeof want);
    drmModeConnector *conn = NULL;
    for (int pass = want[0] ? 0 : 1; pass < 2 && !conn; pass++) {
        for (int i = 0; i < res->count_connectors && !conn; i++) {
            conn = drmModeGetConnector(c->drm_fd, res->connectors[i]);
            char name[32];
            connector_name(conn, name, sizeof name);
            bool ok = conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 &&
                      (pass == 1 || strcmp(name, want) == 0);
            if (!ok && conn) {
                drmModeFreeConnector(conn);
                conn = NULL;
            }
        }
        if (pass == 0 && !conn) {
            fprintf(stderr, "[vrx-compositor] output %s is not connected; using the first connected\n", want);
        }
    }
    if (!conn) {
        fprintf(stderr, "[vrx-compositor] no connected display\n");
        return -1;
    }
    connector_name(conn, c->output, sizeof c->output);
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

    // The CRTC: the one the firmware already drives this output with,
    // else the first the output's encoders can reach (an HDMI port and
    // the DPI panel hang off different CRTCs on vc4)
    c->crtc_id = 0;
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(c->drm_fd, conn->encoder_id);
        if (enc) {
            c->crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    }
    for (int e = 0; e < conn->count_encoders && !c->crtc_id; e++) {
        drmModeEncoder *enc = drmModeGetEncoder(c->drm_fd, conn->encoders[e]);
        if (!enc) continue;
        for (int k = 0; k < res->count_crtcs && !c->crtc_id; k++) {
            if (enc->possible_crtcs & (1u << k)) c->crtc_id = res->crtcs[k];
        }
        drmModeFreeEncoder(enc);
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!c->crtc_id) {
        fprintf(stderr, "[vrx-compositor] no crtc\n");
        return -1;
    }
    fprintf(stderr, "[vrx-compositor] %s %s: %ux%u@%u on crtc %u, %ux%umm\n",
            path, c->output, c->width, c->height, c->mode.vrefresh, c->crtc_id, c->width_mm, c->height_mm);
    plane_probe(c, true);
    return 0;
}

static void client_send(struct vrx_compositor *c, int fd, uint32_t type,
                        const void *payload, size_t len, int pass_fd);
struct app_client;
static void report_app(struct vrx_compositor *c, const struct app_client *a, uint32_t state);

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
        cursor_update(c);
        if (getenv("VRX_PROBE_OVERLAY")) plane_probe_bar(c);
        c->on_screen = c->queued;
        memset(&c->queued, 0, sizeof c->queued);
        c->queued.dmabuf_fd = -1;
        c->queued.app = -1;
        fprintf(stderr, "[vrx-compositor] modeset: first frame from app %d on screen\n",
                c->on_screen.app);
        if (c->on_screen.app >= 0) report_app(c, &c->apps[c->on_screen.app], VRX_APP_SHOWN);
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
    if (c->trace && c->trace_input_ns && c->trace_buffer_ns) {
        uint64_t now = now_ns();
        fprintf(stderr, "[vrx-compositor] latency: input→buffer %.1f ms, input→screen %.1f ms\n",
                (double) (c->trace_buffer_ns - c->trace_input_ns) / 1e6,
                (double) (now - c->trace_input_ns) / 1e6);
        c->trace_input_ns = 0;
    }
    if (c->on_screen.app_fd != c->pending.app_fd) {
        fprintf(stderr, "[vrx-compositor] screen switched to slot %d ('%s')\n", c->pending.app,
                c->pending.app >= 0 ? c->apps[c->pending.app].id : "?");
        if (c->pending.app >= 0) report_app(c, &c->apps[c->pending.app], VRX_APP_SHOWN);
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
static void report_app(struct vrx_compositor *c, const struct app_client *a, uint32_t state) {
    if (c->controller < 0) return;
    struct vrx_msg_app_state m = { .pid = a->pid, .state = state };
    snprintf(m.id, sizeof m.id, "%s", a->id);
    client_send(c, c->apps[c->controller].fd, VRX_MSG_APP_STATE, &m, sizeof m, -1);
}

static void focus_slot(struct vrx_compositor *c, int slot) {
    if (slot == c->focused) return;
    int old = c->focused;
    c->focused = slot;
    // a key held into a focus change stops repeating into the new app
    repeat_arm(c, 0);
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
    if (a->registered) report_app(c, a, VRX_APP_GONE);
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
            report_app(c, a, VRX_APP_REGISTERED);
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
                if (c->apps[i].in_use && c->apps[i].registered) report_app(c, &c->apps[i], VRX_APP_REGISTERED);
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
            if (c->trace && c->trace_input_ns && !c->trace_buffer_ns) c->trace_buffer_ns = now_ns();
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

// ─── the mouse cursor ─────────────────────────────────────────────────────

#define CURSOR_SIZE 64

// The classic arrow, on a 12x21 grid, tip at the origin (the hotspot).
static bool in_arrow(double x, double y) {
    static const double P[][2] = { {0, 0}, {0, 17}, {4, 13}, {7, 20}, {10, 19}, {7, 12}, {12, 12} };
    const int n = sizeof P / sizeof P[0];
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((P[i][1] > y) != (P[j][1] > y) &&
            x < (P[j][0] - P[i][0]) * (y - P[i][1]) / (P[j][1] - P[i][1]) + P[i][0]) {
            in = !in;
        }
    }
    return in;
}

// A white arrow with a black edge, sized to the output: about 1/40 of
// its height, so it reads on the 640-line panel and on a 4K monitor.
static void cursor_init(struct vrx_compositor *c) {
    struct drm_mode_create_dumb cd = { .width = CURSOR_SIZE, .height = CURSOR_SIZE, .bpp = 32 };
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) {
        fprintf(stderr, "[vrx-compositor] no cursor buffer: %s\n", strerror(errno));
        return;
    }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    uint32_t *px = MAP_FAILED;
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0) {
        px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, c->drm_fd, md.offset);
    }
    if (px == MAP_FAILED) {
        fprintf(stderr, "[vrx-compositor] cannot map the cursor: %s\n", strerror(errno));
        struct drm_mode_destroy_dumb dd = { .handle = cd.handle };
        drmIoctl(c->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        return;
    }
    double size = c->height / 40.0;
    if (size < 16) size = 16;
    if (size > CURSOR_SIZE - 2) size = CURSOR_SIZE - 2;
    const double k = 21.0 / size;  // grid units per pixel
    const uint32_t stride = cd.pitch / 4;
    for (int y = 0; y < CURSOR_SIZE; y++) {
        for (int x = 0; x < CURSOR_SIZE; x++) {
            double gx = (x + 0.5) * k, gy = (y + 0.5) * k;
            uint32_t v = 0;
            if (in_arrow(gx, gy)) {
                // the edge: a pixel inside whose neighbour is outside
                bool edge = !in_arrow(gx - k, gy) || !in_arrow(gx + k, gy) ||
                            !in_arrow(gx, gy - k) || !in_arrow(gx, gy + k);
                v = edge ? 0xFF000000u : 0xFFFFFFFFu;
            }
            px[y * stride + x] = v;
        }
    }
    munmap(px, cd.size);
    c->cursor_handle = cd.handle;
}

// Put the cursor where the pointer is, once the CRTC is lit; hide it.
static void cursor_update(struct vrx_compositor *c) {
    if (!c->cursor_handle || !c->mode_set) return;
    if (c->cursor_wanted && !c->cursor_shown) {
        if (drmModeSetCursor2(c->drm_fd, c->crtc_id, c->cursor_handle, CURSOR_SIZE, CURSOR_SIZE, 0, 0) != 0) {
            fprintf(stderr, "[vrx-compositor] cursor plane refused: %s\n", strerror(errno));
            c->cursor_handle = 0;
            return;
        }
        c->cursor_shown = true;
    } else if (!c->cursor_wanted && c->cursor_shown) {
        drmModeSetCursor(c->drm_fd, c->crtc_id, 0, 0, 0);
        c->cursor_shown = false;
    }
    if (c->cursor_shown) drmModeMoveCursor(c->drm_fd, c->crtc_id, (int) c->px, (int) c->py);
}

// ─── input ────────────────────────────────────────────────────────────────

// A held key repeats after 400 ms, 30 times a second: the rates a desktop
// ships with.
static void repeat_arm(struct vrx_compositor *c, uint32_t key) {
    if (c->repeat_fd < 0) return;
    c->repeat_key = key;
    struct itimerspec t = {0};
    if (key) {
        t.it_value.tv_nsec = 400 * 1000000L;
        t.it_interval.tv_nsec = 33 * 1000000L;
    }
    timerfd_settime(c->repeat_fd, 0, &t, NULL);
}

static bool is_modifier(uint32_t key) {
    switch (key) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
    case KEY_LEFTALT: case KEY_RIGHTALT: case KEY_LEFTMETA: case KEY_RIGHTMETA:
    case KEY_CAPSLOCK: case KEY_NUMLOCK: case KEY_SCROLLLOCK:
        return true;
    default:
        return false;
    }
}

static void send_input(struct vrx_compositor *c, const struct vrx_msg_input *m) {
    if (c->focused < 0) return;
    client_send(c, c->apps[c->focused].fd, VRX_MSG_INPUT, m, sizeof *m, -1);
    bool press = (m->kind == VRX_INPUT_TOUCH && m->u.touch.kind == 0) ||
                 (m->kind == VRX_INPUT_KEY && m->u.key.state == 1);
    if (c->trace && press) {
        c->trace_input_ns = now_ns();
        c->trace_buffer_ns = 0;
    }
}

static void send_pointer(struct vrx_compositor *c, uint64_t time_ns, int32_t sx, int32_t sy) {
    struct vrx_msg_input m = { .kind = VRX_INPUT_POINTER };
    m.u.pointer = (struct vrx_input_pointer){
        .time_ns = time_ns,
        .x = (int32_t) c->px, .y = (int32_t) c->py,
        .buttons = c->buttons,
        .scroll_x = sx, .scroll_y = sy,
    };
    c->cursor_wanted = true;
    cursor_update(c);
    send_input(c, &m);
}

static void repeat_fire(struct vrx_compositor *c) {
    uint64_t expirations;
    if (read(c->repeat_fd, &expirations, sizeof expirations) != sizeof expirations) return;
    if (!c->repeat_key) return;
    struct vrx_msg_input m = { .kind = VRX_INPUT_KEY };
    m.u.key = (struct vrx_input_key){ .time_ns = now_ns(), .keycode = c->repeat_key, .state = 2 };
    send_input(c, &m);
}

// Flutter's button bits for a Linux button code
static uint32_t button_bit(uint32_t code) {
    switch (code) {
    case BTN_LEFT: return 1;
    case BTN_RIGHT: return 2;
    case BTN_MIDDLE: return 4;
    case BTN_SIDE: case BTN_BACK: return 8;
    case BTN_EXTRA: case BTN_FORWARD: return 16;
    default: return 0;
    }
}

// a wheel click scrolls about three lines of body text
#define WHEEL_PX_PER_CLICK 50

static void li_dispatch(struct vrx_compositor *c) {
    libinput_dispatch(c->li);
    while (libinput_next_event_type(c->li) != LIBINPUT_EVENT_NONE) {
        struct libinput_event *ev = libinput_get_event(c->li);
        enum libinput_event_type t = libinput_event_get_type(ev);
        struct libinput_event_pointer *p = NULL;
        switch (t) {
        case LIBINPUT_EVENT_DEVICE_ADDED: {
            // a touchpad clicks on a tap, as people expect
            struct libinput_device *d = libinput_event_get_device(ev);
            if (libinput_device_config_tap_get_finger_count(d) > 0) {
                libinput_device_config_tap_set_enabled(d, LIBINPUT_CONFIG_TAP_ENABLED);
            }
            break;
        }
        case LIBINPUT_EVENT_POINTER_MOTION:
            p = libinput_event_get_pointer_event(ev);
            c->px += libinput_event_pointer_get_dx(p);
            c->py += libinput_event_pointer_get_dy(p);
            if (c->px < 0) c->px = 0;
            if (c->py < 0) c->py = 0;
            if (c->px > c->width - 1) c->px = c->width - 1;
            if (c->py > c->height - 1) c->py = c->height - 1;
            send_pointer(c, libinput_event_pointer_get_time_usec(p) * 1000, 0, 0);
            break;
        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
            p = libinput_event_get_pointer_event(ev);
            c->px = libinput_event_pointer_get_absolute_x_transformed(p, c->width);
            c->py = libinput_event_pointer_get_absolute_y_transformed(p, c->height);
            send_pointer(c, libinput_event_pointer_get_time_usec(p) * 1000, 0, 0);
            break;
        case LIBINPUT_EVENT_POINTER_BUTTON: {
            p = libinput_event_get_pointer_event(ev);
            uint32_t bit = button_bit(libinput_event_pointer_get_button(p));
            if (!bit) break;
            if (libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED) {
                c->buttons |= bit;
            } else {
                c->buttons &= ~bit;
            }
            send_pointer(c, libinput_event_pointer_get_time_usec(p) * 1000, 0, 0);
            break;
        }
        case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
        case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
        case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS: {
            // (LIBINPUT_EVENT_POINTER_AXIS repeats these for old clients)
            p = libinput_event_get_pointer_event(ev);
            double v[2] = {0, 0};
            const enum libinput_pointer_axis axes[2] = {
                LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
            };
            for (int a = 0; a < 2; a++) {
                if (!libinput_event_pointer_has_axis(p, axes[a])) continue;
                v[a] = t == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL
                           ? libinput_event_pointer_get_scroll_value_v120(p, axes[a]) / 120.0 * WHEEL_PX_PER_CLICK
                           : libinput_event_pointer_get_scroll_value(p, axes[a]);
            }
            if (v[0] != 0 || v[1] != 0) {
                send_pointer(c, libinput_event_pointer_get_time_usec(p) * 1000, (int32_t) v[0], (int32_t) v[1]);
            }
            break;
        }
        case LIBINPUT_EVENT_KEYBOARD_KEY: {
            struct libinput_event_keyboard *k = libinput_event_get_keyboard_event(ev);
            uint32_t key = libinput_event_keyboard_get_key(k);
            bool down = libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED;
            struct vrx_msg_input m = { .kind = VRX_INPUT_KEY };
            m.u.key = (struct vrx_input_key){
                .time_ns = libinput_event_keyboard_get_time_usec(k) * 1000,
                .keycode = key,
                .state = down ? 1 : 0,
            };
            if (down && !is_modifier(key)) {
                repeat_arm(c, key);
            } else if (!down && key == c->repeat_key) {
                repeat_arm(c, 0);
            }
            send_input(c, &m);
            break;
        }
        case LIBINPUT_EVENT_TOUCH_DOWN:
        case LIBINPUT_EVENT_TOUCH_MOTION:
        case LIBINPUT_EVENT_TOUCH_UP: {
            struct libinput_event_touch *th = libinput_event_get_touch_event(ev);
            uint32_t kind = t == LIBINPUT_EVENT_TOUCH_DOWN ? 0 :
                            t == LIBINPUT_EVENT_TOUCH_MOTION ? 1 : 2;
            struct vrx_msg_input m = { .kind = VRX_INPUT_TOUCH };
            m.u.touch = (struct vrx_input_touch){
                .time_ns = (uint64_t) libinput_event_touch_get_time_usec(th) * 1000,
                .slot = (uint32_t) libinput_event_touch_get_slot(th),
                .kind = kind,
                .x = kind == 2 ? 0 : (int32_t) libinput_event_touch_get_x_transformed(th, c->width),
                .y = kind == 2 ? 0 : (int32_t) libinput_event_touch_get_y_transformed(th, c->height),
            };
            // a finger on the glass: the arrow would only be in the way
            if (c->cursor_wanted) {
                c->cursor_wanted = false;
                cursor_update(c);
            }
            send_input(c, &m);
            break;
        }
        default:
            break;
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
#define TAG_REPEAT    0xFFFFFFFFFFFFFFFBull
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
    c.trace = getenv("VRX_TRACE_LATENCY") != NULL;
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
    c.repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (c.repeat_fd >= 0) {
        ev.data.u64 = TAG_REPEAT;
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.repeat_fd, &ev);
    }
    c.px = c.width / 2.0;
    c.py = c.height / 2.0;
    cursor_init(&c);

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
            } else if (tag == TAG_REPEAT) {
                repeat_fire(&c);
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
