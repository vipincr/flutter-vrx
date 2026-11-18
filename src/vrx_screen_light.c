// SPDX-License-Identifier: MIT
#include "vrx_screen_light.h"

#include <stdlib.h>
#include <string.h>

static void show(struct vrx_screen_light *sl, uint16_t v) {
    if (v == sl->shown) return;
    sl->shown = v;
    sl->ops.apply(sl->ops.ctx, v);
}

void vrx_screen_light_open(struct vrx_screen_light *sl, const struct vrx_screen_light_ops *ops,
                           const struct vrx_light *map, uint16_t floor) {
    *sl = (struct vrx_screen_light){ .ops = *ops, .floor = floor, .cap = 1000 };
    int level = map ? ops->read_level(ops->ctx) : -1;
    sl->shown = map && level > 0 ? vrx_light_permille(map, level) : 0;
    int stored = ops->read_stored(ops->ctx);
    if (stored < 0 || stored > (int) VRX_LIGHT_FULL) {
        // first start without the file: the device's own level is what the person had
        stored = sl->shown >= floor ? sl->shown : (int) VRX_SCREEN_LIGHT_DEFAULT;
        ops->write_stored(ops->ctx, (uint16_t) stored);
    }
    sl->stored = (uint16_t) stored;
    vrx_light_fade_init(&sl->fade, sl->shown);
}

uint16_t vrx_screen_light_target(const struct vrx_screen_light *sl) {
    uint16_t v = sl->stored < sl->floor ? sl->floor : sl->stored;
    uint16_t ceiling = (uint16_t) (sl->floor + ((uint32_t) (VRX_LIGHT_FULL - sl->floor) * sl->cap + 500) / 1000);
    v = v > ceiling ? ceiling : v;
    // the splash's ceiling, never under the floor and never above what the person has
    if (sl->limit && v > sl->limit) v = sl->limit < sl->floor ? sl->floor : sl->limit;
    return v;
}

static void retarget(struct vrx_screen_light *sl, uint32_t now_ms, uint32_t ms) {
    vrx_light_fade_to(&sl->fade, now_ms, sl->lit ? vrx_screen_light_target(sl) : 0, ms);
    show(sl, vrx_light_fade_step(&sl->fade, now_ms));
}

void vrx_screen_light_go(struct vrx_screen_light *sl, uint32_t now_ms, bool lit, uint32_t ms) {
    sl->lit = lit;
    retarget(sl, now_ms, ms);
}

void vrx_screen_light_set(struct vrx_screen_light *sl, uint32_t now_ms, uint16_t permille) {
    if (permille > VRX_LIGHT_FULL) permille = VRX_LIGHT_FULL;
    if (permille != sl->stored) {
        sl->stored = permille;
        sl->ops.write_stored(sl->ops.ctx, permille);
    }
    if (sl->lit) retarget(sl, now_ms, 0);
}

void vrx_screen_light_limit(struct vrx_screen_light *sl, uint32_t now_ms, uint16_t limit, uint32_t ms) {
    sl->limit = limit > VRX_LIGHT_FULL ? VRX_LIGHT_FULL : limit;
    if (sl->lit) retarget(sl, now_ms, ms);
}

void vrx_screen_light_cap(struct vrx_screen_light *sl, uint32_t now_ms, uint16_t cap, uint32_t ms) {
    sl->cap = cap > 1000 ? 1000 : cap;
    if (sl->lit) retarget(sl, now_ms, ms);
}

bool vrx_screen_light_tick(struct vrx_screen_light *sl, uint32_t now_ms) {
    show(sl, vrx_light_fade_step(&sl->fade, now_ms));
    return sl->fade.active;
}

bool vrx_screen_light_is_dark(const struct vrx_screen_light *sl) {
    return !sl->fade.active && sl->shown == 0;
}

// The Groups: line of /proc/<pid>/status lists the gids of the process, supplementary ones included.
bool vrx_status_has_group(const char *status, unsigned gid) {
    const char *line = status;
    while (line && *line) {
        if (strncmp(line, "Groups:", 7) == 0) {
            const char *p = line + 7;
            while (*p && *p != '\n') {
                while (*p == ' ' || *p == '\t') p++;
                char *end;
                unsigned long g = strtoul(p, &end, 10);
                if (end == p) break;
                if (g == gid) return true;
                p = end;
            }
            return false;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return false;
}
