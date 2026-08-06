#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_codec.h"

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

#define TEST_ASSERT_FALSE(condition) \
    do \
    { \
        if ((condition)) \
        { \
            fprintf(stderr, "assertion failed at line %d: %s\n", \
                    __LINE__, #condition); \
            abort(); \
        } \
    } while (0)

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

static void test_capabilities_request_roundtrip(void)
{
    static const char hex[] =
        "0801190807060504030201520b0901000000000000005200";
    uint8_t bytes[64];
    size_t len = 0U;
    ble_link_codec_envelope_t envelope;

    _from_hex(hex, bytes, &len);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
    TEST_ASSERT_EQUAL(1U, envelope.protocol_major);
    TEST_ASSERT_EQUAL(0U, envelope.protocol_minor);
    TEST_ASSERT_EQUAL(72623859790382856ULL, envelope.boot_id);
    TEST_ASSERT_EQUAL(0U, envelope.flags);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_BODY_REQUEST, envelope.body);
    TEST_ASSERT_EQUAL(11U, envelope.body_len);
    ble_link_codec_request_t request;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_request(
                          envelope.body_data, envelope.body_len,
                          &request));
    TEST_ASSERT_EQUAL(1U, request.request_id);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_REQUEST_GET_CAPABILITIES, request.body);
    TEST_ASSERT_EQUAL(0U, request.body_len);
    uint8_t reencoded[64];
    size_t reencoded_len = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_request(
                          &request, reencoded, sizeof(reencoded),
                          &reencoded_len));
    TEST_ASSERT_EQUAL(envelope.body_len, reencoded_len);
    TEST_ASSERT_EQUAL(0, memcmp(reencoded, envelope.body_data,
                                envelope.body_len));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_envelope(
                          &envelope, reencoded, sizeof(reencoded),
                          &reencoded_len));
    TEST_ASSERT_EQUAL(len, reencoded_len);
    TEST_ASSERT_EQUAL(0, memcmp(reencoded, bytes, len));
}

static void test_authorize_prepare_response_roundtrip(void)
{
    static const char hex[] =
        "090100000000000000100162310911100f0e0d0c0b0a12100102030405060708090a"
        "0b0c0d0e0f101a104646464646464646464646464646464620c0cf24";
    uint8_t bytes[128];
    size_t len = 0U;
    ble_link_codec_response_t response;

    _from_hex(hex, bytes, &len);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_codec_decode_response(bytes, len, &response));
    TEST_ASSERT_EQUAL(1U, response.request_id);
    TEST_ASSERT_EQUAL(1U, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_AUTHORIZE_PREPARE,
                      response.body);
    TEST_ASSERT_EQUAL(49U, response.body_len);
    uint8_t reencoded[128];
    size_t reencoded_len = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_response(
                          &response, reencoded, sizeof(reencoded),
                          &reencoded_len));
    TEST_ASSERT_EQUAL(len, reencoded_len);
    TEST_ASSERT_EQUAL(0, memcmp(reencoded, bytes, len));
}

static void test_unknown_field_roundtrip(void)
{
    static const char hex[] =
        "0801190807060504030201520b0901000000000000005200c03e07";
    uint8_t bytes[64];
    size_t len = 0U;
    ble_link_codec_envelope_t envelope;
    uint8_t reencoded[64];
    size_t reencoded_len = 0U;

    _from_hex(hex, bytes, &len);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
    TEST_ASSERT_EQUAL(1U, envelope.protocol_major);
    TEST_ASSERT_EQUAL(72623859790382856ULL, envelope.boot_id);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_BODY_REQUEST, envelope.body);
    TEST_ASSERT_EQUAL(1U, envelope.unknown_fields_count);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_envelope(
                          &envelope, reencoded, sizeof(reencoded),
                          &reencoded_len));
    TEST_ASSERT_EQUAL(len, reencoded_len);
    TEST_ASSERT_EQUAL(0, memcmp(reencoded, bytes, len));
}

static void test_snapshot_envelope_roundtrip(void)
{
    static const char hex[] =
        "08011908070605040302016a1e0907000000000000001213090807060504030201"
        "10031805200128013001";
    uint8_t bytes[64];
    size_t len = 0U;
    ble_link_codec_envelope_t envelope;
    uint8_t reencoded[64];
    size_t reencoded_len = 0U;

    _from_hex(hex, bytes, &len);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
    TEST_ASSERT_EQUAL(1U, envelope.protocol_major);
    TEST_ASSERT_EQUAL(72623859790382856ULL, envelope.boot_id);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_BODY_SNAPSHOT, envelope.body);
    TEST_ASSERT_TRUE(envelope.body_len > 0U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_envelope(
                          &envelope, reencoded, sizeof(reencoded),
                          &reencoded_len));
    TEST_ASSERT_EQUAL(len, reencoded_len);
    TEST_ASSERT_EQUAL(0, memcmp(reencoded, bytes, len));
}

static void test_transfer_control_envelope_roundtrip(void)
{
    static const char hex[] =
        "08011908070605040302017238098877665544332211522d08011100000100000000"
        "001a20ababababababababababababababababababababababababababababababab"
        "ab";
    uint8_t bytes[128];
    size_t len = 0U;
    ble_link_codec_envelope_t envelope;
    uint8_t reencoded[128];
    size_t reencoded_len = 0U;

    _from_hex(hex, bytes, &len);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
    TEST_ASSERT_EQUAL(1U, envelope.protocol_major);
    TEST_ASSERT_EQUAL(72623859790382856ULL, envelope.boot_id);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_BODY_TRANSFER_CONTROL, envelope.body);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_envelope(
                          &envelope, reencoded, sizeof(reencoded),
                          &reencoded_len));
    TEST_ASSERT_EQUAL(len, reencoded_len);
    TEST_ASSERT_EQUAL(0, memcmp(reencoded, bytes, len));
}

static void test_invalid_envelopes_rejected(void)
{
    uint8_t bytes[32];
    size_t len = 0U;
    ble_link_codec_envelope_t envelope;

    _from_hex("1908", bytes, &len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
    _from_hex("520200", bytes, &len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
    _from_hex("0f", bytes, &len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
    _from_hex("720a0112", bytes, &len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
}

static void test_encode_size_query_and_errors(void)
{
    ble_link_codec_envelope_t envelope;

    memset(&envelope, 0, sizeof(envelope));
    envelope.protocol_major = 1U;
    envelope.boot_id = 42U;
    envelope.body = BLE_LINK_CODEC_BODY_REQUEST;
    envelope.body_data = (const uint8_t[])
    {
        0x00
    };
    envelope.body_len = 1U;
    size_t needed = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_envelope(
                          &envelope, NULL, 0U, &needed));
    TEST_ASSERT_TRUE(needed > 0U);
    uint8_t small[2];

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_link_codec_encode_envelope(
                          &envelope, small, sizeof(small),
                          &needed));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_codec_decode_envelope(NULL, 4U, &envelope));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_codec_decode_envelope((const uint8_t[])
    {
        0
    }, 1U,
    NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_codec_encode_envelope(NULL, NULL, 0U, &needed));
    /* A zero envelope omits every proto3 default-valued field. */
    ble_link_codec_envelope_t empty;

    memset(&empty, 0, sizeof(empty));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_envelope(
                          &empty, NULL, 0U, &needed));
    TEST_ASSERT_EQUAL(0U, needed);
    uint8_t empty_buf[4];

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_envelope(
                          &empty, empty_buf, sizeof(empty_buf),
                          &needed));
    TEST_ASSERT_EQUAL(0U, needed);
}

static void test_packed_and_unpacked_flags(void)
{
    /* packed flags: field 4 len-delimited [0x01] */
    static const char packed[] = "0801190807060504030201220101";
    /* unpacked flags: field 4 varint 1 */
    static const char unpacked[] = "08011908070605040302012001";
    uint8_t bytes[16];
    size_t len = 0U;
    ble_link_codec_envelope_t envelope;
    uint8_t reencoded[16];
    size_t reencoded_len = 0U;

    _from_hex(packed, bytes, &len);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
    TEST_ASSERT_EQUAL(1U, envelope.protocol_major);
    TEST_ASSERT_EQUAL(1U, envelope.flags);
    _from_hex(unpacked, bytes, &len);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
    TEST_ASSERT_EQUAL(1U, envelope.protocol_major);
    TEST_ASSERT_EQUAL(1U, envelope.flags);
    /* Canonical re-encode emits packed form. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_envelope(
                          &envelope, reencoded, sizeof(reencoded),
                          &reencoded_len));
    static const uint8_t expected[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x22, 0x01, 0x01,
    };

    TEST_ASSERT_EQUAL(sizeof(expected), reencoded_len);
    TEST_ASSERT_EQUAL(0, memcmp(reencoded, expected, sizeof(expected)));
}

static void test_duplicate_body_rejected(void)
{
    /* two request body fields (10 and 10) */
    static const char hex[] =
        "0801190807060504030201520b09010000000000000052005200";
    uint8_t bytes[32];
    size_t len = 0U;
    ble_link_codec_envelope_t envelope;

    _from_hex(hex, bytes, &len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
}

static void test_wrong_wire_type_rejected(void)
{
    /* protocol_major as fixed64 */
    static const char hex[] = "0d0100000000000000";
    uint8_t bytes[16];
    size_t len = 0U;
    ble_link_codec_envelope_t envelope;
    ble_link_codec_request_t request;
    ble_link_codec_response_t response;

    _from_hex(hex, bytes, &len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_codec_decode_envelope(bytes, len, &envelope));
    /* request_id as varint instead of fixed64 */
    _from_hex("0801", bytes, &len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_codec_decode_request(bytes, len, &request));
    _from_hex("0801", bytes, &len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_codec_decode_response(bytes, len, &response));
}

static void test_invalid_key_and_overflow_varint_rejected(void)
{
    /* field 0 tag */
    uint8_t bytes[4] = {0x00, 0x01, 0x00, 0x00};
    ble_link_codec_envelope_t envelope;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_codec_decode_envelope(bytes, sizeof(bytes),
                              &envelope));
    /* 11-byte varint overflows */
    uint8_t long_varint[12];

    memset(long_varint, 0xff, sizeof(long_varint));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_codec_decode_envelope(long_varint,
                              sizeof(long_varint),
                              &envelope));
}

static void test_zero_fixed64_omitted_in_encode(void)
{
    ble_link_codec_envelope_t envelope;
    ble_link_codec_request_t request;
    uint8_t out[32];
    size_t out_len = 0U;

    memset(&envelope, 0, sizeof(envelope));
    envelope.protocol_major = 1U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_envelope(
                          &envelope, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(2U, out_len);
    TEST_ASSERT_EQUAL(0x08, out[0]);
    TEST_ASSERT_EQUAL(0x01, out[1]);
    memset(&request, 0, sizeof(request));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_encode_request(
                          &request, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(0U, out_len);
}

static void test_invalid_body_tag_rejected(void)
{
    ble_link_codec_envelope_t envelope;
    ble_link_codec_request_t request;
    ble_link_codec_response_t response;
    size_t out_len = 0U;

    memset(&envelope, 0, sizeof(envelope));
    envelope.body = (ble_link_codec_body_t)99U;
    envelope.body_data = (const uint8_t[])
    {
        0x00
    };
    envelope.body_len = 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_codec_encode_envelope(
                          &envelope, NULL, 0U, &out_len));
    memset(&request, 0, sizeof(request));
    request.body = (ble_link_codec_request_tag_t)99U;
    request.body_data = (const uint8_t[])
    {
        0x00
    };
    request.body_len = 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_codec_encode_request(
                          &request, NULL, 0U, &out_len));
    memset(&response, 0, sizeof(response));
    response.body = (ble_link_codec_response_tag_t)99U;
    response.body_data = (const uint8_t[])
    {
        0x00
    };
    response.body_len = 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_codec_encode_response(
                          &response, NULL, 0U, &out_len));
}

static void test_encoder_overflow_and_capacity_rejected(void)
{
    static const uint8_t dummy[1] = {0x00};
    ble_link_codec_envelope_t envelope;
    ble_link_codec_request_t request;
    ble_link_codec_response_t response;
    size_t out_len = 0U;

    /* unknown_fields_count beyond the fixed array capacity, with valid data
     * pointers so only the count guard can reject it. */
    memset(&envelope, 0, sizeof(envelope));
    envelope.protocol_major = 1U;
    for (size_t i = 0U; i < BLE_LINK_CODEC_MAX_UNKNOWN_FIELDS; ++i)
    {
        envelope.unknown_fields[i].data = dummy;
        envelope.unknown_fields[i].len = 1U;
    }
    envelope.unknown_fields_count = BLE_LINK_CODEC_MAX_UNKNOWN_FIELDS + 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_codec_encode_envelope(
                          &envelope, NULL, 0U, &out_len));
    /* body_len == SIZE_MAX must not wrap the size computation. */
    memset(&envelope, 0, sizeof(envelope));
    envelope.protocol_major = 1U;
    envelope.body = BLE_LINK_CODEC_BODY_REQUEST;
    envelope.body_data = dummy;
    envelope.body_len = SIZE_MAX;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_codec_encode_envelope(
                          &envelope, NULL, 0U, &out_len));
    memset(&request, 0, sizeof(request));
    request.request_id = 1U;
    request.body = BLE_LINK_CODEC_REQUEST_GET_CAPABILITIES;
    request.body_data = dummy;
    request.body_len = SIZE_MAX;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_codec_encode_request(
                          &request, NULL, 0U, &out_len));
    memset(&response, 0, sizeof(response));
    response.request_id = 1U;
    response.body = BLE_LINK_CODEC_RESPONSE_CAPABILITIES;
    response.body_data = dummy;
    response.body_len = SIZE_MAX;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_codec_encode_response(
                          &response, NULL, 0U, &out_len));
}

static void test_max_and_wrapped_field_numbers(void)
{
    /* Field number 2^29-1 (max valid): tag varint f8 ff ff ff 0f. */
    static const uint8_t max_field[] =
    {
        0xf8, 0xff, 0xff, 0xff, 0x0f, 0x01,
    };
    /* Field number 2^29 (one past the limit): tag varint 80 80 80 80 10. */
    static const uint8_t past_field[] =
    {
        0x80, 0x80, 0x80, 0x80, 0x10, 0x01,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_request_t request;
    ble_link_codec_response_t response;

    /* Valid max field decodes as unknown and is retained. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          max_field, sizeof(max_field), &envelope));
    TEST_ASSERT_EQUAL(1U, envelope.unknown_fields_count);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_codec_decode_envelope(
                          past_field,
                          sizeof(past_field),
                          &envelope));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_codec_decode_request(
                          past_field, sizeof(past_field), &request));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_codec_decode_response(
                          past_field, sizeof(past_field), &response));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_request(
                          max_field, sizeof(max_field), &request));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          max_field, sizeof(max_field), &response));
}

int main(void)
{
    test_capabilities_request_roundtrip();
    test_authorize_prepare_response_roundtrip();
    test_unknown_field_roundtrip();
    test_snapshot_envelope_roundtrip();
    test_transfer_control_envelope_roundtrip();
    test_invalid_envelopes_rejected();
    test_packed_and_unpacked_flags();
    test_duplicate_body_rejected();
    test_wrong_wire_type_rejected();
    test_invalid_key_and_overflow_varint_rejected();
    test_zero_fixed64_omitted_in_encode();
    test_invalid_body_tag_rejected();
    test_encoder_overflow_and_capacity_rejected();
    test_max_and_wrapped_field_numbers();
    test_encode_size_query_and_errors();
    printf("ble_link_codec: all tests passed\n");
    return 0;
}
