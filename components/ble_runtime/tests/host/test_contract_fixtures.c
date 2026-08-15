#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ble_link_reassembler.h"
#include "ble_link_state.h"
#include "device_link_core.h"
#include "device_link_protocol.h"
#include "device_link_tlv.h"
#include "device_link_wire.h"

/* Generated from contracts/provisioning fixtures (see
 * contract_fixtures_generate.py). */
#include "contract_fixtures.inc"

static uint8_t s_slot_buffer[128];
static ble_link_reassembler_t s_slot;

static uint32_t _read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static uint64_t _read_le64(const uint8_t *bytes)
{
    uint64_t value = 0U;

    for (size_t i = 0U; i < 8U; ++i)
    {
        value |= (uint64_t)bytes[i] << (8U * i);
    }
    return value;
}


static void _test_invalid_response_statuses(void)
{
    for (size_t i = 0U; i < s_invalid_resp_count; ++i)
    {
        device_link_status_t status = 0;

        assert(device_link_wire_decode_status(
                   s_invalid_resp_hex[i], s_invalid_resp_len[i],
                   &status) == ESP_ERR_INVALID_RESPONSE);
    }
    printf("invalid response statuses: %zu rejected\n",
           s_invalid_resp_count);
}

static void _test_direction_cases(void)
{
    for (size_t i = 0U; i < s_direction_count; ++i)
    {
        device_link_wire_header_t header;
        const esp_err_t decode_result = device_link_wire_decode_header(
                                            s_direction_hex[i],
                                            s_direction_len[i], &header);
        const bool expected_valid = s_direction_valid[i] != 0;
        /* The device receives requests (kind 1) and the App receives
         * responses (kind 2); the opposite direction is invalid. */
        const bool direction_ok =
            (s_direction_receiver[i] == 1 &&
             header.kind == DEVICE_LINK_MESSAGE_REQUEST) ||
            (s_direction_receiver[i] == 2 &&
             header.kind == DEVICE_LINK_MESSAGE_RESPONSE);

        assert(decode_result == ESP_OK);
        assert(direction_ok == expected_valid);
    }
    printf("direction cases: %zu checked\n", s_direction_count);
}

static void _test_advertising_vectors(void)
{
    assert(s_adv_public[0] == 2U);
    assert(s_adv_public[1] == 0U);
    assert(s_adv_public[2] != 0U || s_adv_public[3] != 0U ||
           s_adv_public[4] != 0U);
    assert(s_adv_bindable[0] == 2U);
    assert(s_adv_bindable[1] == 1U);
    assert(s_adv_bindable[2] != 0U || s_adv_bindable[3] != 0U ||
           s_adv_bindable[4] != 0U);
    printf("advertising vectors: public/bindable checked\n");
}

static device_link_status_t _dummy_core_method(
    const device_link_request_context_t *context,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len,
    void *arg)
{
    (void)context;
    (void)request;
    (void)request_len;
    (void)response;
    (void)response_capacity;
    (void)response_len;
    (void)arg;
    return DEVICE_LINK_STATUS_INTERNAL;
}

static void _test_channel_methods_matrix(void)
{
    /* The frozen matrix lists the Core methods with their session/control
     * channel; the startup-frozen Core descriptor must agree. */
    device_link_core_t core;
    device_link_core_callbacks_t callbacks;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.method = _dummy_core_method;
    assert(device_link_core_init(&core, &callbacks) == ESP_OK);
    for (size_t i = 0U; i < s_channel_method_count; ++i)
    {
        bool found = false;

        if (s_channel_domain[i] != DEVICE_LINK_DOMAIN_CORE)
        {
            /* Non-core domains are validated by their own adapters once
             * registered; this matrix row is not checkable here. */
            continue;
        }
        for (size_t m = 0U; m < core.domain.method_count; ++m)
        {
            if (core.domain.methods[m].method_id == s_channel_method[i])
            {
                const device_link_channel_t expected_channel =
                    s_channel_kind[i] == 1 ?
                    DEVICE_LINK_CHANNEL_SESSION :
                    DEVICE_LINK_CHANNEL_CONTROL;

                assert(core.domain.methods[m].channel == expected_channel);
                found = true;
                break;
            }
        }
        assert(found);
    }
    printf("channel methods matrix: %zu entries checked\n",
           s_channel_method_count);
}


static void _test_link_state_vectors(void)
{
    size_t skipped = 0U;

    for (size_t i = 0U; i < s_link_state_count; ++i)
    {
        const uint8_t *raw = s_link_state_hex[i];
        const bool expected_valid = s_link_state_valid[i] != 0;
        uint8_t encoded[BLE_LINK_STATE_MAX_ENCODED_BYTES];
        size_t encoded_len = 0U;
        ble_link_state_t state;

        if (s_link_state_len[i] != BLE_LINK_STATE_MAX_ENCODED_BYTES)
        {
            /* Decode-side length vector; the C encoder has no length
             * input, so the short-value case is covered by the C decoder
             * tests instead. */
            skipped++;
            continue;
        }
        memset(&state, 0, sizeof(state));
        state.protocol_major = raw[0];
        state.protocol_minor = raw[1];
        state.profile_major = raw[2];
        state.profile_minor = raw[3];
        state.state_flags = _read_le32(&raw[4]);
        state.boot_id = _read_le64(&raw[8]);
        if (expected_valid)
        {
            assert(ble_link_state_encode(
                       &state, encoded, sizeof(encoded), &encoded_len) ==
                   ESP_OK);
            assert(encoded_len == BLE_LINK_STATE_MAX_ENCODED_BYTES);
            assert(memcmp(encoded, raw, BLE_LINK_STATE_MAX_ENCODED_BYTES) ==
                   0);
        }
        else
        {
            assert(ble_link_state_encode(
                       &state, encoded, sizeof(encoded), &encoded_len) ==
                   ESP_ERR_INVALID_ARG);
        }
    }
    printf("link_state vectors: %zu checked, %zu short-value skipped\n",
           s_link_state_count - skipped, skipped);
}

static void _test_application_headers(void)
{
    for (size_t i = 0U; i < s_app_header_count; ++i)
    {
        device_link_wire_header_t header;
        uint8_t encoded[DEVICE_LINK_WIRE_HEADER_BYTES];

        assert(device_link_wire_decode_header(
                   s_app_header_hex[i], s_app_header_len[i], &header) ==
               ESP_OK);
        assert(header.kind == (uint8_t)s_app_header_kind[i]);
        assert(header.recovery_query == (s_app_header_recovery[i] != 0));
        assert(device_link_wire_encode_header(&header, encoded) == ESP_OK);
        assert(memcmp(encoded, s_app_header_hex[i],
                      DEVICE_LINK_WIRE_HEADER_BYTES) == 0);
    }
    for (size_t i = 0U; i < s_invalid_header_count; ++i)
    {
        device_link_wire_header_t header;

        assert(device_link_wire_decode_header(
                   s_invalid_header_hex[i], s_invalid_header_len[i],
                   &header) == ESP_ERR_INVALID_RESPONSE);
    }
    printf("application headers: %zu valid, %zu invalid\n",
           s_app_header_count, s_invalid_header_count);
}

static void _test_response_statuses(void)
{
    for (size_t i = 0U; i < s_response_status_count; ++i)
    {
        device_link_status_t status = 0;

        assert(device_link_wire_decode_status(
                   s_response_status_hex[i], s_response_status_len[i],
                   &status) == ESP_OK);
        assert(status != 0U);
    }
    for (size_t i = 0U; i < s_link_error_count; ++i)
    {
        uint8_t bytes[DEVICE_LINK_RESPONSE_STATUS_BYTES];
        device_link_status_t status = 0;

        assert(device_link_wire_encode_status(
                   (device_link_status_t)s_link_error_value[i], bytes) ==
               ESP_OK);
        assert(memcmp(bytes, s_link_error_hex[i],
                      DEVICE_LINK_RESPONSE_STATUS_BYTES) == 0);
        assert(device_link_wire_decode_status(
                   bytes, sizeof(bytes), &status) == ESP_OK);
        assert(status == (device_link_status_t)s_link_error_value[i]);
    }
    printf("response statuses: %zu valid, %zu link errors\n",
           s_response_status_count, s_link_error_count);
}

static int _replay_fragments(const uint8_t *const *fragments,
                             const size_t *lens, size_t count)
{
    ble_link_reassembler_init(&s_slot, s_slot_buffer, sizeof(s_slot_buffer));
    int disposition = -1;

    for (size_t i = 0U; i < count; ++i)
    {
        ble_link_fragment_t fragment;

        assert(ble_link_reassembler_parse(
                   fragments[i], lens[i], &fragment) == ESP_OK);
        ble_link_reassembly_disposition_t outcome;
        const esp_err_t result = ble_link_reassembler_accept_ex(
                                     &s_slot, &fragment, &outcome);

        if (i + 1U == count)
        {
            disposition = (int)outcome;
        }
        if (result != ESP_OK)
        {
            return -1;
        }
    }
    return disposition;
}

static void _test_framing_vectors(void)
{
    /* The standalone `valid` vectors mirror the contract validator's
     * structural rules: parseable header, payload inside the frame, START
     * at offset zero, END reaching the total. */
    for (size_t i = 0U; i < s_frag_valid_count; ++i)
    {
        ble_link_fragment_t fragment;

        assert(ble_link_reassembler_parse(
                   s_frag_valid_hex[i], s_frag_valid_len[i], &fragment) ==
               ESP_OK);
        assert(fragment.payload_len > 0U);
        assert(fragment.payload_len <=
               (size_t)fragment.total_length - fragment.offset);
        if ((fragment.flags & BLE_LINK_FRAMING_FLAG_START) != 0U)
        {
            assert(fragment.offset == 0U);
        }
        if ((fragment.flags & BLE_LINK_FRAMING_FLAG_END) != 0U)
        {
            assert(fragment.offset + fragment.payload_len ==
                   fragment.total_length);
        }
    }
    /* Every `invalid` vector must fail parse or cold acceptance. */
    for (size_t i = 0U; i < s_frag_invalid_count; ++i)
    {
        ble_link_fragment_t fragment;
        const esp_err_t parse_result = ble_link_reassembler_parse(
                                           s_frag_invalid_hex[i],
                                           s_frag_invalid_len[i], &fragment);

        if (parse_result != ESP_OK)
        {
            continue;
        }
        ble_link_reassembler_init(&s_slot, s_slot_buffer,
                                  sizeof(s_slot_buffer));
        ble_link_reassembly_disposition_t outcome;

        assert(ble_link_reassembler_accept_ex(
                   &s_slot, &fragment, &outcome) == ESP_ERR_INVALID_ARG);
    }
    /* Stateful sequences: the disposition of the last fragment must match
     * the frozen sequence outcome. */
    for (size_t i = 0U; i < s_seq_count; ++i)
    {
        const int disposition = _replay_fragments(
                                    s_seq_frags_all[i],
                                    s_seq_lens_all[i],
                                    s_seq_frag_counts_all[i]);

        assert(disposition == s_seq_disposition_all[i]);
    }
    printf("framing vectors: %zu valid, %zu invalid, %zu sequences\n",
           s_frag_valid_count, s_frag_invalid_count, s_seq_count);
}

static void _test_authorization_schemas(void)
{
    const struct
    {
        const uint8_t *bytes;
        size_t len;
        uint8_t method;
        bool response;
    } vectors[] =
    {
        {
            s_auth_prepare, s_auth_prepare_len, s_auth_prepare_method,
            s_auth_prepare_is_response
        },
        {
            s_auth_prepare_response, s_auth_prepare_response_len,
            s_auth_prepare_response_method, s_auth_prepare_response_is_response
        },
        {
            s_auth_commit_probe, s_auth_commit_probe_len,
            s_auth_commit_probe_method, s_auth_commit_probe_is_response
        },
        {
            s_auth_confirmation_required, s_auth_confirmation_required_len,
            s_auth_confirmation_required_method,
            s_auth_confirmation_required_is_response
        },
        {
            s_auth_commit_success, s_auth_commit_success_len,
            s_auth_commit_success_method, s_auth_commit_success_is_response
        },
    };

    for (size_t i = 0U; i < sizeof(vectors) / sizeof(vectors[0]); ++i)
    {
        const device_link_tlv_schema_t *schema = vectors[i].response ?
            device_link_core_test_response_schema(vectors[i].method) :
            device_link_core_test_request_schema(vectors[i].method);

        assert(schema != NULL);
        assert(device_link_tlv_validate_message(
                   vectors[i].bytes, vectors[i].len, schema) == ESP_OK);
    }
    printf("authorization schemas: %zu vectors validated\n",
           sizeof(vectors) / sizeof(vectors[0]));
}

int main(void)
{
    _test_link_state_vectors();
    _test_application_headers();
    _test_response_statuses();
    _test_invalid_response_statuses();
    _test_direction_cases();
    _test_advertising_vectors();
    _test_channel_methods_matrix();
    _test_framing_vectors();
    _test_authorization_schemas();
    puts("contract_fixtures: all fixture vectors passed");
    return 0;
}
