// SPDX-License-Identifier: MIT
// The camera plugin's wire parsing (plan 0018 D8). Pure: no EGL, no sockets.
#define _GNU_SOURCE
#include <string.h>

#include <unity.h>

#include "plugins/vrx_camera/wire.h"

#define STR(s) (s), strlen(s)

// required by Unity.
void setUp() {
}

void tearDown() {
}

void test_offer_with_a_format_parses() {
    struct vrx_camera_offer o;
    const char *line =
        "{\"buffers\":4,\"size\":1228800,\"format\":{\"fourcc\":875713112,\"width\":640,\"height\":480,\"stride\":2560,\"modifier\":0}}";
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_OK, vrx_camera_parse_offer(STR(line), &o));
    TEST_ASSERT_EQUAL_UINT32(4, o.buffers);
    TEST_ASSERT_EQUAL_UINT64(1228800, o.size);
    TEST_ASSERT_TRUE(o.has_format);
    TEST_ASSERT_EQUAL_UINT32(875713112, o.format.fourcc);  // 'XR24'
    TEST_ASSERT_EQUAL_UINT32(640, o.format.width);
    TEST_ASSERT_EQUAL_UINT32(480, o.format.height);
    TEST_ASSERT_EQUAL_UINT32(2560, o.format.stride);
    TEST_ASSERT_EQUAL_UINT64(0, o.format.modifier);
}

void test_fourcc_as_text_and_a_modifier_above_32_bits() {
    struct vrx_camera_offer o;
    const char *line =
        "{\"buffers\":2,\"size\":4096,\"format\":{\"fourcc\":\"XR24\",\"width\":16,\"height\":16,\"stride\":64,\"modifier\":72057594037927937}}";
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_OK, vrx_camera_parse_offer(STR(line), &o));
    TEST_ASSERT_EQUAL_UINT32(0x34325258, o.format.fourcc);
    TEST_ASSERT_EQUAL_UINT64(72057594037927937ull, o.format.modifier);
}

void test_an_offer_without_a_format_is_an_offer_without_a_format() {
    // old producers: parses, and the plugin refuses it by has_format
    struct vrx_camera_offer o;
    const char *line = "{\"buffers\":4,\"size\":4096}";
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_OK, vrx_camera_parse_offer(STR(line), &o));
    TEST_ASSERT_FALSE(o.has_format);
}

void test_bad_offers_are_refused() {
    struct vrx_camera_offer o;
    const char *none = "{\"buffers\":0,\"size\":4096}";
    const char *many = "{\"buffers\":17,\"size\":4096}";
    const char *nosize = "{\"buffers\":4,\"size\":0}";
    const char *filled = "{\"index\":3,\"bytes\":10,\"sequence\":1}";
    const char *halfformat = "{\"buffers\":4,\"size\":4096,\"format\":{\"fourcc\":1,\"width\":16}}";
    const char *zerowidth = "{\"buffers\":4,\"size\":4096,\"format\":{\"fourcc\":1,\"width\":0,\"height\":4,\"stride\":4}}";
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_BAD, vrx_camera_parse_offer(STR(none), &o));
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_BAD, vrx_camera_parse_offer(STR(many), &o));
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_BAD, vrx_camera_parse_offer(STR(nosize), &o));
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_BAD, vrx_camera_parse_offer(STR(filled), &o));
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_BAD, vrx_camera_parse_offer(STR(halfformat), &o));
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_BAD, vrx_camera_parse_offer(STR(zerowidth), &o));
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_BAD, vrx_camera_parse_offer(STR("not json"), &o));
}

void test_filled_records() {
    struct vrx_camera_filled f;
    const char *line = "{\"index\":3,\"bytes\":1228800,\"sequence\":17}";
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_OK, vrx_camera_parse_filled(STR(line), &f));
    TEST_ASSERT_EQUAL_UINT32(3, f.index);
    TEST_ASSERT_EQUAL_UINT64(1228800, f.bytes);
    TEST_ASSERT_EQUAL_UINT64(17, f.sequence);
    // a returned record is not a filled one
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_BAD, vrx_camera_parse_filled(STR("{\"index\":3}"), &f));
    // a sequence past 2^32 survives
    const char *big = "{\"index\":0,\"bytes\":1,\"sequence\":5000000000}";
    TEST_ASSERT_EQUAL(VRX_CAMERA_PARSE_OK, vrx_camera_parse_filled(STR(big), &f));
    TEST_ASSERT_EQUAL_UINT64(5000000000ull, f.sequence);
}

void test_return_record_is_one_line() {
    char buf[32];
    size_t n = vrx_camera_format_return(3, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("{\"index\":3}\n", buf);
    TEST_ASSERT_EQUAL(strlen("{\"index\":3}\n"), n);
    TEST_ASSERT_EQUAL(0, vrx_camera_format_return(3, buf, 4));  // does not fit
}

void test_open_call_ends_in_a_nul() {
    char buf[256];
    size_t n = vrx_camera_format_open_call("camerad", "frames", buf, sizeof buf);
    const char *want = "{\"method\":\"io.vrx.Host.Open\",\"parameters\":{\"to\":\"camerad\",\"channel\":\"frames\"}}";
    TEST_ASSERT_EQUAL(strlen(want) + 1, n);
    TEST_ASSERT_EQUAL_STRING(want, buf);
    TEST_ASSERT_EQUAL(0, buf[n - 1]);
    TEST_ASSERT_EQUAL(0, vrx_camera_format_open_call("camerad", "frames", buf, 20));
}

void test_varlink_errors_map_to_short_codes() {
    char err[96];
    const char *busy = "{\"error\":\"io.vrx.Host.Busy\",\"parameters\":{}}";
    TEST_ASSERT_TRUE(vrx_camera_varlink_error(STR(busy), err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("io.vrx.Host.Busy", err);
    TEST_ASSERT_EQUAL_STRING("busy", vrx_camera_error_code(err));
    TEST_ASSERT_EQUAL_STRING("not-permitted", vrx_camera_error_code("io.vrx.Host.NotPermitted"));
    TEST_ASSERT_EQUAL_STRING("not-declared", vrx_camera_error_code("io.vrx.Host.NotDeclared"));
    TEST_ASSERT_EQUAL_STRING("unavailable", vrx_camera_error_code("io.vrx.Host.NotListening"));
    TEST_ASSERT_EQUAL_STRING("failed", vrx_camera_error_code("io.vrx.Host.Whatever"));
    const char *ok = "{\"parameters\":{}}";
    TEST_ASSERT_FALSE(vrx_camera_varlink_error(STR(ok), err, sizeof err));
}

void test_camerad_refusals_on_the_stream_map_to_codes() {
    char err[48];
    const char *busy = "{\"error\":\"busy\"}";
    TEST_ASSERT_TRUE(vrx_camera_stream_error(STR(busy), err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("busy", err);
    TEST_ASSERT_EQUAL_STRING("busy", vrx_camera_error_code(err));
    const char *disabled = "{\"error\":\"disabled\"}";
    TEST_ASSERT_TRUE(vrx_camera_stream_error(STR(disabled), err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("disabled", vrx_camera_error_code(err));
    const char *nocam = "{\"error\":\"no-camera\"}";
    TEST_ASSERT_TRUE(vrx_camera_stream_error(STR(nocam), err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("unavailable", vrx_camera_error_code(err));
    const char *failed = "{\"error\":\"failed\"}";
    TEST_ASSERT_TRUE(vrx_camera_stream_error(STR(failed), err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("failed", vrx_camera_error_code(err));
    // an offer is not a refusal, even if some field were named error
    const char *offer = "{\"buffers\":4,\"size\":4096,\"error\":\"x\"}";
    TEST_ASSERT_FALSE(vrx_camera_stream_error(STR(offer), err, sizeof err));
    const char *filled = "{\"index\":1,\"bytes\":2,\"sequence\":3}";
    TEST_ASSERT_FALSE(vrx_camera_stream_error(STR(filled), err, sizeof err));
}

void test_lines_split_across_reads_and_merged_in_one() {
    struct vrx_camera_lines l;
    char out[64];
    size_t n;
    vrx_camera_lines_init(&l, '\n');
    TEST_ASSERT_TRUE(vrx_camera_lines_feed(&l, STR("{\"index\":1,\"by")));
    TEST_ASSERT_FALSE(vrx_camera_lines_next(&l, out, sizeof out, &n));
    TEST_ASSERT_TRUE(vrx_camera_lines_feed(&l, STR("tes\":2,\"sequence\":3}\n{\"index\":9}\n")));
    TEST_ASSERT_TRUE(vrx_camera_lines_next(&l, out, sizeof out, &n));
    TEST_ASSERT_EQUAL_STRING("{\"index\":1,\"bytes\":2,\"sequence\":3}", out);
    TEST_ASSERT_TRUE(vrx_camera_lines_next(&l, out, sizeof out, &n));
    TEST_ASSERT_EQUAL_STRING("{\"index\":9}", out);
    TEST_ASSERT_FALSE(vrx_camera_lines_next(&l, out, sizeof out, &n));
}

void test_nul_delimited_varlink_messages() {
    struct vrx_camera_lines l;
    char out[64];
    size_t n;
    vrx_camera_lines_init(&l, 0);
    TEST_ASSERT_TRUE(vrx_camera_lines_feed(&l, "{\"a\":1}\0{\"b\":2}\0", 16));
    TEST_ASSERT_TRUE(vrx_camera_lines_next(&l, out, sizeof out, &n));
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", out);
    TEST_ASSERT_TRUE(vrx_camera_lines_next(&l, out, sizeof out, &n));
    TEST_ASSERT_EQUAL_STRING("{\"b\":2}", out);
}

void test_overflow_is_refused() {
    struct vrx_camera_lines l;
    char big[9000];
    memset(big, 'x', sizeof big);
    vrx_camera_lines_init(&l, '\n');
    TEST_ASSERT_FALSE(vrx_camera_lines_feed(&l, big, sizeof big));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_offer_with_a_format_parses);
    RUN_TEST(test_fourcc_as_text_and_a_modifier_above_32_bits);
    RUN_TEST(test_an_offer_without_a_format_is_an_offer_without_a_format);
    RUN_TEST(test_bad_offers_are_refused);
    RUN_TEST(test_filled_records);
    RUN_TEST(test_return_record_is_one_line);
    RUN_TEST(test_open_call_ends_in_a_nul);
    RUN_TEST(test_varlink_errors_map_to_short_codes);
    RUN_TEST(test_camerad_refusals_on_the_stream_map_to_codes);
    RUN_TEST(test_lines_split_across_reads_and_merged_in_one);
    RUN_TEST(test_nul_delimited_varlink_messages);
    RUN_TEST(test_overflow_is_refused);
    return UNITY_END();
}
