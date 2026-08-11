#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_reassembler.h"

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

static uint8_t s_buffer[512];
static ble_link_reassembler_t s_slot;

static void _from_hex(const char *hex, uint8_t *out, size_t *out_len)
{
    size_t len = strlen(hex) / 2U;

    for (size_t i = 0U; i < len; ++i)
    {
        unsigned int high = 0U;
        unsigned int low = 0U;
        char h = hex[2U * i];
        char l = hex[2U * i + 1U];

        if (h >= '0' && h <= '9')
        {
            high = (unsigned int)(h - '0');
        }
        else
        {
            high = (unsigned int)(h - 'a' + 10U);
        }
        if (l >= '0' && l <= '9')
        {
            low = (unsigned int)(l - '0');
        }
        else
        {
            low = (unsigned int)(l - 'a' + 10U);
        }
        out[i] = (uint8_t)((high << 4U) | low);
    }
    *out_len = len;
}

static void _feed(const char *hex, uint8_t *storage, size_t *storage_len,
                  ble_link_fragment_t *out)
{
    size_t len = 0U;

    _from_hex(hex, storage, &len);
    *storage_len = len;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_parse(
                          storage, len, out));
}

static void _init_slot(void)
{
    ble_link_reassembler_init(&s_slot, s_buffer, sizeof(s_buffer));
}

static void test_single_fragment(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010301000800000048656c6c6f216d65", storage, &storage_len, &fragment);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    TEST_ASSERT_EQUAL(0, memcmp(s_buffer, "Hello!me", 8U));
}

static void test_two_fragments(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010101000d000000414141414141414141414141", storage, &storage_len, &fragment);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    _feed("010201000d000c0042", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    TEST_ASSERT_EQUAL(0, memcmp(s_buffer,
                                "AAAAAAAAAAAAB", 13U));
}

static void test_three_fragments(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010102001e000000414141414141414141414141414141414141", storage,
          &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    _feed("010002001e00120042424242424242424242", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    _feed("010202001e001c004343", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    TEST_ASSERT_EQUAL(0, memcmp(s_buffer,
                                "AAAAAAAAAAAAAAAAAA"
                                "BBBBBBBBBB"
                                "CC", 30U));
}

static void test_exact_duplicate_accepted(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010104000b00000048656c6c6f2177", storage, &storage_len, &fragment);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    _feed("010104000b00000048656c6c6f2177", storage, &storage_len, &fragment);
    ble_link_reassembly_disposition_t disposition;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept_ex(
                          &s_slot, &fragment, &disposition));
    TEST_ASSERT_EQUAL(BLE_LINK_REASSEMBLY_DUPLICATE, disposition);
    _feed("010204000b0007006f726c64", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    TEST_ASSERT_EQUAL(0, memcmp(s_buffer, "Hello!world", 11U));
}

static void test_invalid_fragments_rejected(void)
{
    static const char *cases[] =
    {
        "010300000900000048656c6c6f",   /* zero frame id */
        "010701000900000048656c6c6f",   /* unknown flag bit */
        "020301000900000048656c6c6f",   /* unsupported version */
        "0103010000000000",             /* zero total length */
        "010301000c000000454545454545454545454545454545", /* exceeds total */
    };
    uint8_t storage[512];
    size_t storage_len = 0U;

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        _init_slot();
        ble_link_fragment_t fragment;

        _feed(cases[i], storage, &storage_len, &fragment);
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                              &s_slot, &fragment));
    }
    /* truncated header: fixture value is 6 bytes, rejected at parse. */
    static uint8_t truncated[6] = {0x01, 0x01, 0x1e, 0x00, 0x0c, 0x00};
    ble_link_fragment_t fragment;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_parse(
                          truncated, sizeof(truncated),
                          &fragment));
    /* zero-length value: no bytes at all, rejected at parse. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_parse(
                          storage, 0U, &fragment));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_parse(
                          NULL, 8U, &fragment));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_parse(
                          storage, 8U, NULL));
}

static void test_gap_and_overlap_rejected(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010101000c00000048656c6c6f21", storage, &storage_len, &fragment);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* gap: offset 8 skips byte 7 of the 6-byte fragment stream. */
    _feed("010201000c0008006d6521", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    _init_slot();
    _feed("010101000c00000048656c6c6f21", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* conflicting overlap: same offset, different bytes. */
    _feed("010001000c000000776f726c6421", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* unexpected start in a continued frame. */
    _init_slot();
    _feed("010101000c00000048656c6c6f21", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    _feed("010301000c0007006d6521", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* changed total length. */
    _init_slot();
    _feed("010101000c00000048656c6c6f21", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    _feed("010201001f0007006d6521", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                          &s_slot, &fragment));
}

static void test_capacity_exceeded(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;
    uint8_t small[8];

    ble_link_reassembler_init(&s_slot, small, sizeof(small));
    ble_link_fragment_t fragment;

    _feed("010301001000000041414141414141414141414141414141", storage, &storage_len, &fragment);

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_link_reassembler_accept(
                          &s_slot, &fragment));
}

static void test_max_payload_487(void)
{
    /* Fixture max-payload-487: single fragment with 487-byte payload. */
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    static const char *hex =
        "01030300e7010000444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "4444444444444444444444444444444444444444444444444444444444444444"
        "444444444444444444444444444444";

    ble_link_fragment_t fragment;

    _feed(hex, storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(487U, fragment.payload_len);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    for (size_t i = 0U; i < 487U; ++i)
    {
        if (s_buffer[i] != 0x44U)
        {
            fprintf(stderr, "byte %zu mismatch\n", i);
            abort();
        }
    }
}

static void test_frame_reuse_after_completion(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010301000800000048656c6c6f216d65", storage, &storage_len, &fragment);

    ble_link_reassembly_disposition_t disposition;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept_ex(
                          &s_slot, &fragment, &disposition));
    TEST_ASSERT_EQUAL(BLE_LINK_REASSEMBLY_COMPLETE, disposition);
    /* The exact final fragment is accepted but cannot deliver twice. */
    _feed("010301000800000048656c6c6f216d65", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept_ex(
                          &s_slot, &fragment, &disposition));
    TEST_ASSERT_EQUAL(BLE_LINK_REASSEMBLY_DUPLICATE, disposition);
    /* A different message may immediately reuse the same nonzero ID. */
    _feed("0103010008000000476f6f6462796521", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept_ex(
                          &s_slot, &fragment, &disposition));
    TEST_ASSERT_EQUAL(BLE_LINK_REASSEMBLY_COMPLETE, disposition);
    TEST_ASSERT_EQUAL(0, memcmp(s_buffer, "Goodbye!", 8U));
}

static void test_late_duplicate_after_second_completion(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010301000800000048656c6c6f216d65", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    _feed("010302000800000048656c6c6f216d65", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* Only the latest final fragment is a tombstone. Frame IDs do not impose
     * ordering across completed messages, so frame 1 is reusable here. */
    _feed("010301000800000048656c6c6f216d65", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
}

static void test_frame_id_wrap_accepted(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("0103ffff0800000048656c6c6f216d65", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* Any nonzero ID may identify the next message. */
    _feed("010301000800000048656c6c6f216d65", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* Reusing 65535 is valid; frame IDs are not a cross-message sequence. */
    _feed("0103ffff0800000048656c6c6f216d65", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
}

static void test_arbitrary_frame_ids_reusable(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010302000800000048656c6c6f216d65", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* A lower ID is valid for the next different message. */
    _feed("0103010008000000476f6f6462796521", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* Reset still clears both active and final-fragment state. */
    ble_link_reassembler_reset(&s_slot);
    _feed("010301000800000048656c6c6f216d65", storage, &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
}

static void test_wrap_attack_rejected(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010101000d000000414141414141414141414141", storage, &storage_len, &fragment);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* offset = received = 12, payload_len = SIZE_MAX: must not wrap. */
    memset(&fragment, 0, sizeof(fragment));
    fragment.version = BLE_LINK_FRAMING_VERSION;
    fragment.flags = BLE_LINK_FRAMING_FLAG_END;
    fragment.frame_id = 1U;
    fragment.total_length = 13U;
    fragment.offset = 12U;
    fragment.payload = storage;
    fragment.payload_len = SIZE_MAX;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* Empty continuation payload. */
    memset(&fragment, 0, sizeof(fragment));
    fragment.version = BLE_LINK_FRAMING_VERSION;
    fragment.flags = BLE_LINK_FRAMING_FLAG_END;
    fragment.frame_id = 1U;
    fragment.total_length = 13U;
    fragment.offset = 12U;
    fragment.payload = storage;
    fragment.payload_len = 0U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                          &s_slot, &fragment));
}

static void test_flags_only_duplicate_rejected(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010101000d000000414141414141414141414141", storage,
          &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    /* Same frame/offset/payload but START bit cleared: not a duplicate. */
    fragment.flags = 0U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                          &s_slot, &fragment));
}

static void test_explicit_reset_keeps_slot_usable(void)
{
    uint8_t storage[512];
    size_t storage_len = 0U;

    _init_slot();
    ble_link_fragment_t fragment;

    _feed("010101000d000000414141414141414141414141", storage,
          &storage_len, &fragment);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    ble_link_reassembler_reset(&s_slot);
    /* The slot is usable again after an explicit reset. */
    _feed("010301000800000048656c6c6f216d65", storage, &storage_len,
          &fragment);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_reassembler_accept(
                          &s_slot, &fragment));
    TEST_ASSERT_EQUAL(0, memcmp(s_buffer, "Hello!me", 8U));
}

static void test_null_arguments_handled(void)
{
    ble_link_reassembler_init(NULL, s_buffer, sizeof(s_buffer));
    ble_link_reassembler_reset(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                          NULL, NULL));
}

static void test_invalid_arguments(void)
{
    _init_slot();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                          NULL, &(ble_link_fragment_t)
    {
        0
    }));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept(
                          &s_slot, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_reassembler_accept_ex(
                          &s_slot, &(ble_link_fragment_t)
    {
        0
    }, NULL));
}

int main(void)
{
    test_single_fragment();
    test_two_fragments();
    test_three_fragments();
    test_exact_duplicate_accepted();
    test_invalid_fragments_rejected();
    test_gap_and_overlap_rejected();
    test_capacity_exceeded();
    test_max_payload_487();
    test_frame_reuse_after_completion();
    test_late_duplicate_after_second_completion();
    test_frame_id_wrap_accepted();
    test_arbitrary_frame_ids_reusable();
    test_wrap_attack_rejected();
    test_flags_only_duplicate_rejected();
    test_explicit_reset_keeps_slot_usable();
    test_null_arguments_handled();
    test_invalid_arguments();
    printf("ble_link_reassembler: all tests passed\n");
    return 0;
}
