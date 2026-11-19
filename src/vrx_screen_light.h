// SPDX-License-Identifier: MIT
// The screen's light, as the compositor owns it (vrx plan 0033 D1, D4): the person's brightness as one
// perceived per-mille number, the fade between what is shown and what is wanted, and the one-time migration
// from the level an older image left in sysfs. Pure: the device, the stored file and the clock are the
// caller's, so the sequence is testable on a host. The curve is libvrx-light's (D2); nothing here knows a level.
#ifndef VRX_SCREEN_LIGHT_H
#define VRX_SCREEN_LIGHT_H

#include <stdbool.h>
#include <stdint.h>

#include <vrx_light/vrx_light.h>

struct vrx_screen_light_ops {
    // the device's own level now, or < 0 (migration only)
    int (*read_level)(void *ctx);
    // the stored brightness, 0..1000, or < 0 when the file is missing or not a number in range
    int (*read_stored)(void *ctx);
    // 0, or -errno
    int (*write_stored)(void *ctx, uint16_t permille);
    // show this perceived brightness (0 = dark): called once per change of what is shown
    void (*apply)(void *ctx, uint16_t shown);
    void *ctx;
};

struct vrx_screen_light {
    struct vrx_screen_light_ops ops;
    uint16_t floor;           // the dimmest a lit screen goes, per mille (the standard's [light])
    uint16_t stored;          // the person's brightness, per mille
    uint16_t cap;             // vrxhost's cap: per mille of the range above the floor, 1000 = none
    uint16_t limit;           // a ceiling on what is lit, per mille (the resume's splash level); 0 = none
    uint16_t shown;           // what apply() last showed
    bool store_due;           // the person's brightness changed and is not yet in the file (written when the fade settles)
    bool lit;                 // wanted lit (at the person's level, under the cap), else dark
    struct vrx_light_fade fade;
};

// What a missing file is read as when the device's level says nothing (the curve's middle).
#define VRX_SCREEN_LIGHT_DEFAULT 500u

// Open: read the stored brightness; with none, take it from the device's level through `map`
// (vrx_light_permille) and write it once, or VRX_SCREEN_LIGHT_DEFAULT when that level is dark or
// below the floor. `shown` starts at what the device shows now (0 with no `map`), so a fade begins
// from the real light. Not lit until vrx_screen_light_go(). `map` may be NULL (a screen with no level).
void vrx_screen_light_open(struct vrx_screen_light *sl, const struct vrx_screen_light_ops *ops,
                           const struct vrx_light *map, uint16_t floor);

// The brightness lit at: the stored one, held to [floor, 1000] and under the cap.
uint16_t vrx_screen_light_target(const struct vrx_screen_light *sl);

// Go lit or dark over `ms` from where the light is now (0: at once). The later request wins.
void vrx_screen_light_go(struct vrx_screen_light *sl, uint32_t now_ms, bool lit, uint32_t ms);

// The person set a brightness: the light glides to it from where it is over the standard's [light] change_ms, a sample that lands
// mid-glide retargets it from the level shown then, and the number is written once, when the glide has settled (at once when dark).
void vrx_screen_light_set(struct vrx_screen_light *sl, uint32_t now_ms, uint16_t permille);

// A ceiling on the lit brightness (the resume's splash is lit to its own level, under the person's), 0 to lift it: over `ms` when lit.
void vrx_screen_light_limit(struct vrx_screen_light *sl, uint32_t now_ms, uint16_t limit, uint32_t ms);

// vrxhost's cap changed: over `ms` when lit.
void vrx_screen_light_cap(struct vrx_screen_light *sl, uint32_t now_ms, uint16_t cap, uint32_t ms);

// Step the fade to `now_ms` and show it. True while a fade is still running (the caller keeps its timer).
bool vrx_screen_light_tick(struct vrx_screen_light *sl, uint32_t now_ms);

// No fade runs and nothing is shown.
bool vrx_screen_light_is_dark(const struct vrx_screen_light *sl);

// Whether `status` (the text of /proc/<pid>/status) lists `gid` among the process's groups: the display
// grant is the group vrx-display, and the connection's own credentials do not carry supplementary groups.
bool vrx_status_has_group(const char *status, unsigned gid);

#endif
