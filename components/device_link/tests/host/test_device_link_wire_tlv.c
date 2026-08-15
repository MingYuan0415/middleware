#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_link_core.h"
#include "device_link_tlv.h"
#include "device_link_wire.h"

typedef enum core_response_mode
{
    CORE_RESPONSE_VALID = 0,
    CORE_RESPONSE_PASSWORD_TOO_LONG,
    CORE_RESPONSE_CREDENTIAL_ZERO,
} core_response_mode_t;

static core_response_mode_t s_core_response_mode;
static unsigned s_core_handler_calls;

static esp_err_t _core_digest(
    const uint8_t *request, size_t request_len,
    uint8_t digest[DEVICE_LINK_REPLAY_DIGEST_BYTES], void *arg)
{
    (void)arg;
    uint32_t value = 2166136261U;

    for (size_t i = 0U; i < request_len; ++i)
    {
        value ^= request[i];
        value *= 16777619U;
    }
    memset(digest, 0, DEVICE_LINK_REPLAY_DIGEST_BYTES);
    memcpy(digest, &value, sizeof(value));
    return ESP_OK;
}

static device_link_status_t _core_method(
    const device_link_request_context_t *context,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len,
    void *arg)
{
    (void)request;
    (void)request_len;
    (void)arg;
    s_core_handler_calls++;
    if (context->header.method_id != 3U)
    {
        /* Negative request tests must be rejected before the handler. */
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    device_link_tlv_writer_t writer;
    uint8_t credential[16];
    uint8_t password[17];

    memset(credential, 0x11, sizeof(credential));
    memset(password, 0x22, sizeof(password));
    device_link_tlv_writer_init(&writer, response, response_capacity);
    assert(device_link_tlv_put_fixed64(
               &writer, 1U, UINT64_C(0x1111111111111111)) == ESP_OK);
    if (s_core_response_mode == CORE_RESPONSE_CREDENTIAL_ZERO)
    {
        memset(credential, 0U, sizeof(credential));
    }
    assert(device_link_tlv_put_bytes(
               &writer, 2U, credential, sizeof(credential)) == ESP_OK);
    if (s_core_response_mode == CORE_RESPONSE_PASSWORD_TOO_LONG)
    {
        assert(device_link_tlv_put_bytes(
                   &writer, 3U, password, sizeof(password)) == ESP_OK);
    }
    else
    {
        assert(device_link_tlv_put_bytes(
                   &writer, 3U, password, 16U) == ESP_OK);
    }
    assert(device_link_tlv_put_uint(&writer, 4U, 120000U) == ESP_OK);
    assert(device_link_tlv_put_uint(
               &writer, 5U, DEVICE_LINK_PERMISSION_CORE_READ) == ESP_OK);
    assert(device_link_tlv_writer_finish(&writer, response_len) == ESP_OK);
    return DEVICE_LINK_STATUS_OK;
}

static void _test_core_auth_schema(void)
{
    const uint64_t boot_id = UINT64_C(0x0102030405060708);
    device_link_core_t core;
    device_link_core_callbacks_t callbacks;
    uint8_t request[64];
    size_t request_len = 0U;
    uint8_t response[128];
    size_t response_len = 0U;
    device_link_status_t status = 0;
    device_link_wire_header_t prepare_header;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.method = _core_method;
    assert(device_link_core_init(&core, &callbacks) == ESP_OK);
    uint8_t replay_response[DEVICE_LINK_REPLAY_SLOTS][128];
    device_link_call_replay_t replay[DEVICE_LINK_REPLAY_SLOTS];

    memset(replay, 0, sizeof(replay));
    for (size_t i = 0U; i < DEVICE_LINK_REPLAY_SLOTS; ++i)
    {
        replay[i].response = replay_response[i];
        replay[i].response_capacity = sizeof(replay_response[i]);
    }
    device_link_router_t router;

    assert(device_link_router_init(
               &router, boot_id, &core.domain, 1U, replay,
               DEVICE_LINK_REPLAY_SLOTS, _core_digest, NULL) == ESP_OK);
    assert(device_link_router_seal(&router) == ESP_OK);
    device_link_request_context_t facts;

    memset(&facts, 0, sizeof(facts));
    facts.connection_generation = 1U;
    facts.security_epoch = 1U;
    facts.channel = DEVICE_LINK_CHANNEL_SESSION;
    facts.admission = DEVICE_LINK_ADMISSION_UNBOUND_PUBLIC;
    facts.security_authenticated = true;

    /* Baseline: a valid AuthorizePrepare round-trips. */
    prepare_header = (device_link_wire_header_t)
    {
        .kind = DEVICE_LINK_MESSAGE_REQUEST,
        .domain_id = DEVICE_LINK_DOMAIN_CORE,
        .domain_major = DEVICE_LINK_CORE_MAJOR,
        .method_id = 3U,
        .call_id = 1U,
        .boot_id = boot_id,
    };

    assert(device_link_wire_encode_header(&prepare_header, request) == ESP_OK);
    request[DEVICE_LINK_WIRE_HEADER_BYTES] = 0x04U;
    request[DEVICE_LINK_WIRE_HEADER_BYTES + 1U] =
        DEVICE_LINK_PERMISSION_CORE_READ;
    request_len = DEVICE_LINK_WIRE_HEADER_BYTES + 2U;
    s_core_response_mode = CORE_RESPONSE_VALID;
    s_core_handler_calls = 0U;
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) == ESP_OK);
    assert(s_core_handler_calls == 1U);

    assert(device_link_wire_decode_status(
               &response[DEVICE_LINK_WIRE_HEADER_BYTES],
               response_len - DEVICE_LINK_WIRE_HEADER_BYTES,
               &status) == ESP_OK);
    assert(status == DEVICE_LINK_STATUS_OK);
    assert(response_len > DEVICE_LINK_WIRE_HEADER_BYTES +
           DEVICE_LINK_RESPONSE_STATUS_BYTES);

    /* application_password longer than the frozen 16 bytes is rejected. */
    s_core_response_mode = CORE_RESPONSE_PASSWORD_TOO_LONG;
    s_core_handler_calls = 0U;
    prepare_header.call_id = 2U;
    assert(device_link_wire_encode_header(&prepare_header, request) == ESP_OK);
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) == ESP_OK);
    assert(s_core_handler_calls == 1U);
    assert(device_link_wire_decode_status(
               &response[DEVICE_LINK_WIRE_HEADER_BYTES],
               response_len - DEVICE_LINK_WIRE_HEADER_BYTES,
               &status) == ESP_OK);
    assert(status == DEVICE_LINK_STATUS_INTERNAL);

    /* All-zero credential_id violates the NONZERO rule. */
    s_core_response_mode = CORE_RESPONSE_CREDENTIAL_ZERO;
    s_core_handler_calls = 0U;
    prepare_header.call_id = 3U;
    assert(device_link_wire_encode_header(&prepare_header, request) == ESP_OK);
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) == ESP_OK);
    assert(s_core_handler_calls == 1U);
    assert(device_link_wire_decode_status(
               &response[DEVICE_LINK_WIRE_HEADER_BYTES],
               response_len - DEVICE_LINK_WIRE_HEADER_BYTES,
               &status) == ESP_OK);
    assert(status == DEVICE_LINK_STATUS_INTERNAL);

    /* AuthorizeCommitRequest with an all-zero credential_id is rejected
     * before the handler runs (NONZERO on the request schema). */
    const device_link_wire_header_t commit_header =
    {
        .kind = DEVICE_LINK_MESSAGE_REQUEST,
        .domain_id = DEVICE_LINK_DOMAIN_CORE,
        .domain_major = DEVICE_LINK_CORE_MAJOR,
        .method_id = 4U,
        .call_id = 4U,
        .boot_id = boot_id,
    };

    assert(device_link_wire_encode_header(&commit_header, request) == ESP_OK);
    request[DEVICE_LINK_WIRE_HEADER_BYTES] = 0x09U;
    memset(&request[DEVICE_LINK_WIRE_HEADER_BYTES + 1U], 0x11U, 8U);
    request[DEVICE_LINK_WIRE_HEADER_BYTES + 9U] = 0x12U;
    request[DEVICE_LINK_WIRE_HEADER_BYTES + 10U] = 16U;
    memset(&request[DEVICE_LINK_WIRE_HEADER_BYTES + 11U], 0U, 16U);
    request_len = DEVICE_LINK_WIRE_HEADER_BYTES + 27U;
    s_core_handler_calls = 0U;
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) == ESP_OK);
    assert(s_core_handler_calls == 0U);
    assert(device_link_wire_decode_status(
               &response[DEVICE_LINK_WIRE_HEADER_BYTES],
               response_len - DEVICE_LINK_WIRE_HEADER_BYTES,
               &status) == ESP_OK);
    assert(status == DEVICE_LINK_STATUS_INVALID_ARGUMENT);

    /* GetAuthorizationRequest with an all-zero credential_id is rejected
     * (recovery query required and NONZERO credential). */
    const device_link_wire_header_t get_auth_header =
    {
        .kind = DEVICE_LINK_MESSAGE_REQUEST,
        .recovery_query = true,
        .domain_id = DEVICE_LINK_DOMAIN_CORE,
        .domain_major = DEVICE_LINK_CORE_MAJOR,
        .method_id = 5U,
        .call_id = 5U,
        .boot_id = boot_id,
    };

    assert(device_link_wire_encode_header(
               &get_auth_header, request) == ESP_OK);
    request[DEVICE_LINK_WIRE_HEADER_BYTES] = 0x0aU;
    request[DEVICE_LINK_WIRE_HEADER_BYTES + 1U] = 16U;
    memset(&request[DEVICE_LINK_WIRE_HEADER_BYTES + 2U], 0U, 16U);
    request_len = DEVICE_LINK_WIRE_HEADER_BYTES + 18U;
    s_core_handler_calls = 0U;
    assert(device_link_router_process(
               &router, &facts, request, request_len,
               response, sizeof(response), &response_len) == ESP_OK);
    assert(s_core_handler_calls == 0U);
    assert(device_link_wire_decode_status(
               &response[DEVICE_LINK_WIRE_HEADER_BYTES],
               response_len - DEVICE_LINK_WIRE_HEADER_BYTES,
               &status) == ESP_OK);
    assert(status == DEVICE_LINK_STATUS_INVALID_ARGUMENT);
}

static void _test_wire_header(void)
{
    const device_link_wire_header_t header =
    {
        .kind = DEVICE_LINK_MESSAGE_REQUEST,
        .recovery_query = true,
        .domain_id = DEVICE_LINK_DOMAIN_CORE,
        .domain_major = 2U,
        .method_id = 5U,
        .call_id = 0x12345678U,
        .boot_id = UINT64_C(0x0102030405060708),
    };
    const uint8_t expected[DEVICE_LINK_WIRE_HEADER_BYTES] =
    {
        0x15, 0x00, 0x02, 0x05,
        0x78, 0x56, 0x34, 0x12,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    };
    uint8_t encoded[DEVICE_LINK_WIRE_HEADER_BYTES];
    device_link_wire_header_t decoded;

    assert(device_link_wire_encode_header(&header, encoded) == ESP_OK);
    assert(memcmp(encoded, expected, sizeof(expected)) == 0);
    assert(device_link_wire_decode_header(
               encoded, sizeof(encoded), &decoded) == ESP_OK);
    assert(decoded.kind == header.kind);
    assert(decoded.recovery_query);
    assert(decoded.call_id == header.call_id);
    assert(decoded.boot_id == header.boot_id);

    encoded[0] |= 0x02U;
    assert(device_link_wire_decode_header(
               encoded, sizeof(encoded), &decoded) ==
           ESP_ERR_INVALID_RESPONSE);
    encoded[0] = 0x15U;
    encoded[4] = 0U;
    encoded[5] = 0U;
    encoded[6] = 0U;
    encoded[7] = 0U;
    assert(device_link_wire_decode_header(
               encoded, sizeof(encoded), &decoded) ==
           ESP_ERR_INVALID_RESPONSE);

    uint8_t status_bytes[2];
    device_link_status_t status;

    assert(device_link_wire_encode_status(
               DEVICE_LINK_STATUS_CONFLICT, status_bytes) == ESP_OK);
    assert(status_bytes[0] == 13U && status_bytes[1] == 0U);
    assert(device_link_wire_decode_status(
               status_bytes, sizeof(status_bytes), &status) == ESP_OK);
    assert(status == DEVICE_LINK_STATUS_CONFLICT);
    status_bytes[0] = 0U;
    assert(device_link_wire_decode_status(
               status_bytes, sizeof(status_bytes), &status) ==
           ESP_ERR_INVALID_RESPONSE);
}

static void _test_tlv_canonical(void)
{
    uint8_t encoded[64];
    device_link_tlv_writer_t writer;
    size_t encoded_len = 0U;

    device_link_tlv_writer_init(&writer, encoded, sizeof(encoded));
    assert(device_link_tlv_put_uint(&writer, 1U, 127U) == ESP_OK);
    assert(device_link_tlv_put_sint(&writer, 2U, -2) == ESP_OK);
    assert(device_link_tlv_put_bytes(
               &writer, 3U, (const uint8_t *)"ok", 2U) == ESP_OK);
    assert(device_link_tlv_put_fixed64(
               &writer, 4U, UINT64_C(0x0102030405060708)) == ESP_OK);
    assert(device_link_tlv_writer_finish(&writer, &encoded_len) == ESP_OK);

    const uint8_t expected[] =
    {
        0x04, 0x7f,
        0x09, 0x03,
        0x0e, 0x02, 0x00, 'o', 'k',
        0x13, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    };
    assert(encoded_len == sizeof(expected));
    assert(memcmp(encoded, expected, sizeof(expected)) == 0);

    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has_field = false;

    assert(device_link_tlv_reader_init(
               &reader, encoded, encoded_len) == ESP_OK);
    assert(device_link_tlv_reader_next(&reader, &field, &has_field) == ESP_OK);
    assert(has_field && field.id == 1U &&
           field.value.unsigned_value == 127U);
    assert(device_link_tlv_reader_next(&reader, &field, &has_field) == ESP_OK);
    assert(has_field && field.value.signed_value == -2);
    assert(device_link_tlv_reader_next(&reader, &field, &has_field) == ESP_OK);
    assert(has_field && field.value.bytes.len == 2U);
    assert(device_link_tlv_reader_next(&reader, &field, &has_field) == ESP_OK);
    assert(has_field && field.value.fixed64_value ==
           UINT64_C(0x0102030405060708));
    assert(device_link_tlv_reader_next(&reader, &field, &has_field) == ESP_OK);
    assert(!has_field);

    const uint8_t nonminimal[] = {0x04, 0x80, 0x00};
    assert(device_link_tlv_reader_init(
               &reader, nonminimal, sizeof(nonminimal)) == ESP_OK);
    assert(device_link_tlv_reader_next(
               &reader, &field, &has_field) == ESP_ERR_INVALID_RESPONSE);

    const uint8_t unordered[] = {0x08, 0x01, 0x04, 0x01};
    assert(device_link_tlv_reader_init(
               &reader, unordered, sizeof(unordered)) == ESP_OK);
    assert(device_link_tlv_reader_next(&reader, &field, &has_field) == ESP_OK);
    assert(device_link_tlv_reader_next(
               &reader, &field, &has_field) == ESP_ERR_INVALID_RESPONSE);
}

static void _test_schema_validation(void)
{
    static const uint64_t enum_values[] = {1U, 2U};
    static const device_link_tlv_field_rule_t nested_rules[] =
    {
        {
            .id = 1U,
            .wire_type = DEVICE_LINK_TLV_UNSIGNED,
            .flags = DEVICE_LINK_TLV_RULE_REQUIRED |
            DEVICE_LINK_TLV_RULE_BOOL,
            .maximum_unsigned = 1U,
        },
    };
    static const device_link_tlv_schema_t nested_schema =
    {
        .fields = nested_rules,
        .field_count = 1U,
        .maximum_encoded_bytes = 2U,
    };
    static const device_link_tlv_field_rule_t rules[] =
    {
        {
            .id = 1U,
            .wire_type = DEVICE_LINK_TLV_UNSIGNED,
            .flags = DEVICE_LINK_TLV_RULE_REQUIRED,
            .minimum_unsigned = 1U,
            .maximum_unsigned = 2U,
            .enum_values = enum_values,
            .enum_count = 2U,
        },
        {
            .id = 2U,
            .wire_type = DEVICE_LINK_TLV_LENGTH,
            .flags = DEVICE_LINK_TLV_RULE_UTF8,
            .maximum_bytes = 4U,
        },
        {
            .id = 3U,
            .wire_type = DEVICE_LINK_TLV_UNSIGNED,
            .flags = DEVICE_LINK_TLV_RULE_REPEATED,
            .maximum_count = 2U,
            .maximum_unsigned = UINT64_MAX,
        },
        {
            .id = 4U,
            .wire_type = DEVICE_LINK_TLV_LENGTH,
            .flags = DEVICE_LINK_TLV_RULE_MESSAGE,
            .maximum_bytes = 2U,
            .nested = &nested_schema,
        },
    };
    static const device_link_tlv_schema_t schema =
    {
        .fields = rules,
        .field_count = 4U,
        .maximum_encoded_bytes = 32U,
    };
    const uint8_t valid[] =
    {
        0x04, 0x01,
        0x0a, 0x02, 0x00, 'o', 'k',
        0x0c, 0x03,
        0x0c, 0x04,
        0x12, 0x02, 0x00, 0x04, 0x01,
        0x16, 0x01, 0x00, 0xff,
    };
    assert(device_link_tlv_validate_message(
               valid, sizeof(valid), &schema) == ESP_OK);

    const uint8_t missing[] = {0x0c, 0x01};
    assert(device_link_tlv_validate_message(
               missing, sizeof(missing), &schema) ==
           ESP_ERR_INVALID_RESPONSE);

    const uint8_t duplicate[] = {0x04, 0x01, 0x04, 0x02};
    assert(device_link_tlv_validate_message(
               duplicate, sizeof(duplicate), &schema) ==
           ESP_ERR_INVALID_RESPONSE);

    const uint8_t unknown_enum[] = {0x04, 0x03};
    assert(device_link_tlv_validate_message(
               unknown_enum, sizeof(unknown_enum), &schema) ==
           ESP_ERR_INVALID_RESPONSE);

    const uint8_t bad_utf8[] = {0x04, 0x01, 0x0a, 0x01, 0x00, 0x80};
    assert(device_link_tlv_validate_message(
               bad_utf8, sizeof(bad_utf8), &schema) ==
           ESP_ERR_INVALID_RESPONSE);

    const uint8_t too_many[] =
    {
        0x04, 0x01, 0x0c, 0x01, 0x0c, 0x02, 0x0c, 0x03,
    };
    assert(device_link_tlv_validate_message(
               too_many, sizeof(too_many), &schema) ==
           ESP_ERR_INVALID_RESPONSE);
}

int main(void)
{
    _test_wire_header();
    _test_tlv_canonical();
    _test_schema_validation();
    _test_core_auth_schema();
    puts("device_link wire/tlv tests passed");
    return 0;
}
