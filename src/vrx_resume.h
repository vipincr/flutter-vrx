// SPDX-License-Identifier: MIT
// The resume's presentation, as the compositor runs it (vrx plan 0034 D1, D3, D5): the splash drawn in the dark before the system
// sleeps, lit on the wake and held until the essentials are back (or the bound), then through dark into the UI. Pure: the glass,
// the light, the essentials' reporters and the clock are the caller's, so the sequence is testable on a host. The gate is
// libvrx-light's (vrx_light_resume.h), the one both UI runtimes compile; the lengths and the level are the standard's.
#ifndef VRX_RESUME_H
#define VRX_RESUME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vrx_light/vrx_light_resume.h>

enum vrx_resume_stage {
    VRX_RESUME_IDLE,
    VRX_RESUME_PARKING,   // PrepareForSleep(true): the light is coming down
    VRX_RESUME_ASLEEP,    // the splash is on the glass and the request has been let go
    VRX_RESUME_WAITING,   // woke: the splash is lit, the essentials are awaited
    VRX_RESUME_DIPPING,   // the essentials are back (or the bound passed): the light is going out for the UI
};

enum vrx_resume_act {
    VRX_RESUME_NOTHING,
    VRX_RESUME_DRAW_SPLASH,   // the light is down: put the splash on the glass, then let the request go
    VRX_RESUME_LIGHT_SPLASH,  // woke with the splash up: light it (to the standard's level), over the in-length
    VRX_RESUME_DIP,           // go dark (the out-length), then show the UI
    VRX_RESUME_SHOW_UI,       // the light is down: the UI's frame on the glass, then light the person's brightness
};

struct vrx_resume {
    enum vrx_resume_stage stage;
    uint32_t t0;              // when the splash was lit
    uint32_t max_ms;          // the standard's essentials_max_ms
    unsigned inputs_held;     // the input devices present when the sleep began
    resume_have_t have;
    bool clock_unknown;       // the last wake's bound passed with no clock source
};

void vrx_resume_init(struct vrx_resume *r, uint32_t max_ms);

// PrepareForSleep(true), with the input devices present now. A sleep with no splash to draw is one the caller aborts.
void vrx_resume_sleep(struct vrx_resume *r, unsigned inputs_now);
// The light is down. DRAW_SPLASH while parking, else NOTHING (a screen going off is not a resume).
enum vrx_resume_act vrx_resume_dark(struct vrx_resume *r);
// PrepareForSleep(false), with the input devices present now. LIGHT_SPLASH when the splash is up; anything else leaves the old wake (the caller lights the UI).
enum vrx_resume_act vrx_resume_wake(struct vrx_resume *r, uint32_t now_ms, unsigned inputs_now);
// The essentials the kit reports (VRX_MSG_RESUME_ESSENTIALS), and the input devices present now. Each counts only while waiting (a report from before the wake is a reading from before the sleep).
void vrx_resume_report(struct vrx_resume *r, bool facts, bool clock);
void vrx_resume_inputs(struct vrx_resume *r, unsigned inputs_now);
// Judge the wait: DIP once, when all three are back or the bound passed; the clock is marked unknown when only the bound ended it.
enum vrx_resume_act vrx_resume_check(struct vrx_resume *r, uint32_t now_ms);
// Milliseconds to the bound while waiting, else 0.
uint32_t vrx_resume_left(const struct vrx_resume *r, uint32_t now_ms);
// The light is down again after a dip: SHOW_UI once.
enum vrx_resume_act vrx_resume_dipped(struct vrx_resume *r);
// The sleep did not go, or no splash could be drawn: back to idle.
void vrx_resume_abort(struct vrx_resume *r);

// The splash frame's file in `dir`: <W>x<H>-<FOURCC>.raw. False when the name does not fit.
bool vrx_resume_splash_path(char *out, size_t n, const char *dir, unsigned w, unsigned h, const char *fourcc);
// `px` (w * h XRGB8888 pixels, rows top to bottom, `stride_px` pixels a row) from the file, or the page colour when it is missing or
// not exactly w * h * 4 bytes: said once through `say`, never a crash. True when the file was used.
bool vrx_resume_splash_fill(uint32_t *px, unsigned stride_px, unsigned w, unsigned h, const char *dir, uint32_t page_xrgb,
                            void (*say)(const char *what, void *ctx), void *ctx);

#endif
