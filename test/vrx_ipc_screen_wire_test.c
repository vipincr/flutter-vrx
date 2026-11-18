// SPDX-License-Identifier: MIT
// The screen's words (vrx_ipc.h VRX_MSG_SCREEN, VRX_MSG_POWER_OFF) against vrx-linux's vectors/screen-wire.json, the one
// definition of them (its rust/protocol/src/screen.rs is held to the same file). argv[1] is that file; a missing one
// fails the test, it never passes it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unity.h>

#include "vrx_ipc.h"

static const char *vector_path;
static char vector[4096];

void setUp() {
    FILE *f = fopen(vector_path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "vrx-linux's vectors/screen-wire.json is not where the test was told");
    size_t n = fread(vector, 1, sizeof vector - 1, f);
    fclose(f);
    vector[n] = '\0';
}

void tearDown() {
}

// the number after `"key":` in the vector, found after `section`
static long word(const char *section, const char *key) {
    const char *s = strstr(vector, section);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, section);
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *k = strstr(s, needle);
    TEST_ASSERT_NOT_NULL_MESSAGE(k, key);
    return strtol(k + strlen(needle), NULL, 10);
}

void test_the_brightness_word_is_the_vectors() {
    TEST_ASSERT_EQUAL_INT(word("\"brightness\"", "cannot_dim"), VRX_BRIGHTNESS_CANNOT_DIM);
    TEST_ASSERT_EQUAL_INT(0, word("\"brightness\"", "min"));
    TEST_ASSERT_EQUAL_INT(1000, word("\"brightness\"", "max"));
}

void test_the_power_off_words_are_the_vectors() {
    TEST_ASSERT_EQUAL_INT(word("\"power_off_seconds\"", "closed"), VRX_POWER_OFF_CLOSED);
    TEST_ASSERT_EQUAL_INT(word("\"power_off_seconds\"", "darken"), VRX_POWER_OFF_DARKEN);
}

void test_the_screen_message_is_two_words_and_the_brightness_is_signed() {
    TEST_ASSERT_EQUAL_size_t(8, sizeof(struct vrx_msg_screen));
    TEST_ASSERT_EQUAL_UINT32(24, VRX_MSG_SCREEN);
    struct vrx_msg_screen m = { .on = 1, .brightness = VRX_BRIGHTNESS_CANNOT_DIM };
    TEST_ASSERT_TRUE(m.brightness < 0);
}

int main(int argc, char **argv) {
    vector_path = argc > 1 ? argv[1] : "";
    UNITY_BEGIN();
    RUN_TEST(test_the_brightness_word_is_the_vectors);
    RUN_TEST(test_the_power_off_words_are_the_vectors);
    RUN_TEST(test_the_screen_message_is_two_words_and_the_brightness_is_signed);
    return UNITY_END();
}
