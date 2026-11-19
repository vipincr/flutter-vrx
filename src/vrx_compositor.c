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
#include <dirent.h>
#include <grp.h>
#include <inttypes.h>
#include <libinput.h>
#include <libudev.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <systemd/sd-bus.h>

#include <vrx_light/vrx_light_sysfs.h>

#include "vrx_ipc.h"
#include "vrx_kms.h"
#include "vrx_light_standard.h"
#include "vrx_resume.h"
#include "vrx_screen_light.h"
#include "vrx_vt.h"

// ─── app client ───────────────────────────────────────────────────────────

// connections: the apps plus the controller. vrx runs every built-in
// applet at once (the shell, residents, Home's tiles warmed up: eleven on
// 0.1.0077), and 8 refused the shell's restart on the bench
#define MAX_APPS 32

// the dim plane's black buffer, stretched over the whole CRTC
#define DIM_FB_SIZE 64

struct app_client {
    bool in_use;        // slot occupied; slots never move (epoll holds them)
    int fd;
    pid_t pid;          // SO_PEERCRED at accept: who vrxhost spawned
    uid_t uid;
    bool registered;
    uint64_t last_recv_ns;    // its previous frame's arrival (plan 0014 D5)
    uint64_t vblank_sent_ns;  // when it was last told the vblank phase (D6)
    bool controller;    // this connection is vrxhost, not an app
    bool no_chrome;     // asked for the whole screen: the shell steps aside
                        // while it is focused
    uint32_t region_top;  // its last SHELL_REGION: kept, since the shell
    bool region_full;     // reports on its first frame and vrxhost may
                          // name it after that
    bool shown;         // as the shell, its frame has been on the panel
    char id[sizeof(struct vrx_msg_register)];
};

// One KMS plane and the ids of the properties an atomic request sets on it
// (plan 0017 D9). 0 = the plane does not have that property.
struct kplane {
    uint32_t id;
    uint32_t fb, crtc, src_x, src_y, src_w, src_h, crtc_x, crtc_y, crtc_w, crtc_h;
    uint32_t alpha;     // opaque (0xffff) every commit where the plane has it
    uint32_t rotation;  // set to rotate_180 where the panel is upside down
    uint64_t rotate_180;
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
    // the flip record (plan 0014 D5): when the buffer arrived, when the
    // app's frame before it arrived (0 = none: a still screen), and when
    // its flip was committed
    uint64_t recv_ns, prev_recv_ns, commit_ns;
};

struct vrx_compositor {
    int drm_fd;
    // the profiler's switch (plan 0014 D1, D5; vrxhost's VRX_MSG_PROFILING).
    // While on, every flip is recorded on ftrace's trace_marker: each
    // applet's arrival-to-screen time, the refreshes it skipped, drops by
    // cause, and input-to-screen latency (input sent -> the app's next
    // buffer -> its flip), the number a person feels as responsiveness
    bool profiling;
    int marker_fd;             // trace_marker, -1 = closed
    uint64_t trace_input_ns;   // the input being answered, 0 = none
    uint64_t trace_buffer_ns;  // when the answering buffer arrived
    // the panel's vblank phase (D5, D6), from flip events
    bool flip_ts_monotonic;    // DRM_CAP_TIMESTAMP_MONOTONIC: events carry it
    uint64_t period_ns;        // one refresh, from the mode's timings
    uint32_t last_flip_seq;
    uint64_t last_flip_ns;     // 0 = no flip yet (or reset by DPMS)
    uint64_t drops_app, drops_compositor, drops_underrun, drops_unattributed;
    uint64_t underruns_seen;
    // walks find controls by label (plan 0014 D8): the apps build their
    // semantics trees while this is on, and each tree lands in
    // /run/vrx-ui/semantics/<id>.json
    bool semantics;
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

    // screen power (plan 0010, plan 0023 D1): vrxhost decides when the
    // screen goes dark or lights (MSG_SCREEN) and how far it dims
    // (MSG_LIGHT_CAP); this executes it. The light itself (plan 0033) is
    // `light`: the person's brightness, per mille and perceived, the fade,
    // and the migration; this file only shows what it says.
    char backlight[96];        // /sys/class/backlight/<name>, "" = none
    int bl_max;                // its max_brightness, 0 = no backlight
    struct vrx_light bl_map;   // the backlight's levels against perceived brightness
    struct vrx_screen_light light;
    int light_fd;              // timerfd: a fade's steps, every LIGHT_STEP_MS while one runs
    bool fade_ok;              // this screen can fade: a dimmable light, or a dim plane on the full tier
    bool handoff_wait;         // the first frame waits for the light to be down (plan 0033 D5)
    bool off_pending;          // the screen goes off when the light is down (screen_off_why)
    char off_why[24];
    bool shutdown_dip;         // the system is going down: release our logind delay when the light is down
    uint32_t resume_deadline_ms;  // after a resume, light at the latest then; 0 = not waiting
    // the resume's presentation (vrx plan 0034): the splash drawn in the dark before the system sleeps, lit on the wake
    struct vrx_resume resume;
    uint32_t splash_fb;        // the splash's framebuffer (XRGB8888, the CRTC's size), 0 = none made
    bool splash_on;            // the primary plane shows the splash, and app frames wait
    bool splash_sent;          // a commit carrying the splash is in flight
    bool splash_release;       // the sleep delay is released when that commit's flip lands
    // how brightness is shown: the backlight's levels, or a black plane
    // over everything at partial alpha (DIM_ALPHA), or not at all
    enum { DIM_NONE, DIM_BACKLIGHT, DIM_ALPHA } dim;
    uint32_t active_prop;      // the CRTC's ACTIVE property: the screen's power
    uint32_t primary_plane;    // the CRTC's primary plane
    // the dim plane (DIM_ALPHA): an overlay above the shell's, scanning
    // out a small black buffer stretched over the CRTC. The app's and the
    // shell's planes stay opaque; dimming them by their own alpha made the
    // shell's drawer see-through over the app (bench, CM4, 2026-10-14).
    uint32_t dim_plane, dim_alpha;  // the plane and its alpha property, 0 = none
    uint32_t dim_fb;           // the black buffer's framebuffer, 0 = none
    uint64_t dim_want;         // the alpha the scene shows the dim plane at, 0 = off
    uint64_t dim_sent;         // what the last commit showed
    // the scene (plan 0017 D9): every plane goes out in one non-blocking
    // atomic commit per vblank. Frames from the app and from the shell
    // wait in `queued` and `shell_queued`, ride a commit as `pending` and
    // `shell_pending`, and become `on_screen` and `shell_frame` when the
    // flip event says that commit was scanned out.
    struct kplane kp_primary, kp_shell, kp_dim;
    bool commit_pending;       // a commit is in flight: its event is due
    bool scene_dirty;          // something changed since the last commit
    bool scene_retry;          // the last commit met EBUSY with nothing in flight
    bool wake_pending;         // the next commit turns the CRTC back on
    uint32_t commit_planes;    // SCENE_* planes the pending commit changed
    uint32_t probe_fb;         // VRX_PROBE_OVERLAY's bar, shown while the shell has no frame
    uint32_t probe_w, probe_h;
    // the panel is mounted upside down (vrx plan 0013 D10): the
    // connector's standard "panel orientation" says so (a carrier sets it
    // on the kernel command line, video=<conn>:panel_orientation=...); the
    // planes scan out turned, touch is turned back, the cursor drawn turned
    bool upside_down;
    bool dark;                 // the screen is off
    struct vrx_msg_screen reported_screen;  // the last VRX_MSG_SCREEN sent
    bool reported_screen_valid;
    int32_t power_off_left;    // vrxhost's last power-off countdown word, -1 = none
    int power_off_dark;        // 0, or the countdown ran out: 1 = answer vrxhost when the light is down, 2 = answered (the light returns if vrxhost says the count is over)
    bool light_after_flip;     // waking: light once the newest frame is up
    // the system bus: PrepareForSleep, so apps hear the system sleep
    sd_bus *bus;
    int bl_level;              // the backlight level last written, -1 = none yet
    int sig_fd;                // SIGTERM / SIGINT stop; SIGUSR1 writes the flip ring
    // input facts for the kit (plan 0013 D6, D7): touchscreens and
    // keyboards present, by udev's classification; a device's kinds are
    // kept so its removal undoes exactly what its arrival did
    int touchscreens, keyboards;
    struct { struct libinput_device *dev; unsigned kinds; } inputs[32];
    // one finger drives the UI (plan 0013 D8): the touch slot whose
    // contact is delivered, -1 when no contact is down
    int primary_touch;
    int sleep_inhibit_fd;      // our logind `sleep` delay inhibitor, -1
    int sleep_ack_fd;          // timerfd: release the delay after 1 s
    bool sleeping;             // between PrepareForSleep(true) and (false)
    int touches;               // contacts down now

    struct app_client apps[MAX_APPS];
    int focused;               // slot, -1 = none

    // the shell (plan 0008 D10): vrxhost names it by pid; its frames go to
    // an overlay plane above the focused app; input inside its region goes
    // to it. A touch belongs to whoever got its first contact.
    int shell;                 // slot, -1 = none
    pid_t want_shell;
    uint32_t shell_top;        // the strip's height in output pixels
    bool shell_full;           // its drawer has the whole screen
    uint32_t shell_plane;      // the overlay plane, 0 = none found
    struct frame shell_frame;  // on the plane now
    struct frame shell_queued, shell_pending;
    int touch_owner[16];       // slot a touch slot's contact went to, -1
    int pointer_owner;         // slot a held mouse button went to, -1
    int controller;            // slot of vrxhost's connection, -1 = none
    pid_t want_focus;          // the controller's choice, 0 = none; may name
                               // an app that has not registered yet

    // scanout pipeline: what is on screen, what a requested flip will show,
    // and the newest frame waiting for the flip slot
    struct frame on_screen, pending, queued;

    bool running;
};

static void cursor_update(struct vrx_compositor *c);
static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name);
static uint64_t mode_period_ns(const drmModeModeInfo *m);
static void repeat_arm(struct vrx_compositor *c, uint32_t key);

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

// the light's clock (a fade is measured in whole milliseconds; the wrap is the stepper's to handle)
static uint32_t now_ms(void) { return (uint32_t) (now_ns() / 1000000ull); }

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

static bool conf_value(const char *path, const char *key, char *out, size_t cap);

// The connector's "panel orientation" (Normal, Upside Down, Left Side
// Up, Right Side Up), or "" when the kernel knows none.
static void panel_orientation(int fd, uint32_t connector, char *out, size_t cap) {
    out[0] = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, connector, DRM_MODE_OBJECT_CONNECTOR);
    for (uint32_t i = 0; props && i < props->count_props && !out[0]; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p && strcmp(p->name, "panel orientation") == 0) {
            for (int e = 0; e < p->count_enums; e++) {
                if (p->enums[e].value == props->prop_values[i]) snprintf(out, cap, "%s", p->enums[e].name);
            }
        }
        if (p) drmModeFreeProperty(p);
    }
    if (props) drmModeFreeObjectProperties(props);
}

// A plane's rotation property and whether it offers `want` (a
// DRM_MODE_ROTATE_* bit); 0 when the plane has no rotation at all.
static uint32_t plane_rotation(int fd, uint32_t plane, uint64_t want, bool *offers) {
    *offers = false;
    uint32_t id = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
    for (uint32_t i = 0; props && i < props->count_props && !id; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p && strcmp(p->name, "rotation") == 0) {
            id = p->prop_id;
            for (int e = 0; e < p->count_enums; e++) {
                if ((1ull << p->enums[e].value) == want) *offers = true;
            }
        }
        if (p) drmModeFreeProperty(p);
    }
    if (props) drmModeFreeObjectProperties(props);
    return id;
}

// The output asked for: VRX_OUTPUT, else `output=` in the screen settings
// file (/etc/vrx/display/screen.conf, the one display settings file), else
// none.
static void output_wanted(char *out, size_t cap) {
    out[0] = 0;
    const char *env = getenv("VRX_OUTPUT");
    if (env && env[0]) {
        snprintf(out, cap, "%s", env);
        return;
    }
    conf_value("/etc/vrx/display/screen.conf", "output", out, cap);
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
            if (!c->primary_plane && has_type && type == DRM_PLANE_TYPE_PRIMARY) c->primary_plane = p->plane_id;
        }
        drmModeFreePlane(p);
    }
out:
    if (planes) drmModeFreePlaneResources(planes);
    if (res) drmModeFreeResources(res);
    return overlay;
}

// VRX_PROBE_OVERLAY=1: a bar across the top, a sixth of the screen tall,
// translucent blue fading to clear, on the shell's overlay plane. The
// bench looks at the panel: blended over the app, the shell can live on a
// plane. The bar is a framebuffer the scene commit shows on that plane
// while the shell has no frame of its own (plan 0017 D9).
static void plane_probe_bar(struct vrx_compositor *c) {
    uint32_t plane = c->shell_plane;
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
    c->probe_fb = fb;
    c->probe_w = w;
    c->probe_h = h;
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
    // every plane is committed atomically (plan 0017 D9): there is no
    // legacy path to fall back to, so a driver without atomic is refused
    // here, loudly, rather than on the first frame
    if (drmSetClientCap(c->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
        drmSetClientCap(c->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        fprintf(stderr, "[vrx-compositor] %s has no atomic modesetting: %s\n", path, strerror(errno));
        return -1;
    }
    // flip events carry the vblank's CLOCK_MONOTONIC time where the driver
    // says so (plan 0014 D5, D6); otherwise the record uses arrival time
    uint64_t mono = 0;
    c->flip_ts_monotonic = drmGetCap(c->drm_fd, DRM_CAP_TIMESTAMP_MONOTONIC, &mono) == 0 && mono;
    fprintf(stderr, "[vrx-compositor] flip timestamps: %s\n",
            c->flip_ts_monotonic ? "CLOCK_MONOTONIC (the vblank's own)" : "not monotonic: the record uses arrival time");

    drmModeRes *res = drmModeGetResources(c->drm_fd);
    if (!res) {
        fprintf(stderr, "[vrx-compositor] drmModeGetResources: %s\n", strerror(errno));
        return -1;
    }
    // The output: the one named (VRX_OUTPUT, else screen.conf's
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
    char orientation[32];
    panel_orientation(c->drm_fd, c->connector_id, orientation, sizeof orientation);
    c->upside_down = strcmp(orientation, "Upside Down") == 0;
    if (strcmp(orientation, "Left Side Up") == 0 || strcmp(orientation, "Right Side Up") == 0) {
        fprintf(stderr, "[vrx-compositor] %s is mounted %s: a quarter turn is not supported here; drawing unrotated\n",
                c->output[0] ? c->output : "the panel", orientation);
    } else if (c->upside_down) {
        fprintf(stderr, "[vrx-compositor] the panel is mounted upside down: scanning out turned 180 degrees\n");
    }
    c->mode = conn->modes[mi];
    c->period_ns = mode_period_ns(&c->mode);
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

static void screen_light(struct vrx_compositor *c, uint32_t ms);
static void light_go(struct vrx_compositor *c, bool lit, uint32_t ms);
static void light_timer(struct vrx_compositor *c);
static uint64_t dim_alpha(uint16_t shown);
static void screen_report(struct vrx_compositor *c);
static void screen_facts(const struct vrx_compositor *c);
static void display_on(struct vrx_compositor *c, const char *why);
static void display_off(struct vrx_compositor *c, const char *why);
static void display_tell(struct vrx_compositor *c, int only);
static bool chrome_shown(const struct vrx_compositor *c);

// ─── the scene (plan 0017 D9) ─────────────────────────────────────────────
//
// What is on the panel is one scene: the focused app's frame on the primary
// plane, the shell's frame on an overlay above it, and the dim plane above
// both. Every change to any of them goes out as ONE atomic commit, flagged
// non-blocking with a flip event, so this thread never waits for a vblank
// (the legacy per-plane and page-flip calls each did, and a shell frame
// per vblank stalled the app plane, input and the socket behind it).
//
// A frame from either app lands in `queued` / `shell_queued` (a newer one
// supersedes it) and marks the scene dirty. scene_commit() sends the scene
// when no commit is in flight; the frames it carried become `pending` /
// `shell_pending`. The flip event (on_page_flip) retires the frames the
// commit replaced and sends again if the scene went dirty meanwhile: at
// most one commit is ever in flight per CRTC. The CRTC's first modeset
// stays drmModeSetCrtc (it needs a framebuffer and a mode, once).
static uint64_t commit_errors;  // scene commits the driver refused outright (the flip ring's header)
enum { SCENE_PRIMARY = 1, SCENE_SHELL = 2, SCENE_DIM = 4, SCENE_WAKE = 8 };

static void kplane_init(struct vrx_compositor *c, struct kplane *k, uint32_t plane, bool rotate) {
    memset(k, 0, sizeof *k);
    if (!plane) return;
    const int fd = c->drm_fd;
    k->id = plane;
    k->fb = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    k->crtc = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    k->src_x = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    k->src_y = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    k->src_w = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    k->src_h = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    k->crtc_x = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    k->crtc_y = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    k->crtc_w = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    k->crtc_h = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    k->alpha = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "alpha");
    if (!k->fb || !k->crtc || !k->src_w || !k->crtc_w) {
        fprintf(stderr, "[vrx-compositor] plane %u is missing atomic properties: not used\n", plane);
        memset(k, 0, sizeof *k);
        return;
    }
    if (rotate) {
        // the panel is mounted upside down: the planes scan out turned. A
        // quarter turn is refused loudly: vc4's planes offer only 0 and 180
        // (and the reflections), and drawing sideways content unrotated
        // beats a wrong guess at the geometry.
        bool offers;
        k->rotation = plane_rotation(fd, plane, DRM_MODE_ROTATE_180, &offers);
        if (!k->rotation || !offers) {
            fprintf(stderr, "[vrx-compositor] plane %u cannot turn 180 degrees: the panel stays upside down\n", plane);
            k->rotation = 0;
        } else {
            k->rotate_180 = DRM_MODE_ROTATE_180;
        }
    }
}

static void kadd(drmModeAtomicReq *req, uint32_t obj, uint32_t prop, uint64_t value) {
    if (obj && prop) drmModeAtomicAddProperty(req, obj, prop, value);
}

// Put `fb` (sw x sh pixels) on the plane, stretched to dw x dh at the CRTC's origin.
static void kplane_place(drmModeAtomicReq *req, const struct kplane *k, uint32_t crtc, uint32_t fb,
                         uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
    kadd(req, k->id, k->fb, fb);
    kadd(req, k->id, k->crtc, crtc);
    kadd(req, k->id, k->src_x, 0);
    kadd(req, k->id, k->src_y, 0);
    kadd(req, k->id, k->src_w, (uint64_t) sw << 16);
    kadd(req, k->id, k->src_h, (uint64_t) sh << 16);
    kadd(req, k->id, k->crtc_x, 0);
    kadd(req, k->id, k->crtc_y, 0);
    kadd(req, k->id, k->crtc_w, dw);
    kadd(req, k->id, k->crtc_h, dh);
}

static void kplane_off(drmModeAtomicReq *req, const struct kplane *k) {
    kadd(req, k->id, k->fb, 0);
    kadd(req, k->id, k->crtc, 0);
}

// Build the scene and send it with `flags`. `planes` reports which planes
// the commit changes (what the frames it carries will have replaced).
// Returns drmModeAtomicCommit's result with errno kept.
static int scene_send(struct vrx_compositor *c, bool with_dim, bool with_shell, uint32_t flags, uint32_t *planes) {
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        errno = ENOMEM;
        return -1;
    }
    *planes = 0;
    if (c->wake_pending) {
        kadd(req, c->crtc_id, c->active_prop, 1);
        *planes |= SCENE_WAKE;
    }
    // the resume's splash (plan 0034): it takes the primary plane, turned as the app's frames are, the shell's plane is off, and the
    // app's newest frame waits in `queued` for the splash to go
    const bool splash = c->splash_on && c->splash_fb;
    if (splash) with_shell = false;
    // primary: the app's newest frame, else what is on it now
    const struct frame *p = c->queued.valid ? &c->queued : &c->on_screen;
    if (splash) {
        kplane_place(req, &c->kp_primary, c->crtc_id, c->splash_fb, c->width, c->height, c->width, c->height);
        kadd(req, c->kp_primary.id, c->kp_primary.alpha, 0xffff);
        kadd(req, c->kp_primary.id, c->kp_primary.rotation, c->kp_primary.rotate_180);   // upright, as the app's frames are
    } else if (p->valid) {
        kplane_place(req, &c->kp_primary, c->crtc_id, p->fb_id, c->width, c->height, c->width, c->height);
        kadd(req, c->kp_primary.id, c->kp_primary.alpha, 0xffff);
        kadd(req, c->kp_primary.id, c->kp_primary.rotation, c->kp_primary.rotate_180);
        if (c->queued.valid) *planes |= SCENE_PRIMARY;
    }
    // shell: its newest frame while the chrome is shown (the bench probe's
    // bar while it has none), else the plane is off
    if (c->kp_shell.id) {
        const struct frame *sf = NULL;
        if (with_shell && chrome_shown(c)) {
            sf = c->shell_queued.valid ? &c->shell_queued : c->shell_frame.valid ? &c->shell_frame : NULL;
        }
        if (sf) {
            kplane_place(req, &c->kp_shell, c->crtc_id, sf->fb_id, c->width, c->height, c->width, c->height);
        } else if (with_shell && c->probe_fb) {
            kplane_place(req, &c->kp_shell, c->crtc_id, c->probe_fb, c->probe_w, c->probe_h, c->probe_w, c->probe_h);
        } else {
            kplane_off(req, &c->kp_shell);
        }
        if (sf || c->probe_fb) {
            kadd(req, c->kp_shell.id, c->kp_shell.alpha, 0xffff);
            kadd(req, c->kp_shell.id, c->kp_shell.rotation, c->kp_shell.rotate_180);
        }
        if (sf && sf == &c->shell_queued) *planes |= SCENE_SHELL;
    }
    // dim: a black buffer over everything at the wanted alpha
    if (c->kp_dim.id) {
        uint64_t want = with_dim ? c->dim_want : 0;
        if (want) {
            kplane_place(req, &c->kp_dim, c->crtc_id, c->dim_fb, DIM_FB_SIZE, DIM_FB_SIZE, c->width, c->height);
            kadd(req, c->kp_dim.id, c->kp_dim.alpha, want);
        } else {
            kplane_off(req, &c->kp_dim);
        }
        if (want != c->dim_sent) *planes |= SCENE_DIM;
    }
    int r = drmModeAtomicCommit(c->drm_fd, req, flags, c);
    int e = errno;
    drmModeAtomicFree(req);
    errno = e;
    return r;
}

// The driver refused the scene. Say so once per cause, then find the plane
// it will not take: the dim plane first (a screen that does not dim is a
// fair price), then the shell's. A test-only commit tells which without
// touching the screen. The compositor goes on with that plane off.
static int scene_refused(struct vrx_compositor *c, uint32_t flags, uint32_t *planes) {
    int cause = errno;
    static uint64_t said;
    if (cause < 64 && !(said & (1ull << cause))) {
        said |= 1ull << cause;
        fprintf(stderr, "[vrx-compositor] atomic commit refused: %s\n", strerror(cause));
    }
    uint32_t test = (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) | DRM_MODE_ATOMIC_TEST_ONLY, scratch;
    if (c->kp_dim.id && c->dim_want && scene_send(c, false, true, test, &scratch) == 0) {
        fprintf(stderr, "[vrx-compositor] dim plane %u refused (%s): this screen does not dim\n", c->dim_plane,
                strerror(cause));
        memset(&c->kp_dim, 0, sizeof c->kp_dim);
        c->dim_plane = 0;
        c->dim_want = 0;
        c->dim = DIM_NONE;
        screen_facts(c);
        return scene_send(c, true, true, flags, planes);
    }
    if (c->kp_shell.id && scene_send(c, true, false, test, &scratch) == 0) {
        fprintf(stderr, "[vrx-compositor] shell plane %u refused (%s): the shell has no plane\n", c->shell_plane,
                strerror(cause));
        memset(&c->kp_shell, 0, sizeof c->kp_shell);
        c->shell_plane = 0;
        frame_release(c, &c->shell_queued);
        frame_release(c, &c->shell_frame);
        return scene_send(c, true, true, flags, planes);
    }
    errno = cause;
    return -1;
}

// The first frame: one modeset (drmModeSetCrtc, the one legacy call), after
// which the planes and everything else go by commit.
static bool scene_modeset(struct vrx_compositor *c) {
    if (!c->queued.valid) return false;
    c->queued.commit_ns = now_ns();
    if (drmModeSetCrtc(c->drm_fd, c->crtc_id, c->queued.fb_id, 0, 0, &c->connector_id, 1, &c->mode) != 0) {
        fprintf(stderr, "[vrx-compositor] drmModeSetCrtc: %s\n", strerror(errno));
        frame_release(c, &c->queued);
        return false;
    }
    c->mode_set = true;
    c->on_screen = c->queued;
    memset(&c->queued, 0, sizeof c->queued);
    c->queued.dmabuf_fd = -1;
    c->queued.app = -1;
    c->scene_dirty = true;
    if (c->dim == DIM_ALPHA) c->dim_want = dim_alpha(c->light.shown);
    cursor_update(c);
    fprintf(stderr, "[vrx-compositor] modeset: first frame from app %d on screen\n", c->on_screen.app);
    if (c->on_screen.app >= 0) report_app(c, &c->apps[c->on_screen.app], VRX_APP_SHOWN);
    return true;
}

// Send the scene if something changed and nothing is in flight. Called by
// everything that changes it; the flip event calls it again.
static void scene_commit(struct vrx_compositor *c) {
    if (c->dark) return;  // the newest frames wait for the wake
    bool first = false;
    if (!c->mode_set) {
        if (c->handoff_wait) return;  // the splash's light is still coming down (plan 0033 D5)
        if (!scene_modeset(c)) return;
        first = true;
    }
    if (c->commit_pending || !c->scene_dirty) return;
    // a wake turns the CRTC on, which needs ALLOW_MODESET and is the one
    // commit that waits (DPMS-on did too, and nothing animates across it)
    uint32_t flags = c->wake_pending ? DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT
                                     : DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    uint64_t t = now_ns();
    if (c->queued.valid) c->queued.commit_ns = t;
    if (c->shell_queued.valid) c->shell_queued.commit_ns = t;
    uint32_t planes = 0;
    c->scene_dirty = false;
    int r = scene_send(c, true, true, flags, &planes);
    if (r != 0 && errno == EBUSY) {
        // the driver still has the last one; its event (or the loop's
        // retry, when nothing is known to be in flight) sends this again
        c->scene_dirty = true;
        c->scene_retry = !c->commit_pending;
        return;
    }
    if (r != 0) r = scene_refused(c, flags, &planes);
    if (r != 0) {
        // nothing the driver takes: the frames never show and are returned
        commit_errors++;
        frame_release(c, &c->queued);
        frame_release(c, &c->shell_queued);
        if (c->wake_pending) {
            // a screen that cannot be lit is worse than a stale one: turn
            // the CRTC on with whatever its planes already hold
            drmModeAtomicReq *req = drmModeAtomicAlloc();
            if (req) {
                kadd(req, c->crtc_id, c->active_prop, 1);
                drmModeAtomicCommit(c->drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
                drmModeAtomicFree(req);
            }
            c->wake_pending = false;
        }
        return;
    }
    c->scene_retry = false;
    c->wake_pending = false;
    c->commit_pending = true;
    c->commit_planes = planes;
    c->dim_sent = c->kp_dim.id ? c->dim_want : 0;
    if (c->splash_on && c->splash_fb) c->splash_sent = true;
    if (planes & SCENE_PRIMARY) {
        c->pending = c->queued;
        memset(&c->queued, 0, sizeof c->queued);
        c->queued.dmabuf_fd = -1;
        c->queued.app = -1;
    }
    if (planes & SCENE_SHELL) {
        c->shell_pending = c->shell_queued;
        memset(&c->shell_queued, 0, sizeof c->shell_queued);
        c->shell_queued.dmabuf_fd = -1;
        c->shell_queued.app = -1;
    }
    // the first frame is committed: the light comes up on it (a dim plane has no splash to hide)
    if (first) screen_light(c, c->dim == DIM_BACKLIGHT ? VRX_STD_THROUGH_DARK_IN_MS : 0);
}

// The scene changed under the commit (the shell came or went, the chrome
// was hidden or shown, focus moved): send it.
static void scene_update(struct vrx_compositor *c) {
    c->scene_dirty = true;
    scene_commit(c);
}

// The planes the scene uses, once the KMS device and the screen are known.
static void scene_init(struct vrx_compositor *c) {
    kplane_init(c, &c->kp_primary, c->primary_plane, c->upside_down);
    kplane_init(c, &c->kp_shell, c->shell_plane, c->upside_down);
    kplane_init(c, &c->kp_dim, c->dim_plane, false);
    if (!c->kp_dim.id && c->dim_plane) {
        c->dim_plane = 0;
        c->dim = DIM_NONE;
    }
    if (!c->kp_shell.id) c->shell_plane = 0;
    if (!c->kp_primary.id || !c->active_prop) {
        fprintf(stderr, "[vrx-compositor] the CRTC or its primary plane lacks atomic properties\n");
    }
    if (getenv("VRX_PROBE_OVERLAY")) plane_probe_bar(c);
}

// ─── the semantics trees (plan 0014 D8) ─────────────────────────────────
#define SEMANTICS_DIR VRX_UI_SOCKET_DIR "/semantics"

static void semantics_tell(struct vrx_compositor *c, int only) {
    struct vrx_msg_semantics m = { .on = c->semantics };
    for (int i = 0; i < MAX_APPS; i++) {
        struct app_client *a = &c->apps[i];
        if (!a->in_use || !a->registered || (only >= 0 && i != only)) continue;
        client_send(c, a->fd, VRX_MSG_SEMANTICS, &m, sizeof m, -1);
    }
}

static void semantics_set(struct vrx_compositor *c, bool on) {
    if (on == c->semantics) return;
    c->semantics = on;
    if (on) {
        mkdir(SEMANTICS_DIR, 0755);
    } else {
        DIR *d = opendir(SEMANTICS_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                // the directory, a slash and the longest d_name (255) and its NUL: no name is ever cut into another's path
                char path[sizeof SEMANTICS_DIR + 1 + 256];
                snprintf(path, sizeof path, SEMANTICS_DIR "/%s", e->d_name);
                unlink(path);
            }
            closedir(d);
        }
    }
    fprintf(stderr, "[vrx-compositor] semantics %s\n", on ? "on: trees in " SEMANTICS_DIR : "off");
    semantics_tell(c, -1);
}

// An app's tree, as JSON in the memfd it sent: written atomically for the
// walk driver. Bounded: a tree over 1 MiB is refused.
static void semantics_store(struct vrx_compositor *c, const struct app_client *a, int fd, uint32_t length) {
    if (!c->semantics || length == 0 || length > (1u << 20)) return;
    char *json = malloc(length);
    if (!json) return;
    ssize_t n = pread(fd, json, length, 0);
    if (n == (ssize_t) length) {
        char path[256], tmp[256];
        snprintf(path, sizeof path, SEMANTICS_DIR "/%s.json", a->id);
        snprintf(tmp, sizeof tmp, SEMANTICS_DIR "/.%s.json", a->id);
        int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (out >= 0) {
            bool ok = write(out, json, length) == (ssize_t) length;
            close(out);
            if (ok) rename(tmp, path);
            else unlink(tmp);
        }
    }
    free(json);
}

// ─── the flip record (plan 0014 D5, D6, D7) ─────────────────────────────

// One line on ftrace's trace_marker, while profiling is on. Perfetto's
// systrace importer reads `C|pid|name|value` as a counter track.
static void marker(struct vrx_compositor *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void marker(struct vrx_compositor *c, const char *fmt, ...) {
    if (!c->profiling || c->marker_fd < 0) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0 && write(c->marker_fd, line, (size_t) (n < (int) sizeof line ? n : (int) sizeof line - 1)) < 0) {
        fprintf(stderr, "[vrx-compositor] trace_marker: %s; the flip record stops\n", strerror(errno));
        close(c->marker_fd);
        c->marker_fd = -1;
    }
}

static void flip_ring_dump(const char *why);

static void profiling_set(struct vrx_compositor *c, bool on) {
    if (on == c->profiling) return;
    // what happened before the switch: the ring's last 60 s
    if (on) flip_ring_dump("profiling switched on");
    c->profiling = on;
    if (on) {
        c->marker_fd = open("/sys/kernel/tracing/trace_marker", O_WRONLY | O_CLOEXEC);
        if (c->marker_fd < 0) c->marker_fd = open("/sys/kernel/debug/tracing/trace_marker", O_WRONLY | O_CLOEXEC);
        fprintf(stderr, "[vrx-compositor] profiling on: flip record %s\n",
                c->marker_fd >= 0 ? "to trace_marker" : "not written (tracefs is not mounted)");
        c->drops_app = c->drops_compositor = c->drops_underrun = c->drops_unattributed = 0;
    } else {
        if (c->marker_fd >= 0) close(c->marker_fd);
        c->marker_fd = -1;
        c->trace_input_ns = 0;
        fprintf(stderr, "[vrx-compositor] profiling off\n");
    }
}

// One refresh, from the mode's own timings (the vrefresh field rounds).
static uint64_t mode_period_ns(const drmModeModeInfo *m) {
    if (m->clock && m->htotal && m->vtotal) return (uint64_t) m->htotal * m->vtotal * 1000000ull / m->clock;
    return 1000000000ull / (m->vrefresh ? m->vrefresh : 60);
}

// The vc4 HVS underrun count (debugfs); 0 where there is none.
static uint64_t hvs_underruns(void) {
    uint64_t total = 0;
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof path, "/sys/kernel/debug/dri/%d/hvs_underrun", i);
        FILE *f = fopen(path, "re");
        if (!f) continue;
        unsigned long long n = 0;
        if (fscanf(f, "%llu", &n) == 1) total += n;
        fclose(f);
    }
    return total;
}

// Tell an app the panel's vblank phase (D6): on focus, then at most once a
// second while its frames flip (the phase does not drift; a message per
// flip would only cost wakes).
static void send_vblank(struct vrx_compositor *c, int slot, bool now) {
    if (slot < 0 || slot >= MAX_APPS || !c->apps[slot].in_use || !c->apps[slot].registered || !c->last_flip_ns) return;
    uint64_t t = now_ns();
    if (!now && t - c->apps[slot].vblank_sent_ns < 1000000000ull) return;
    c->apps[slot].vblank_sent_ns = t;
    struct vrx_msg_vblank m = { .flip_ns = c->last_flip_ns, .period_ns = c->period_ns };
    client_send(c, c->apps[slot].fd, VRX_MSG_VBLANK, &m, sizeof m, -1);
}

// The flip ring (plan 0014 D5): the last 60 s of flips, kept whether or
// not profiling is on, so a walk or a person who saw a stutter can ask for
// what just happened. A static array written in place per flip: no
// allocation and a few stores per frame. SIGUSR1 (or switching profiling
// on) writes it to /run/vrx-ui/flips, oldest first.
#define FLIP_RING_LEN 8192        // 60 s at up to 120 Hz, with margin
#define FLIP_RING_NS (60ull * 1000000000ull)
#define FLIP_RING_FILE VRX_UI_SOCKET_DIR "/flips"
enum flip_cause { FLIP_OK, FLIP_APP_LATE, FLIP_COMPOSITOR_LATE, FLIP_UNDERRUN, FLIP_UNATTRIBUTED };
static const char *const flip_cause_name[] = { "ok", "app_late", "compositor_late", "underrun", "unattributed" };
struct flip_entry {
    uint64_t flip_ns, recv_ns, commit_ns;
    uint32_t seq, delta, skipped;
    uint8_t cause;
    uint8_t planes;  // SCENE_* planes the commit changed
    char id[22];
};
static struct flip_entry flip_ring[FLIP_RING_LEN];
static uint32_t flip_ring_next, flip_ring_count;

static void flip_ring_push(const struct frame *f, const char *id, uint32_t seq, uint32_t delta,
                           uint32_t skipped, enum flip_cause cause, uint32_t planes, uint64_t flip_ns) {
    struct flip_entry *e = &flip_ring[flip_ring_next];
    e->flip_ns = flip_ns;
    e->recv_ns = f->recv_ns;
    e->commit_ns = f->commit_ns;
    e->seq = seq;
    e->delta = delta;
    e->skipped = skipped;
    e->cause = (uint8_t) cause;
    e->planes = (uint8_t) planes;
    size_t n = strnlen(id, sizeof e->id - 1);
    memcpy(e->id, id, n);
    e->id[n] = 0;
    flip_ring_next = (flip_ring_next + 1) % FLIP_RING_LEN;
    if (flip_ring_count < FLIP_RING_LEN) flip_ring_count++;
}

// The ring's last 60 s, oldest first, to /run/vrx-ui/flips (a new file
// renamed over the old, so a reader never sees half of one).
static void flip_ring_dump(const char *why) {
    char tmp[] = FLIP_RING_FILE ".tmp";
    FILE *out = fopen(tmp, "we");
    if (!out) {
        fprintf(stderr, "[vrx-compositor] %s: %s\n", tmp, strerror(errno));
        return;
    }
    uint32_t first = (flip_ring_next + FLIP_RING_LEN - flip_ring_count) % FLIP_RING_LEN;
    uint64_t newest = flip_ring_count ? flip_ring[(flip_ring_next + FLIP_RING_LEN - 1) % FLIP_RING_LEN].flip_ns : 0;
    fprintf(out, "# vrx flip ring (plan 0014 D5), the last 60 s, oldest first (%s)\n", why);
    fprintf(out, "# scene commits refused since start: %" PRIu64 "\n", commit_errors);
    fprintf(out, "# planes: the planes the commit changed, P primary (the app), S shell, D dim, W wake (plan 0017 D9)\n");
    fprintf(out, "# flip_ns\tseq\tdelta\tskipped\tapp\tarrival_to_screen_us\tcommit_to_screen_us\tcause\tplanes\n");
    unsigned written = 0;
    for (uint32_t i = 0; i < flip_ring_count; i++) {
        const struct flip_entry *e = &flip_ring[(first + i) % FLIP_RING_LEN];
        if (newest > FLIP_RING_NS && e->flip_ns < newest - FLIP_RING_NS) continue;
        char planes[5] = "";
        snprintf(planes, sizeof planes, "%s%s%s%s", e->planes & SCENE_PRIMARY ? "P" : "", e->planes & SCENE_SHELL ? "S" : "",
                 e->planes & SCENE_DIM ? "D" : "", e->planes & SCENE_WAKE ? "W" : "");
        fprintf(out, "%" PRIu64 "\t%u\t%u\t%u\t%s\t%" PRIu64 "\t%" PRIu64 "\t%s\t%s\n", e->flip_ns, e->seq, e->delta,
                e->skipped, e->id, e->recv_ns && e->flip_ns > e->recv_ns ? (e->flip_ns - e->recv_ns) / 1000 : 0,
                e->commit_ns && e->flip_ns > e->commit_ns ? (e->flip_ns - e->commit_ns) / 1000 : 0,
                flip_cause_name[e->cause], planes[0] ? planes : "-");
        written++;
    }
    fclose(out);
    chmod(tmp, 0644);
    if (rename(tmp, FLIP_RING_FILE) != 0) {
        fprintf(stderr, "[vrx-compositor] %s: %s\n", FLIP_RING_FILE, strerror(errno));
        return;
    }
    fprintf(stderr, "[vrx-compositor] flip ring: %u flips to %s (%s)\n", written, FLIP_RING_FILE, why);
}

// A flip completed: record it (D5). A refresh counts as dropped only when
// the sequence skipped while the frame's applet was producing frames
// steadily (its previous frame came within three refreshes): a still
// screen skips refreshes by design. The cause is named by when the frame
// arrived and was committed against the deadline (the first vblank after
// the previous flip): arrived late, the app was late; arrived in time but
// committed late, the compositor was late; committed in time and the HVS
// underrun count moved, an underrun.
static void flip_record(struct vrx_compositor *c, const struct frame *f, uint32_t seq, uint32_t planes,
                        uint64_t flip_ns) {
    pid_t pid = getpid();
    const char *id = f->app >= 0 && f->app < MAX_APPS ? c->apps[f->app].id : "gone";
    uint32_t delta = 0, skipped_ring = 0;
    enum flip_cause cause_ring = FLIP_OK;
    if (c->last_flip_ns && c->period_ns && seq > c->last_flip_seq) {
        delta = seq - c->last_flip_seq;
        uint64_t deadline = c->last_flip_ns + c->period_ns;
        bool producing = f->prev_recv_ns && f->recv_ns - f->prev_recv_ns < 3 * c->period_ns;
        marker(c, "C|%d|vrx.flip.seq_delta|%u", (int) pid, delta);
        if (delta > 1 && producing) {
            uint64_t underruns = hvs_underruns();
            uint64_t skipped = delta - 1;
            const char *cause;
            if (f->recv_ns >= deadline) {
                c->drops_app += skipped;
                cause_ring = FLIP_APP_LATE;
            } else if (f->commit_ns >= deadline) {
                c->drops_compositor += skipped;
                cause_ring = FLIP_COMPOSITOR_LATE;
            } else if (underruns > c->underruns_seen) {
                c->drops_underrun += skipped;
                cause_ring = FLIP_UNDERRUN;
            } else {
                c->drops_unattributed += skipped;
                cause_ring = FLIP_UNATTRIBUTED;
            }
            cause = flip_cause_name[cause_ring];
            skipped_ring = (uint32_t) skipped;
            c->underruns_seen = underruns;
            marker(c, "C|%d|vrx.dropped.%s|%llu", (int) pid, cause,
                   (unsigned long long) (!strcmp(cause, "app_late") ? c->drops_app
                                         : !strcmp(cause, "compositor_late") ? c->drops_compositor
                                         : !strcmp(cause, "underrun") ? c->drops_underrun
                                                                       : c->drops_unattributed));
            marker(c, "C|%d|vrx.%s.dropped|%llu", (int) pid, id, (unsigned long long) skipped);
        }
    }
    if (f->recv_ns && flip_ns > f->recv_ns) {
        marker(c, "C|%d|vrx.%s.arrival_to_screen_us|%llu", (int) pid, id,
               (unsigned long long) ((flip_ns - f->recv_ns) / 1000));
    }
    // input to screen (D7): the input this frame answered
    if (c->trace_input_ns && c->trace_buffer_ns && flip_ns > c->trace_input_ns) {
        marker(c, "C|%d|vrx.input_to_buffer_us|%llu", (int) pid,
               (unsigned long long) ((c->trace_buffer_ns - c->trace_input_ns) / 1000));
        marker(c, "C|%d|vrx.input_to_screen_us|%llu", (int) pid,
               (unsigned long long) ((flip_ns - c->trace_input_ns) / 1000));
        c->trace_input_ns = 0;
    }
    flip_ring_push(f, id, seq, delta, skipped_ring, cause_ring, planes, flip_ns);
    c->last_flip_seq = seq;
    c->last_flip_ns = flip_ns;
}

// DRM says the commit was scanned out: the frames it replaced are free. A
// commit that changed no frame (the dim plane's alpha, a wake) only moves
// the vblank phase; the record keeps one entry per commit that did, headed
// by the frame that arrived last, so a late shell frame and a late app
// frame are each seen as what they are.
static void sleep_release(struct vrx_compositor *c);
static void resume_essentials(struct vrx_compositor *c, bool facts, bool clock);

static void on_page_flip(int fd, unsigned int seq, unsigned int sec,
                         unsigned int usec, void *data) {
    (void) fd;
    struct vrx_compositor *c = data;
    // the event's timestamp is the vblank's, when the driver stamps with
    // CLOCK_MONOTONIC; otherwise now is the nearest honest time
    uint64_t flip_ns = c->flip_ts_monotonic ? (uint64_t) sec * 1000000000ull + (uint64_t) usec * 1000ull : now_ns();
    c->commit_pending = false;
    const struct frame *head = NULL;
    if (c->pending.valid && c->shell_pending.valid) {
        head = c->shell_pending.recv_ns > c->pending.recv_ns ? &c->shell_pending : &c->pending;
    } else if (c->pending.valid) {
        head = &c->pending;
    } else if (c->shell_pending.valid) {
        head = &c->shell_pending;
    }
    if (head) {
        flip_record(c, head, seq, c->commit_planes, flip_ns);
    } else {
        c->last_flip_seq = seq;
        c->last_flip_ns = flip_ns;
    }
    if (c->pending.valid) {
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
    }
    if (c->shell_pending.valid) {
        frame_release(c, &c->shell_frame);
        c->shell_frame = c->shell_pending;
        memset(&c->shell_pending, 0, sizeof c->shell_pending);
        c->shell_pending.dmabuf_fd = -1;
        c->shell_pending.app = -1;
    }
    // the shell's first frame on its plane is the UI coming up, which
    // vrxhost's update health waits on (vrx plan 0016 D6). The switch above
    // reports only the primary plane's app, so on 0.1.0122 the shell was
    // never reported and no slot could be kept (bench, 2026-10-16).
    if (c->shell >= 0 && !c->apps[c->shell].shown && c->kp_shell.id && chrome_shown(c) &&
        c->shell_frame.valid && c->shell_frame.app == c->shell) {
        c->apps[c->shell].shown = true;
        fprintf(stderr, "[vrx-compositor] the shell's first frame is on plane %u\n", c->shell_plane);
        report_app(c, &c->apps[c->shell], VRX_APP_SHOWN);
    }
    c->commit_planes = 0;
    if (c->splash_sent && c->splash_release) {
        // the splash is on the glass: the request may go (plan 0034 D5)
        c->splash_release = false;
        sleep_release(c);
    }
    c->splash_sent = false;
    send_vblank(c, c->on_screen.app, false);
    send_vblank(c, c->shell, false);
    scene_commit(c);
    if (c->light_after_flip && !c->commit_pending) {
        // waking: the current screen is up, now it can be seen
        c->light_after_flip = false;
        screen_light(c, VRX_STD_THROUGH_DARK_IN_MS);
    }
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

// The shell is on screen: there is one, and the focused app has not asked
// for the whole screen.
static bool chrome_shown(const struct vrx_compositor *c) {
    return c->shell >= 0 && !(c->focused >= 0 && c->apps[c->focused].no_chrome);
}

// Tell every app but the shell how tall the strip is. An app that took
// the whole screen knows it did and lays itself out without the strip;
// telling it 0 meant its first frame after giving the screen back was
// laid out under the strip (bench 2026-10-13).
static void send_insets(struct vrx_compositor *c, int only) {
    struct vrx_msg_insets m = { .top = c->shell >= 0 ? c->shell_top : 0 };
    for (int i = 0; i < MAX_APPS; i++) {
        struct app_client *a = &c->apps[i];
        if (!a->in_use || !a->registered || i == c->shell || (only >= 0 && i != only)) continue;
        client_send(c, a->fd, VRX_MSG_INSETS, &m, sizeof m, -1);
    }
}

static void apply_want_shell(struct vrx_compositor *c) {
    int slot = -1;
    for (int i = 0; c->want_shell > 0 && i < MAX_APPS; i++) {
        struct app_client *a = &c->apps[i];
        if (a->in_use && a->registered && a->pid == c->want_shell) { slot = i; break; }
    }
    if (slot == c->shell) return;
    c->shell = slot;
    c->shell_top = slot >= 0 ? c->apps[slot].region_top : 0;
    c->shell_full = slot >= 0 && c->apps[slot].region_full;
    if (slot < 0) {
        frame_release(c, &c->shell_queued);
        frame_release(c, &c->shell_frame);
        scene_update(c);
    } else {
        fprintf(stderr, "[vrx-compositor] shell -> slot %d ('%s', pid %d), plane %u\n", slot, c->apps[slot].id,
                (int) c->apps[slot].pid, c->shell_plane);
    }
    send_insets(c, -1);
    if (slot >= 0 && c->power_off_left >= 0) {
        struct vrx_msg_power_off m = { .secs_left = c->power_off_left };
        client_send(c, c->apps[slot].fd, VRX_MSG_POWER_OFF, &m, sizeof m, -1);
    }
}

static void focus_slot(struct vrx_compositor *c, int slot) {
    if (slot == c->focused) return;
    int old = c->focused;
    c->focused = slot;
    // a frame from the old focus still waiting for the flip slot never shows
    frame_release(c, &c->queued);
    // the shell steps aside for an app that has the whole screen
    scene_update(c);
    // a key held into a focus change stops repeating into the new app
    repeat_arm(c, 0);
    if (old >= 0 && c->apps[old].in_use) {
        client_send(c, c->apps[old].fd, VRX_MSG_FOCUS,
                    &(struct vrx_msg_focus){ .focused = 0 }, sizeof(struct vrx_msg_focus), -1);
    }
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
    send_vblank(c, slot, true);
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
    struct frame *frames[] = { &c->on_screen, &c->pending, &c->queued, &c->shell_frame, &c->shell_pending,
                               &c->shell_queued };
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        if (frames[i]->valid && frames[i]->app == slot) frames[i]->app = -1;
    }
    memset(a, 0, sizeof *a);
    a->fd = -1;
    // what comes next on screen is vrxhost's decision (it sees the exit
    // both here and as its child's status)
    if (c->focused == slot) {
        c->focused = -1;
        scene_update(c);
    }
    if (c->shell == slot) {
        // its plane goes dark and apps may use the whole screen again
        c->shell = -1;
        frame_release(c, &c->shell_queued);
        frame_release(c, &c->shell_frame);
        scene_update(c);
        send_insets(c, -1);
    }
    for (int i = 0; i < 16; i++) {
        if (c->touch_owner[i] == slot) c->touch_owner[i] = -1;
    }
    if (c->pointer_owner == slot) c->pointer_owner = -1;
}

// Whether the process holds the display grant (group vrx-display): the one
// thing that lets an applet set the brightness. Read from /proc, where the
// unit's SupplementaryGroups show; the connection's own credentials do not carry them.
static bool display_grant(pid_t pid) {
    struct group *gr = getgrnam("vrx-display");
    if (!gr) return false;
    char path[64], text[4096];
    snprintf(path, sizeof path, "/proc/%d/status", (int) pid);
    FILE *f = fopen(path, "re");
    if (!f) return false;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    return vrx_status_has_group(text, gr->gr_gid);
}

// Handle one message. `fd` is the descriptor that rode it, or -1; this
// function takes ownership of it.
static void client_handle(struct vrx_compositor *c, int slot,
                          const struct vrx_msg_header *h, void *payload, int fd) {
    struct app_client *a = &c->apps[slot];
    // a connection is an app (REGISTER) or the controller (CONTROL), once
    bool app_msg = h->type == VRX_MSG_BUFFER || h->type == VRX_MSG_CLOSED || h->type == VRX_MSG_CHROME ||
                   h->type == VRX_MSG_SEMANTICS_TREE || h->type == VRX_MSG_BRIGHTNESS;
    if ((app_msg && !a->registered) ||
        ((h->type == VRX_MSG_SET_FOCUS || h->type == VRX_MSG_SET_SHELL || h->type == VRX_MSG_LIGHT_CAP ||
          h->type == VRX_MSG_PROFILING || h->type == VRX_MSG_SEMANTICS || h->type == VRX_MSG_SCREEN ||
          h->type == VRX_MSG_POWER_OFF) &&
         !a->controller) ||
        (h->type == VRX_MSG_POWER_OFF_ANSWER && slot != c->shell) ||
        (h->type == VRX_MSG_SHELL_REGION && !a->registered) ||
        (h->type == VRX_MSG_BACK && slot != c->shell) ||
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
            // an app starting while the screen is off starts hidden
            if (c->dark) display_tell(c, slot);
            // and one starting during a walk's planning builds its tree
            if (c->semantics) semantics_tell(c, slot);
            if (c->want_shell > 0 && a->pid == c->want_shell) {
                apply_want_shell(c);
            } else {
                send_insets(c, slot);
            }
            if (c->want_focus > 0 && a->pid == c->want_focus && slot != c->shell) focus_slot(c, slot);
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
            // a new controller knows nothing: the screen as it is now
            c->reported_screen_valid = false;
            screen_report(c);
            break;
        }
        case VRX_MSG_SCREEN: {
            struct vrx_msg_screen *m = payload;
            if (h->length < sizeof *m) goto bad;
            // vrxhost's word (plan 0023 D1): its idle chain, the lid, the
            // power key, a resume, a home app's On/Off
            if (m->on) {
                display_on(c, "vrxhost");
            } else {
                display_off(c, "vrxhost");
            }
            // the answer either way: what the screen is now
            screen_report(c);
            break;
        }
        case VRX_MSG_POWER_OFF: {
            struct vrx_msg_power_off *m = payload;
            if (h->length < sizeof *m) goto bad;
            // vrxhost's countdown: the shell shows it (the dialog is the
            // design system's, drawn by the shell's UI), and a shell that
            // comes later is told what is still running
            if (m->secs_left == VRX_POWER_OFF_DARKEN) {
                // through dark (plan 0033 D1): the light comes down, and vrxhost is told when it is
                c->power_off_dark = 1;
                light_go(c, false, VRX_STD_THROUGH_DARK_OUT_MS);
                break;
            }
            if (c->power_off_dark) {
                // vrxhost did not power off (logind refused): the light comes back
                c->power_off_dark = 0;
                screen_light(c, VRX_STD_THROUGH_DARK_IN_MS);
            }
            c->power_off_left = m->secs_left < 0 ? VRX_POWER_OFF_CLOSED : m->secs_left;
            if (c->shell >= 0) {
                client_send(c, c->apps[c->shell].fd, VRX_MSG_POWER_OFF, m, sizeof *m, -1);
            }
            break;
        }
        case VRX_MSG_POWER_OFF_ANSWER: {
            struct vrx_msg_power_off_answer *m = payload;
            if (h->length < sizeof *m) goto bad;
            if (c->controller >= 0) {
                client_send(c, c->apps[c->controller].fd, VRX_MSG_POWER_OFF_ANSWER, m, sizeof *m, -1);
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
        case VRX_MSG_SET_SHELL: {
            struct vrx_msg_set_shell *m = payload;
            if (h->length < sizeof *m) goto bad;
            c->want_shell = m->pid;
            apply_want_shell(c);
            break;
        }
        case VRX_MSG_SEMANTICS: {
            struct vrx_msg_semantics *m = payload;
            if (h->length < sizeof *m) goto bad;
            semantics_set(c, m->on != 0);
            break;
        }
        case VRX_MSG_SEMANTICS_TREE: {
            struct vrx_msg_semantics_tree *m = payload;
            if (h->length < sizeof *m || fd < 0) goto bad;
            semantics_store(c, a, fd, m->length);
            break;
        }
        case VRX_MSG_PROFILING: {
            struct vrx_msg_profiling *m = payload;
            if (h->length < sizeof *m) goto bad;
            profiling_set(c, m->on != 0);
            break;
        }
        case VRX_MSG_LIGHT_CAP: {
            struct vrx_msg_light_cap *m = payload;
            if (h->length < sizeof *m) goto bad;
            uint32_t cap = m->permille > 1000 ? 1000 : m->permille;
            if (cap != c->light.cap) {
                fprintf(stderr, "[vrx-compositor] brightness cap %u/1000 above the floor%s\n", cap,
                        cap == 1000 ? " (none)" : cap == 0 ? " (dimmed)" : " (low power)");
                bool raised = cap > c->light.cap;
                vrx_screen_light_cap(&c->light, now_ms(), (uint16_t) cap,
                                     c->fade_ok ? (raised ? VRX_STD_THROUGH_DARK_IN_MS : VRX_STD_THROUGH_DARK_OUT_MS) : 0);
                light_timer(c);
            }
            break;
        }
        case VRX_MSG_RESUME_ESSENTIALS: {
            // the kit's word on the resume's essentials (plan 0034 D3): a Facts reading after the wake, a known clock source
            struct vrx_msg_resume_essentials *m = payload;
            if (h->length < sizeof *m) goto bad;
            resume_essentials(c, m->facts != 0, m->clock != 0);
            break;
        }
        case VRX_MSG_BRIGHTNESS: {
            // an applet with the display grant (Settings, the shell) sets the person's brightness
            struct vrx_msg_brightness *m = payload;
            if (h->length < sizeof *m) goto bad;
            if (!display_grant(a->pid)) {
                fprintf(stderr, "[vrx-compositor] app '%s' (pid %d) set the brightness without the display grant: ignored\n",
                        a->id, (int) a->pid);
                break;
            }
            if (c->dim == DIM_NONE) break;
            vrx_screen_light_set(&c->light, now_ms(), (uint16_t) (m->permille > 1000 ? 1000 : m->permille));
            light_timer(c);
            screen_report(c);
            break;
        }
        case VRX_MSG_SHELL_REGION: {
            struct vrx_msg_shell_region *m = payload;
            if (h->length < sizeof *m) goto bad;
            a->region_top = m->top > c->height ? c->height : m->top;
            a->region_full = m->full != 0;
            // the shell-to-be's region waits for vrxhost to name it
            if (slot != c->shell) break;
            bool changed = a->region_top != c->shell_top;
            c->shell_top = a->region_top;
            c->shell_full = a->region_full;
            if (changed) send_insets(c, -1);
            break;
        }
        case VRX_MSG_BACK:
            if (c->focused >= 0 && c->focused != c->shell) {
                client_send(c, c->apps[c->focused].fd, VRX_MSG_BACK, NULL, 0, -1);
            }
            break;
        case VRX_MSG_CHROME: {
            struct vrx_msg_chrome *m = payload;
            if (h->length < sizeof *m || slot == c->shell) goto bad;
            if (a->no_chrome == (m->hidden != 0)) break;
            a->no_chrome = m->hidden != 0;
            if (slot == c->focused) scene_update(c);
            break;
        }
        case VRX_MSG_BUFFER: {
            struct vrx_msg_buffer *b = payload;
            if (h->length < sizeof *b || fd < 0) goto bad;
            uint64_t arrived = now_ns();
            struct frame f = {
                .valid = true, .app = slot, .app_fd = a->fd,
                .sequence = b->sequence, .dmabuf_fd = fd,
                .recv_ns = arrived, .prev_recv_ns = a->last_recv_ns,
            };
            a->last_recv_ns = arrived;
            fd = -1;  // the frame owns it now
            if (slot == c->shell) {
                // the shell's plane: the frame rides the next scene commit,
                // and a newer one supersedes it if that is still waiting
                if (!c->shell_plane || !frame_import(c, &f, b)) {
                    frame_release(c, &f);
                    break;
                }
                if ((!chrome_shown(c) || !c->mode_set) && !c->shell_pending.valid) {
                    // nothing on the panel shows the shell: it only keeps
                    // its newest frame for when the chrome comes back
                    frame_release(c, &c->shell_frame);
                    c->shell_frame = f;
                    break;
                }
                frame_release(c, &c->shell_queued);
                c->shell_queued = f;
                scene_update(c);
                break;
            }
            if (slot != c->focused || !frame_import(c, &f, b)) {
                // unfocused (or unimportable): straight back to the app
                frame_release(c, &f);
                break;
            }
            if (c->profiling && c->trace_input_ns && !c->trace_buffer_ns) c->trace_buffer_ns = now_ns();
            // newest wins: a frame still waiting for the flip slot is
            // superseded and returned
            frame_release(c, &c->queued);
            c->queued = f;
            scene_update(c);
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
            // on an upside-down panel the cursor plane is not turned: draw
            // the arrow turned, so it points up-left to the person
            if (c->upside_down) {
                px[(CURSOR_SIZE - 1 - y) * stride + (CURSOR_SIZE - 1 - x)] = v;
            } else {
                px[y * stride + x] = v;
            }
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
    if (c->cursor_shown) {
        // the pointer is in the person's frame; the cursor plane in the
        // panel's: turned, the arrow's tip is the image's far corner
        if (c->upside_down) {
            drmModeMoveCursor(c->drm_fd, c->crtc_id, (int) c->width - 1 - (int) c->px - (CURSOR_SIZE - 1),
                              (int) c->height - 1 - (int) c->py - (CURSOR_SIZE - 1));
        } else {
            drmModeMoveCursor(c->drm_fd, c->crtc_id, (int) c->px, (int) c->py);
        }
    }
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

// Where an event goes: the shell inside its region (all of the screen
// while its drawer is open), the focused app elsewhere. A touch stays
// with whoever its first contact went to; so does a held mouse button.
static int input_target(struct vrx_compositor *c, const struct vrx_msg_input *m) {
    bool shell = chrome_shown(c);
    switch (m->kind) {
    case VRX_INPUT_TOUCH: {
        uint32_t s = m->u.touch.slot < 16 ? m->u.touch.slot : 15;
        if (m->u.touch.kind == 0) {
            c->touch_owner[s] = shell && (c->shell_full || (uint32_t) m->u.touch.y < c->shell_top) ? c->shell : c->focused;
        }
        int t = c->touch_owner[s];
        if (m->u.touch.kind == 2) c->touch_owner[s] = -1;
        return t;
    }
    case VRX_INPUT_POINTER: {
        if (c->pointer_owner >= 0) {
            int t = c->pointer_owner;
            if (m->u.pointer.buttons == 0) c->pointer_owner = -1;
            return t;
        }
        int t = shell && (c->shell_full || (uint32_t) m->u.pointer.y < c->shell_top) ? c->shell : c->focused;
        if (m->u.pointer.buttons != 0) c->pointer_owner = t;
        return t;
    }
    default:
        return shell && c->shell_full ? c->shell : c->focused;
    }
}

// ── screen power (plan 0010, plan 0023 D1) and the light (plan 0033) ─────
// vrxhost decides; the compositor executes. vrxhost's idle chain (dim,
// screen off, sleep, deep sleep, from the last input it reads on evdev)
// sends MSG_SCREEN to turn the light to 0 and the connector to DPMS off,
// or both back on, and MSG_LIGHT_CAP to dim (0: the floor) or to cap the
// light under low power. The compositor is the one writer of the light.
// Brightness is one number, per mille and perceived (libvrx-light's curve),
// stored once in /data/vrx/display/brightness by vrx_screen_light; a slider
// asks for it with MSG_BRIGHTNESS. A lit screen never goes under the
// standard's floor, so a stored 0 is still a screen a person can read (the
// owner, 2026-10-14).
//
// A system moment goes through dark (plan 0033 D1): the light fades down,
// the screen changes in one paint, the light fades up. The handoff (first
// frame), screen off and on, sleep, shutdown and resume are those moments;
// an app's launch or close is not. A screen that cannot dim fades through
// black in frames only on the full tier (a dim plane over everything); on
// the others, and where there is no plane, the moment is a cut.
#define BRIGHTNESS_FILE "/data/vrx/display/brightness"
#define MOTION_TIER_FILE "/usr/lib/vrx/motion-tier"
#define SCREEN_FACTS VRX_UI_SOCKET_DIR "/screen"
// the rate a fade is stepped at (about a 60 Hz panel's frame)
#define LIGHT_STEP_MS 16
// after a resume the light waits for the first frame; at the latest this long
#define RESUME_WAIT_MS 300

static void screen_facts(const struct vrx_compositor *c);

static int sysfs_int(const char *dir, const char *what) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", dir, what);
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

// The light's one Linux writer is libvrx-light's (it also unblanks a backlight that came up powered down, which a
// bare `brightness` write leaves dark: bench 2026-11-19, the PW-SH7).
static void backlight_set(struct vrx_compositor *c, int level) {
    if (!c->backlight[0] || level == c->bl_level) return;
    if (!c->bl_map.dir[0]) snprintf(c->bl_map.dir, sizeof c->bl_map.dir, "%s", c->backlight);
    const int rc = vrx_light_write_level(&c->bl_map, level);
    if (rc < 0) {
        fprintf(stderr, "[vrx-compositor] %s: %s\n", c->backlight, strerror(-rc));
        return;
    }
    c->bl_level = level;
}

// The dim plane's alpha for a shown brightness: encoded values scale with
// alpha, and an encoded share is close to a perceived one, so the plane
// is black at 1 - shown/1000 (0 at full).
static uint64_t dim_alpha(uint16_t shown) {
    return (uint64_t) (0xffff * (1000 - (shown > 1000 ? 1000 : shown)) / 1000);
}

// Show the dim plane at `alpha` over the whole CRTC, or take it off at 0.
// The alpha rides the scene's next commit (the one in flight finishes
// first), so the slider moves with whatever else is moving. A plane the
// driver refuses leaves the screen undimmed and says so once (scene_refused).
static void dim_show(struct vrx_compositor *c, uint64_t alpha) {
    if (!c->dim_plane || !c->mode_set || alpha == c->dim_want) return;
    c->dim_want = alpha;
    scene_update(c);
}

// vrx_screen_light's output: what the screen shows now, 0 = dark. A dimmable
// backlight takes the level of that brightness; a light that cannot dim is
// on or off, with the dim plane (where there is one) doing the rest.
static void light_apply(void *ctx, uint16_t shown) {
    struct vrx_compositor *c = ctx;
    if (c->dim == DIM_BACKLIGHT) {
        backlight_set(c, shown ? vrx_light_level(&c->bl_map, shown) : 0);
        return;
    }
    backlight_set(c, shown ? c->bl_max : 0);
    // dark is the panel going off, not a black plane: the plane keeps its last alpha
    if (c->dim == DIM_ALPHA && shown) dim_show(c, dim_alpha(shown));
}

static int light_read_level(void *ctx) {
    const struct vrx_compositor *c = ctx;
    return c->dim == DIM_BACKLIGHT ? sysfs_int(c->backlight, "brightness") : c->bl_max;
}

static int light_read_stored(void *ctx) {
    (void) ctx;
    FILE *f = fopen(BRIGHTNESS_FILE, "re");
    if (!f) return -1;
    long v = -1;
    char junk;
    if (fscanf(f, "%ld%c", &v, &junk) < 1 || v < 0 || v > 1000) v = -1;
    fclose(f);
    return (int) v;
}

// Written to a new file and renamed over, so a power cut leaves the old number or the new one.
static int light_write_stored(void *ctx, uint16_t permille) {
    (void) ctx;
    // /data is mounted before systemd (the pre-init); the directories are ours to make, /data itself is not
    mkdir("/data/vrx", 0755);
    mkdir("/data/vrx/display", 0755);
    char tmp[] = BRIGHTNESS_FILE ".tmp";
    FILE *f = fopen(tmp, "we");
    if (!f) {
        fprintf(stderr, "[vrx-compositor] %s: %s\n", tmp, strerror(errno));
        return -errno;
    }
    fprintf(f, "%u\n", permille);
    if (fclose(f) != 0 || rename(tmp, BRIGHTNESS_FILE) != 0) {
        fprintf(stderr, "[vrx-compositor] %s: %s\n", BRIGHTNESS_FILE, strerror(errno));
        return -errno;
    }
    return 0;
}

static void screen_off_now(struct vrx_compositor *c, const char *why);

// The steps of a fade, while one runs; and what waits for the light to be down.
static void light_timer(struct vrx_compositor *c) {
    bool run = c->light.fade.active || c->resume_deadline_ms;
    struct itimerspec t = {0};
    if (run) {
        t = (struct itimerspec){ .it_interval = { .tv_nsec = LIGHT_STEP_MS * 1000000L },
                                 .it_value = { .tv_nsec = LIGHT_STEP_MS * 1000000L } };
    } else if (c->resume.stage == VRX_RESUME_WAITING) {
        // the splash is held for the essentials: one shot at the bound, and an event ends it sooner
        uint32_t left = vrx_resume_left(&c->resume, now_ms());
        t.it_value = (struct timespec){ .tv_sec = left / 1000, .tv_nsec = (long) (left % 1000) * 1000000L };
    }
    timerfd_settime(c->light_fd, 0, &t, NULL);
}

static void sleep_release(struct vrx_compositor *c);
static void light_go(struct vrx_compositor *c, bool lit, uint32_t ms);
static void screen_light(struct vrx_compositor *c, uint32_t ms);

// ─── the resume's presentation (vrx plan 0034 D1, D3, D5) ───────────────
// Before the system sleeps and the light is down, the splash goes on the glass and only then is the
// request let go; on the wake the light rises to the standard's splash level and the splash holds
// until the kit says a Facts reading and a clock source are back and the input devices are,
// or until the standard's bound; then through dark into the UI. The sequence is vrx_resume.c's.
#define SPLASH_DIR "/usr/share/vrx/splash"

static void splash_say(const char *what, void *ctx) {
    (void) ctx;
    fprintf(stderr, "[vrx-compositor] splash: %s\n", what);
}

// The splash's framebuffer, made once: the file (or the page colour) in a dumb XRGB8888 buffer of the CRTC's size.
static bool splash_make(struct vrx_compositor *c) {
    if (c->splash_fb) return true;
    if (!c->mode_set) return false;
    struct drm_mode_create_dumb cd = { .width = c->width, .height = c->height, .bpp = 32 };
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0) return false;
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) != 0) return false;
    uint32_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, c->drm_fd, md.offset);
    if (px == MAP_FAILED) return false;
    // the page colour of a splash with no file: black, the colour the panel is dark in (the compositor knows no scheme)
    vrx_resume_splash_fill(px, cd.pitch / 4, c->width, c->height, SPLASH_DIR, 0x000000, splash_say, NULL);
    munmap(px, cd.size);
    uint32_t handles[4] = { cd.handle }, pitches[4] = { cd.pitch }, offsets[4] = { 0 }, fb = 0;
    if (drmModeAddFB2(c->drm_fd, c->width, c->height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0) != 0) {
        fprintf(stderr, "[vrx-compositor] splash: AddFB2 %s\n", strerror(errno));
        return false;
    }
    c->splash_fb = fb;
    return true;
}

static unsigned inputs_now(const struct vrx_compositor *c) { return (unsigned) (c->touchscreens + c->keyboards); }

// The light is down while parking: the splash goes on the glass and the sleep may go once it is there.
static void resume_dark(struct vrx_compositor *c) {
    if (vrx_resume_dark(&c->resume) != VRX_RESUME_DRAW_SPLASH) return;
    if (!splash_make(c)) {
        fprintf(stderr, "[vrx-compositor] splash: none could be made; the wake shows the last frame\n");
        vrx_resume_abort(&c->resume);
        sleep_release(c);
        return;
    }
    c->splash_on = true;
    c->splash_release = true;
    scene_update(c);                  // the flip that lands it releases the sleep delay (on_page_flip)
}

// The light is down after the essentials: the UI's frame goes back on the glass and the light rises to the person's.
static void resume_dipped(struct vrx_compositor *c) {
    if (vrx_resume_dipped(&c->resume) != VRX_RESUME_SHOW_UI) return;
    c->splash_on = false;
    vrx_screen_light_limit(&c->light, now_ms(), 0, 0);
    c->light_after_flip = true;
    c->resume_deadline_ms = now_ms() + RESUME_WAIT_MS;
    scene_update(c);
    light_timer(c);
}

// The wait's verdict, at an event or the bound.
static void resume_check(struct vrx_compositor *c) {
    if (vrx_resume_check(&c->resume, now_ms()) != VRX_RESUME_DIP) return;
    fprintf(stderr, "[vrx-compositor] resume: the splash held %u ms, the clock %s\n",
            (unsigned) (now_ms() - c->resume.t0), c->resume.clock_unknown ? "unknown" : "known or the wait complete");
    light_go(c, false, VRX_STD_THROUGH_DARK_OUT_MS);
}

static void resume_essentials(struct vrx_compositor *c, bool facts, bool clock) {
    vrx_resume_report(&c->resume, facts, clock);
    resume_check(c);
}

// What was waiting on the light being down, once it is.
static void light_settled(struct vrx_compositor *c) {
    if (!vrx_screen_light_is_dark(&c->light)) return;
    if (c->handoff_wait) {
        // the splash's light is down: the first frame can go out
        c->handoff_wait = false;
        scene_commit(c);
    }
    if (c->power_off_dark == 1 && c->controller >= 0) {
        c->power_off_dark = 2;
        // the light is down for vrxhost's power off: the answer it waits for (the one Power off now gives)
        struct vrx_msg_power_off_answer m = { .now = 1 };
        client_send(c, c->apps[c->controller].fd, VRX_MSG_POWER_OFF_ANSWER, &m, sizeof m, -1);
    }
    if (c->off_pending) {
        c->off_pending = false;
        screen_off_now(c, c->off_why);
    }
    if (c->shutdown_dip) {
        c->shutdown_dip = false;
        sleep_release(c);
    }
    if (c->resume.stage == VRX_RESUME_PARKING) resume_dark(c);
    else if (c->resume.stage == VRX_RESUME_DIPPING) resume_dipped(c);
}

// Lit or dark over `ms` (0 where this screen cannot fade).
static void light_go(struct vrx_compositor *c, bool lit, uint32_t ms) {
    vrx_screen_light_go(&c->light, now_ms(), lit, c->fade_ok ? ms : 0);
    light_timer(c);
    if (!c->light.fade.active) light_settled(c);
}

// Light the screen at the person's brightness (not while it is off or going off).
static void screen_light(struct vrx_compositor *c, uint32_t ms) {
    if (c->dark || c->off_pending) return;
    c->resume_deadline_ms = 0;
    light_go(c, true, ms);
}

static void light_tick(struct vrx_compositor *c) {
    uint64_t expirations;
    if (read(c->light_fd, &expirations, sizeof expirations) != sizeof expirations) return;
    uint32_t t = now_ms();
    vrx_screen_light_tick(&c->light, t);
    if (c->resume.stage == VRX_RESUME_WAITING && vrx_resume_left(&c->resume, t) <= 1) resume_check(c);
    if (c->resume_deadline_ms && (int32_t) (t - c->resume_deadline_ms) >= 0) {
        // no frame came: the screen is as it was, and lit
        screen_light(c, VRX_STD_THROUGH_DARK_IN_MS);
    }
    light_timer(c);
    if (!c->light.fade.active) light_settled(c);
}

// The person's brightness, per mille and perceived (the wire's unit; each UI maps it to its own slider),
// VRX_BRIGHTNESS_CANNOT_DIM where the screen cannot dim.
static int screen_brightness(const struct vrx_compositor *c) {
    return c->dim == DIM_NONE ? VRX_BRIGHTNESS_CANNOT_DIM : c->light.stored;
}

// Tell the controller what the screen is now (vrx plan 0015 D12): its
// facts carry it to Matter. Sent only on a change.
static void screen_report(struct vrx_compositor *c) {
    if (c->controller < 0) return;
    struct vrx_msg_screen m = { .on = !c->dark, .brightness = screen_brightness(c) };
    if (c->reported_screen_valid && m.on == c->reported_screen.on && m.brightness == c->reported_screen.brightness) return;
    c->reported_screen = m;
    c->reported_screen_valid = true;
    client_send(c, c->apps[c->controller].fd, VRX_MSG_SCREEN, &m, sizeof m, -1);
}

static int on_prepare_for_sleep(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int on_prepare_for_shutdown(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static void sleep_delay_take(struct vrx_compositor *c);

static void logind_init(struct vrx_compositor *c) {
    if (sd_bus_open_system(&c->bus) < 0) {
        fprintf(stderr, "[vrx-compositor] no system bus: apps are not told when the system sleeps\n");
        c->bus = NULL;
        return;
    }
    sd_bus_match_signal(c->bus, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager", "PrepareForSleep", on_prepare_for_sleep, c);
    sd_bus_match_signal(c->bus, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager", "PrepareForShutdown", on_prepare_for_shutdown, c);
    sleep_delay_take(c);
}

static void bus_readable(struct vrx_compositor *c) {
    while (sd_bus_process(c->bus, NULL) > 0) {}
}

// SIGUSR1: write the flip ring (plan 0014 D5) and carry on. Anything else stops us;
// the person's brightness is in its file, so nothing is put back first.
static void on_stop(struct vrx_compositor *c) {
    struct signalfd_siginfo si;
    if (read(c->sig_fd, &si, sizeof si) != sizeof si) return;
    if (si.ssi_signo == SIGUSR1) {
        flip_ring_dump("SIGUSR1");
        return;
    }
    fprintf(stderr, "[vrx-compositor] stopping (signal %u)\n", si.ssi_signo);
    c->running = false;
}

// Tell every app whether there is a screen to draw on.
static void display_tell(struct vrx_compositor *c, int only) {
    struct vrx_msg_display m = { .on = !c->dark };
    for (int i = 0; i < MAX_APPS; i++) {
        struct app_client *a = &c->apps[i];
        if (!a->in_use || !a->registered || (only >= 0 && i != only)) continue;
        client_send(c, a->fd, VRX_MSG_DISPLAY, &m, sizeof m, -1);
    }
}

// Light first, then the panel (plan 0033 D1): the light fades down, and
// when it is down the CRTC's ACTIVE property goes to 0 in a commit that
// waits, so a commit still in flight finishes inside it (its event is read
// as usual), and nothing is committed until the screen is back (plan 0010
// D5, plan 0017 D9). A request while the light is coming down changes
// nothing; display_on cancels it.
static void display_off(struct vrx_compositor *c, const char *why) {
    if (c->dark) return;
    snprintf(c->off_why, sizeof c->off_why, "%s", why);
    if (c->off_pending) return;
    c->off_pending = true;
    light_go(c, false, VRX_STD_THROUGH_DARK_OUT_MS);
}

static void screen_off_now(struct vrx_compositor *c, const char *why) {
    if (c->dark) return;
    c->dark = true;
    c->light_after_flip = false;
    c->wake_pending = false;
    // the sequence counter is not meaningful across an off CRTC: the next
    // flip starts the record again instead of counting the dark as drops
    c->last_flip_ns = 0;
    if (c->active_prop && c->mode_set) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (req) {
            kadd(req, c->crtc_id, c->active_prop, 0);
            if (drmModeAtomicCommit(c->drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) != 0) {
                fprintf(stderr, "[vrx-compositor] CRTC off: %s\n", strerror(errno));
            }
            drmModeAtomicFree(req);
        }
    }
    display_tell(c, -1);
    fprintf(stderr, "[vrx-compositor] screen off (%s)\n", why);
    screen_report(c);
}

// The panel and the newest scene in one commit, then the light once that
// commit is up, so the first thing seen is the current screen.
static void display_on(struct vrx_compositor *c, const char *why) {
    if (c->off_pending) {
        // asked on again while the light was coming down: it goes back up, the screen never went off
        c->off_pending = false;
        light_go(c, true, VRX_STD_THROUGH_DARK_IN_MS);
        return;
    }
    if (!c->dark) return;
    c->dark = false;
    display_tell(c, -1);
    if (c->active_prop && c->mode_set) {
        c->wake_pending = true;
        c->scene_dirty = true;
        c->light_after_flip = true;
        scene_commit(c);
    }
    if (!c->commit_pending) {
        c->light_after_flip = false;
        screen_light(c, VRX_STD_THROUGH_DARK_IN_MS);
    }
    fprintf(stderr, "[vrx-compositor] screen on (%s)\n", why);
    screen_report(c);
}

enum { INPUT_TOUCH = 1, INPUT_KEYBOARD = 2 };

// The kinds a device is, for the kit's facts.
static unsigned input_kinds(struct libinput_device *d) {
    struct udev_device *u = libinput_device_get_udev_device(d);
    unsigned k = 0;
    const char *v;
    if (u && (v = udev_device_get_property_value(u, "ID_INPUT_TOUCHSCREEN")) && strcmp(v, "1") == 0) k |= INPUT_TOUCH;
    if (u && (v = udev_device_get_property_value(u, "ID_INPUT_KEYBOARD")) && strcmp(v, "1") == 0) k |= INPUT_KEYBOARD;
    if (u) udev_device_unref(u);
    return k;
}

static void input_facts_changed(struct vrx_compositor *c, struct libinput_device *d, bool added) {
    unsigned kinds = 0;
    if (added) {
        kinds = input_kinds(d);
        if (!kinds) return;
        for (size_t i = 0; i < sizeof c->inputs / sizeof c->inputs[0]; i++) {
            if (!c->inputs[i].dev) {
                c->inputs[i].dev = d;
                c->inputs[i].kinds = kinds;
                break;
            }
        }
    } else {
        for (size_t i = 0; i < sizeof c->inputs / sizeof c->inputs[0]; i++) {
            if (c->inputs[i].dev == d) {
                kinds = c->inputs[i].kinds;
                c->inputs[i].dev = NULL;
                break;
            }
        }
        if (!kinds) return;
    }
    int step = added ? 1 : -1;
    if (kinds & INPUT_TOUCH) c->touchscreens += step;
    if (kinds & INPUT_KEYBOARD) c->keyboards += step;
    fprintf(stderr, "[vrx-compositor] input: %d touchscreen%s, %d keyboard%s\n", c->touchscreens,
            c->touchscreens == 1 ? "" : "s", c->keyboards, c->keyboards == 1 ? "" : "s");
    screen_facts(c);
    vrx_resume_inputs(&c->resume, inputs_now(c));   // the resume's input essential (plan 0034 D3)
    resume_check(c);
}

// The value of `key` in a key=value file; false when absent.
static bool conf_value(const char *path, const char *key, char *out, size_t cap) {
    FILE *f = fopen(path, "re");
    if (!f) return false;
    bool found = false;
    char line[256];
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        char *v = line;
        while (*v == ' ' || *v == '\t') v++;
        if (strncmp(v, key, klen) != 0) continue;
        v += klen;
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '=') continue;
        v++;
        while (*v == ' ' || *v == '\t') v++;
        v[strcspn(v, " \t\r\n#")] = 0;
        snprintf(out, cap, "%s", v);
        found = true;
    }
    fclose(f);
    return found;
}

// What this screen can do, for the kit: whether a brightness slider means
// anything here, and what input is present. What the board's power offers
// (sleep, deep sleep, Power off) is vrxhost's (io.vrx.Host.PowerSettings).
// Written at start and whenever an input device comes or goes.
static void screen_facts(const struct vrx_compositor *c) {
    static const char *names[] = { "none", "backlight", "alpha" };
    char tmp[] = SCREEN_FACTS ".tmp";
    FILE *f = fopen(tmp, "we");
    if (!f) {
        fprintf(stderr, "[vrx-compositor] %s: %s\n", tmp, strerror(errno));
        return;
    }
    fprintf(f, "dim=%s\n", names[c->dim]);
    // input facts (plan 0013 D6, D7): the kit floors touch targets only
    // where a finger can touch, and the on-screen keyboard steps aside
    // where a physical one is plugged in
    fprintf(f, "touch=%d\n", c->touchscreens > 0);
    fprintf(f, "keyboard=%d\n", c->keyboards > 0);
    fclose(f);
    chmod(tmp, 0644);
    if (rename(tmp, SCREEN_FACTS) != 0) fprintf(stderr, "[vrx-compositor] %s: %s\n", SCREEN_FACTS, strerror(errno));
}

static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; props && i < props->count_props && !id; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p && strcmp(p->name, name) == 0) id = p->prop_id;
        if (p) drmModeFreeProperty(p);
    }
    if (props) drmModeFreeObjectProperties(props);
    return id;
}

// The dim plane: an overlay this CRTC can use, other than the shell's,
// with an alpha property, highest in z (above the shell when both say),
// and a black XRGB buffer of DIM_FB_SIZE square the HVS stretches over the
// whole screen. None found: the screen does not dim (dim=none).
static void dim_init(struct vrx_compositor *c) {
    drmModeRes *res = drmModeGetResources(c->drm_fd);
    drmModePlaneRes *planes = drmModeGetPlaneResources(c->drm_fd);
    int crtc_index = -1;
    for (int k = 0; res && k < res->count_crtcs; k++) {
        if (res->crtcs[k] == c->crtc_id) crtc_index = k;
    }
    bool shell_has_z = false;
    uint64_t shell_z = c->shell_plane ? plane_prop(c->drm_fd, c->shell_plane, "zpos", &shell_has_z) : 0;
    uint64_t best_z = 0;
    for (uint32_t i = 0; planes && crtc_index >= 0 && i < planes->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(c->drm_fd, planes->planes[i]);
        if (!p) continue;
        bool has_type, has_z, xrgb = false;
        uint64_t type = plane_prop(c->drm_fd, p->plane_id, "type", &has_type);
        uint64_t z = plane_prop(c->drm_fd, p->plane_id, "zpos", &has_z);
        for (uint32_t f = 0; f < p->count_formats; f++) xrgb |= p->formats[f] == DRM_FORMAT_XRGB8888;
        bool usable = (p->possible_crtcs & (1u << crtc_index)) && has_type && type == DRM_PLANE_TYPE_OVERLAY &&
                      p->plane_id != c->shell_plane && xrgb &&
                      prop_id(c->drm_fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "alpha") &&
                      (!shell_has_z || !has_z || z > shell_z);
        if (usable && (!c->dim_plane || (has_z && z > best_z))) {
            c->dim_plane = p->plane_id;
            best_z = has_z ? z : 0;
        }
        drmModeFreePlane(p);
    }
    if (planes) drmModeFreePlaneResources(planes);
    if (res) drmModeFreeResources(res);
    if (!c->dim_plane) {
        fprintf(stderr, "[vrx-compositor] no overlay plane above the shell's: this screen does not dim\n");
        return;
    }
    c->dim_alpha = prop_id(c->drm_fd, c->dim_plane, DRM_MODE_OBJECT_PLANE, "alpha");
    struct drm_mode_create_dumb cd = { .width = DIM_FB_SIZE, .height = DIM_FB_SIZE, .bpp = 32 };
    struct drm_mode_map_dumb md = { 0 };
    uint32_t *px = MAP_FAILED;
    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0) {
        md.handle = cd.handle;
        if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0) {
            px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, c->drm_fd, md.offset);
        }
    }
    uint32_t handles[4] = { cd.handle }, pitches[4] = { cd.pitch }, offsets[4] = { 0 };
    int r = -1;
    if (px != MAP_FAILED) {
        memset(px, 0, cd.size);  // black: XRGB zero
        munmap(px, cd.size);
        r = drmModeAddFB2(c->drm_fd, DIM_FB_SIZE, DIM_FB_SIZE, DRM_FORMAT_XRGB8888, handles, pitches, offsets,
                          &c->dim_fb, 0);
    }
    if (r != 0) {
        fprintf(stderr, "[vrx-compositor] dim plane %u: no black buffer (%s): this screen does not dim\n",
                c->dim_plane, strerror(errno));
        c->dim_plane = 0;
        c->dim_fb = 0;
        return;
    }
    fprintf(stderr, "[vrx-compositor] dim plane %u (zpos %" PRIu64 ") over the shell's %u\n", c->dim_plane, best_z,
            c->shell_plane);
}

static void light_open(struct vrx_compositor *c) {
    // the backlight's levels against perceived brightness: read from the board (libvrx-light)
    bool mapped = false;
    if (c->dim == DIM_BACKLIGHT) {
        mapped = vrx_light_open(&c->bl_map, c->backlight) == 0;
        if (!mapped) {
            fprintf(stderr, "[vrx-compositor] %s: levels unreadable, taken as linear\n", c->backlight);
            mapped = vrx_light_init_scale(&c->bl_map, VRX_LIGHT_SCALE_LINEAR, c->bl_max) == 0;
        }
    } else if (c->bl_max > 0) {
        // a light that is on or off: on is full, which is what a first start stores
        mapped = vrx_light_init_scale(&c->bl_map, VRX_LIGHT_SCALE_LINEAR, c->bl_max) == 0;
    }
    struct vrx_screen_light_ops ops = { .read_level = light_read_level, .read_stored = light_read_stored,
                                        .write_stored = light_write_stored, .apply = light_apply, .ctx = c };
    vrx_screen_light_open(&c->light, &ops, mapped ? &c->bl_map : NULL, VRX_STD_LIGHT_FLOOR_PERMILLE);
    // a screen fades where it can dim: a backlight always; a dim plane in frames, on the full tier only
    char tier[16] = "";
    FILE *f = fopen(MOTION_TIER_FILE, "re");
    if (f) {
        if (fscanf(f, "%15s", tier) != 1) tier[0] = 0;
        fclose(f);
    }
    c->fade_ok = c->dim == DIM_BACKLIGHT || (c->dim == DIM_ALPHA && strcmp(tier, "full") == 0);
    fprintf(stderr, "[vrx-compositor] light: %u/1000 stored, shown %u, floor %u, fades %s\n", c->light.stored,
            c->light.shown, c->light.floor, c->fade_ok ? "yes" : "no (a cut)");
}

static void screen_init(struct vrx_compositor *c) {
    c->sleep_ack_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    c->light_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    c->sleep_inhibit_fd = -1;
    vrx_resume_init(&c->resume, VRX_STD_RESUME_ESSENTIALS_MAX_MS);
    c->bl_level = -1;
    DIR *d = opendir("/sys/class/backlight");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(c->backlight, sizeof c->backlight, "/sys/class/backlight/%.64s", e->d_name);
            break;
        }
        closedir(d);
    }
    c->bl_max = c->backlight[0] ? sysfs_int(c->backlight, "max_brightness") : 0;
    if (c->bl_max < 0) c->bl_max = 0;
    // (the app's and the shell's planes are made opaque by every scene
    // commit, whatever an earlier compositor left in their alpha properties)
    if (c->bl_max <= 1) dim_init(c);
    c->dim = c->bl_max > 1 ? DIM_BACKLIGHT : c->dim_fb ? DIM_ALPHA : DIM_NONE;
    fprintf(stderr, "[vrx-compositor] screen: light %s (max %d), DPMS %s, dims by %s\n",
            c->backlight[0] ? c->backlight : "none", c->bl_max, c->active_prop ? "yes" : "no",
            c->dim == DIM_BACKLIGHT ? "the backlight" : c->dim == DIM_ALPHA ? "a black plane over the screen" : "nothing");
    screen_facts(c);
    light_open(c);
}

// The handoff (plan 0033 D5): the splash's light comes down before the
// first frame replaces its picture, and goes up after. A screen with no
// dimmable light has no splash picture to hide and skips it.
static void light_handoff_begin(struct vrx_compositor *c) {
    if (c->dim != DIM_BACKLIGHT) return;
    c->handoff_wait = true;
    light_go(c, false, VRX_STD_THROUGH_DARK_OUT_MS);
}

// Before sleep or shutdown: a logind delay inhibitor, so applets get
// PrepareForSleep as lifecycle (paused) and up to a second to save, and the
// light is down before the request goes through (plan 0033 D1); released then.
static void sleep_delay_take(struct vrx_compositor *c) {
    if (!c->bus || c->sleep_inhibit_fd >= 0) return;
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(c->bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", "Inhibit", &err, &reply, "ssss", "sleep:shutdown",
                               "vrx-compositor", "applets save and the light goes down before the system sleeps or stops", "delay");
    int fd = -1;
    if (r >= 0 && sd_bus_message_read(reply, "h", &fd) >= 0) {
        c->sleep_inhibit_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    } else {
        fprintf(stderr, "[vrx-compositor] sleep delay inhibitor: %s\n", err.message ? err.message : strerror(-r));
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
}

static void sleep_tell(struct vrx_compositor *c) {
    struct vrx_msg_sleep m = { .sleeping = c->sleeping };
    for (int i = 0; i < MAX_APPS; i++) {
        struct app_client *a = &c->apps[i];
        if (!a->in_use || !a->registered) continue;
        client_send(c, a->fd, VRX_MSG_SLEEP, &m, sizeof m, -1);
    }
}

static int on_prepare_for_sleep(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void) ret_error;
    struct vrx_compositor *c = userdata;
    int start = 0;
    if (sd_bus_message_read(m, "b", &start) < 0) return 0;
    c->sleeping = start != 0;
    sleep_tell(c);
    if (c->sleeping) {
        fprintf(stderr, "[vrx-compositor] the system is going to sleep: applets told\n");
        struct itimerspec t = { .it_value = { .tv_sec = 1 } };
        if (c->sleep_ack_fd >= 0) timerfd_settime(c->sleep_ack_fd, 0, &t, NULL);
        // the light is down well inside that second, and then the splash goes on the glass (plan 0034 D5)
        if (!c->dark && !c->off_pending && c->mode_set && c->fade_ok) vrx_resume_sleep(&c->resume, inputs_now(c));
        light_go(c, false, VRX_STD_THROUGH_DARK_OUT_MS);
    } else {
        fprintf(stderr, "[vrx-compositor] the system woke\n");
        sleep_delay_take(c);
        if (vrx_resume_wake(&c->resume, now_ms(), inputs_now(c)) == VRX_RESUME_LIGHT_SPLASH) {
            // the splash is up: lit to its own level, held until the essentials are back (resume_check), then through dark
            c->splash_release = false;
            vrx_screen_light_limit(&c->light, now_ms(), VRX_STD_RESUME_SPLASH_LEVEL_PERMILLE, 0);
            screen_light(c, VRX_STD_THROUGH_DARK_IN_MS);
            resume_check(c);
        } else if (!c->dark && !c->off_pending) {
            // resume: the light comes up on the first frame after it, or when none comes
            c->light_after_flip = true;
            c->resume_deadline_ms = now_ms() + RESUME_WAIT_MS;
            light_timer(c);
        }
    }
    return 0;
}

// The system is going down (start) or the shutdown was called off. The light is down before logind
// goes on, and our delay is released the moment it is, not a second later.
static int on_prepare_for_shutdown(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void) ret_error;
    struct vrx_compositor *c = userdata;
    int start = 0;
    if (sd_bus_message_read(m, "b", &start) < 0) return 0;
    if (start) {
        fprintf(stderr, "[vrx-compositor] the system is going down: the light goes first\n");
        c->shutdown_dip = true;
        light_go(c, false, VRX_STD_THROUGH_DARK_OUT_MS);
    } else {
        fprintf(stderr, "[vrx-compositor] the shutdown was called off\n");
        c->shutdown_dip = false;
        sleep_delay_take(c);
        screen_light(c, VRX_STD_THROUGH_DARK_IN_MS);
    }
    return 0;
}

static void sleep_release(struct vrx_compositor *c) {
    if (c->sleep_inhibit_fd >= 0) {
        close(c->sleep_inhibit_fd);
        c->sleep_inhibit_fd = -1;
    }
}

// A second after PrepareForSleep(true): let the sleep go ahead.
static void sleep_ack_fire(struct vrx_compositor *c) {
    uint64_t expirations;
    if (read(c->sleep_ack_fd, &expirations, sizeof expirations) != sizeof expirations) return;
    sleep_release(c);
}

static void send_input(struct vrx_compositor *c, const struct vrx_msg_input *m) {
    int target = input_target(c, m);
    if (target < 0 || !c->apps[target].in_use) return;
    client_send(c, c->apps[target].fd, VRX_MSG_INPUT, m, sizeof *m, -1);
    bool press = (m->kind == VRX_INPUT_TOUCH && m->u.touch.kind == 0) ||
                 (m->kind == VRX_INPUT_KEY && m->u.key.state == 1);
    if (c->profiling && press) {
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
            input_facts_changed(c, d, true);
            break;
        }
        case LIBINPUT_EVENT_DEVICE_REMOVED: {
            struct libinput_device *d = libinput_event_get_device(ev);
            input_facts_changed(c, d, false);
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
        case LIBINPUT_EVENT_TOUCH_CANCEL:
            // the kernel took the touch back: no contact is down any more
            c->primary_touch = -1;
            break;
        case LIBINPUT_EVENT_TOUCH_DOWN:
        case LIBINPUT_EVENT_TOUCH_MOTION:
        case LIBINPUT_EVENT_TOUCH_UP: {
            struct libinput_event_touch *th = libinput_event_get_touch_event(ev);
            uint32_t kind = t == LIBINPUT_EVENT_TOUCH_DOWN ? 0 :
                            t == LIBINPUT_EVENT_TOUCH_MOTION ? 1 : 2;
            // one finger drives the UI (plan 0013 D8): the first contact
            // down is delivered, later ones are held back until it lifts,
            // so two fingers cannot press two controls at once
            int slot = libinput_event_touch_get_slot(th);
            if (kind == 0 && c->primary_touch < 0) c->primary_touch = slot;
            if (slot != c->primary_touch) break;
            if (kind == 2) c->primary_touch = -1;
            struct vrx_msg_input m = { .kind = VRX_INPUT_TOUCH };
            m.u.touch = (struct vrx_input_touch){
                .time_ns = (uint64_t) libinput_event_touch_get_time_usec(th) * 1000,
                .slot = (uint32_t) libinput_event_touch_get_slot(th),
                .kind = kind,
                .x = kind == 2 ? 0 : (int32_t) libinput_event_touch_get_x_transformed(th, c->width),
                .y = kind == 2 ? 0 : (int32_t) libinput_event_touch_get_y_transformed(th, c->height),
            };
            // the glass turned with the panel: its coordinates are the
            // panel's, so they turn back with the picture (a mouse moves in
            // the person's frame and is left alone)
            if (c->upside_down && kind != 2) {
                m.u.touch.x = (int32_t) c->width - 1 - m.u.touch.x;
                m.u.touch.y = (int32_t) c->height - 1 - m.u.touch.y;
            }
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
#define TAG_LIGHT     0xFFFFFFFFFFFFFFF9ull
#define TAG_BUS       0xFFFFFFFFFFFFFFF8ull
#define TAG_SIGNAL    0xFFFFFFFFFFFFFFF6ull
#define TAG_SLEEPACK  0xFFFFFFFFFFFFFFF4ull
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
    const char *vt_path = VRX_VT_DEFAULT;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--compositor")) continue;
        if (i + 1 < argc && !strcmp(argv[i], "--device")) device = argv[++i];
        if (i + 1 < argc && !strcmp(argv[i], "--vt")) vt_path = argv[++i];
    }
    struct vrx_compositor c = {0};
    c.marker_fd = -1;
    c.focused = -1;
    c.controller = -1;
    c.power_off_left = -1;
    c.shell = -1;
    c.pointer_owner = -1;
    for (int i = 0; i < 16; i++) c.touch_owner[i] = -1;
    c.primary_touch = -1;
    struct frame *frames[] = { &c.on_screen, &c.pending, &c.queued, &c.shell_frame, &c.shell_pending,
                               &c.shell_queued };
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        frames[i]->dmabuf_fd = -1;
        frames[i]->app = -1;
    }
    for (int i = 0; i < MAX_APPS; i++) c.apps[i].fd = -1;
    if (kms_init(&c, device) != 0) return 1;
    // GUI first (plan 0028 D2): the panel's VT draws no console and reads no
    // key while the compositor runs; a board with no VT has none to hold
    struct vrx_vt vt = { .fd = -1 };
    if (vrx_vt_hold(&vt, vt_path) != 0) {
        fprintf(stderr, "[vrx-compositor] no VT held (%s): %s\n", vt_path, strerror(errno));
    }
    c.shell_plane = plane_probe(&c, false);
    if (sock_init(&c) != 0) {
        vrx_vt_release(&vt);
        return 1;
    }
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
    c.active_prop = prop_id(c.drm_fd, c.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    screen_init(&c);
    scene_init(&c);
    if (c.light_fd >= 0) {
        ev.data.u64 = TAG_LIGHT;
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.light_fd, &ev);
    }
    if (c.sleep_ack_fd >= 0) {
        ev.data.u64 = TAG_SLEEPACK;
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.sleep_ack_fd, &ev);
    }
    logind_init(&c);
    if (c.bus) {
        ev.data.u64 = TAG_BUS;
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, sd_bus_get_fd(c.bus), &ev);
    }
    // libinput queues a DEVICE_ADDED per device at start without making its
    // fd readable: read them now, or nothing counts as a wake device until
    // the first input arrives
    if (c.li) li_dispatch(&c);
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGUSR1);
    sigprocmask(SIG_BLOCK, &stop, NULL);
    c.sig_fd = signalfd(-1, &stop, SFD_CLOEXEC | SFD_NONBLOCK);
    if (c.sig_fd >= 0) {
        ev.data.u64 = TAG_SIGNAL;
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.sig_fd, &ev);
    }
    c.px = c.width / 2.0;
    c.py = c.height / 2.0;
    cursor_init(&c);

    c.running = true;
    light_handoff_begin(&c);

    fprintf(stderr, "[vrx-compositor] running (socket %s)\n", VRX_UI_SOCKET);
    while (c.running) {
        struct epoll_event out[8];
        // a commit the driver turned away with nothing in flight has no
        // event to retry it: look again in a few ms
        int n = epoll_wait(c.epoll_fd, out, 8, c.scene_retry ? 4 : -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (c.scene_retry && !c.commit_pending) scene_commit(&c);
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
            } else if (tag == TAG_LIGHT) {
                light_tick(&c);
            } else if (tag == TAG_BUS) {
                bus_readable(&c);
            } else if (tag == TAG_SIGNAL) {
                on_stop(&c);
            } else if (tag == TAG_SLEEPACK) {
                sleep_ack_fire(&c);
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
    vrx_vt_release(&vt);
    return 0;
}
