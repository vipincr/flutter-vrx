// SPDX-License-Identifier: MIT
//
// flutter-vrx IPC protocol (plan 0005, milestone B2).
//
// The wire between `flutter-vrx --compositor` (DRM master, input owner,
// supervisor) and each `flutter-vrx --app` process (own UI thread, own
// engine, renders into a dmabuf it hands over). One unix socket at
// VRX_UI_SOCKET; length-prefixed little-endian messages; file descriptors
// travel as SCM_RIGHTS on the message that names them.
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
};

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

// Input event kinds. Coordinates are output pixels, origin top-left.
enum vrx_input_kind {
    VRX_INPUT_POINTER = 1,  // move / button
    VRX_INPUT_TOUCH = 2,    // multi-touch slot events
    VRX_INPUT_KEY = 3,      // xkb-translated key
};

struct vrx_input_pointer {
    uint64_t time_ns;
    int32_t x, y;
    uint32_t buttons;  // bit per button, 1 = down (button 1 = left)
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

#endif  // VRX_IPC_H
