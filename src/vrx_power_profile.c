// SPDX-License-Identifier: MIT
// The board's power profile for the idle cascade (see vrx_power_profile.h).

#include "vrx_power_profile.h"

#include <stdio.h>
#include <string.h>

void vrx_profile_init(struct vrx_power_profile *p) {
    memset(p, 0, sizeof *p);
    snprintf(p->off, sizeof p->off, "halt");
}

static int off_rank(const char *v) {
    if (strcmp(v, "standby") == 0) return 2;
    if (strcmp(v, "halt") == 0) return 1;
    if (strcmp(v, "none") == 0) return 0;
    return -1;
}

enum vrx_profile_result vrx_profile_line(struct vrx_power_profile *p, const char *line, bool runtime) {
    char kind[16] = "", value[16] = "", extra[32] = "";
    int n = sscanf(line, " %15s %15s %31s", kind, value, extra);
    if (n < 2) return VRX_PROFILE_SKIPPED;

    if (strcmp(kind, "sleep") == 0) {
        bool none = strcmp(value, "none") == 0;
        if (runtime && !none && strcmp(p->sleep, value) != 0) return VRX_PROFILE_RAISE;
        snprintf(p->sleep, sizeof p->sleep, "%s", none ? "" : value);
        return VRX_PROFILE_APPLIED;
    }
    if (strcmp(kind, "deep") == 0) {
        bool none = strcmp(value, "none") == 0;
        if (!none && strcmp(value, "halt") != 0) return VRX_PROFILE_SKIPPED;
        if (runtime && !none && strcmp(p->deep, value) != 0) return VRX_PROFILE_RAISE;
        snprintf(p->deep, sizeof p->deep, "%s", none ? "" : value);
        return VRX_PROFILE_APPLIED;
    }
    if (strcmp(kind, "charger-sleep") == 0) {
        bool yes = strcmp(value, "yes") == 0;
        if (!yes && strcmp(value, "no") != 0) return VRX_PROFILE_SKIPPED;
        // sleeping on a charger is more sleep: a runtime line may turn it off only
        if (runtime && yes && !p->charger_sleep) return VRX_PROFILE_RAISE;
        p->charger_sleep = yes;
        return VRX_PROFILE_APPLIED;
    }
    if (strcmp(kind, "off") == 0) {
        int rank = off_rank(value);
        if (rank < 0) return VRX_PROFILE_SKIPPED;
        char wake[16] = "";
        if (n == 3 && strncmp(extra, "wake=", 5) == 0) {
            const char *w = extra + 5;
            if (strcmp(w, "button") == 0 || strcmp(w, "replug") == 0) snprintf(wake, sizeof wake, "%s", w);
        }
        if (runtime) {
            int now = off_rank(p->off);
            if (rank > now) return VRX_PROFILE_RAISE;
            // the same state keeps its wake: a runtime line does not re-describe it
            if (rank == now) return VRX_PROFILE_APPLIED;
        }
        snprintf(p->off, sizeof p->off, "%s", value);
        snprintf(p->off_wake, sizeof p->off_wake, "%s", rank == 2 ? wake : "");
        return VRX_PROFILE_APPLIED;
    }
    return VRX_PROFILE_SKIPPED;
}

void vrx_profile_text(struct vrx_power_profile *p, const char *text, bool runtime,
                      void (*raised)(const char *line, void *ctx), void *ctx) {
    const char *s = text;
    while (s && *s) {
        const char *end = strchr(s, '\n');
        size_t len = end ? (size_t) (end - s) : strlen(s);
        char line[256];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, s, len);
        line[len] = 0;
        line[strcspn(line, "#\r")] = 0;
        if (vrx_profile_line(p, line, runtime) == VRX_PROFILE_RAISE && raised) raised(line, ctx);
        s = end ? end + 1 : NULL;
    }
}

bool vrx_profile_finish(struct vrx_power_profile *p, bool has_wakealarm) {
    if (p->deep[0] && !has_wakealarm) {
        p->deep[0] = 0;
        return true;
    }
    return false;
}

const char *vrx_profile_sleep_offered(const struct vrx_power_profile *p, bool wake_source) {
    return p->sleep[0] && wake_source ? p->sleep : "none";
}

const char *vrx_profile_deep_offered(const struct vrx_power_profile *p) {
    return p->deep[0] ? p->deep : "none";
}

const char *vrx_cascade_refusal(const struct vrx_power_profile *p, enum vrx_cascade_step step,
                                const struct vrx_cascade_inputs *in) {
    if (step == VRX_STEP_SLEEP && !p->sleep[0]) return "the board offers no sleep";
    if (step == VRX_STEP_DEEP && !p->deep[0]) return "the board offers no deep sleep";
    if (!in->dark) return "the screen is on";
    if (in->held) return "the screen is held on";
    if (in->sleep_blocked) return "an applet holds sleep off";
    if (!in->bus) return "no system bus";
    if (step == VRX_STEP_SLEEP && !in->wake_source) return "nothing a person uses can wake the board from sleep";
    if (step == VRX_STEP_DEEP && !in->has_wakealarm) return "no RTC can wake the board";
    if (!p->charger_sleep && in->on_charger) return "on a charger";
    return NULL;
}
