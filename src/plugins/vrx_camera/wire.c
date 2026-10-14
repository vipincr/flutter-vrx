// SPDX-License-Identifier: MIT
#include "plugins/vrx_camera/wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The records are flat objects with unique key names (the nested "format"
// object shares no key with its parent), so a key scan is enough and a
// general JSON parser is not needed.

static const char *find_key(const char *s, size_t len, const char *key) {
    char pat[32];
    int n = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (n <= 0 || (size_t) n >= sizeof pat) {
        return NULL;
    }
    for (size_t i = 0; i + (size_t) n <= len; i++) {
        if (memcmp(s + i, pat, n) != 0) {
            continue;
        }
        size_t j = i + n;
        while (j < len && (s[j] == ' ' || s[j] == '\t')) {
            j++;
        }
        if (j >= len || s[j] != ':') {
            continue;
        }
        j++;
        while (j < len && (s[j] == ' ' || s[j] == '\t')) {
            j++;
        }
        return s + j;
    }
    return NULL;
}

static bool get_u64(const char *s, size_t len, const char *key, uint64_t *out) {
    const char *v = find_key(s, len, key);
    if (v == NULL || v >= s + len || *v < '0' || *v > '9') {
        return false;
    }
    // the line is not NUL terminated in general; copy the digits
    char digits[24];
    size_t n = 0;
    while (v + n < s + len && v[n] >= '0' && v[n] <= '9') {
        if (n + 1 >= sizeof digits) {
            return false;
        }
        digits[n] = v[n];
        n++;
    }
    digits[n] = 0;
    errno = 0;
    char *end;
    unsigned long long x = strtoull(digits, &end, 10);
    if (errno != 0 || *end != 0) {
        return false;
    }
    *out = x;
    return true;
}

static bool get_u32(const char *s, size_t len, const char *key, uint32_t *out) {
    uint64_t x;
    if (!get_u64(s, len, key, &x) || x > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t) x;
    return true;
}

// A fourcc is a number or a four character string ("XR24").
static bool get_fourcc(const char *s, size_t len, uint32_t *out) {
    const char *v = find_key(s, len, "fourcc");
    if (v == NULL || v >= s + len) {
        return false;
    }
    if (*v == '"') {
        if (v + 6 > s + len || v[5] != '"') {
            return false;
        }
        *out = (uint32_t) (uint8_t) v[1] | ((uint32_t) (uint8_t) v[2] << 8) | ((uint32_t) (uint8_t) v[3] << 16) | ((uint32_t) (uint8_t) v[4] << 24);
        return true;
    }
    return get_u32(s, len, "fourcc", out);
}

enum vrx_camera_parse vrx_camera_parse_offer(const char *line, size_t len, struct vrx_camera_offer *out) {
    struct vrx_camera_offer o;
    memset(&o, 0, sizeof o);

    if (!get_u32(line, len, "buffers", &o.buffers) || !get_u64(line, len, "size", &o.size)) {
        return VRX_CAMERA_PARSE_BAD;
    }
    if (o.buffers < 1 || o.buffers > VRX_CAMERA_MAX_BUFFERS || o.size == 0) {
        return VRX_CAMERA_PARSE_BAD;
    }

    if (find_key(line, len, "format") != NULL) {
        struct vrx_camera_format *f = &o.format;
        if (!get_fourcc(line, len, &f->fourcc) || !get_u32(line, len, "width", &f->width) || !get_u32(line, len, "height", &f->height) ||
            !get_u32(line, len, "stride", &f->stride)) {
            return VRX_CAMERA_PARSE_BAD;
        }
        if (!get_u64(line, len, "modifier", &f->modifier)) {
            f->modifier = 0;  // DRM_FORMAT_MOD_LINEAR
        }
        if (f->width == 0 || f->height == 0 || f->stride == 0) {
            return VRX_CAMERA_PARSE_BAD;
        }
        o.has_format = true;
    }

    *out = o;
    return VRX_CAMERA_PARSE_OK;
}

enum vrx_camera_parse vrx_camera_parse_filled(const char *line, size_t len, struct vrx_camera_filled *out) {
    struct vrx_camera_filled f;
    if (!get_u32(line, len, "index", &f.index) || !get_u64(line, len, "bytes", &f.bytes) || !get_u64(line, len, "sequence", &f.sequence)) {
        return VRX_CAMERA_PARSE_BAD;
    }
    *out = f;
    return VRX_CAMERA_PARSE_OK;
}

size_t vrx_camera_format_return(uint32_t index, char *buf, size_t cap) {
    int n = snprintf(buf, cap, "{\"index\":%u}\n", index);
    return (n > 0 && (size_t) n < cap) ? (size_t) n : 0;
}

size_t vrx_camera_format_open_call(const char *to, const char *channel, char *buf, size_t cap) {
    int n = snprintf(buf, cap, "{\"method\":\"io.vrx.Host.Open\",\"parameters\":{\"to\":\"%s\",\"channel\":\"%s\"}}", to, channel);
    if (n <= 0 || (size_t) n + 1 >= cap) {
        return 0;
    }
    buf[n] = 0;  // Varlink's message end
    return (size_t) n + 1;
}

bool vrx_camera_varlink_error(const char *msg, size_t len, char *out, size_t cap) {
    const char *v = find_key(msg, len, "error");
    if (v == NULL || v >= msg + len || *v != '"') {
        return false;
    }
    v++;
    size_t n = 0;
    while (v + n < msg + len && v[n] != '"') {
        n++;
    }
    if (v + n >= msg + len || n + 1 > cap) {
        return false;
    }
    memcpy(out, v, n);
    out[n] = 0;
    return true;
}

const char *vrx_camera_error_code(const char *e) {
    static const struct {
        const char *suffix, *code;
    } table[] = {
        { "NotPermitted", "not-permitted" },
        { "NotDeclared", "not-declared" },
        { "NotListening", "unavailable" },
        { "NotOffered", "unavailable" },
        { "NoSuchApplet", "unavailable" },
        { "Busy", "busy" },
        { "Disabled", "disabled" },
        { "NotAnApplet", "not-permitted" },
        // camerad's own first line on the stream: {"error":"busy"}
        { "busy", "busy" },
        { "disabled", "disabled" },
        { "no-camera", "unavailable" },
    };
    const char *dot = strrchr(e, '.');
    const char *name = dot ? dot + 1 : e;
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(name, table[i].suffix) == 0) {
            return table[i].code;
        }
    }
    return "failed";
}

void vrx_camera_lines_init(struct vrx_camera_lines *l, char delimiter) {
    l->len = 0;
    l->delimiter = delimiter;
}

bool vrx_camera_lines_feed(struct vrx_camera_lines *l, const char *data, size_t n) {
    if (n > sizeof l->buf - l->len) {
        return false;
    }
    memcpy(l->buf + l->len, data, n);
    l->len += n;
    return true;
}

bool vrx_camera_lines_next(struct vrx_camera_lines *l, char *out, size_t cap, size_t *len_out) {
    for (size_t i = 0; i < l->len; i++) {
        if (l->buf[i] != l->delimiter) {
            continue;
        }
        if (i + 1 > cap) {
            // a record that cannot fit the caller's buffer: drop it
            memmove(l->buf, l->buf + i + 1, l->len - i - 1);
            l->len -= i + 1;
            return false;
        }
        memcpy(out, l->buf, i);
        out[i] = 0;
        *len_out = i;
        memmove(l->buf, l->buf + i + 1, l->len - i - 1);
        l->len -= i + 1;
        return true;
    }
    return false;
}

bool vrx_camera_stream_error(const char *line, size_t len, char *out, size_t cap) {
    return find_key(line, len, "buffers") == NULL && vrx_camera_varlink_error(line, len, out, cap);
}
