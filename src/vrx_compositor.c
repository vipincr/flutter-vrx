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

#include "vrx_ipc.h"
#include "vrx_kms.h"

// ─── app client ───────────────────────────────────────────────────────────

// connections: the apps plus the controller. vrx runs every built-in
// applet at once (the shell, residents, Home's tiles warmed up: eleven on
// 0.1.0077), and 8 refused the shell's restart on the bench
#define MAX_APPS 32

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

    // screen power (plan 0010): no input for light_off seconds turns the
    // light and the panel off; the input that wakes them only wakes them
    int idle_fd;               // timerfd
    int conf_fd;               // inotify on the screen settings directory
    uint32_t light_off;        // seconds, 0 = never
    int brightness;            // the setting, 0-100, mapped above a floor
    char backlight[96];        // /sys/class/backlight/<name>, "" = none
    int bl_max;                // its max_brightness, 0 = no backlight
    // how brightness is shown: the backlight's levels, or a black plane
    // over everything at partial alpha (DIM_ALPHA), or not at all
    enum { DIM_NONE, DIM_BACKLIGHT, DIM_ALPHA } dim;
    uint32_t dpms_prop;        // the connector's DPMS property, 0 = none
    uint32_t primary_plane;    // the CRTC's primary plane
    // the dim plane (DIM_ALPHA): an overlay above the shell's, scanning
    // out a small black buffer stretched over the CRTC. The app's and the
    // shell's planes stay opaque; dimming them by their own alpha made the
    // shell's drawer see-through over the app (bench, CM4, 2026-10-14).
    uint32_t dim_plane, dim_alpha;  // the plane and its alpha property, 0 = none
    uint32_t dim_fb;           // the black buffer's framebuffer, 0 = none
    bool dim_shown;            // the dim plane is on the CRTC now
    // the panel is mounted upside down (vrx plan 0013 D10): the
    // connector's standard "panel orientation" says so (a carrier sets it
    // on the kernel command line, video=<conn>:panel_orientation=...); the
    // planes scan out turned, touch is turned back, the cursor drawn turned
    bool upside_down;
    bool dark;                 // the screen is off
    struct vrx_msg_screen reported_screen;  // the last VRX_MSG_SCREEN sent
    bool reported_screen_valid;
    bool light_after_flip;     // waking: light once the newest frame is up
    bool was_held;             // an app held the screen on, last we looked
    // holds are logind's idle inhibitors (plan 0010 D3): the pids holding
    // one, honoured for the focused app and the shell only
    sd_bus *bus;
    pid_t inhibitors[32];
    int n_inhibitors;
    struct udev_monitor *bl_mon;  // the backlight's change events
    int bl_level;              // the backlight's level while lit, as set
    int sig_fd;                // SIGTERM / SIGINT stop; SIGUSR1 writes the flip ring
    int wake_devices;          // touch screens, pointers, keyboards present
    // input facts for the kit (plan 0013 D6, D7): touchscreens and
    // keyboards present, by udev's classification; a device's kinds are
    // kept so its removal undoes exactly what its arrival did
    int touchscreens, keyboards;
    struct { struct libinput_device *dev; unsigned kinds; } inputs[32];
    // one finger drives the UI (plan 0013 D8): the touch slot whose
    // contact is delivered, -1 when no contact is down
    int primary_touch;
    // the cascade after the screen (plan 0012 D13): sleep, then deep sleep,
    // counted from the same last input; what this board offers comes from
    // its power profile (sleep, deep, charger-sleep lines)
    int cascade_fd;            // timerfd on CLOCK_BOOTTIME: the next step
    enum { STEP_NONE, STEP_SLEEP, STEP_DEEP } cascade_next;
    uint32_t sleep_after;      // seconds from the last input, 0 = never
    uint32_t deep_after;       // seconds from the last input, 0 = never
    char sleep_state[16];      // the profile's sleep state, "" = none offered
    char deep_action[16];      // the profile's deep sleep, "" = none offered
    bool charger_sleep;        // the profile wants sleep on a charger too
    bool has_wakealarm;        // an RTC that can wake the board on a timer
    bool sleep_blocked;        // a logind `sleep` block inhibitor is held
    bool refusal_logged;       // this dark period's refusal was said once
    int sleep_inhibit_fd;      // our logind `sleep` delay inhibitor, -1
    int sleep_ack_fd;          // timerfd: release the delay after 1 s
    bool sleeping;             // between PrepareForSleep(true) and (false)
    uint32_t light_cap;        // permille above the floor; 1000 = none (D10)
    int touches;               // contacts down now
    bool swallow_touch;        // the waking touch, until every contact is up
    bool swallow_pointer;      // the waking click, until its buttons are up
    uint32_t swallow_key;      // the waking key, until it is released

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
static uint64_t mode_period_ns(const drmModeModeInfo *m);
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

// Turn the planes this compositor scans out with by 180 degrees, once
// the CRTC is lit: the panel is mounted upside down. A quarter turn is
// refused loudly: vc4's planes offer only 0 and 180 (and the
// reflections), and drawing sideways content unrotated beats a wrong
// guess at the geometry.
static void planes_rotate(struct vrx_compositor *c) {
    if (!c->upside_down) return;
    const uint32_t planes[] = { c->primary_plane, c->shell_plane };
    for (size_t i = 0; i < sizeof planes / sizeof planes[0]; i++) {
        if (!planes[i]) continue;
        bool offers;
        uint32_t prop = plane_rotation(c->drm_fd, planes[i], DRM_MODE_ROTATE_180, &offers);
        if (!prop || !offers) {
            fprintf(stderr, "[vrx-compositor] plane %u cannot turn 180 degrees: the panel stays upside down\n", planes[i]);
            continue;
        }
        if (drmModeObjectSetProperty(c->drm_fd, planes[i], DRM_MODE_OBJECT_PLANE, prop, DRM_MODE_ROTATE_180) != 0) {
            fprintf(stderr, "[vrx-compositor] plane %u rotate-180: %s\n", planes[i], strerror(errno));
        }
    }
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

static void shell_show(struct vrx_compositor *c);
static void screen_light(struct vrx_compositor *c);
static void screen_report(struct vrx_compositor *c);
static void display_on(struct vrx_compositor *c, const char *why);
static void display_off(struct vrx_compositor *c, const char *why);
static void display_tell(struct vrx_compositor *c, int only);
static void hold_changed(struct vrx_compositor *c, const char *why);

// Ask for the queued frame to be flipped in, if the flip slot is free and
// the screen is on (a flip on a CRTC that is off is refused; the newest
// frame waits in `queued` for the wake, plan 0010 D5).
static void flip_queued(struct vrx_compositor *c) {
    if (!c->queued.valid || c->pending.valid || c->dark) return;
    c->queued.commit_ns = now_ns();
    if (!c->mode_set) {
        // the very first frame: one modeset, after which everything flips
        if (drmModeSetCrtc(c->drm_fd, c->crtc_id, c->queued.fb_id, 0, 0,
                           &c->connector_id, 1, &c->mode) != 0) {
            fprintf(stderr, "[vrx-compositor] drmModeSetCrtc: %s\n", strerror(errno));
            frame_release(c, &c->queued);
            return;
        }
        c->mode_set = true;
        planes_rotate(c);
        screen_light(c);
        cursor_update(c);
        if (getenv("VRX_PROBE_OVERLAY")) plane_probe_bar(c);
        shell_show(c);
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
                char path[256];
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
    char id[23];
};
static struct flip_entry flip_ring[FLIP_RING_LEN];
static uint32_t flip_ring_next, flip_ring_count;

static void flip_ring_push(const struct frame *f, const char *id, uint32_t seq, uint32_t delta,
                           uint32_t skipped, enum flip_cause cause, uint64_t flip_ns) {
    struct flip_entry *e = &flip_ring[flip_ring_next];
    e->flip_ns = flip_ns;
    e->recv_ns = f->recv_ns;
    e->commit_ns = f->commit_ns;
    e->seq = seq;
    e->delta = delta;
    e->skipped = skipped;
    e->cause = (uint8_t) cause;
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
    fprintf(out, "# flip_ns\tseq\tdelta\tskipped\tapp\tarrival_to_screen_us\tcommit_to_screen_us\tcause\n");
    unsigned written = 0;
    for (uint32_t i = 0; i < flip_ring_count; i++) {
        const struct flip_entry *e = &flip_ring[(first + i) % FLIP_RING_LEN];
        if (newest > FLIP_RING_NS && e->flip_ns < newest - FLIP_RING_NS) continue;
        fprintf(out, "%" PRIu64 "\t%u\t%u\t%u\t%s\t%" PRIu64 "\t%" PRIu64 "\t%s\n", e->flip_ns, e->seq, e->delta,
                e->skipped, e->id, e->recv_ns && e->flip_ns > e->recv_ns ? (e->flip_ns - e->recv_ns) / 1000 : 0,
                e->commit_ns && e->flip_ns > e->commit_ns ? (e->flip_ns - e->commit_ns) / 1000 : 0,
                flip_cause_name[e->cause]);
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
static void flip_record(struct vrx_compositor *c, const struct frame *f, uint32_t seq, uint64_t flip_ns) {
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
    flip_ring_push(f, id, seq, delta, skipped_ring, cause_ring, flip_ns);
    c->last_flip_seq = seq;
    c->last_flip_ns = flip_ns;
}

// DRM says the requested flip happened: the old screen buffer is free.
static void on_page_flip(int fd, unsigned int seq, unsigned int sec,
                         unsigned int usec, void *data) {
    (void) fd;
    struct vrx_compositor *c = data;
    // the event's timestamp is the vblank's, when the driver stamps with
    // CLOCK_MONOTONIC; otherwise now is the nearest honest time
    uint64_t flip_ns = c->flip_ts_monotonic ? (uint64_t) sec * 1000000000ull + (uint64_t) usec * 1000ull : now_ns();
    flip_record(c, &c->pending, seq, flip_ns);
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
    if (c->light_after_flip) {
        // waking: the current screen is up, now it can be seen
        c->light_after_flip = false;
        screen_light(c);
    }
    send_vblank(c, c->on_screen.app, false);
    send_vblank(c, c->shell, false);
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

// Put the shell's current frame on its plane (once the CRTC is lit), or
// take the plane off when there is none or the focused app has the whole
// screen.
static void shell_show(struct vrx_compositor *c) {
    if (!c->shell_plane || !c->mode_set) return;
    struct frame *f = &c->shell_frame;
    if (!f->valid || !chrome_shown(c)) {
        drmModeSetPlane(c->drm_fd, c->shell_plane, c->crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        return;
    }
    if (drmModeSetPlane(c->drm_fd, c->shell_plane, c->crtc_id, f->fb_id, 0, 0, 0, c->width, c->height, 0, 0,
                        c->width << 16, c->height << 16) != 0) {
        fprintf(stderr, "[vrx-compositor] shell plane %u: %s\n", c->shell_plane, strerror(errno));
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
        frame_release(c, &c->shell_frame);
        shell_show(c);
    } else {
        fprintf(stderr, "[vrx-compositor] shell -> slot %d ('%s', pid %d), plane %u\n", slot, c->apps[slot].id,
                (int) c->apps[slot].pid, c->shell_plane);
    }
    send_insets(c, -1);
    hold_changed(c, "shell changed");
}

static void focus_slot(struct vrx_compositor *c, int slot) {
    if (slot == c->focused) return;
    int old = c->focused;
    c->focused = slot;
    // the shell steps aside for an app that has the whole screen
    shell_show(c);
    // a key held into a focus change stops repeating into the new app
    repeat_arm(c, 0);
    if (old >= 0 && c->apps[old].in_use) {
        client_send(c, c->apps[old].fd, VRX_MSG_FOCUS,
                    &(struct vrx_msg_focus){ .focused = 0 }, sizeof(struct vrx_msg_focus), -1);
    }
    // a frame from the old focus still waiting for the flip slot never shows
    frame_release(c, &c->queued);
    // a hold counts only while its app is in front
    hold_changed(c, "focus changed");
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
    struct frame *frames[] = { &c->on_screen, &c->pending, &c->queued };
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        if (frames[i]->valid && frames[i]->app == slot) frames[i]->app = -1;
    }
    memset(a, 0, sizeof *a);
    a->fd = -1;
    // what comes next on screen is vrxhost's decision (it sees the exit
    // both here and as its child's status)
    if (c->focused == slot) {
        c->focused = -1;
        shell_show(c);
    }
    if (c->shell_frame.valid && c->shell_frame.app == slot) c->shell_frame.app = -1;
    if (c->shell == slot) {
        // its plane goes dark and apps may use the whole screen again
        c->shell = -1;
        frame_release(c, &c->shell_frame);
        shell_show(c);
        send_insets(c, -1);
    }
    for (int i = 0; i < 16; i++) {
        if (c->touch_owner[i] == slot) c->touch_owner[i] = -1;
    }
    if (c->pointer_owner == slot) c->pointer_owner = -1;
    // its inhibitor dies with the process (logind's fd); until logind says
    // so, a gone app's pid matches nothing focused
    hold_changed(c, "app gone");
}

// Handle one message. `fd` is the descriptor that rode it, or -1; this
// function takes ownership of it.
static void client_handle(struct vrx_compositor *c, int slot,
                          const struct vrx_msg_header *h, void *payload, int fd) {
    struct app_client *a = &c->apps[slot];
    // a connection is an app (REGISTER) or the controller (CONTROL), once
    bool app_msg = h->type == VRX_MSG_BUFFER || h->type == VRX_MSG_CLOSED || h->type == VRX_MSG_CHROME ||
                   h->type == VRX_MSG_SEMANTICS_TREE;
    if ((app_msg && !a->registered) ||
        ((h->type == VRX_MSG_SET_FOCUS || h->type == VRX_MSG_SET_SHELL || h->type == VRX_MSG_LIGHT_CAP ||
          h->type == VRX_MSG_PROFILING || h->type == VRX_MSG_SEMANTICS || h->type == VRX_MSG_SCREEN) &&
         !a->controller) ||
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
            if (m->on) {
                display_on(c, "matter");
            } else if (c->wake_devices > 0) {
                display_off(c, "matter");
            } else {
                // the idle timeout's rule (plan 0010 D10): never dark with
                // nothing a person can wake it with
                fprintf(stderr, "[vrx-compositor] screen off refused (matter): nothing can wake it\n");
            }
            // the answer either way: what the screen is now
            screen_report(c);
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
            if (cap != c->light_cap) {
                fprintf(stderr, "[vrx-compositor] brightness cap %u/1000 above the floor%s\n", cap,
                        cap == 1000 ? " (none)" : " (the battery is low)");
                c->light_cap = cap;
                screen_light(c);
            }
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
            if (slot == c->focused) shell_show(c);
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
                // the shell's plane: a legacy SetPlane on an atomic driver
                // commits synchronously, so the frame it replaces is off
                // screen when it returns and goes straight back
                if (!c->shell_plane || !frame_import(c, &f, b)) {
                    frame_release(c, &f);
                    break;
                }
                struct frame old = c->shell_frame;
                c->shell_frame = f;
                shell_show(c);
                frame_release(c, &old);
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

// ── screen power (plan 0010) ────────────────────────────────────────────
// The compositor alone turns the screen off and on (D1): after light_off
// seconds without input the light goes to 0 and the connector to DPMS off;
// the next touch, click or key brings both back and is not delivered. No
// setting means "off": brightness is a position 0-100 that maps into a
// floor-to-full range, so a stored 0 is still a screen a person can read
// (the owner, 2026-10-14). Linux first (the owner, same day): a hold is a
// logind idle inhibitor, a dimmable backlight's level is its own sysfs
// value (persisted by systemd-backlight, written by applets holding the
// display grant), and this file holds only what Linux has no home for:
// the timeout, and the brightness of a screen dimmed by plane alpha.
#define SCREEN_DIR "/etc/vrx/display"
#define SCREEN_FILE SCREEN_DIR "/screen.conf"
#define SCREEN_FACTS VRX_UI_SOCKET_DIR "/screen"
#define LIGHT_OFF_DEFAULT 120
// the floors (D8): a tenth of a dimmable backlight; with a dim plane, the
// screen at 0.4 of its full level (the black plane at alpha 0.6), about
// 13 % of full luminance, since alpha scales encoded values
#define BACKLIGHT_FLOOR 0.1
#define DIM_LEVEL_FLOOR 0.4
// the dim plane's black buffer, stretched over the whole CRTC
#define DIM_FB_SIZE 64

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

static void backlight_set(const struct vrx_compositor *c, int level) {
    if (!c->backlight[0]) return;
    char path[128];
    snprintf(path, sizeof path, "%s/brightness", c->backlight);
    FILE *f = fopen(path, "we");
    if (!f) {
        fprintf(stderr, "[vrx-compositor] %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "%d\n", level);
    fclose(f);
}

// The lowest level a dimmable backlight may sit at while lit.
static int backlight_floor(const struct vrx_compositor *c) {
    int f = (int) (c->bl_max * BACKLIGHT_FLOOR + 0.5);
    return f < 1 ? 1 : f;
}

// The light's level while lit: full on a two-level light; on a dimmable
// one, its own level as last set, never below the floor.
static int light_level(const struct vrx_compositor *c) {
    if (c->dim != DIM_BACKLIGHT) return c->bl_max;
    int level = c->bl_level < backlight_floor(c) ? backlight_floor(c) : c->bl_level;
    // the battery's cap (D10): a share of the range above the floor
    int cap = backlight_floor(c) + (int) ((c->bl_max - backlight_floor(c)) * c->light_cap / 1000.0 + 0.5);
    return level > cap ? cap : level;
}

// The dim plane's alpha for the set brightness (and the battery's cap):
// 0 at full, 1 - DIM_LEVEL_FLOOR at the floor.
static uint64_t dim_alpha(const struct vrx_compositor *c) {
    double share = c->brightness / 100.0, cap = c->light_cap / 1000.0;
    if (share > cap) share = cap;
    if (share < 0) share = 0;
    double level = DIM_LEVEL_FLOOR + (1 - DIM_LEVEL_FLOOR) * share;
    return (uint64_t) (0xffff * (1 - level) + 0.5);
}

// Show the dim plane at `alpha` over the whole CRTC, or take it off at 0.
// A plane that refuses (no scaling, a commit error) leaves the screen
// undimmed and says so once: dim=none, never a per-plane alpha again.
static void dim_show(struct vrx_compositor *c, uint64_t alpha) {
    if (!c->dim_plane || !c->mode_set) return;
    if (alpha == 0) {
        if (c->dim_shown) drmModeSetPlane(c->drm_fd, c->dim_plane, c->crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        c->dim_shown = false;
        return;
    }
    if (drmModeObjectSetProperty(c->drm_fd, c->dim_plane, DRM_MODE_OBJECT_PLANE, c->dim_alpha, alpha) != 0 ||
        drmModeSetPlane(c->drm_fd, c->dim_plane, c->crtc_id, c->dim_fb, 0, 0, 0, c->width, c->height,
                        0, 0, DIM_FB_SIZE << 16, DIM_FB_SIZE << 16) != 0) {
        fprintf(stderr, "[vrx-compositor] dim plane %u refused (%s): this screen does not dim\n", c->dim_plane,
                strerror(errno));
        drmModeSetPlane(c->drm_fd, c->dim_plane, c->crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        c->dim_shown = false;
        c->dim_plane = 0;
        c->dim = DIM_NONE;
        screen_facts(c);
        return;
    }
    c->dim_shown = true;
}

// Light the screen at the set brightness: the backlight, and on a light
// that cannot dim, the black dim plane over everything.
static void screen_light(struct vrx_compositor *c) {
    if (c->dark) return;
    if (c->dim == DIM_ALPHA) dim_show(c, dim_alpha(c));
    backlight_set(c, light_level(c));
}

// The brightness as a position (0-100) the way the kit's slider reads it,
// -1 where the screen cannot dim.
static int screen_position(const struct vrx_compositor *c) {
    if (c->dim == DIM_BACKLIGHT) return c->bl_max > 0 ? (100 * c->bl_level + c->bl_max / 2) / c->bl_max : -1;
    if (c->dim == DIM_ALPHA) return c->brightness < 0 ? 100 : c->brightness;
    return -1;
}

// Tell the controller what the screen is now (vrx plan 0015 D12): its
// facts carry it to Matter. Sent only on a change.
static void screen_report(struct vrx_compositor *c) {
    if (c->controller < 0) return;
    struct vrx_msg_screen m = { .on = !c->dark, .position = screen_position(c) };
    if (c->reported_screen_valid && m.on == c->reported_screen.on && m.position == c->reported_screen.position) return;
    c->reported_screen = m;
    c->reported_screen_valid = true;
    client_send(c, c->apps[c->controller].fd, VRX_MSG_SCREEN, &m, sizeof m, -1);
}

static int on_prepare_for_sleep(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static void sleep_delay_take(struct vrx_compositor *c);
static void cascade_arm(struct vrx_compositor *c);
static void cascade_disarm(struct vrx_compositor *c);
static bool sleep_wake_source(void);
static void profile_read(struct vrx_compositor *c);

// An idle inhibitor is held by the focused app or the shell. One held by
// an app in the background counts for nothing, the open-book lesson
// (evrx-rt docs/20): the screen is held for what is being watched.
static bool held(const struct vrx_compositor *c) {
    for (int k = 0; k < c->n_inhibitors; k++) {
        if (c->focused >= 0 && c->apps[c->focused].pid == c->inhibitors[k]) return true;
        if (c->shell >= 0 && c->apps[c->shell].pid == c->inhibitors[k]) return true;
    }
    return false;
}

// logind's idle inhibitors, as pids. Read at start and whenever logind
// says BlockInhibited changed (it does, on every take and release).
static void inhibitors_read(struct vrx_compositor *c) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    c->n_inhibitors = 0;
    c->sleep_blocked = false;
    int r = sd_bus_call_method(c->bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", "ListInhibitors", &err, &reply, "");
    if (r < 0) {
        fprintf(stderr, "[vrx-compositor] ListInhibitors: %s\n", err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return;
    }
    if (sd_bus_message_enter_container(reply, 'a', "(ssssuu)") >= 0) {
        const char *what, *who, *why, *mode;
        uint32_t uid, pid;
        while (sd_bus_message_read(reply, "(ssssuu)", &what, &who, &why, &mode, &uid, &pid) > 0) {
            if (strstr(what, "idle") && strcmp(mode, "block") == 0 && c->n_inhibitors < 32) {
                c->inhibitors[c->n_inhibitors++] = (pid_t) pid;
                fprintf(stderr, "[vrx-compositor] idle inhibitor: %s (pid %u): %s\n", who, pid, why);
            }
            // a recording, a transfer, a model answering: sleep waits
            // (logind lets root ignore a block, so the cascade checks it)
            if (strstr(what, "sleep") && strcmp(mode, "block") == 0) {
                c->sleep_blocked = true;
                fprintf(stderr, "[vrx-compositor] sleep inhibitor: %s (pid %u): %s\n", who, pid, why);
            }
        }
    }
    sd_bus_message_unref(reply);
}

static int on_logind_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void) ret_error;
    struct vrx_compositor *c = userdata;
    const char *iface;
    if (sd_bus_message_read(m, "s", &iface) < 0) return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
    bool changed = false;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *name;
        if (sd_bus_message_read(m, "s", &name) < 0) break;
        if (strcmp(name, "BlockInhibited") == 0) changed = true;
        sd_bus_message_skip(m, "v");
        sd_bus_message_exit_container(m);
    }
    if (changed) {
        inhibitors_read(c);
        hold_changed(c, "logind inhibitors changed");
    }
    return 0;
}

static void logind_init(struct vrx_compositor *c) {
    if (sd_bus_open_system(&c->bus) < 0) {
        fprintf(stderr, "[vrx-compositor] no system bus: nothing can hold the screen on\n");
        c->bus = NULL;
        return;
    }
    sd_bus_match_signal(c->bus, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
                        "org.freedesktop.DBus.Properties", "PropertiesChanged", on_logind_changed, c);
    sd_bus_match_signal(c->bus, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager", "PrepareForSleep", on_prepare_for_sleep, c);
    inhibitors_read(c);
    sleep_delay_take(c);
}

static void bus_readable(struct vrx_compositor *c) {
    while (sd_bus_process(c->bus, NULL) > 0) {}
}

// The backlight changed (an applet with the display grant set a level,
// through the kit): that is the brightness now. Below the floor it is put
// back up; while the screen is off it is remembered for the wake.
static void backlight_changed(struct vrx_compositor *c) {
    struct udev_device *d;
    while ((d = udev_monitor_receive_device(c->bl_mon))) udev_device_unref(d);
    if (c->dim != DIM_BACKLIGHT) return;
    int level = sysfs_int(c->backlight, "brightness");
    if (level < 0) return;
    if (c->dark) {
        if (level > 0) {
            c->bl_level = level;
            backlight_set(c, 0);
        }
        return;
    }
    if (level == c->bl_level && level >= backlight_floor(c)) return;
    c->bl_level = level;
    if (level < backlight_floor(c)) {
        fprintf(stderr, "[vrx-compositor] backlight %d is below the floor; %d\n", level, backlight_floor(c));
        backlight_set(c, backlight_floor(c));
        c->bl_level = backlight_floor(c);
    }
    screen_report(c);
}

// Stopping (shutdown, a restart): the light goes back to its level first,
// so systemd-backlight saves the person's brightness, not the idle 0.
// SIGUSR1: write the flip ring (plan 0014 D5) and carry on.
static void on_stop(struct vrx_compositor *c) {
    struct signalfd_siginfo si;
    if (read(c->sig_fd, &si, sizeof si) != sizeof si) return;
    if (si.ssi_signo == SIGUSR1) {
        flip_ring_dump("SIGUSR1");
        return;
    }
    if (c->dark) backlight_set(c, light_level(c));
    fprintf(stderr, "[vrx-compositor] stopping (signal %u)\n", si.ssi_signo);
    c->running = false;
}

// The timer runs only while the screen is on, nothing holds it, the
// setting allows it, and something can wake the screen again (D10).
static void idle_arm(struct vrx_compositor *c) {
    if (c->idle_fd < 0) return;
    bool armed = c->light_off && !c->dark && !held(c) && c->wake_devices > 0;
    struct itimerspec t = { .it_value = { .tv_sec = armed ? c->light_off : 0 } };
    timerfd_settime(c->idle_fd, 0, &t, NULL);
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

// Light first, then the panel; a flip still in flight finishes inside the
// blocking DPMS commit, and nothing flips until the screen is back (D5).
static void display_off(struct vrx_compositor *c, const char *why) {
    if (c->dark) return;
    c->dark = true;
    c->light_after_flip = false;
    // the sequence counter is not meaningful across an off CRTC: the next
    // flip starts the record again instead of counting the dark as drops
    c->last_flip_ns = 0;
    backlight_set(c, 0);
    if (c->dpms_prop && c->mode_set &&
        drmModeConnectorSetProperty(c->drm_fd, c->connector_id, c->dpms_prop, DRM_MODE_DPMS_OFF) != 0) {
        fprintf(stderr, "[vrx-compositor] DPMS off: %s\n", strerror(errno));
    }
    display_tell(c, -1);
    idle_arm(c);
    fprintf(stderr, "[vrx-compositor] screen off (%s)\n", why);
    c->refusal_logged = false;
    cascade_arm(c);
    screen_report(c);
}

// The panel first, then the newest frame, then the light once that frame
// is up, so the first thing seen is the current screen.
static void display_on(struct vrx_compositor *c, const char *why) {
    if (!c->dark) return;
    c->dark = false;
    if (c->dpms_prop && c->mode_set &&
        drmModeConnectorSetProperty(c->drm_fd, c->connector_id, c->dpms_prop, DRM_MODE_DPMS_ON) != 0) {
        fprintf(stderr, "[vrx-compositor] DPMS on: %s\n", strerror(errno));
    }
    display_tell(c, -1);
    if (c->queued.valid && !c->pending.valid) {
        c->light_after_flip = true;
        flip_queued(c);
        if (!c->pending.valid) c->light_after_flip = false;
    }
    if (!c->light_after_flip) screen_light(c);
    idle_arm(c);
    cascade_disarm(c);
    fprintf(stderr, "[vrx-compositor] screen on (%s)\n", why);
    screen_report(c);
}

// A hold that starts counting wakes the screen; one that stops counting
// restarts the full timeout rather than ending the screen at once.
static void hold_changed(struct vrx_compositor *c, const char *why) {
    bool h = held(c);
    if (h == c->was_held) return;
    c->was_held = h;
    fprintf(stderr, "[vrx-compositor] screen %s (%s)\n", h ? "held on" : "no longer held", why);
    if (h) display_on(c, why);
    idle_arm(c);
}

// A device a person wakes the screen with, as udev classifies it: a
// touchscreen, keyboard, mouse, touchpad or tablet. vc4's HDMI CEC inputs
// are present with or without a TV and udev tags them only as keys and a
// pointing stick, so they do not count (bench, CM4, 2026-10-14).
static bool can_wake(struct libinput_device *d) {
    static const char *kinds[] = { "ID_INPUT_TOUCHSCREEN", "ID_INPUT_KEYBOARD", "ID_INPUT_MOUSE",
                                   "ID_INPUT_TOUCHPAD", "ID_INPUT_TABLET" };
    struct udev_device *u = libinput_device_get_udev_device(d);
    bool yes = false;
    for (size_t i = 0; u && i < sizeof kinds / sizeof kinds[0] && !yes; i++) {
        const char *v = udev_device_get_property_value(u, kinds[i]);
        yes = v && strcmp(v, "1") == 0;
    }
    if (u) udev_device_unref(u);
    return yes;
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
}

static void wake_devices_changed(struct vrx_compositor *c, int delta, const char *name) {
    c->wake_devices += delta;
    fprintf(stderr, "[vrx-compositor] %s %s: %d device%s can wake the screen%s\n", name,
            delta > 0 ? "added" : "removed", c->wake_devices, c->wake_devices == 1 ? "" : "s",
            c->wake_devices ? "" : "; it stays on");
    if (!c->wake_devices) display_on(c, "nothing left to wake it");
    idle_arm(c);
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

// A whole number, or `fallback` for anything else (garbage is logged).
static int conf_int(const char *key, int fallback) {
    char v[32];
    if (!conf_value(SCREEN_FILE, key, v, sizeof v)) return fallback;
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end) {
        fprintf(stderr, "[vrx-compositor] %s: %s=%s is not a number; using %d\n", SCREEN_FILE, key, v, fallback);
        return fallback;
    }
    return n < 0 ? 0 : n > 86400 ? 86400 : (int) n;
}

static void screen_read_conf(struct vrx_compositor *c) {
    int light_off = conf_int("light_off", LIGHT_OFF_DEFAULT);
    // the cascade's later steps (plan 0012 D13); 0 or absent: never. The
    // Settings rows keep light_off <= sleep_after <= deep_after; a step
    // set earlier than the one before it is taken as never.
    c->sleep_after = (uint32_t) conf_int("sleep_after", 0);
    c->deep_after = (uint32_t) conf_int("deep_after", 0);
    // a dimmable backlight keeps its own level (sysfs, systemd-backlight);
    // the file's brightness is only for a screen dimmed by plane alpha
    int brightness = c->dim == DIM_BACKLIGHT ? 100 : conf_int("brightness", 100);
    if (brightness > 100) brightness = 100;
    if ((uint32_t) light_off != c->light_off) {
        fprintf(stderr, "[vrx-compositor] screen off after %ds idle%s\n", light_off, light_off ? "" : " (never)");
        c->light_off = (uint32_t) light_off;
    }
    if (brightness != c->brightness) {
        if (c->dim != DIM_BACKLIGHT) {
            fprintf(stderr, "[vrx-compositor] brightness %d%s\n", brightness, c->dim == DIM_NONE ? " (this screen cannot dim)" : "");
        }
        c->brightness = brightness;
        screen_light(c);
        screen_report(c);
    }
    idle_arm(c);
}

// What this screen can do, for the kit: whether a brightness slider means
// anything here, which cascade steps exist, and what input is present.
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
    // the cascade's offered steps (plan 0012 D13), for Settings' rows
    fprintf(f, "sleep=%s\n", c->sleep_state[0] && sleep_wake_source() ? c->sleep_state : "none");
    fprintf(f, "deep=%s\n", c->deep_action[0] ? c->deep_action : "none");
    fprintf(f, "wakealarm=%s\n", c->has_wakealarm ? "yes" : "no");
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

static void screen_init(struct vrx_compositor *c) {
    c->idle_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    c->cascade_fd = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK);
    c->sleep_ack_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    c->sleep_inhibit_fd = -1;
    c->light_cap = 1000;
    profile_read(c);
    DIR *d = opendir("/sys/class/backlight");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(c->backlight, sizeof c->backlight, "/sys/class/backlight/%s", e->d_name);
            break;
        }
        closedir(d);
    }
    c->bl_max = c->backlight[0] ? sysfs_int(c->backlight, "max_brightness") : 0;
    if (c->bl_max < 0) c->bl_max = 0;
    c->dpms_prop = prop_id(c->drm_fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "DPMS");
    // the app's and the shell's planes are opaque, whatever an earlier
    // compositor left in their alpha properties
    const uint32_t opaque[] = { c->primary_plane, c->shell_plane };
    for (size_t i = 0; i < sizeof opaque / sizeof opaque[0]; i++) {
        uint32_t a = opaque[i] ? prop_id(c->drm_fd, opaque[i], DRM_MODE_OBJECT_PLANE, "alpha") : 0;
        if (a) drmModeObjectSetProperty(c->drm_fd, opaque[i], DRM_MODE_OBJECT_PLANE, a, 0xffff);
    }
    if (c->bl_max <= 1) dim_init(c);
    c->dim = c->bl_max > 1 ? DIM_BACKLIGHT : c->dim_fb ? DIM_ALPHA : DIM_NONE;
    // the level systemd-backlight restored at boot is the brightness
    c->bl_level = c->backlight[0] ? sysfs_int(c->backlight, "brightness") : 0;
    if (c->dim == DIM_BACKLIGHT) {
        struct udev *udev = udev_new();
        c->bl_mon = udev ? udev_monitor_new_from_netlink(udev, "udev") : NULL;
        if (c->bl_mon) {
            udev_monitor_filter_add_match_subsystem_devtype(c->bl_mon, "backlight", NULL);
            udev_monitor_enable_receiving(c->bl_mon);
        }
    }
    fprintf(stderr, "[vrx-compositor] screen: light %s (max %d), DPMS %s, dims by %s\n",
            c->backlight[0] ? c->backlight : "none", c->bl_max, c->dpms_prop ? "yes" : "no",
            c->dim == DIM_BACKLIGHT ? "the backlight" : c->dim == DIM_ALPHA ? "a black plane over the screen" : "nothing");
    screen_facts(c);
    // the directory, not the file: a writer renames a new file over it
    c->conf_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (c->conf_fd >= 0 && inotify_add_watch(c->conf_fd, SCREEN_DIR, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        fprintf(stderr, "[vrx-compositor] watch %s: %s\n", SCREEN_DIR, strerror(errno));
    }
    c->brightness = -1;  // logged and applied on the first read
    screen_read_conf(c);
}

static void screen_conf_changed(struct vrx_compositor *c) {
    char buf[4096];
    while (read(c->conf_fd, buf, sizeof buf) > 0) {}
    screen_read_conf(c);
}

static void idle_fire(struct vrx_compositor *c) {
    uint64_t expirations;
    if (read(c->idle_fd, &expirations, sizeof expirations) != sizeof expirations) return;
    if (c->dark || !c->light_off || held(c) || !c->wake_devices) return;
    display_off(c, "idle");
}

// Every input re-arms the timer. On a dark screen the first touch, click
// or key turns it on and is not delivered, nor is the rest of it (a touch
// until every contact is up, a click until its buttons are, a key until
// its release); motion wakes and goes through.
static bool idle_swallows(struct vrx_compositor *c, const struct vrx_msg_input *m) {
    if (m->kind == VRX_INPUT_TOUCH) {
        if (m->u.touch.kind == 0) c->touches++;
        if (m->u.touch.kind == 2 && c->touches > 0) c->touches--;
    }
    if (c->dark) {
        display_on(c, "input");
        if (m->kind == VRX_INPUT_TOUCH) c->swallow_touch = true;
        if (m->kind == VRX_INPUT_POINTER && m->u.pointer.buttons) c->swallow_pointer = true;
        if (m->kind == VRX_INPUT_KEY && m->u.key.state == 1) c->swallow_key = m->u.key.keycode;
    }
    idle_arm(c);
    switch (m->kind) {
    case VRX_INPUT_TOUCH:
        if (!c->swallow_touch) return false;
        if (c->touches == 0) c->swallow_touch = false;
        return true;
    case VRX_INPUT_POINTER:
        if (!c->swallow_pointer) return false;
        if (m->u.pointer.buttons == 0) c->swallow_pointer = false;
        return true;
    case VRX_INPUT_KEY:
        if (!c->swallow_key || m->u.key.keycode != c->swallow_key) return false;
        if (m->u.key.state == 0) c->swallow_key = 0;
        return true;
    default:
        return false;
    }
}


// ── the cascade: sleep and deep sleep (plan 0012 D13, D14) ──────────────
// After the screen goes off the compositor takes the next steps from the
// same last input: sleep (logind Suspend) after sleep_after, deep sleep
// (logind Halt into the SoC's standby) after deep_after, where the board
// offers them. What a board offers is a fact of its power profile, one
// file with the measured numbers (vrx-power-tuning installs it):
//   sleep <state> saving=<pct> measured=<date> meter=<name>
//   deep halt
//   charger-sleep yes
// The build refuses a sleep line saving less than the owner's threshold
// (10 % of the idle floor) or without its measurement; here a missing
// line means the step is not offered. Linux does the rest: systemd-sleep
// writes the state, logind tells everyone through PrepareForSleep.
#define PROFILE_FILE "/usr/share/vrx/power-profile"
#define PROFILE_DIR "/usr/share/vrx/power-profile.d"

static void profile_line(struct vrx_compositor *c, const char *line) {
    char kind[16] = "", value[16] = "";
    if (sscanf(line, " %15s %15s", kind, value) < 2) return;
    if (strcmp(kind, "sleep") == 0 && strcmp(value, "none") != 0) {
        snprintf(c->sleep_state, sizeof c->sleep_state, "%s", value);
    } else if (strcmp(kind, "deep") == 0 && strcmp(value, "none") != 0) {
        snprintf(c->deep_action, sizeof c->deep_action, "%s", value);
    } else if (strcmp(kind, "charger-sleep") == 0) {
        c->charger_sleep = strcmp(value, "yes") == 0;
    }
}

static void profile_file(struct vrx_compositor *c, const char *path) {
    FILE *f = fopen(path, "re");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "#\r\n")] = 0;
        profile_line(c, line);
    }
    fclose(f);
}

// The family's profile, then each carrier's in power-profile.d (sorted).
static void profile_read(struct vrx_compositor *c) {
    profile_file(c, PROFILE_FILE);
    struct dirent **names;
    int n = scandir(PROFILE_DIR, &names, NULL, alphasort);
    for (int i = 0; i < n; i++) {
        if (names[i]->d_name[0] != '.') {
            char path[320];
            snprintf(path, sizeof path, "%s/%s", PROFILE_DIR, names[i]->d_name);
            profile_file(c, path);
        }
        free(names[i]);
    }
    if (n >= 0) free(names);
    c->has_wakealarm = access("/sys/class/rtc/rtc0/wakealarm", W_OK) == 0;
    // deep sleep is a halt the board cannot leave on its own: offered only
    // with a timed wake (the owner, 2026-10-14: no "Power off when idle"
    // on a board without one, it would lose alarms)
    if (c->deep_action[0] && !c->has_wakealarm) {
        fprintf(stderr, "[vrx-compositor] the profile offers deep sleep (%s) but no RTC can wake the board: not offered\n",
                c->deep_action);
        c->deep_action[0] = 0;
    }
    fprintf(stderr, "[vrx-compositor] cascade: sleep %s, deep sleep %s, timed wake %s\n",
            c->sleep_state[0] ? c->sleep_state : "not offered", c->deep_action[0] ? c->deep_action : "not offered",
            c->has_wakealarm ? "yes" : "no");
}

// A person can wake the board from sleep: an input device a person uses
// (udev's classification, as can_wake) whose device, or a parent, the
// kernel will let wake the system (power/wakeup = enabled). A touch
// controller whose driver frees its interrupt in suspend (goodix) has no
// such parent, and then sleep would be a dark board nobody can wake.
static bool sleep_wake_source(void) {
    static const char *kinds[] = { "ID_INPUT_TOUCHSCREEN", "ID_INPUT_KEYBOARD", "ID_INPUT_MOUSE",
                                   "ID_INPUT_TOUCHPAD", "ID_INPUT_TABLET" };
    struct udev *udev = udev_new();
    if (!udev) return false;
    struct udev_enumerate *e = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(e, "input");
    udev_enumerate_scan_devices(e);
    bool found = false;
    struct udev_list_entry *it;
    udev_list_entry_foreach(it, udev_enumerate_get_list_entry(e)) {
        if (found) break;
        struct udev_device *d = udev_device_new_from_syspath(udev, udev_list_entry_get_name(it));
        if (!d) continue;
        bool person = false;
        for (size_t i = 0; i < sizeof kinds / sizeof kinds[0] && !person; i++) {
            const char *v = udev_device_get_property_value(d, kinds[i]);
            person = v && strcmp(v, "1") == 0;
        }
        for (struct udev_device *p = d; person && p && !found; p = udev_device_get_parent(p)) {
            const char *w = udev_device_get_sysattr_value(p, "power/wakeup");
            found = w && strcmp(w, "enabled") == 0;
        }
        udev_device_unref(d);
    }
    udev_enumerate_unref(e);
    udev_unref(udev);
    return found;
}

// On a charger: a mains or USB supply reports itself online.
static bool on_charger(void) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return false;
    bool online = false;
    struct dirent *e;
    while (!online && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[288], type[32] = "";
        snprintf(dir, sizeof dir, "/sys/class/power_supply/%s", e->d_name);
        char path[320];
        snprintf(path, sizeof path, "%s/type", dir);
        FILE *f = fopen(path, "re");
        if (f) {
            if (!fgets(type, sizeof type, f)) type[0] = 0;
            fclose(f);
        }
        if (strncmp(type, "Battery", 7) != 0 && sysfs_int(dir, "online") == 1) online = true;
    }
    closedir(d);
    return online;
}

static void cascade_set(struct vrx_compositor *c, int seconds) {
    if (c->cascade_fd < 0) return;
    struct itimerspec t = { .it_value = { .tv_sec = seconds > 0 ? seconds : 0 } };
    timerfd_settime(c->cascade_fd, 0, &t, NULL);
}

// After the screen went off (light_off after the last input): arm the next
// step the board offers, counted from the same last input. CLOCK_BOOTTIME,
// so a step after a sleep still counts the time asleep.
static void cascade_arm(struct vrx_compositor *c) {
    c->cascade_next = STEP_NONE;
    if (c->sleep_state[0] && c->sleep_after > c->light_off) {
        c->cascade_next = STEP_SLEEP;
        cascade_set(c, (int) (c->sleep_after - c->light_off));
    } else if (c->deep_action[0] && c->deep_after > c->light_off) {
        c->cascade_next = STEP_DEEP;
        cascade_set(c, (int) (c->deep_after - c->light_off));
    } else {
        cascade_set(c, 0);
    }
}

static void cascade_disarm(struct vrx_compositor *c) {
    c->cascade_next = STEP_NONE;
    cascade_set(c, 0);
}

// Why this step must not happen now, or NULL.
static const char *cascade_refusal(struct vrx_compositor *c, int step) {
    if (!c->dark) return "the screen is on";
    if (held(c)) return "the screen is held on";
    if (c->sleep_blocked) return "an applet holds sleep off";
    if (!c->bus) return "no system bus";
    if (step == STEP_SLEEP && !sleep_wake_source()) return "nothing a person uses can wake the board from sleep";
    if (step == STEP_DEEP && !c->has_wakealarm) return "no RTC can wake the board";
    if (!c->charger_sleep && on_charger()) return "on a charger";
    return NULL;
}

static void cascade_fire(struct vrx_compositor *c) {
    uint64_t expirations;
    if (read(c->cascade_fd, &expirations, sizeof expirations) != sizeof expirations) return;
    int step = c->cascade_next;
    if (step == STEP_NONE) return;
    const char *why = cascade_refusal(c, step);
    if (why) {
        if (!c->refusal_logged) {
            fprintf(stderr, "[vrx-compositor] %s refused: %s\n", step == STEP_SLEEP ? "sleep" : "deep sleep", why);
            c->refusal_logged = true;
        }
        // a step that cannot happen now is tried again in a minute (a
        // hold may end, a charger may come off)
        cascade_set(c, 60);
        return;
    }
    const char *method = step == STEP_SLEEP ? "Suspend" : "Halt";
    fprintf(stderr, "[vrx-compositor] idle: %s (%s)\n", step == STEP_SLEEP ? "sleep" : "deep sleep",
            step == STEP_SLEEP ? c->sleep_state : c->deep_action);
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(c->bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", method, &err, NULL, "b", 0);
    if (r < 0) {
        fprintf(stderr, "[vrx-compositor] logind %s: %s\n", method, err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        cascade_set(c, 60);
        return;
    }
    c->cascade_next = STEP_NONE;
    // after a sleep, deep sleep follows from the same last input
    if (step == STEP_SLEEP && c->deep_action[0] && c->deep_after > c->sleep_after) {
        c->cascade_next = STEP_DEEP;
        cascade_set(c, (int) (c->deep_after - c->sleep_after));
    }
}

// Before sleep: a logind delay inhibitor, so applets get PrepareForSleep
// as lifecycle (paused) and up to a second to save; released then.
static void sleep_delay_take(struct vrx_compositor *c) {
    if (!c->bus || c->sleep_inhibit_fd >= 0) return;
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(c->bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", "Inhibit", &err, &reply, "ssss", "sleep",
                               "vrx-compositor", "applets save before the system sleeps", "delay");
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
    } else {
        fprintf(stderr, "[vrx-compositor] the system woke\n");
        sleep_delay_take(c);
        // the screen stays dark until a person wakes it (an alarm holds it
        // on); the next step counts again from now
        c->refusal_logged = false;
        if (c->dark) cascade_arm(c);
    }
    return 0;
}

// A second after PrepareForSleep(true): let the sleep go ahead.
static void sleep_ack_fire(struct vrx_compositor *c) {
    uint64_t expirations;
    if (read(c->sleep_ack_fd, &expirations, sizeof expirations) != sizeof expirations) return;
    if (c->sleep_inhibit_fd >= 0) {
        close(c->sleep_inhibit_fd);
        c->sleep_inhibit_fd = -1;
    }
}

static void send_input(struct vrx_compositor *c, const struct vrx_msg_input *m) {
    if (idle_swallows(c, m)) return;
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
            if (can_wake(d)) {
                libinput_device_set_user_data(d, c);
                wake_devices_changed(c, +1, libinput_device_get_name(d));
            }
            input_facts_changed(c, d, true);
            break;
        }
        case LIBINPUT_EVENT_DEVICE_REMOVED: {
            struct libinput_device *d = libinput_event_get_device(ev);
            if (libinput_device_get_user_data(d) == c) wake_devices_changed(c, -1, libinput_device_get_name(d));
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
#define TAG_IDLE      0xFFFFFFFFFFFFFFFAull
#define TAG_CONF      0xFFFFFFFFFFFFFFF9ull
#define TAG_BUS       0xFFFFFFFFFFFFFFF8ull
#define TAG_BACKLIGHT 0xFFFFFFFFFFFFFFF7ull
#define TAG_SIGNAL    0xFFFFFFFFFFFFFFF6ull
#define TAG_CASCADE   0xFFFFFFFFFFFFFFF5ull
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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--compositor")) continue;
        if (i + 1 < argc && !strcmp(argv[i], "--device")) device = argv[++i];
    }
    struct vrx_compositor c = {0};
    c.marker_fd = -1;
    c.focused = -1;
    c.controller = -1;
    c.shell = -1;
    c.pointer_owner = -1;
    for (int i = 0; i < 16; i++) c.touch_owner[i] = -1;
    c.primary_touch = -1;
    c.shell_frame.dmabuf_fd = -1;
    c.shell_frame.app = -1;
    struct frame *frames[] = { &c.on_screen, &c.pending, &c.queued };
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        frames[i]->dmabuf_fd = -1;
        frames[i]->app = -1;
    }
    for (int i = 0; i < MAX_APPS; i++) c.apps[i].fd = -1;
    if (kms_init(&c, device) != 0) return 1;
    c.shell_plane = plane_probe(&c, false);
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
    screen_init(&c);
    if (c.idle_fd >= 0) {
        ev.data.u64 = TAG_IDLE;
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.idle_fd, &ev);
    }
    if (c.conf_fd >= 0) {
        ev.data.u64 = TAG_CONF;
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.conf_fd, &ev);
    }
    if (c.cascade_fd >= 0) {
        ev.data.u64 = TAG_CASCADE;
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, c.cascade_fd, &ev);
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
    if (c.bl_mon) {
        ev.data.u64 = TAG_BACKLIGHT;
        epoll_ctl(c.epoll_fd, EPOLL_CTL_ADD, udev_monitor_get_fd(c.bl_mon), &ev);
    }
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
            } else if (tag == TAG_IDLE) {
                idle_fire(&c);
            } else if (tag == TAG_CONF) {
                screen_conf_changed(&c);
            } else if (tag == TAG_BUS) {
                bus_readable(&c);
            } else if (tag == TAG_BACKLIGHT) {
                backlight_changed(&c);
            } else if (tag == TAG_SIGNAL) {
                on_stop(&c);
            } else if (tag == TAG_CASCADE) {
                cascade_fire(&c);
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
    return 0;
}
