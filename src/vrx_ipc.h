// SPDX-License-Identifier: MIT
//
// flutter-vrx IPC protocol (plan 0005, milestones B2 and B3).
//
// The wire between `flutter-vrx --compositor` (DRM master, input owner)
// and each `flutter-vrx --app` process (own UI thread, own engine, renders
// into a dmabuf it hands over), plus vrxhost as the one controller that
// names the focused app (plan 0007 amendment C). One unix socket at
// VRX_UI_SOCKET of type SOCK_SEQPACKET: every send is one message and every
// recvmsg returns exactly one, so neither side frames or buffers (a stream
// socket merges and splits messages, which lost the FOCUS that follows
// REGISTERED). File descriptors travel as SCM_RIGHTS on the message that
// names them.
//
// The socket is root:VRX_UI_SOCKET_GROUP 0660, the group apps run under.
// A connection's first message makes it an app (REGISTER) or the controller
// (CONTROL, accepted from root only); the compositor identifies both by
// SO_PEERCRED, so the pid vrxhost spawned is the pid it focuses.
//
// v1 is deliberately small: register, present, focus, input, close. The
// compositor shows exactly one fullscreen app (the focused one) by flipping
// KMS scanout to its latest presented buffer. No negotiation beyond the
// buffer format/modifier list the compositor declares at registration.

#ifndef VRX_IPC_H
#define VRX_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VRX_UI_SOCKET_DIR "/run/vrx-ui"
#define VRX_UI_SOCKET VRX_UI_SOCKET_DIR "/vrx.sock"
#define VRX_UI_SOCKET_GROUP "vrx"
#define VRX_MAX_FORMATS 8

// Message types. APP->COMP or COMP->APP as noted.
enum vrx_msg_type {
    // app -> comp: I am starting; id = the app bundle id (registry name).
    VRX_MSG_REGISTER = 1,
    // comp -> app: accepted; here is the output geometry and the buffer
    // formats/modifiers I can scan out directly.
    VRX_MSG_REGISTERED = 2,
    // app -> comp: a presented frame. Exactly one fd rides this message.
    VRX_MSG_BUFFER = 3,
    // comp -> app: focus state changed (focused apps pump frames, others
    // should suspend).
    VRX_MSG_FOCUS = 4,
    // comp -> app: an input event (pointer, touch, key), already routed.
    VRX_MSG_INPUT = 5,
    // app -> comp: I am exiting; drop my scanout and fall back.
    VRX_MSG_CLOSED = 6,
    // comp -> app: fatal protocol error; exit.
    VRX_MSG_SHUTDOWN = 7,
    // comp -> app: I no longer scan out (or will never scan out) the buffer
    // you presented with this sequence; you may render into it again. Until
    // this arrives the app must not reuse the buffer, or it draws into the
    // frame on screen.
    VRX_MSG_RELEASE = 8,
    // vrxhost -> comp, first message: I am the controller. No payload.
    VRX_MSG_CONTROL = 9,
    // vrxhost -> comp: focus the app with this pid (0 = none). An app that
    // has not registered yet takes the focus when it does.
    VRX_MSG_SET_FOCUS = 10,
    // comp -> vrxhost: an app disconnected (state 0), registered (1), or
    // had its first frame put on screen (2: what "the UI is up" means).
    // On CONTROL the compositor reports every app already registered.
    VRX_MSG_APP_STATE = 11,
    // vrxhost -> comp: the app with this pid is the shell (0 = none). Its
    // frames go to an overlay plane above the focused app, which no app
    // can cover (plan 0008 D10); it is never focused.
    VRX_MSG_SET_SHELL = 12,
    // shell -> comp: its region, in output pixels from the top: the strip
    // (input there goes to the shell, apps are told to keep clear), or the
    // whole screen while its drawer is open (all input is the shell's).
    VRX_MSG_SHELL_REGION = 13,
    // comp -> app: the shell's strip covers the top `top` pixels; sent on
    // REGISTER and whenever it changes.
    VRX_MSG_INSETS = 14,
    // app -> comp: give me the whole screen (hidden = 1) or the strip back
    // (0). While that app is focused the shell's plane is off and its input
    // goes to the app; the app lays itself out without the strip (a
    // keyboard on a 58mm panel). Its INSETS stay the strip's height.
    VRX_MSG_CHROME = 15,
    // shell -> comp -> the focused app: the strip's Back was pressed. The
    // app goes back a screen, or leaves (Nav.pop) at its first. No payload.
    VRX_MSG_BACK = 16,
    // comp -> app: the screen went off (on = 0) or came back (1); sent on
    // REGISTER too while it is off. The app stops drawing while it is off
    // (Flutter: AppLifecycleState.hidden, plan 0010 D4).
    VRX_MSG_DISPLAY = 17,
    // vrxhost -> comp: cap the screen's brightness at `permille` of the
    // range above the floor (1000 = no cap) without touching the stored
    // setting: the battery's WARN zone (plan 0012 D10). The compositor
    // stays the light's one owner.
    VRX_MSG_LIGHT_CAP = 18,
    // comp -> app: the system is about to sleep (sleeping = 1) or woke (0),
    // from logind's PrepareForSleep, which the compositor delays by up to
    // a second for this (plan 0012 D14). The app saves what it must
    // (Flutter: AppLifecycleState.paused).
    VRX_MSG_SLEEP = 19,
    // vrxhost -> comp: the profiler's switch (on = 1/0, plan 0014 D1, D5).
    // While on, the compositor writes its flip record to ftrace's
    // trace_marker: per-frame spans, drops by cause, input-to-screen.
    VRX_MSG_PROFILING = 20,
    // comp -> app: the panel's vblank phase (plan 0014 D6): the timestamp
    // of a real flip (CLOCK_MONOTONIC) and the refresh period. The app
    // answers the engine's frame requests with the last real vblank and
    // the next, where it used the engine's timer-based guess. Sent on
    // focus and then at most once a second while the app draws.
    VRX_MSG_VBLANK = 21,
    // vrxhost -> comp -> every app: build the semantics tree (on = 1) or
    // stop (0) (plan 0014 D8). Walks find controls by label while it is
    // on; the tree costs a build pass per frame, so timing runs keep it
    // off.
    VRX_MSG_SEMANTICS = 22,
    // app -> comp: the app's semantics tree as JSON in a memfd (the one fd
    // riding the message), written by the compositor to
    // /run/vrx-ui/semantics/<app id>.json for the walk driver.
    VRX_MSG_SEMANTICS_TREE = 23,
};

// Every message on the wire: fixed header, then a type-specific payload.
struct vrx_msg_header {
    uint32_t type;    // enum vrx_msg_type
    uint32_t length;  // payload length in bytes, excluding this header
};

struct vrx_msg_register {
    char id[64];  // NUL-terminated bundle id, e.g. "hello_world"
};

struct vrx_msg_registered {
    uint32_t width;    // output pixel size
    uint32_t height;
    uint32_t n_formats;
    uint32_t formats[VRX_MAX_FORMATS];        // DRM fourccs
    uint64_t modifiers[VRX_MAX_FORMATS];      // per-format modifier, paired
    // The panel's physical size (the DRM connector's, which panel drivers
    // take from the device tree's width-mm/height-mm; EDID for HDMI);
    // 0 = unknown. The app derives its pixel ratio from it, see
    // VRX_DP_PER_MM.
    uint32_t width_mm;
    uint32_t height_mm;
};

// vrx's logical density: Flutter's 160 dp per inch, so 1mm = 6.299dp and
// vrx_kit's mm-derived metrics (9mm touch minimum) hold on every panel.
// An app's pixel ratio is therefore (px per mm) / VRX_DP_PER_MM.
#define VRX_DP_PER_MM (160.0 / 25.4)

struct vrx_msg_buffer {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;      // DRM fourcc, one of the declared set
    uint64_t modifier;
    uint32_t sequence;    // app frame counter, monotonic
};

struct vrx_msg_focus {
    uint32_t focused;  // 0/1
};

struct vrx_msg_release {
    uint32_t sequence;  // the vrx_msg_buffer.sequence being returned
};

struct vrx_msg_set_focus {
    int32_t pid;
};

enum vrx_app_state {
    VRX_APP_GONE = 0,
    VRX_APP_REGISTERED = 1,
    VRX_APP_SHOWN = 2,
};

struct vrx_msg_set_shell {
    int32_t pid;
};

struct vrx_msg_shell_region {
    uint32_t top;   // the strip's height in output pixels
    uint32_t full;  // 1 while the drawer has the whole screen
};

struct vrx_msg_insets {
    uint32_t top;
};

struct vrx_msg_chrome {
    uint32_t hidden;  // 1: the app has the whole screen while focused
};

struct vrx_msg_display {
    uint32_t on;  // 0/1
};

struct vrx_msg_light_cap {
    uint32_t permille;  // 0-1000 of the range above the floor; 1000 = none
};

struct vrx_msg_sleep {
    uint32_t sleeping;  // 0/1
};

struct vrx_msg_profiling {
    uint32_t on;  // 0/1
};

struct vrx_msg_semantics {
    uint32_t on;  // 0/1
};

struct vrx_msg_semantics_tree {
    uint32_t length;  // bytes of JSON in the memfd
};

struct vrx_msg_vblank {
    uint64_t flip_ns;    // a real flip's timestamp, CLOCK_MONOTONIC
    uint64_t period_ns;  // the refresh period
};

struct vrx_msg_app_state {
    int32_t pid;
    uint32_t state;  // VRX_APP_GONE / VRX_APP_REGISTERED / VRX_APP_SHOWN
    char id[64];     // the app's REGISTER id
};

// Input event kinds. Coordinates are output pixels, origin top-left.
enum vrx_input_kind {
    VRX_INPUT_POINTER = 1,  // move / button
    VRX_INPUT_TOUCH = 2,    // multi-touch slot events
    VRX_INPUT_KEY = 3,      // xkb-translated key
};

struct vrx_input_pointer {
    uint64_t time_ns;
    int32_t x, y;
    // bit per button, 1 = down, Flutter's numbering: 1 left, 2 right,
    // 4 middle, 8 back, 16 forward
    uint32_t buttons;
    // a wheel or touchpad scroll in output pixels (positive: right, down);
    // 0 on a move or a button change
    int32_t scroll_x, scroll_y;
};

struct vrx_input_touch {
    uint64_t time_ns;
    uint32_t slot;
    uint32_t kind;   // 0 down, 1 move, 2 up
    int32_t x, y;
};

struct vrx_input_key {
    uint64_t time_ns;
    uint32_t keycode;  // linux KEY_* / evdev code, pre-xkb
    uint32_t state;    // 0 release, 1 press
};

struct vrx_msg_input {
    uint32_t kind;  // enum vrx_input_kind
    union {
        struct vrx_input_pointer pointer;
        struct vrx_input_touch touch;
        struct vrx_input_key key;
    } u;
};

struct vrx_msg_closed {};

// The app id for a bundle path. Bundles sit at
// <root>/<app>/<engine-version>/release (meta-flutter's layout; vrx applets
// use /system/defaults/<id>/<engine-version>/release), so the id is the
// component two above "release"; any other path uses its last component.
#include <stdio.h>
#include <string.h>
static inline void vrx_bundle_id(const char *path, char *out, size_t out_len) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    char *last = strrchr(tmp, '/');
    if (last && strcmp(last + 1, "release") == 0) {
        *last = 0;                          // .../<app>/<ver>
        char *ver = strrchr(tmp, '/');
        if (ver) {
            *ver = 0;                       // .../<app>
            char *app = strrchr(tmp, '/');
            snprintf(out, out_len, "%s", app ? app + 1 : tmp);
            return;
        }
    }
    snprintf(out, out_len, "%s", last ? last + 1 : tmp);
}

#endif  // VRX_IPC_H
