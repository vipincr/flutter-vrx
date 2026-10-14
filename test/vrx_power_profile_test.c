// SPDX-License-Identifier: MIT
// The power profile and the cascade's refusal rule (vrx plans 0012 D13,
// 0020 D3, D4; G030 no-sleep-without-wake). Pure: no logind, udev or files.
#define _GNU_SOURCE
#include <string.h>

#include <unity.h>

#include "vrx_power_profile.h"

// required by Unity.
void setUp() {
}

void tearDown() {
}

static int raises;
static void count_raise(const char *line, void *ctx) {
    (void) line;
    (void) ctx;
    raises++;
}

static struct vrx_power_profile read_profile(const char *installed, const char *runtime) {
    struct vrx_power_profile p;
    vrx_profile_init(&p);
    raises = 0;
    vrx_profile_text(&p, installed, false, count_raise, NULL);
    if (runtime) vrx_profile_text(&p, runtime, true, count_raise, NULL);
    return p;
}

// everything the cascade needs to go ahead, so each test removes one thing
static struct vrx_cascade_inputs ready(void) {
    struct vrx_cascade_inputs in = {
        .dark = true, .held = false, .sleep_blocked = false, .bus = true,
        .wake_source = true, .has_wakealarm = true, .on_charger = false,
    };
    return in;
}

void test_no_suspend_without_a_wake_source() {
    struct vrx_power_profile p = read_profile("sleep s2idle saving=12% measured=2026-10-15 meter=fnb58\n", NULL);
    struct vrx_cascade_inputs in = ready();
    TEST_ASSERT_NULL(vrx_cascade_refusal(&p, VRX_STEP_SLEEP, &in));
    in.wake_source = false;
    TEST_ASSERT_NOT_NULL(vrx_cascade_refusal(&p, VRX_STEP_SLEEP, &in));
    // and Settings is told there is no sleep
    TEST_ASSERT_EQUAL_STRING("none", vrx_profile_sleep_offered(&p, false));
    TEST_ASSERT_EQUAL_STRING("s2idle", vrx_profile_sleep_offered(&p, true));
}

void test_no_halt_without_a_wake_alarm() {
    struct vrx_power_profile p = read_profile("deep halt\n", NULL);
    TEST_ASSERT_TRUE(vrx_profile_finish(&p, false));
    TEST_ASSERT_EQUAL_STRING("none", vrx_profile_deep_offered(&p));
    struct vrx_cascade_inputs in = ready();
    in.has_wakealarm = false;
    TEST_ASSERT_NOT_NULL(vrx_cascade_refusal(&p, VRX_STEP_DEEP, &in));
}

void test_deep_halt_with_a_wake_alarm_is_offered() {
    struct vrx_power_profile p = read_profile("deep halt\n", NULL);
    TEST_ASSERT_FALSE(vrx_profile_finish(&p, true));
    TEST_ASSERT_EQUAL_STRING("halt", vrx_profile_deep_offered(&p));
    struct vrx_cascade_inputs in = ready();
    TEST_ASSERT_NULL(vrx_cascade_refusal(&p, VRX_STEP_DEEP, &in));
}

void test_a_runtime_withdrawal_removes_deep_sleep() {
    struct vrx_power_profile p = read_profile("deep halt\noff standby wake=button\n", "deep none\noff halt\n");
    vrx_profile_finish(&p, true);
    TEST_ASSERT_EQUAL_STRING("none", vrx_profile_deep_offered(&p));
    TEST_ASSERT_EQUAL_STRING("halt", p.off);
    TEST_ASSERT_EQUAL_STRING("", p.off_wake);
    TEST_ASSERT_EQUAL(0, raises);
    struct vrx_cascade_inputs in = ready();
    TEST_ASSERT_NOT_NULL(vrx_cascade_refusal(&p, VRX_STEP_DEEP, &in));
}

void test_a_raising_runtime_line_is_ignored() {
    struct vrx_power_profile p = read_profile("off halt\n", "deep halt\nsleep s2idle\noff standby wake=button\ncharger-sleep yes\n");
    TEST_ASSERT_EQUAL_STRING("", p.deep);
    TEST_ASSERT_EQUAL_STRING("", p.sleep);
    TEST_ASSERT_EQUAL_STRING("halt", p.off);
    TEST_ASSERT_FALSE(p.charger_sleep);
    TEST_ASSERT_EQUAL(4, raises);
}

void test_a_runtime_line_cannot_swap_one_sleep_state_for_another() {
    struct vrx_power_profile p = read_profile("sleep s2idle saving=12% measured=2026-10-15 meter=fnb58\n", "sleep deep\n");
    TEST_ASSERT_EQUAL_STRING("s2idle", p.sleep);
    TEST_ASSERT_EQUAL(1, raises);
}

void test_off_defaults_to_halt() {
    struct vrx_power_profile p = read_profile("deep halt\n", NULL);
    TEST_ASSERT_EQUAL_STRING("halt", p.off);
    TEST_ASSERT_EQUAL_STRING("", p.off_wake);
    struct vrx_power_profile empty = read_profile("", NULL);
    TEST_ASSERT_EQUAL_STRING("halt", empty.off);
}

void test_off_standby_is_lowered_to_halt_by_the_device() {
    struct vrx_power_profile p = read_profile("off standby wake=replug\n", NULL);
    TEST_ASSERT_EQUAL_STRING("standby", p.off);
    TEST_ASSERT_EQUAL_STRING("replug", p.off_wake);
    vrx_profile_text(&p, "off halt # the bootloader keeps its rails up\n", true, count_raise, NULL);
    TEST_ASSERT_EQUAL_STRING("halt", p.off);
    TEST_ASSERT_EQUAL_STRING("", p.off_wake);
}

void test_a_runtime_line_keeps_the_wake_of_the_same_state() {
    struct vrx_power_profile p = read_profile("off standby wake=button\n", "off standby wake=replug\n");
    TEST_ASSERT_EQUAL_STRING("standby", p.off);
    TEST_ASSERT_EQUAL_STRING("button", p.off_wake);
}

void test_a_carrier_narrows_its_family() {
    // installed lines replace earlier ones: the carrier's file comes after the family's
    struct vrx_power_profile p = read_profile("sleep s2idle saving=12% measured=x meter=y\ndeep halt\n", NULL);
    vrx_profile_text(&p, "sleep none\ndeep none\n", false, count_raise, NULL);
    TEST_ASSERT_EQUAL_STRING("", p.sleep);
    TEST_ASSERT_EQUAL_STRING("", p.deep);
}

void test_other_kinds_and_unknown_values_are_skipped() {
    struct vrx_power_profile p = read_profile(
        "model floor 1414 measured=2026-10-14 meter=fnb58\nwalk-budget p95_frame_ms 16.7\n"
        "deep sometimes\noff maybe\ncharger-sleep perhaps\n# deep halt\n",
        NULL);
    TEST_ASSERT_EQUAL_STRING("", p.deep);
    TEST_ASSERT_EQUAL_STRING("halt", p.off);
    TEST_ASSERT_FALSE(p.charger_sleep);
}

void test_refusals_before_the_wake_checks() {
    struct vrx_power_profile p = read_profile("sleep s2idle saving=12% measured=x meter=y\n", NULL);
    struct vrx_cascade_inputs in = ready();
    in.dark = false;
    TEST_ASSERT_NOT_NULL(vrx_cascade_refusal(&p, VRX_STEP_SLEEP, &in));
    in = ready();
    in.held = true;
    TEST_ASSERT_NOT_NULL(vrx_cascade_refusal(&p, VRX_STEP_SLEEP, &in));
    in = ready();
    in.sleep_blocked = true;
    TEST_ASSERT_NOT_NULL(vrx_cascade_refusal(&p, VRX_STEP_SLEEP, &in));
    in = ready();
    in.on_charger = true;
    TEST_ASSERT_NOT_NULL(vrx_cascade_refusal(&p, VRX_STEP_SLEEP, &in));
    vrx_profile_text(&p, "charger-sleep yes\n", false, NULL, NULL);
    TEST_ASSERT_NULL(vrx_cascade_refusal(&p, VRX_STEP_SLEEP, &in));
}

void test_a_step_the_board_does_not_offer_is_refused() {
    struct vrx_power_profile p = read_profile("", NULL);
    struct vrx_cascade_inputs in = ready();
    TEST_ASSERT_NOT_NULL(vrx_cascade_refusal(&p, VRX_STEP_SLEEP, &in));
    TEST_ASSERT_NOT_NULL(vrx_cascade_refusal(&p, VRX_STEP_DEEP, &in));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_no_suspend_without_a_wake_source);
    RUN_TEST(test_no_halt_without_a_wake_alarm);
    RUN_TEST(test_deep_halt_with_a_wake_alarm_is_offered);
    RUN_TEST(test_a_runtime_withdrawal_removes_deep_sleep);
    RUN_TEST(test_a_raising_runtime_line_is_ignored);
    RUN_TEST(test_a_runtime_line_cannot_swap_one_sleep_state_for_another);
    RUN_TEST(test_off_defaults_to_halt);
    RUN_TEST(test_off_standby_is_lowered_to_halt_by_the_device);
    RUN_TEST(test_a_runtime_line_keeps_the_wake_of_the_same_state);
    RUN_TEST(test_a_carrier_narrows_its_family);
    RUN_TEST(test_other_kinds_and_unknown_values_are_skipped);
    RUN_TEST(test_refusals_before_the_wake_checks);
    RUN_TEST(test_a_step_the_board_does_not_offer_is_refused);
    return UNITY_END();
}
