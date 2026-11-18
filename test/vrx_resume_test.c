// SPDX-License-Identifier: MIT
// The compositor's resume (vrx plan 0034 D1, D3, D5): the splash drawn in the dark before the system sleeps, lit on the wake,
// held until a Facts reading, the input devices and a clock source are back (or the bound), then the dip into the UI. The splash
// frame's file: exactly w * h * 4 bytes of rows top to bottom, anything else the page colour and said once.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <unity.h>

#include "vrx_light_standard.h"
#include "vrx_resume.h"

#define MAX VRX_STD_RESUME_ESSENTIALS_MAX_MS

static struct vrx_resume r;

void setUp(void) { vrx_resume_init(&r, MAX); }
void tearDown(void) {}

// a sleep, its dark and its wake, with 2 input devices held
static void asleep(void) {
    vrx_resume_sleep(&r, 2);
    TEST_ASSERT_EQUAL(VRX_RESUME_DRAW_SPLASH, vrx_resume_dark(&r));
}

static void test_the_splash_is_drawn_when_the_light_is_down_and_not_before(void) {
    vrx_resume_sleep(&r, 2);
    TEST_ASSERT_EQUAL(VRX_RESUME_PARKING, r.stage);
    TEST_ASSERT_EQUAL(VRX_RESUME_DRAW_SPLASH, vrx_resume_dark(&r));
    TEST_ASSERT_EQUAL(VRX_RESUME_ASLEEP, r.stage);
    TEST_ASSERT_EQUAL_MESSAGE(VRX_RESUME_NOTHING, vrx_resume_dark(&r), "drawn once");
}

static void test_a_screen_going_dark_with_no_sleep_draws_nothing(void) {
    TEST_ASSERT_EQUAL(VRX_RESUME_NOTHING, vrx_resume_dark(&r));
}

static void test_a_wake_waits_for_all_three(void) {
    asleep();
    TEST_ASSERT_EQUAL(VRX_RESUME_LIGHT_SPLASH, vrx_resume_wake(&r, 1000, 2));
    TEST_ASSERT_EQUAL(VRX_RESUME_NOTHING, vrx_resume_check(&r, 1000));
    vrx_resume_report(&r, true, false);
    TEST_ASSERT_EQUAL_MESSAGE(VRX_RESUME_NOTHING, vrx_resume_check(&r, 1100), "facts alone");
    vrx_resume_report(&r, false, true);
    TEST_ASSERT_EQUAL_MESSAGE(VRX_RESUME_DIP, vrx_resume_check(&r, 1200), "facts, input and clock");
    TEST_ASSERT_FALSE(r.clock_unknown);
    TEST_ASSERT_EQUAL_MESSAGE(VRX_RESUME_NOTHING, vrx_resume_check(&r, 1300), "the dip is asked for once");
}

static void test_the_clock_is_not_the_facts(void) {
    // the clock and the input back, the facts not: the splash stays until the facts arrive
    asleep();
    vrx_resume_wake(&r, 0, 2);
    vrx_resume_report(&r, false, true);
    TEST_ASSERT_EQUAL_MESSAGE(VRX_RESUME_NOTHING, vrx_resume_check(&r, 100), "facts missing");
    vrx_resume_report(&r, true, false);
    TEST_ASSERT_EQUAL_MESSAGE(VRX_RESUME_DIP, vrx_resume_check(&r, 200), "and then it goes");
}

static void test_input_missing_holds_until_it_returns(void) {
    asleep();
    vrx_resume_wake(&r, 0, 1);                         // one of the two devices is not back
    vrx_resume_report(&r, true, true);
    TEST_ASSERT_EQUAL(VRX_RESUME_NOTHING, vrx_resume_check(&r, 10));
    vrx_resume_inputs(&r, 2);
    TEST_ASSERT_EQUAL(VRX_RESUME_DIP, vrx_resume_check(&r, 20));
}

static void test_the_bound_ends_the_wait_and_marks_the_clock_unknown(void) {
    asleep();
    vrx_resume_wake(&r, 5000, 2);
    vrx_resume_report(&r, true, false);
    TEST_ASSERT_EQUAL(VRX_RESUME_NOTHING, vrx_resume_check(&r, 5000 + MAX - 1));
    TEST_ASSERT_EQUAL(VRX_RESUME_DIP, vrx_resume_check(&r, 5000 + MAX));
    TEST_ASSERT_TRUE(r.clock_unknown);
}

static void test_the_bound_with_the_facts_missing_leaves_a_known_clock_known(void) {
    asleep();
    vrx_resume_wake(&r, 0, 2);
    vrx_resume_report(&r, false, true);
    TEST_ASSERT_EQUAL(VRX_RESUME_DIP, vrx_resume_check(&r, MAX));
    TEST_ASSERT_FALSE(r.clock_unknown);
}

static void test_a_report_from_before_the_wake_is_not_counted(void) {
    asleep();
    vrx_resume_report(&r, true, true);                 // the kit's reading from before the sleep
    vrx_resume_wake(&r, 0, 2);
    TEST_ASSERT_EQUAL(VRX_RESUME_NOTHING, vrx_resume_check(&r, 1));
}

static void test_the_dip_is_followed_by_the_ui(void) {
    asleep();
    vrx_resume_wake(&r, 0, 2);
    vrx_resume_report(&r, true, true);
    TEST_ASSERT_EQUAL(VRX_RESUME_DIP, vrx_resume_check(&r, 1));
    TEST_ASSERT_EQUAL(VRX_RESUME_SHOW_UI, vrx_resume_dipped(&r));
    TEST_ASSERT_EQUAL(VRX_RESUME_IDLE, r.stage);
    TEST_ASSERT_EQUAL_MESSAGE(VRX_RESUME_NOTHING, vrx_resume_dipped(&r), "once");
}

static void test_a_wake_with_no_splash_is_the_old_wake(void) {
    vrx_resume_sleep(&r, 2);                           // woke before the light was down: no splash was drawn
    TEST_ASSERT_EQUAL(VRX_RESUME_NOTHING, vrx_resume_wake(&r, 0, 2));
    TEST_ASSERT_EQUAL(VRX_RESUME_IDLE, r.stage);
}

static void test_the_time_left_runs_to_the_bound(void) {
    asleep();
    vrx_resume_wake(&r, 100, 2);
    TEST_ASSERT_EQUAL_UINT32(MAX, vrx_resume_left(&r, 100));
    TEST_ASSERT_EQUAL_UINT32(MAX - 400, vrx_resume_left(&r, 500));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, vrx_resume_left(&r, 100 + MAX + 50), "past it: a tick, not zero (zero is not waiting)");
}

// --- the splash file ---

static char dir[64];
static int said;
static void say(const char *what, void *ctx) { (void) what; (void) ctx; said++; }

static void write_file(const char *name, size_t bytes, unsigned seed) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    for (size_t i = 0; f && i < bytes; i++) fputc((int) ((seed + i) & 0xff), f);
    if (f) fclose(f);
}

static void test_the_splash_file(void) {
    char path[160];
    TEST_ASSERT_TRUE(vrx_resume_splash_path(path, sizeof path, "/usr/share/vrx/splash", 800, 480, "XR24"));
    TEST_ASSERT_EQUAL_STRING("/usr/share/vrx/splash/800x480-XR24.raw", path);
    TEST_ASSERT_FALSE_MESSAGE(vrx_resume_splash_path(path, 8, "/usr/share/vrx/splash", 800, 480, "XR24"), "not cut");

    snprintf(dir, sizeof dir, "/tmp/vrx-resume-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    enum { W = 6, H = 3, STRIDE = 8 };
    uint32_t px[STRIDE * H];

    said = 0;
    TEST_ASSERT_FALSE(vrx_resume_splash_fill(px, STRIDE, W, H, dir, 0x112233, say, NULL));
    TEST_ASSERT_EQUAL_MESSAGE(1, said, "a missing file is said once");
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) TEST_ASSERT_EQUAL_HEX32(0x112233, px[y * STRIDE + x]);

    write_file("6x3-XR24.raw", W * H * 4 - 4, 1);
    said = 0;
    TEST_ASSERT_FALSE_MESSAGE(vrx_resume_splash_fill(px, STRIDE, W, H, dir, 0x112233, say, NULL), "a pixel short");
    TEST_ASSERT_EQUAL(1, said);
    write_file("6x3-XR24.raw", W * H * 4 + 4, 1);
    TEST_ASSERT_FALSE_MESSAGE(vrx_resume_splash_fill(px, STRIDE, W, H, dir, 0x112233, say, NULL), "a pixel long");

    write_file("6x3-XR24.raw", W * H * 4, 9);
    TEST_ASSERT_TRUE(vrx_resume_splash_fill(px, STRIDE, W, H, dir, 0x112233, say, NULL));
    // rows top to bottom with no header, into the buffer's own stride
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) {
            size_t i = ((size_t) y * W + x) * 4;
            uint32_t want = (uint32_t) ((9 + i) & 0xff) | (uint32_t) ((9 + i + 1) & 0xff) << 8 | (uint32_t) ((9 + i + 2) & 0xff) << 16 |
                            (uint32_t) ((9 + i + 3) & 0xff) << 24;
            TEST_ASSERT_EQUAL_HEX32(want, px[y * STRIDE + x]);
        }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_splash_is_drawn_when_the_light_is_down_and_not_before);
    RUN_TEST(test_a_screen_going_dark_with_no_sleep_draws_nothing);
    RUN_TEST(test_a_wake_waits_for_all_three);
    RUN_TEST(test_the_clock_is_not_the_facts);
    RUN_TEST(test_input_missing_holds_until_it_returns);
    RUN_TEST(test_the_bound_ends_the_wait_and_marks_the_clock_unknown);
    RUN_TEST(test_the_bound_with_the_facts_missing_leaves_a_known_clock_known);
    RUN_TEST(test_a_report_from_before_the_wake_is_not_counted);
    RUN_TEST(test_the_dip_is_followed_by_the_ui);
    RUN_TEST(test_a_wake_with_no_splash_is_the_old_wake);
    RUN_TEST(test_the_time_left_runs_to_the_bound);
    RUN_TEST(test_the_splash_file);
    return UNITY_END();
}
