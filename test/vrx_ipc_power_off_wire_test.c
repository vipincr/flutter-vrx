// SPDX-License-Identifier: MIT
// The power-off countdown's wire (vrx_ipc.h): the numbers and sizes
// vrx-linux's rust/host/src/compositor.rs encodes (MSG_POWER_OFF 25 down,
// MSG_POWER_OFF_ANSWER 26 up, one 4-byte word each). Pure: no sockets.
#include <stddef.h>

#include <unity.h>

#include "vrx_ipc.h"

// required by Unity.
void setUp() {
}

void tearDown() {
}

void test_message_numbers_are_vrxhosts() {
    TEST_ASSERT_EQUAL_UINT32(25, VRX_MSG_POWER_OFF);
    TEST_ASSERT_EQUAL_UINT32(26, VRX_MSG_POWER_OFF_ANSWER);
}

void test_each_payload_is_one_word() {
    TEST_ASSERT_EQUAL_size_t(4, sizeof(struct vrx_msg_power_off));
    TEST_ASSERT_EQUAL_size_t(4, sizeof(struct vrx_msg_power_off_answer));
}

void test_seconds_left_is_signed_so_minus_one_means_over() {
    struct vrx_msg_power_off m = { .secs_left = -1 };
    TEST_ASSERT_TRUE(m.secs_left < 0);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_message_numbers_are_vrxhosts);
    RUN_TEST(test_each_payload_is_one_word);
    RUN_TEST(test_seconds_left_is_signed_so_minus_one_means_over);
    return UNITY_END();
}
