// SPDX-License-Identifier: MIT
// The compositor's VT hold (plan 0028 D2). Pure: a fake ioctl stands in for the kernel.
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/kd.h>
#include <string.h>
#include <unistd.h>

#include <unity.h>

#include "vrx_vt.h"

static long g_mode, g_kb;
static int g_log[16], g_logn;
static int g_fail_req;  // a request the fake refuses (0: none)

static int fake_ioctl(int fd, unsigned long req, void *arg) {
    (void) fd;
    if ((int) req == g_fail_req) return -1;
    g_log[g_logn++ % 16] = (int) req;
    switch (req) {
        case KDGETMODE: *(long *) arg = g_mode; return 0;
        case KDGKBMODE: *(int *) arg = (int) g_kb; return 0;
        case KDSETMODE: g_mode = (long) arg; return 0;
        case KDSKBMODE: g_kb = (long) arg; return 0;
    }
    return -1;
}

static int devnull(void) { return open("/dev/null", O_RDWR); }

void setUp() {
    g_mode = KD_TEXT;
    g_kb = K_UNICODE;
    g_logn = 0;
    g_fail_req = 0;
}
void tearDown() {}

void test_hold_takes_graphics_and_keys_off() {
    struct vrx_vt vt = { .ioctl_fn = fake_ioctl };
    TEST_ASSERT_EQUAL(0, vrx_vt_hold_fd(&vt, devnull()));
    TEST_ASSERT_EQUAL(KD_GRAPHICS, g_mode);
    TEST_ASSERT_EQUAL(K_OFF, g_kb);
    vrx_vt_release(&vt);
}

void test_release_restores_what_was_found() {
    g_kb = K_XLATE;
    struct vrx_vt vt = { .ioctl_fn = fake_ioctl };
    TEST_ASSERT_EQUAL(0, vrx_vt_hold_fd(&vt, devnull()));
    vrx_vt_release(&vt);
    TEST_ASSERT_EQUAL(KD_TEXT, g_mode);
    TEST_ASSERT_EQUAL(K_XLATE, g_kb);
    TEST_ASSERT_EQUAL(-1, vt.fd);
}

void test_release_gives_the_keyboard_back_before_text() {
    struct vrx_vt vt = { .ioctl_fn = fake_ioctl };
    TEST_ASSERT_EQUAL(0, vrx_vt_hold_fd(&vt, devnull()));
    g_logn = 0;
    vrx_vt_release(&vt);
    TEST_ASSERT_EQUAL(2, g_logn);
    TEST_ASSERT_EQUAL(KDSKBMODE, g_log[0]);
    TEST_ASSERT_EQUAL(KDSETMODE, g_log[1]);
}

void test_a_crashed_holders_leftover_is_not_restored() {
    g_mode = KD_GRAPHICS;
    g_kb = K_OFF;
    struct vrx_vt vt = { .ioctl_fn = fake_ioctl };
    TEST_ASSERT_EQUAL(0, vrx_vt_hold_fd(&vt, devnull()));
    vrx_vt_release(&vt);
    TEST_ASSERT_EQUAL(KD_TEXT, g_mode);
    TEST_ASSERT_EQUAL(K_UNICODE, g_kb);
}

void test_a_fd_that_is_not_a_vt_changes_nothing() {
    struct vrx_vt vt = { .ioctl_fn = fake_ioctl };
    g_fail_req = (int) KDGKBMODE;
    TEST_ASSERT_EQUAL(-1, vrx_vt_hold_fd(&vt, devnull()));
    TEST_ASSERT_EQUAL(KD_TEXT, g_mode);
    TEST_ASSERT_EQUAL(K_UNICODE, g_kb);
    vrx_vt_release(&vt);  // no-op
}

void test_keys_off_refused_puts_graphics_back() {
    struct vrx_vt vt = { .ioctl_fn = fake_ioctl };
    g_fail_req = (int) KDSKBMODE;
    TEST_ASSERT_EQUAL(-1, vrx_vt_hold_fd(&vt, devnull()));
    TEST_ASSERT_EQUAL(KD_TEXT, g_mode);
}

void test_a_missing_path_is_not_fatal() {
    struct vrx_vt vt = { .ioctl_fn = fake_ioctl };
    TEST_ASSERT_EQUAL(-1, vrx_vt_hold(&vt, "/nonexistent/vt"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hold_takes_graphics_and_keys_off);
    RUN_TEST(test_release_restores_what_was_found);
    RUN_TEST(test_release_gives_the_keyboard_back_before_text);
    RUN_TEST(test_a_crashed_holders_leftover_is_not_restored);
    RUN_TEST(test_a_fd_that_is_not_a_vt_changes_nothing);
    RUN_TEST(test_keys_off_refused_puts_graphics_back);
    RUN_TEST(test_a_missing_path_is_not_fatal);
    return UNITY_END();
}
