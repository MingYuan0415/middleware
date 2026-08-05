#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_link_framing.h"

#define TEST_ASSERT_TRUE(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            fprintf(stderr, "assertion failed at line %d: %s\n", \
                    __LINE__, #condition); \
            abort(); \
        } \
    } while (0)

#define TEST_ASSERT_FALSE(condition) \
    do \
    { \
        if ((condition)) \
        { \
            fprintf(stderr, "assertion failed at line %d: !(%s)\n", \
                    __LINE__, #condition); \
            abort(); \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL(expected, actual) \
    do \
    { \
        const long expected_value = (long)(expected); \
        const long actual_value = (long)(actual); \
        if (expected_value != actual_value) \
        { \
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%ld != %ld)\n", \
                    __LINE__, #expected, #actual, expected_value, actual_value); \
            abort(); \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL_U64(expected, actual) \
    do \
    { \
        const uint64_t expected_value = (uint64_t)(expected); \
        const uint64_t actual_value = (uint64_t)(actual); \
        if (expected_value != actual_value) \
        { \
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%llu != %llu)\n", \
                    __LINE__, #expected, #actual, \
                    (unsigned long long)expected_value, \
                    (unsigned long long)actual_value); \
            abort(); \
        } \
    } while (0)

static void _encode(device_link_fragment_header_t *header,
                    const uint8_t *payload, size_t payload_len,
                    uint8_t *out, size_t out_capacity, size_t *out_len)
{
    TEST_ASSERT_EQUAL(DEVICE_LINK_FRAME_OK,
                      device_link_framing_encode(header, payload, payload_len,
                              out, out_capacity, out_len));
}

static void test_encode_parse_roundtrip(void)
{
    const uint8_t payload[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F};
    device_link_fragment_header_t header =
    {
        .version = DEVICE_LINK_FRAMING_VERSION,
        .flags = DEVICE_LINK_FRAMING_FLAG_START |
        DEVICE_LINK_FRAMING_FLAG_END,
        .frame_id = 0x1234U,
        .total_length = sizeof(payload),
        .offset = 0U,
    };
    uint8_t value[64];
    size_t value_len = 0U;
    device_link_fragment_header_t parsed;
    const uint8_t *parsed_payload;
    size_t parsed_len = 0U;

    _encode(&header, payload, sizeof(payload), value, sizeof(value), &value_len);
    TEST_ASSERT_EQUAL(DEVICE_LINK_FRAMING_HEADER_BYTES + sizeof(payload),
                      value_len);
    TEST_ASSERT_EQUAL(0x01U, value[0]);
    TEST_ASSERT_EQUAL(0x03U, value[1]);
    TEST_ASSERT_EQUAL(DEVICE_LINK_FRAME_OK,
                      device_link_framing_parse(value, value_len, &parsed,
                              &parsed_payload, &parsed_len));
    TEST_ASSERT_EQUAL(header.version, parsed.version);
    TEST_ASSERT_EQUAL(header.flags, parsed.flags);
    TEST_ASSERT_EQUAL(header.frame_id, parsed.frame_id);
    TEST_ASSERT_EQUAL(header.total_length, parsed.total_length);
    TEST_ASSERT_EQUAL(header.offset, parsed.offset);
    TEST_ASSERT_EQUAL(sizeof(payload), parsed_len);
    TEST_ASSERT_TRUE(memcmp(parsed_payload, payload, sizeof(payload)) == 0);
}

static void _feed_ok(device_link_reassembler_t *reassembler,
                     const uint8_t *value, size_t value_len,
                     device_link_frame_result_t expected,
                     size_t expected_delivered)
{
    size_t delivered = 0U;
    const device_link_frame_result_t result =
        device_link_reassembler_feed(reassembler, value, value_len, &delivered);
    TEST_ASSERT_EQUAL(expected, result);
    TEST_ASSERT_EQUAL_U64(expected_delivered, delivered);
}

static void _feed_hex(device_link_reassembler_t *reassembler,
                      const char *hex, device_link_frame_result_t expected,
                      size_t expected_delivered)
{
    const size_t value_len = strlen(hex) / 2U;
    uint8_t value[DEVICE_LINK_FRAMING_MAX_VALUE_BYTES];
    for (size_t i = 0U; i < value_len; ++i)
    {
        const char high = hex[2U * i];
        const char low = hex[2U * i + 1U];
        value[i] = (uint8_t)(((high <= '9') ? (high - '0') :
                              (high - 'a' + 10)) << 4U) |
                   (uint8_t)((low <= '9') ? (low - '0') :
                             (low - 'a' + 10));
    }
    _feed_ok(reassembler, value, value_len, expected, expected_delivered);
}

static void test_single_fragment_completes(void)
{
    const char hex[] = "010301000800000048656c6c6f216d65";
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, hex, DEVICE_LINK_FRAME_COMPLETE, 8U);
    TEST_ASSERT_TRUE(memcmp(buffer, "Hello!me", 8U) == 0);
    device_link_reassembler_reset(&reassembler);
    TEST_ASSERT_FALSE(reassembler.failed);
}

static void test_two_fragments_complete(void)
{
    const char first[] = "010101000d000000414141414141414141414141";
    const char last[] = "010201000d000c0042";
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, first, DEVICE_LINK_FRAME_ACCEPTED, 0U);
    _feed_hex(&reassembler, last, DEVICE_LINK_FRAME_COMPLETE, 13U);
    TEST_ASSERT_EQUAL_U64(13U, reassembler.next_offset);
    device_link_reassembler_reset(&reassembler);
    TEST_ASSERT_FALSE(reassembler.failed);
}

static void test_three_fragments_complete(void)
{
    const char first[] =
        "010102001e000000414141414141414141414141414141414141";
    const char middle[] = "010002001e00120042424242424242424242";
    const char last[] = "010202001e001c004343";
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, first, DEVICE_LINK_FRAME_ACCEPTED, 0U);
    _feed_hex(&reassembler, middle, DEVICE_LINK_FRAME_ACCEPTED, 0U);
    _feed_hex(&reassembler, last, DEVICE_LINK_FRAME_COMPLETE, 30U);
    TEST_ASSERT_TRUE(memcmp(buffer, "AAAAAAAAAAAAAAAAAABBBBBBBBBBCC", 30U) == 0);
}

static void test_max_payload_487(void)
{
    const size_t payload_len = 487U;
    uint8_t payload[512];
    uint8_t value[512];
    size_t value_len = 0U;
    uint8_t buffer[512];
    device_link_reassembler_t reassembler;
    device_link_fragment_header_t header =
    {
        .version = DEVICE_LINK_FRAMING_VERSION,
        .flags = DEVICE_LINK_FRAMING_FLAG_START |
        DEVICE_LINK_FRAMING_FLAG_END,
        .frame_id = 3U,
        .total_length = payload_len,
        .offset = 0U,
    };

    memset(payload, 0x44, sizeof(payload));
    _encode(&header, payload, payload_len, value, sizeof(value), &value_len);
    TEST_ASSERT_EQUAL(DEVICE_LINK_FRAMING_HEADER_BYTES + payload_len, value_len);
    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_ok(&reassembler, value, value_len, DEVICE_LINK_FRAME_COMPLETE,
             payload_len);
    TEST_ASSERT_TRUE(memcmp(buffer, payload, payload_len) == 0);
}

static void test_exact_duplicate_idempotent(void)
{
    const char first[] = "010104000b00000048656c6c6f2177";
    const char last[] = "010204000b0007006f726c64";
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, first, DEVICE_LINK_FRAME_ACCEPTED, 0U);
    _feed_hex(&reassembler, first, DEVICE_LINK_FRAME_DUPLICATE, 0U);
    _feed_hex(&reassembler, first, DEVICE_LINK_FRAME_DUPLICATE, 0U);
    _feed_hex(&reassembler, last, DEVICE_LINK_FRAME_COMPLETE, 11U);
    TEST_ASSERT_TRUE(memcmp(buffer, "Hello!world", 11U) == 0);
}

static void test_reject_zero_frame_id(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "010300000900000048656c6c6f",
              DEVICE_LINK_FRAME_REJECTED, 0U);
    TEST_ASSERT_TRUE(reassembler.failed);
    device_link_reassembler_reset(&reassembler);
    _feed_hex(&reassembler, "010301000800000048656c6c6f216d65",
              DEVICE_LINK_FRAME_COMPLETE, 8U);
}

static void test_reject_unknown_flag_bit(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "010701000900000048656c6c6f",
              DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_gap(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "010101000c00000048656c6c6f21",
              DEVICE_LINK_FRAME_ACCEPTED, 0U);
    _feed_hex(&reassembler, "010001000c0008006d6521",
              DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_overlap_conflict(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "010101000c00000048656c6c6f21",
              DEVICE_LINK_FRAME_ACCEPTED, 0U);
    _feed_hex(&reassembler, "010001000c000000776f726c6421",
              DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_exceeds_total_length(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "010301000c000000454545454545454545454545454545",
              DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_unexpected_start(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "010101000c00000048656c6c6f21",
              DEVICE_LINK_FRAME_ACCEPTED, 0U);
    _feed_hex(&reassembler, "010301000c0007006d6521776f",
              DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_changed_total_length(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "010101000c00000048656c6c6f21",
              DEVICE_LINK_FRAME_ACCEPTED, 0U);
    _feed_hex(&reassembler, "010201000a0007006d6521",
              DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_changed_frame_id(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "010101000c00000048656c6c6f21",
              DEVICE_LINK_FRAME_ACCEPTED, 0U);
    _feed_hex(&reassembler, "010001010c0007006d6521",
              DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_unsupported_version(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "020301000900000048656c6c6f",
              DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_truncated_header(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "01011e000c00", DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_empty_value(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "", DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_zero_total_length(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "0103010000000000", DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_filled_without_end(void)
{
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "010101000b00000048656c6c6f21776f726c64",
              DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_after_complete(void)
{
    const char single[] = "010301000800000048656c6c6f216d65";
    uint8_t buffer[64];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, single, DEVICE_LINK_FRAME_COMPLETE, 8U);
    _feed_hex(&reassembler, single, DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_reject_capacity_too_small(void)
{
    uint8_t buffer[4];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, "010101000b00000048656c6c6f21776f726c64",
              DEVICE_LINK_FRAME_REJECTED, 0U);
}

static void test_encode_rejects_bad_arguments(void)
{
    const uint8_t payload[] = {0x01};
    device_link_fragment_header_t header =
    {
        .version = DEVICE_LINK_FRAMING_VERSION,
        .flags = DEVICE_LINK_FRAMING_FLAG_START |
        DEVICE_LINK_FRAMING_FLAG_END,
        .frame_id = 1U,
        .total_length = 1U,
        .offset = 0U,
    };
    uint8_t out[16];
    size_t out_len = 0U;

    TEST_ASSERT_EQUAL(DEVICE_LINK_FRAME_ERR_INVALID_ARG,
                      device_link_framing_encode(&header, payload, 1U,
                              NULL, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(DEVICE_LINK_FRAME_ERR_INVALID_ARG,
                      device_link_framing_encode(&header, payload, 1U,
                              out, 0U, &out_len));
    header.frame_id = 0U;
    TEST_ASSERT_EQUAL(DEVICE_LINK_FRAME_ERR_INVALID_FRAGMENT,
                      device_link_framing_encode(&header, payload, 1U,
                              out, sizeof(out), &out_len));
}

static void test_parse_rejects_truncated(void)
{
    const uint8_t value[] = {0x01, 0x01, 0x1E, 0x00, 0x0C, 0x00};
    device_link_fragment_header_t header;
    const uint8_t *payload;
    size_t payload_len = 0U;

    TEST_ASSERT_EQUAL(DEVICE_LINK_FRAME_ERR_INVALID_FRAGMENT,
                      device_link_framing_parse(value, sizeof(value), &header,
                              &payload, &payload_len));
}

static void test_max_total_length_u16(void)
{
    const size_t payload_len = 0xFFFFU;
    uint8_t *payload = malloc(payload_len);
    uint8_t *buffer = malloc(payload_len);
    device_link_reassembler_t reassembler;
    device_link_fragment_header_t header;
    uint8_t value[DEVICE_LINK_FRAMING_MAX_VALUE_BYTES];
    size_t value_len = 0U;
    size_t offset = 0U;

    TEST_ASSERT_TRUE(payload != NULL);
    TEST_ASSERT_TRUE(buffer != NULL);
    memset(payload, 0x55, payload_len);
    device_link_reassembler_init(&reassembler, buffer, payload_len);
    while (offset < payload_len)
    {
        size_t chunk = payload_len - offset;
        const bool first = offset == 0U;
        bool last;

        if (chunk > DEVICE_LINK_FRAMING_MAX_VALUE_BYTES -
                DEVICE_LINK_FRAMING_HEADER_BYTES)
        {
            chunk = DEVICE_LINK_FRAMING_MAX_VALUE_BYTES -
                    DEVICE_LINK_FRAMING_HEADER_BYTES;
        }
        last = offset + chunk == payload_len;
        header.version = DEVICE_LINK_FRAMING_VERSION;
        header.flags = (uint8_t)((first ? DEVICE_LINK_FRAMING_FLAG_START : 0U) |
                                 (last ? DEVICE_LINK_FRAMING_FLAG_END : 0U));
        header.frame_id = 1U;
        header.total_length = (uint16_t)payload_len;
        header.offset = (uint16_t)offset;
        TEST_ASSERT_EQUAL(DEVICE_LINK_FRAME_OK,
                          device_link_framing_encode(&header, payload + offset,
                                  chunk, value,
                                  sizeof(value), &value_len));
        _feed_ok(&reassembler, value, value_len,
                 last ? DEVICE_LINK_FRAME_COMPLETE : DEVICE_LINK_FRAME_ACCEPTED,
                 last ? payload_len : 0U);
        offset += chunk;
    }
    TEST_ASSERT_TRUE(memcmp(buffer, payload, payload_len) == 0);
    free(buffer);
    free(payload);
}

static void test_capacity_limited_rejects_oversize(void)
{
    const char first[] = "010101000b00000048656c6c6f2177";
    uint8_t buffer[7];
    device_link_reassembler_t reassembler;

    device_link_reassembler_init(&reassembler, buffer, sizeof(buffer));
    _feed_hex(&reassembler, first, DEVICE_LINK_FRAME_REJECTED, 0U);
}

int main(void)
{
    test_encode_parse_roundtrip();
    test_single_fragment_completes();
    test_two_fragments_complete();
    test_three_fragments_complete();
    test_max_payload_487();
    test_max_total_length_u16();
    test_capacity_limited_rejects_oversize();
    test_exact_duplicate_idempotent();
    test_reject_zero_frame_id();
    test_reject_unknown_flag_bit();
    test_reject_gap();
    test_reject_overlap_conflict();
    test_reject_exceeds_total_length();
    test_reject_unexpected_start();
    test_reject_changed_total_length();
    test_reject_changed_frame_id();
    test_reject_unsupported_version();
    test_reject_truncated_header();
    test_reject_empty_value();
    test_reject_zero_total_length();
    test_reject_filled_without_end();
    test_reject_after_complete();
    test_reject_capacity_too_small();
    test_encode_rejects_bad_arguments();
    test_parse_rejects_truncated();
    printf("device_link_framing: all tests passed\n");
    return 0;
}
