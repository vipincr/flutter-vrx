// SPDX-License-Identifier: MIT
#include "vrx_resume.h"

#include <stdio.h>
#include <string.h>

void vrx_resume_init(struct vrx_resume *r, uint32_t max_ms) {
    *r = (struct vrx_resume){ .max_ms = max_ms };
}

void vrx_resume_sleep(struct vrx_resume *r, unsigned inputs_now) {
    r->stage = VRX_RESUME_PARKING;
    r->inputs_held = inputs_now;
    r->have = (resume_have_t){ 0 };
}

enum vrx_resume_act vrx_resume_dark(struct vrx_resume *r) {
    if (r->stage != VRX_RESUME_PARKING) return VRX_RESUME_NOTHING;
    r->stage = VRX_RESUME_ASLEEP;
    return VRX_RESUME_DRAW_SPLASH;
}

enum vrx_resume_act vrx_resume_wake(struct vrx_resume *r, uint32_t now_ms, unsigned inputs_now) {
    if (r->stage != VRX_RESUME_ASLEEP) {
        r->stage = VRX_RESUME_IDLE;   // a wake with no splash drawn (the sleep was cut short): the old wake
        return VRX_RESUME_NOTHING;
    }
    r->stage = VRX_RESUME_WAITING;
    r->t0 = now_ms;
    r->have.input = inputs_now >= r->inputs_held;
    return VRX_RESUME_LIGHT_SPLASH;
}

void vrx_resume_report(struct vrx_resume *r, bool facts, bool clock) {
    if (r->stage != VRX_RESUME_WAITING) return;
    r->have.facts = r->have.facts || facts;
    r->have.clock = r->have.clock || clock;
}

void vrx_resume_inputs(struct vrx_resume *r, unsigned inputs_now) {
    if (r->stage != VRX_RESUME_WAITING) return;
    r->have.input = inputs_now >= r->inputs_held;
}

enum vrx_resume_act vrx_resume_check(struct vrx_resume *r, uint32_t now_ms) {
    if (r->stage != VRX_RESUME_WAITING) return VRX_RESUME_NOTHING;
    const resume_verdict_t v = resume_gate(r->have, now_ms - r->t0, r->max_ms);
    if (v == RESUME_WAIT) return VRX_RESUME_NOTHING;
    r->clock_unknown = resume_gate_clock_unknown(v, r->have);
    r->stage = VRX_RESUME_DIPPING;
    return VRX_RESUME_DIP;
}

uint32_t vrx_resume_left(const struct vrx_resume *r, uint32_t now_ms) {
    if (r->stage != VRX_RESUME_WAITING) return 0;
    const uint32_t waited = now_ms - r->t0;
    return waited < r->max_ms ? r->max_ms - waited : 1;
}

enum vrx_resume_act vrx_resume_dipped(struct vrx_resume *r) {
    if (r->stage != VRX_RESUME_DIPPING) return VRX_RESUME_NOTHING;
    r->stage = VRX_RESUME_IDLE;
    return VRX_RESUME_SHOW_UI;
}

void vrx_resume_abort(struct vrx_resume *r) {
    r->stage = VRX_RESUME_IDLE;
}

bool vrx_resume_splash_path(char *out, size_t n, const char *dir, unsigned w, unsigned h, const char *fourcc) {
    if (!out || !n || !dir || !fourcc || !w || !h) return false;
    int len = snprintf(out, n, "%s/%ux%u-%s.raw", dir, w, h, fourcc);
    return len > 0 && (size_t) len < n;
}

bool vrx_resume_splash_fill(uint32_t *px, unsigned stride_px, unsigned w, unsigned h, const char *dir, uint32_t page_xrgb,
                            void (*say)(const char *what, void *ctx), void *ctx) {
    char path[160], what[220];
    bool used = false;
    if (vrx_resume_splash_path(path, sizeof path, dir, w, h, "XR24")) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            snprintf(what, sizeof what, "%s is missing: the resume shows the page colour", path);
            if (say) say(what, ctx);
        } else {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            rewind(f);
            if (size == (long) ((size_t) w * h * 4)) {
                used = true;
                for (unsigned y = 0; y < h && used; y++) used = fread(px + (size_t) y * stride_px, 4, w, f) == w;
            } else {
                snprintf(what, sizeof what, "%s is %ld bytes, not %zu: the resume shows the page colour", path, size, (size_t) w * h * 4);
                if (say) say(what, ctx);
            }
            fclose(f);
        }
    }
    if (!used)
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) px[(size_t) y * stride_px + x] = page_xrgb;
    return used;
}
