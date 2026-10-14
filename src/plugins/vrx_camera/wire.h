// SPDX-License-Identifier: MIT
/*
 * vrx camera plugin: the pure wire parts (vrx plan 0018 D8).
 *
 * No EGL, no sockets, no flutter-pi: everything here takes bytes and returns
 * values, so test/vrx_camera_wire_test.c runs it on any host.
 *
 * The wire is vrx-linux's buffer queue (rust/protocol/src/bufq.rs), one JSON
 * object per line on the channel's stream:
 *   producer -> consumer  {"buffers":N,"size":S,"format":{"fourcc":F,"width":W,
 *                          "height":H,"stride":P,"modifier":M}}   with N fds
 *   producer -> consumer  {"index":i,"bytes":b,"sequence":n}      filled
 *   consumer -> producer  {"index":i}                             given back
 * "format" is optional on the wire; this plugin refuses an offer without it
 * (a viewfinder cannot guess width, height or pixel format).
 *
 * Varlink (vrxhost, /run/vrx/io.vrx.Host): one JSON object per message, each
 * ended by a NUL byte.
 */

#ifndef _FLUTTERPI_SRC_PLUGINS_VRX_CAMERA_WIRE_H
#define _FLUTTERPI_SRC_PLUGINS_VRX_CAMERA_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VRX_CAMERA_MAX_BUFFERS 16  // rust/protocol bufq::MAX_BUFFERS

struct vrx_camera_format {
    uint32_t fourcc;  // DRM fourcc
    uint32_t width, height, stride;
    uint64_t modifier;
};

struct vrx_camera_offer {
    uint32_t buffers;
    uint64_t size;
    bool has_format;
    struct vrx_camera_format format;
};

struct vrx_camera_filled {
    uint32_t index;
    uint64_t bytes;
    uint64_t sequence;
};

enum vrx_camera_parse {
    VRX_CAMERA_PARSE_OK = 0,
    VRX_CAMERA_PARSE_BAD = 1,   // not a record of that kind
};

/// Parse one offer line (no trailing newline needed). `buffers` must be in
/// 1..VRX_CAMERA_MAX_BUFFERS and `size` non-zero, else BAD.
enum vrx_camera_parse vrx_camera_parse_offer(const char *line, size_t len, struct vrx_camera_offer *out);

/// Parse one filled record. A record without "bytes" and "sequence" is BAD
/// (that is a returned record, not a filled one).
enum vrx_camera_parse vrx_camera_parse_filled(const char *line, size_t len, struct vrx_camera_filled *out);

/// Write `{"index":i}\n` into `buf`; returns the length, or 0 if it does not fit.
size_t vrx_camera_format_return(uint32_t index, char *buf, size_t cap);

/// Write the Varlink Open call for `<to>.<channel>` including its NUL;
/// returns the length, or 0 if it does not fit.
size_t vrx_camera_format_open_call(const char *to, const char *channel, char *buf, size_t cap);

/// Name of a Varlink error in `msg` (e.g. "io.vrx.Host.Busy"), copied into
/// `out`; false when the message has no "error" key.
bool vrx_camera_varlink_error(const char *msg, size_t len, char *out, size_t cap);

/// camerad decides exclusivity after vrxhost's Open succeeded, so a refused
/// claimant's first line is a bare {"error":"busy"} (or "disabled",
/// "no-camera", "failed"), not an offer. True, with the name in `out`, for a
/// line that has an "error" key and no "buffers".
bool vrx_camera_stream_error(const char *line, size_t len, char *out, size_t cap);

/// Map a Varlink error name (or an offer problem) to the short code Dart
/// sees in PlatformException.code.
const char *vrx_camera_error_code(const char *varlink_error);

/// Splits a byte stream into newline-terminated lines (the stream) or
/// NUL-terminated messages (Varlink). Feed bytes; take whole records.
struct vrx_camera_lines {
    char buf[8192];
    size_t len;
    char delimiter;
};

void vrx_camera_lines_init(struct vrx_camera_lines *l, char delimiter);

/// Append bytes; false on overflow (a record longer than the buffer, which
/// no well-formed peer sends).
bool vrx_camera_lines_feed(struct vrx_camera_lines *l, const char *data, size_t n);

/// Pop the next whole record into `out` (NUL terminated, delimiter dropped);
/// false when none is complete.
bool vrx_camera_lines_next(struct vrx_camera_lines *l, char *out, size_t cap, size_t *len_out);

#endif
