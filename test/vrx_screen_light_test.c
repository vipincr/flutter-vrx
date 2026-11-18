// SPDX-License-Identifier: MIT
// The compositor's light (vrx plan 0033 D4, D5, D1): the stored brightness and its one-time migration,
// the fade out before the first frame, the fade in after it, and a retarget midway. A fake device, file and clock.
#include <stdlib.h>
#include <string.h>

#include <unity.h>

#include "vrx_light_standard.h"
#include "vrx_screen_light.h"

#define FLOOR VRX_STD_LIGHT_FLOOR_PERMILLE

static struct {
    int level;            // the device's level the migration reads, -1 unreadable
    int stored;           // the file: -1 missing or garbage
    int writes;           // writes of the file
    uint16_t shown[512];  // every apply, in order
    int applies;
} g;

static int read_level(void *ctx) { (void) ctx; return g.level; }
static int read_stored(void *ctx) { (void) ctx; return g.stored; }
static int write_stored(void *ctx, uint16_t v) { (void) ctx; g.stored = v; g.writes++; return 0; }
static void apply(void *ctx, uint16_t v) { (void) ctx; g.shown[g.applies++ % 512] = v; }

static struct vrx_light map;
static struct vrx_screen_light sl;

static const struct vrx_screen_light_ops ops = { read_level, read_stored, write_stored, apply, NULL };

void setUp(void) {
    memset(&g, 0, sizeof g);
    g.stored = -1;
    // a kernel CIE table: the level index is already perceived
    vrx_light_init_scale(&map, VRX_LIGHT_SCALE_PERCEIVED, 1000);
}
void tearDown(void) {}

static uint16_t last_shown(void) { return g.shown[(g.applies - 1) % 512]; }

// Step every 16 ms to `until_ms`, as the compositor's timer does.
static void run(uint32_t from_ms, uint32_t until_ms) {
    for (uint32_t t = from_ms + 16; t <= until_ms; t += 16) vrx_screen_light_tick(&sl, t);
    vrx_screen_light_tick(&sl, until_ms);
}

static void assert_monotone(int from, int to, bool down) {
    for (int i = from + 1; i < to; i++) {
        if (down) TEST_ASSERT_TRUE(g.shown[i] < g.shown[i - 1]);
        else TEST_ASSERT_TRUE(g.shown[i] > g.shown[i - 1]);
    }
}

// --- migration -----------------------------------------------------------------------------------------------

void test_a_stored_number_is_kept_and_not_rewritten(void) {
    g.stored = 700;
    g.level = 300;
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    TEST_ASSERT_EQUAL(700, sl.stored);
    TEST_ASSERT_EQUAL(0, g.writes);
}

void test_no_file_takes_the_level_through_the_curve_inverse(void) {
    g.level = 640;  // a perceived level: 640/1000
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    TEST_ASSERT_EQUAL(640, sl.stored);
    TEST_ASSERT_EQUAL(640, g.stored);
    TEST_ASSERT_EQUAL(1, g.writes);
    TEST_ASSERT_EQUAL(640, sl.shown);
}

void test_a_linear_device_level_is_migrated_through_the_curve(void) {
    // level 128 of 255 on a linear light is half the luminance: L* 76.1 (Y = ((L+16)/116)^3 = 0.5), not 500
    vrx_light_init_scale(&map, VRX_LIGHT_SCALE_LINEAR, 255);
    g.level = 128;
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    TEST_ASSERT_INT_WITHIN(3, 761, sl.stored);
}

void test_no_file_and_a_dark_or_unreadable_light_stores_the_default(void) {
    g.level = 0;
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    TEST_ASSERT_EQUAL(VRX_SCREEN_LIGHT_DEFAULT, sl.stored);
    TEST_ASSERT_EQUAL(0, sl.shown);
    g.stored = -1;
    g.level = -1;
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    TEST_ASSERT_EQUAL(VRX_SCREEN_LIGHT_DEFAULT, sl.stored);
}

void test_no_file_and_a_level_below_the_floor_stores_the_default(void) {
    g.level = FLOOR - 20;  // a fade-in caught mid-way, or a nearly dark light
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    TEST_ASSERT_EQUAL(VRX_SCREEN_LIGHT_DEFAULT, sl.stored);
}

void test_a_screen_with_no_map_starts_dark(void) {
    vrx_screen_light_open(&sl, &ops, NULL, FLOOR);
    TEST_ASSERT_EQUAL(0, sl.shown);
    TEST_ASSERT_EQUAL(VRX_SCREEN_LIGHT_DEFAULT, sl.stored);
}

// --- the handoff -----------------------------------------------------------------------------------------------

void test_the_light_is_down_before_the_first_frame_and_up_after(void) {
    g.stored = 800;
    g.level = 500;  // the splash's light
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    // the compositor starts: fade out from where the light is
    vrx_screen_light_go(&sl, 1000, false, VRX_STD_THROUGH_DARK_OUT_MS);
    TEST_ASSERT_FALSE(vrx_screen_light_is_dark(&sl));
    run(1000, 1000 + VRX_STD_THROUGH_DARK_OUT_MS / 2);
    TEST_ASSERT_FALSE(vrx_screen_light_is_dark(&sl));  // the first frame still waits
    int down_end = g.applies;
    run(1000 + VRX_STD_THROUGH_DARK_OUT_MS / 2, 1000 + VRX_STD_THROUGH_DARK_OUT_MS);
    TEST_ASSERT_TRUE(vrx_screen_light_is_dark(&sl));
    TEST_ASSERT_EQUAL(0, last_shown());
    assert_monotone(0, g.applies, true);
    // the first frame is committed: fade in to the stored level
    int up_start = g.applies;
    vrx_screen_light_go(&sl, 2000, true, VRX_STD_THROUGH_DARK_IN_MS);
    run(2000, 2000 + VRX_STD_THROUGH_DARK_IN_MS);
    TEST_ASSERT_EQUAL(800, last_shown());
    TEST_ASSERT_FALSE(sl.fade.active);
    assert_monotone(up_start, g.applies, false);
    TEST_ASSERT_TRUE(down_end > 2);
}

void test_the_fade_out_starts_from_the_real_level_not_from_full(void) {
    g.stored = 800;
    g.level = 300;
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    vrx_screen_light_go(&sl, 0, false, 150);
    vrx_screen_light_tick(&sl, 16);
    TEST_ASSERT_TRUE(g.shown[0] < 300);
}

void test_a_retarget_midway_goes_from_where_the_light_is(void) {
    g.stored = 900;
    g.level = 900;
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    vrx_screen_light_go(&sl, 0, false, 150);
    vrx_screen_light_tick(&sl, 75);
    uint16_t mid = last_shown();
    TEST_ASSERT_INT_WITHIN(5, 450, mid);
    // screen on again at 75 ms: up from the middle, never a jump to 0 or 900
    vrx_screen_light_go(&sl, 75, true, 250);
    TEST_ASSERT_EQUAL(mid, last_shown());
    vrx_screen_light_tick(&sl, 91);
    TEST_ASSERT_TRUE(last_shown() > mid && last_shown() < 900);
    run(91, 75 + 250);
    TEST_ASSERT_EQUAL(900, last_shown());
}

void test_the_later_request_wins(void) {
    g.stored = 600;
    g.level = 600;
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    vrx_screen_light_go(&sl, 0, false, 150);
    vrx_screen_light_go(&sl, 20, true, 250);
    vrx_screen_light_go(&sl, 40, false, 150);
    run(40, 40 + 150);
    TEST_ASSERT_TRUE(vrx_screen_light_is_dark(&sl));
}

void test_a_screen_that_cannot_fade_cuts(void) {
    g.stored = 600;
    vrx_screen_light_open(&sl, &ops, NULL, FLOOR);
    vrx_screen_light_go(&sl, 0, true, 0);
    TEST_ASSERT_EQUAL(1, g.applies);
    TEST_ASSERT_EQUAL(600, last_shown());
    TEST_ASSERT_FALSE(sl.fade.active);
    vrx_screen_light_go(&sl, 10, false, 0);
    TEST_ASSERT_EQUAL(0, last_shown());
    TEST_ASSERT_TRUE(vrx_screen_light_is_dark(&sl));
}

void test_the_clock_may_wrap(void) {
    g.stored = 600;
    g.level = 600;
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    uint32_t t0 = 0xFFFFFFFFu - 40;
    vrx_screen_light_go(&sl, t0, false, 150);
    run(t0, t0 + 150);
    TEST_ASSERT_TRUE(vrx_screen_light_is_dark(&sl));
}

// --- the person's brightness -------------------------------------------------------------------------------------

void test_set_while_lit_is_shown_at_once_and_stored(void) {
    g.stored = 500;
    g.level = 500;
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    vrx_screen_light_go(&sl, 0, true, 0);
    vrx_screen_light_set(&sl, 10, 300);
    TEST_ASSERT_EQUAL(300, last_shown());
    TEST_ASSERT_EQUAL(300, g.stored);
    vrx_screen_light_set(&sl, 20, 300);
    TEST_ASSERT_EQUAL(1, g.writes);  // an unchanged number is not rewritten
}

void test_the_resume_limit_holds_the_light_under_the_person_and_over_the_floor(void) {
    g.stored = 900;
    g.level = 0;
    vrx_screen_light_open(&sl, &ops, &map, FLOOR);
    vrx_screen_light_limit(&sl, 0, VRX_STD_RESUME_SPLASH_LEVEL_PERMILLE, 0);
    vrx_screen_light_go(&sl, 0, true, 0);
    TEST_ASSERT_EQUAL_MESSAGE(VRX_STD_RESUME_SPLASH_LEVEL_PERMILLE, last_shown(), "lit to the splash's level, under the person's 900");
    vrx_screen_light_limit(&sl, 10, 0, 0);
    TEST_ASSERT_EQUAL_MESSAGE(900, last_shown(), "lifted: the person's own");
    vrx_screen_light_set(&sl, 20, 300);
    vrx_screen_light_limit(&sl, 30, VRX_STD_RESUME_SPLASH_LEVEL_PERMILLE, 0);
    TEST_ASSERT_EQUAL_MESSAGE(300, last_shown(), "a limit above the person's brightness changes nothing");
    vrx_screen_light_limit(&sl, 40, 10, 0);
    TEST_ASSERT_EQUAL_MESSAGE(FLOOR, last_shown(), "and never under the lit floor");
}

void test_set_while_dark_is_stored_and_shows_nothing(void) {
    g.stored = 500;
    vrx_screen_light_open(&sl, &ops, NULL, FLOOR);
    vrx_screen_light_set(&sl, 0, 700);
    TEST_ASSERT_EQUAL(700, g.stored);
    TEST_ASSERT_EQUAL(0, g.applies);
    vrx_screen_light_go(&sl, 5, true, 0);
    TEST_ASSERT_EQUAL(700, last_shown());
}

void test_a_lit_screen_never_goes_under_the_floor(void) {
    g.stored = 500;
    vrx_screen_light_open(&sl, &ops, NULL, FLOOR);
    vrx_screen_light_set(&sl, 0, 0);
    vrx_screen_light_go(&sl, 0, true, 0);
    TEST_ASSERT_EQUAL(FLOOR, last_shown());
    TEST_ASSERT_EQUAL(0, g.stored);  // the person's number is kept as set
    TEST_ASSERT_EQUAL(FLOOR, vrx_screen_light_target(&sl));
}

void test_the_cap_is_a_share_of_the_range_above_the_floor(void) {
    g.stored = 1000;
    vrx_screen_light_open(&sl, &ops, NULL, FLOOR);
    vrx_screen_light_go(&sl, 0, true, 0);
    vrx_screen_light_cap(&sl, 0, 0, 0);
    TEST_ASSERT_EQUAL(FLOOR, last_shown());  // dimmed: the floor
    vrx_screen_light_cap(&sl, 0, 500, 0);
    TEST_ASSERT_INT_WITHIN(1, FLOOR + (1000 - FLOOR) / 2, last_shown());
    vrx_screen_light_cap(&sl, 0, 1000, 0);
    TEST_ASSERT_EQUAL(1000, last_shown());
    // a person below the cap is not raised to it
    vrx_screen_light_set(&sl, 0, 200);
    vrx_screen_light_cap(&sl, 0, 500, 0);
    TEST_ASSERT_EQUAL(200, last_shown());
}

void test_a_cap_over_a_fade_in_is_the_same_fade(void) {
    g.stored = 1000;
    vrx_screen_light_open(&sl, &ops, NULL, FLOOR);
    vrx_screen_light_go(&sl, 0, true, 250);
    vrx_screen_light_cap(&sl, 100, 0, 150);  // low power arrives during the fade-in
    run(100, 100 + 150);
    TEST_ASSERT_EQUAL(FLOOR, last_shown());
}

// --- the display grant ----------------------------------------------------------------------------------------

void test_the_grant_is_read_from_the_groups_line(void) {
    const char *status = "Name:\tapp\nGid:\t1000\t1000\t1000\t1000\nGroups:\t44 105 968 1000 \nVmPeak:\t1 kB\n";
    TEST_ASSERT_TRUE(vrx_status_has_group(status, 968));
    TEST_ASSERT_TRUE(vrx_status_has_group(status, 44));
    TEST_ASSERT_FALSE(vrx_status_has_group(status, 96));    // a prefix of a gid is not the gid
    TEST_ASSERT_FALSE(vrx_status_has_group(status, 9680));
    TEST_ASSERT_FALSE(vrx_status_has_group("Name:\tapp\nGroups:\t\n", 968));
    TEST_ASSERT_FALSE(vrx_status_has_group("Name:\tapp\n", 968));
    TEST_ASSERT_FALSE(vrx_status_has_group("", 968));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_stored_number_is_kept_and_not_rewritten);
    RUN_TEST(test_no_file_takes_the_level_through_the_curve_inverse);
    RUN_TEST(test_a_linear_device_level_is_migrated_through_the_curve);
    RUN_TEST(test_no_file_and_a_dark_or_unreadable_light_stores_the_default);
    RUN_TEST(test_no_file_and_a_level_below_the_floor_stores_the_default);
    RUN_TEST(test_a_screen_with_no_map_starts_dark);
    RUN_TEST(test_the_light_is_down_before_the_first_frame_and_up_after);
    RUN_TEST(test_the_fade_out_starts_from_the_real_level_not_from_full);
    RUN_TEST(test_a_retarget_midway_goes_from_where_the_light_is);
    RUN_TEST(test_the_later_request_wins);
    RUN_TEST(test_a_screen_that_cannot_fade_cuts);
    RUN_TEST(test_the_clock_may_wrap);
    RUN_TEST(test_set_while_lit_is_shown_at_once_and_stored);
    RUN_TEST(test_the_resume_limit_holds_the_light_under_the_person_and_over_the_floor);
    RUN_TEST(test_set_while_dark_is_stored_and_shows_nothing);
    RUN_TEST(test_a_lit_screen_never_goes_under_the_floor);
    RUN_TEST(test_the_cap_is_a_share_of_the_range_above_the_floor);
    RUN_TEST(test_a_cap_over_a_fade_in_is_the_same_fade);
    RUN_TEST(test_the_grant_is_read_from_the_groups_line);
    return UNITY_END();
}
