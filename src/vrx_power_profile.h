// SPDX-License-Identifier: MIT
//
// The board's power profile, as the compositor's idle cascade reads it
// (vrx plans 0012 D13 and 0020 D1, D3, D4). Pure: text in, values out, no
// files, udev or logind, so the cascade's decisions are tested on a host
// (G030).
//
// Lines the cascade reads (other kinds, such as `model` and `walk-budget`,
// belong to vrxhost and are skipped here):
//   sleep <state>|none ...           the step after the screen goes off
//   deep halt|none                   deep sleep, a power-off with a timed wake
//   charger-sleep yes|no             sleep on a charger too
//   off standby|halt|none [wake=button|replug]
//                                    what a person's Power off does: standby
//                                    (the power chip's outputs off), halt
//                                    (rails up), none (no Power off)
// The installed files (the family's, then each carrier's) are read in order
// and a later line replaces an earlier one, so a carrier can narrow its
// family. The runtime directory (/run/vrx/power-profile.d) is read last and
// may only withdraw: a line there that would raise a step is ignored.

#ifndef VRX_POWER_PROFILE_H
#define VRX_POWER_PROFILE_H

#include <stdbool.h>
#include <stddef.h>

struct vrx_power_profile {
    char sleep[16];      // "" = no sleep offered
    char deep[16];       // "" = no deep sleep offered ("halt" otherwise)
    bool charger_sleep;
    char off[16];        // "standby", "halt" or "none"; "halt" by default
    char off_wake[16];   // "button" or "replug" for standby, else ""
};

enum vrx_profile_result {
    VRX_PROFILE_APPLIED,
    VRX_PROFILE_SKIPPED,   // not a cascade line, or a value it does not know
    VRX_PROFILE_RAISE,     // a runtime line that would raise a step: ignored
};

enum vrx_cascade_step { VRX_STEP_NONE, VRX_STEP_SLEEP, VRX_STEP_DEEP };

// What the cascade knows about the board right now, besides the profile.
struct vrx_cascade_inputs {
    bool dark;             // the screen is off
    bool held;             // an idle inhibitor holds the screen on
    bool sleep_blocked;    // a logind sleep block inhibitor is held
    bool bus;              // the system bus is connected
    bool wake_source;      // an input a person uses can wake the board
    bool has_wakealarm;    // an RTC can wake the board on a timer
    bool on_charger;
};

void vrx_profile_init(struct vrx_power_profile *p);

// One line. `runtime` lines (the /run directory) may only withdraw.
enum vrx_profile_result vrx_profile_line(struct vrx_power_profile *p, const char *line, bool runtime);

// A whole file's text: `#` starts a comment. Calls `raised` (may be NULL)
// once for each runtime line ignored because it would raise a step.
void vrx_profile_text(struct vrx_power_profile *p, const char *text, bool runtime,
                      void (*raised)(const char *line, void *ctx), void *ctx);

// After every file: deep sleep is a power-off the board cannot leave on its
// own, so it is offered only with a timed wake (the owner, 2026-10-14).
// Returns true when it withdrew deep sleep for that reason.
bool vrx_profile_finish(struct vrx_power_profile *p, bool has_wakealarm);

// The steps Settings may show: sleep only with a wake source.
const char *vrx_profile_sleep_offered(const struct vrx_power_profile *p, bool wake_source);
const char *vrx_profile_deep_offered(const struct vrx_power_profile *p);

// Why `step` must not happen now, or NULL (G030: never a sleep nobody can
// wake, never a deep sleep without a timed wake).
const char *vrx_cascade_refusal(const struct vrx_power_profile *p, enum vrx_cascade_step step,
                                const struct vrx_cascade_inputs *in);

#endif
