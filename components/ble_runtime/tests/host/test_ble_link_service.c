#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_codec.h"
#include "ble_link_events.h"
#include "ble_link_reassembler.h"
#include "ble_link_service.h"
#include "ble_link_session.h"

#include "esp_random.h"

#include "device_link_security.h"

static esp_err_t _sec_stub_request(
    const uint8_t *request, size_t request_len,
    uint8_t **response, size_t *response_len, void *arg)
{
    (void)request;
    (void)request_len;
    (void)arg;
    *response = NULL;
    *response_len = 0U;
    return ESP_ERR_NOT_SUPPORTED;
}

static const device_link_security_config_t s_sec_config =
{
    .username = "microtech",
    .session_id = 1U,
    .request_cb = _sec_stub_request,
    .request_arg = NULL,
};

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
        const long long expected_value = (long long)(expected); \
        const long long actual_value = (long long)(actual); \
        if (expected_value != actual_value) \
        { \
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%lld != %lld)\n", \
                    __LINE__, #expected, #actual, expected_value, actual_value); \
            abort(); \
        } \
    } while (0)

#define BOOT_ID 72623859790382856ULL
#define GEN 1U

/* Capture sink state. */
static uint8_t s_capture[16][512];
static size_t s_capture_lens[16];
static size_t s_capture_count;
static ble_link_service_tx_channel_t s_capture_channels[16];

/* Reassembly of the captured outbound fragments. */
static uint8_t s_outbound[1024];
static size_t s_outbound_len;

static ble_link_service_facts_t s_facts;

static esp_err_t _capture(const uint8_t *value, size_t len,
                          ble_link_service_tx_channel_t channel,
                          bool is_last, void *arg)
{
    (void)is_last;
    (void)arg;
    TEST_ASSERT_TRUE(s_capture_count < 16U);
    TEST_ASSERT_TRUE(len <= sizeof(s_capture[0]));
    memcpy(s_capture[s_capture_count], value, len);
    s_capture_lens[s_capture_count] = len;
    s_capture_channels[s_capture_count] = channel;
    s_capture_count++;
    if (is_last)
    {
        ble_link_service_response_completed();
    }
    return ESP_OK;
}

/**
 * @brief Frame one message payload as a single fragment and feed it.
 */
static void _feed_single_channel(
    const uint8_t *payload, size_t payload_len,
    ble_link_service_rx_channel_t channel)
{
    uint8_t framed[512];
    const size_t total = payload_len + 1U;

    framed[0] = 1U;
    framed[1] = 3U; /* START|END */
    framed[2] = 0x01U;
    framed[3] = 0x00U;
    framed[4] = (uint8_t)(total & 0xffU);
    framed[5] = (uint8_t)((total >> 8U) & 0xffU);
    framed[6] = 0x00U;
    framed[7] = 0x00U;
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], payload, payload_len);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &s_facts, channel, framed, 8U + total));
}

static void _feed_single(const uint8_t *payload, size_t payload_len)
{
    _feed_single_channel(payload, payload_len,
                         BLE_LINK_SERVICE_RX_CONTROL);
}

/**
 * @brief Reassemble captured fragments into one message.
 */
static void _reassemble_captured(void)
{
    s_outbound_len = 0U;
    for (size_t i = 0U; i < s_capture_count; ++i)
    {
        const uint8_t *f = s_capture[i];
        const size_t f_len = s_capture_lens[i];

        TEST_ASSERT_TRUE(f_len >= 8U);
        memcpy(&s_outbound[s_outbound_len], &f[8], f_len - 8U);
        s_outbound_len += f_len - 8U;
    }
    /* The reassembled message begins with the transport type byte. */
    TEST_ASSERT_TRUE(s_outbound_len >= 1U);
    TEST_ASSERT_EQUAL(BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED,
                      s_outbound[0]);
    memmove(s_outbound, &s_outbound[1], s_outbound_len - 1U);
    s_outbound_len -= 1U;
}

static void _set_facts(bool encrypted, bool authenticated, bool authorized)
{
    memset(&s_facts, 0, sizeof(s_facts));
    s_facts.active_boot_id = BOOT_ID;
    s_facts.connection_generation = GEN;
    s_facts.preferred_att_mtu = 495U;
    s_facts.encrypted = encrypted;
    s_facts.session_authenticated = authenticated;
    s_facts.authorized = authorized;
    s_facts.identity_known = true;
    s_facts.secure_connections_bond_verified = true;
}

static void _establish_session(void)
{
    ble_link_session_init(BOOT_ID);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN,
                          BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN,
                          BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    uint32_t epoch = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          GEN, &epoch));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(
                          GEN, true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, 1U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN, 1U, 1U));
    _set_facts(true, true, true);
}

static void _reset(void)
{
    memset(s_capture, 0, sizeof(s_capture));
    memset(s_capture_lens, 0, sizeof(s_capture_lens));
    memset(s_capture_channels, 0, sizeof(s_capture_channels));
    s_capture_count = 0U;
    ble_link_service_reset();
    ble_link_service_init(BOOT_ID, _capture, NULL, NULL, 32U);
    ble_link_events_init();
    _establish_session();
    _set_facts(true, true, true);
    (void)device_link_security_init(&s_sec_config);
}

static void test_capabilities_request(void)
{
    /* Frozen fixture: request_id=1, get_capabilities empty. */
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    _feed_single(request, sizeof(request));
    TEST_ASSERT_EQUAL(1U, s_capture_count);
    TEST_ASSERT_EQUAL(BLE_LINK_SERVICE_TX_CONTROL_RESPONSE,
                      s_capture_channels[0]);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_BODY_RESPONSE, envelope.body);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(1U, response.request_id);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_CAPABILITIES, response.body);
}

static void test_capabilities_response_bytes(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    ble_link_codec_envelope_t envelope;

    _reset();
    _feed_single(request, sizeof(request));
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    static const uint8_t expected[] =
    {
        0x0a, 0x02, 0x08, 0x01,
        0x12, 0x02, 0x08, 0x01,
        0x1a, 0x0d, 0x08, 0x01, 0x10, 0x08, 0x18, 0xf2,
        0x03, 0x20, 0x80, 0x20, 0x28, 0x80, 0x08,
        0x22, 0x0e, 0x08, 0x01, 0x10, 0x10, 0x18, 0x01,
        0x20, 0x02, 0x28, 0x01, 0x30, 0x01, 0x38, 0x01,
    };
    /* actual: 0a020801 12020801 1a0d0801100818f203208020288008
     *          220e0801101018012002280130013801 2802 */

    ble_link_codec_response_t response;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_CAPABILITIES, response.body);
    TEST_ASSERT_EQUAL(sizeof(expected), response.body_len);
    TEST_ASSERT_EQUAL(0, memcmp(response.body_data, expected,
                                sizeof(expected)));
}

static void test_snapshot_request(void)
{
    /* request_id=2, get_link_snapshot. */
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0x00,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    _feed_single(request, sizeof(request));
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(2U, response.request_id);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_SNAPSHOT, response.body);
    /* Snapshot: event_sequence=0 (no events yet), link_state present. */
    TEST_ASSERT_EQUAL(0x09U, response.body_data[0]);
}

/* authorize_prepare request, request_id=3. */
static const uint8_t s_prepare_request[] =
{
    0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
};

/**
 * @brief Parse the captured prepare response (Envelope at s_outbound) and
 * build a matching AuthorizeCommit request body.
 *
 * The device generates a random txn id and credential id per prepare, so
 * the commit must echo what the device produced. The response body layout
 * is fixed by the encoder: tag(0x09)+fixed64 txn, tag(0x12)+len(0x10)+16B
 * credential.
 */
static uint8_t s_pending_txn[8];
static uint8_t s_pending_credential[16];
static bool s_pending_captured;

static void _capture_pending_credential(void)
{
    if (s_pending_captured)
    {
        return;
    }
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_AUTHORIZE_PREPARE,
                      response.body);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, response.error);
    TEST_ASSERT_TRUE(response.body_len >= 27U);
    TEST_ASSERT_EQUAL(0x09U, response.body_data[0]);
    TEST_ASSERT_EQUAL(0x12U, response.body_data[9]);
    TEST_ASSERT_EQUAL(0x10U, response.body_data[10]);
    memcpy(s_pending_txn, &response.body_data[1], 8U);
    memcpy(s_pending_credential, &response.body_data[11], 16U);
    s_pending_captured = true;
}

static size_t _build_commit_body(uint8_t *out, size_t out_cap,
                                 uint64_t request_id)
{
    _capture_pending_credential();
    const size_t commit_len = 1U + 8U + 1U + 1U + 16U;
    uint8_t commit_body[32];

    TEST_ASSERT_TRUE(commit_len <= sizeof(commit_body));
    commit_body[0] = 0x09U;
    memcpy(&commit_body[1], s_pending_txn, 8U);
    commit_body[9] = 0x12U;
    commit_body[10] = 0x10U;
    memcpy(&commit_body[11], s_pending_credential, 16U);
    /* Request message: request_id + authorize_commit body (field 13). */
    const size_t request_len = 1U + 8U + 2U + commit_len;
    uint8_t request_msg[48];

    TEST_ASSERT_TRUE(request_len <= sizeof(request_msg));
    request_msg[0] = 0x09U;
    for (size_t i = 0U; i < 8U; ++i)
    {
        request_msg[1U + i] = (uint8_t)(request_id >> (8U * i));
    }
    request_msg[9] = 0x6aU;
    request_msg[10] = (uint8_t)commit_len;
    memcpy(&request_msg[11], commit_body, commit_len);
    /* Wrap the request message in a full Envelope. */
    const size_t len = 2U + 9U + 2U + request_len;

    TEST_ASSERT_TRUE(len <= out_cap);
    out[0] = 0x08U;
    out[1] = 0x01U;
    out[2] = 0x19U;
    const uint64_t boot = BOOT_ID;

    for (size_t i = 0U; i < 8U; ++i)
    {
        out[3U + i] = (uint8_t)(boot >> (8U * i));
    }
    out[11] = 0x52U;
    out[12] = (uint8_t)request_len;
    memcpy(&out[13], request_msg, request_len);
    return len;
}

static void test_authorize_flow(void)
{
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;
    uint8_t commit[64];

    _reset();
    s_pending_captured = false;
    _set_facts(true, true, true);
    esp_random_fake_reset(0x5eed5eedU);
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    TEST_ASSERT_EQUAL(1U, s_capture_count);
    TEST_ASSERT_EQUAL(BLE_LINK_SERVICE_TX_SESSION,
                      s_capture_channels[0]);
    _reassemble_captured();
    const size_t commit_len = _build_commit_body(commit, sizeof(commit), 5U);

    /* A commit before local confirmation is refused. */
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    TEST_ASSERT_TRUE(ble_link_service_pending_confirmation());
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_CONFIRMATION_REQUIRED, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_NONE, response.body);

    /* Confirm locally, then commit succeeds with a fresh request id
     * (the dispatcher replay guard rejects reused ids). */
    ble_link_service_confirm_binding(true);
    TEST_ASSERT_TRUE(!ble_link_service_pending_confirmation());
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    const size_t commit2_len = _build_commit_body(commit, sizeof(commit), 6U);

    TEST_ASSERT_EQUAL(commit_len, commit2_len);
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT,
                      response.body);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, response.error);
}

static void test_authorize_commit_wrong_credential(void)
{
    /* Wrong credential: all 0xff. */
    static const uint8_t commit[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x26, 0x09, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x1b,
        0x09, 0x11, 0x10, 0x0f, 0x0e, 0x0d, 0x0c, 0x0b,
        0x0a, 0x12, 0x10, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    /* Prepare first. */
    static const uint8_t prepare[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
    };

    _feed_single_channel(prepare, sizeof(prepare),
                         BLE_LINK_SERVICE_RX_SESSION);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, sizeof(commit),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_INVALID_ARGUMENT, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_NONE, response.body);
}

static void test_subscribe_then_publish(void)
{
    /* subscribe_events, request_id=4. */
    static const uint8_t subscribe[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x00,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    _feed_single(subscribe, sizeof(subscribe));
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_EVENT_SUBSCRIPTION,
                      response.body);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, response.error);

    /* Publish a link_state change. */
    ble_link_state_snapshot_t link_state;

    memset(&link_state, 0, sizeof(link_state));
    link_state.boot_id = BOOT_ID;
    link_state.binding_state = BLE_LINK_BINDING_BOUND;
    link_state.authorization_state = BLE_LINK_AUTHORIZATION_AUTHORIZED;
    link_state.encrypted = true;
    link_state.secure_connections_bond_verified = true;
    link_state.identity_known = true;
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_publish_link_state(
                          &s_facts, &link_state));
    TEST_ASSERT_EQUAL(1U, s_capture_count);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_BODY_EVENT, envelope.body);
    /* Event: sequence fixed64 == 1. */
    TEST_ASSERT_EQUAL(0x09U, envelope.body_data[0]);
    TEST_ASSERT_EQUAL(0x01U, envelope.body_data[1]);
}

static void test_no_subscriber_no_output(void)
{
    ble_link_state_snapshot_t link_state;

    _reset();
    memset(&link_state, 0, sizeof(link_state));
    link_state.boot_id = BOOT_ID;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_publish_link_state(
                          &s_facts, &link_state));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
}

static void test_intermediate_fragment(void)
{
    uint8_t framed[512];

    _reset();
    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 1U; /* START only. */
    framed[2] = 0x01U;
    framed[4] = 40U;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, 20U));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
}

static void test_admission_denied(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };

    _reset();
    /* Not authorized: control admission fails. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          false, 2U));
    _set_facts(true, true, false);
    uint8_t framed[512];
    const size_t total = sizeof(request) + 1U;

    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x01U;
    framed[4] = (uint8_t)(total & 0xffU);
    framed[5] = (uint8_t)((total >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed,
                          8U + total));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
}

static void test_bad_fragment_rejected(void)
{
    _reset();
    static const uint8_t bad[8] = {0x01, 0x03, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          bad, sizeof(bad)));
}

static void test_authorize_commit_truncated_rejected(void)
{
    /* credential field declares 200 bytes but only 16 are present. */
    static const uint8_t commit[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x27, 0x09, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x1c,
        0x09, 0x11, 0x10, 0x0f, 0x0e, 0x0d, 0x0c, 0x0b,
        0x0a, 0x12, 0xc8, 0x01, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
        0x0d, 0x0e, 0x0f, 0x10,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    _set_facts(true, true, true);
    static const uint8_t prepare[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
    };
    uint8_t framed[512];

    _feed_single_channel(prepare, sizeof(prepare),
                         BLE_LINK_SERVICE_RX_SESSION);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x02U;
    framed[4] = (uint8_t)((sizeof(commit) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(commit) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], commit, sizeof(commit));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_SESSION,
                          framed, 9U + sizeof(commit)));
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_INVALID_ARGUMENT, response.error);
}

static void test_dispatch_error_encoded(void)
{
    /* Duplicate request_id across two feeds must yield CONFLICT response. */
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    _feed_single(request, sizeof(request));
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    /* Same request_id again. */
    uint8_t framed[512];

    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x02U;
    framed[4] = (uint8_t)((sizeof(request) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(request) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, 9U + sizeof(request)));
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_CONFLICT, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_NONE, response.body);
}

static void test_bootstrap_admission_for_authorize(void)
{
    /* Authorize flow works with only an authenticated session (no
     * authorization yet): prepare must pass with bootstrap admission. */
    static const uint8_t prepare[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    /* Revoke authorization but keep the authenticated session. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          false, 2U));
    _set_facts(true, true, false);
    _feed_single_channel(prepare, sizeof(prepare),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_AUTHORIZE_PREPARE,
                      response.body);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, response.error);
    /* The same bootstrap session cannot run capabilities. */
    static const uint8_t capabilities[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[512];

    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x02U;
    framed[4] = (uint8_t)((sizeof(capabilities) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(capabilities) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], capabilities, sizeof(capabilities));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed,
                          9U + sizeof(capabilities)));
}

static void test_generation_change_resets_state(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    /* Start a partial frame in generation 1. */
    uint8_t framed[512];

    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 1U;
    framed[2] = 0x01U;
    framed[4] = 40U;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, 20U));
    /* Generation 2 with a complete single-fragment message. */
    _set_facts(true, true, true);
    s_facts.connection_generation = 2U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          2U, BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          2U, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          2U, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    uint32_t epoch = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          2U, &epoch));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(
                          2U, true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, 1U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          2U, 1U, epoch));
    s_facts.active_boot_id = BOOT_ID;
    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x01U;
    framed[4] = (uint8_t)((sizeof(request) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(request) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, 9U + sizeof(request)));
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_CAPABILITIES, response.body);
}

static void test_event_wire_structure(void)
{
    /* Event { sequence=1; link_state_changed=10 { link_state=1 {...} } } */
    ble_link_codec_envelope_t envelope;

    _reset();
    /* Subscribe first. */
    static const uint8_t subscribe[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x00,
    };

    _feed_single(subscribe, sizeof(subscribe));
    ble_link_state_snapshot_t link_state;

    memset(&link_state, 0, sizeof(link_state));
    link_state.boot_id = BOOT_ID;
    link_state.binding_state = BLE_LINK_BINDING_BOUND;
    link_state.authorization_state = BLE_LINK_AUTHORIZATION_AUTHORIZED;
    link_state.encrypted = true;
    link_state.secure_connections_bond_verified = true;
    link_state.identity_known = true;
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_publish_link_state(
                          &s_facts, &link_state));
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_BODY_EVENT, envelope.body);
    /* Event: 09 <seq> 52 <len> 0a <len> 09 <boot64> ... */
    TEST_ASSERT_EQUAL(0x09U, envelope.body_data[0]);
    TEST_ASSERT_EQUAL(0x01U, envelope.body_data[1]);
    TEST_ASSERT_EQUAL(0x52U, envelope.body_data[9]);
    TEST_ASSERT_EQUAL(0x0aU, envelope.body_data[11]);
}

static void test_publish_after_revoke_no_output(void)
{
    ble_link_state_snapshot_t link_state;

    _reset();
    static const uint8_t subscribe[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x00,
    };

    _feed_single(subscribe, sizeof(subscribe));
    memset(&link_state, 0, sizeof(link_state));
    link_state.boot_id = BOOT_ID;
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    /* Revoke authorization: events must stop and the subscription clears. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          false, 2U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_publish_link_state(
                          &s_facts, &link_state));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
    /* Re-authorize: a new subscription is needed. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, 3U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN, 3U, 1U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_publish_link_state(
                          &s_facts, &link_state));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
}

static void test_committed_replay_idempotent(void)
{
    /* authorize_prepare, confirm, then commit twice with the same txn
     * (fresh request ids): the second commit is an idempotent replay. */
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;
    uint8_t commit[64];

    _reset();
    s_pending_captured = false;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    ble_link_service_confirm_binding(true);
    const size_t commit_len = _build_commit_body(commit, sizeof(commit), 5U);

    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    /* Replay the same commit with a fresh request id: idempotent success
     * with the full result. */
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    (void)_build_commit_body(commit, sizeof(commit), 6U);
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT,
                      response.body);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, response.error);
}

static void test_timeout_closes_security2(void)
{
    uint32_t error = 0U;

    _reset();
    ble_link_service_idle_timeout(GEN);
    /* The Security 2 session is closed: control admission fails. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_query_admission(
                          GEN, BLE_LINK_SESSION_CHANNEL_CONTROL,
                          &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED, error);
}

static void test_stale_timeout_ignored(void)
{
    uint32_t error = 0U;

    _reset();
    /* A timeout from a retired generation has no effect. */
    ble_link_service_idle_timeout(GEN + 10U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_query_admission(
                          GEN, BLE_LINK_SESSION_CHANNEL_CONTROL,
                          &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, error);
}

static void test_stale_feed_ignored(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[512];

    _reset();
    /* Advance to generation 2 with a fresh session. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          2U, BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          2U, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          2U, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    uint32_t epoch = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          2U, &epoch));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(
                          2U, true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, 1U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          2U, 1U, epoch));
    s_facts.connection_generation = 2U;
    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x01U;
    framed[4] = (uint8_t)((sizeof(request) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(request) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, 9U + sizeof(request)));
    /* A stale feed from generation 1 has no effect. */
    ble_link_service_facts_t stale = s_facts;

    stale.connection_generation = 1U;
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &stale, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, 9U + sizeof(request)));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
}

static void test_channel_mismatch_rejected(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[512];

    _reset();
    /* A control request on the session channel is rejected. */
    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x01U;
    framed[4] = (uint8_t)((sizeof(request) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(request) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_feed(
                          &s_facts,
                          BLE_LINK_SERVICE_RX_SESSION,
                          framed,
                          9U + sizeof(request)));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
    /* The reverse direction: a bootstrap request on the control channel. */
    static const uint8_t prepare[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
    };

    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x02U;
    framed[4] = (uint8_t)((sizeof(prepare) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(prepare) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], prepare, sizeof(prepare));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_feed(
                          &s_facts,
                          BLE_LINK_SERVICE_RX_CONTROL,
                          framed,
                          9U + sizeof(prepare)));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
}

static void test_boot_mismatch_closes_session(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };

    _reset();
    _establish_session();
    _set_facts(true, true, true);
    /* A foreign boot id in the envelope is terminal: the session is
     * closed, no response is emitted. */
    s_facts.active_boot_id = BOOT_ID ^ 1U;
    uint8_t framed[512];

    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x01U;
    framed[4] = (uint8_t)((sizeof(request) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(request) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_feed(
                          &s_facts,
                          BLE_LINK_SERVICE_RX_CONTROL,
                          framed,
                          9U + sizeof(request)));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
    uint32_t error = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_query_admission(
                          GEN, BLE_LINK_SESSION_CHANNEL_CONTROL,
                          &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED, error);
}

static void test_one_transaction_at_a_time(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    _reset();
    _establish_session();
    _set_facts(true, true, true);
    /* MTU 23 makes the response multi-fragment; the synchronous sink
     * completes the transaction at the last fragment, so the gate is
     * released and a second request is served (not BUSY). */
    s_facts.preferred_att_mtu = 23U;
    _feed_single_channel(request, sizeof(request),
                         BLE_LINK_SERVICE_RX_CONTROL);
    TEST_ASSERT_TRUE(!ble_link_service_response_in_flight());
    _feed_single_channel(request, sizeof(request),
                         BLE_LINK_SERVICE_RX_CONTROL);
    /* Both requests were served (two multi-fragment responses). */
    TEST_ASSERT_TRUE(s_capture_count > 2U);
    TEST_ASSERT_TRUE(!ble_link_service_response_in_flight());
}

static void test_authorize_prepare_produces_transaction(void)
{
    /* The authorize flow is always enabled: prepare produces a transaction
     * and an authorize-prepare response. */
    static const uint8_t prepare[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    ble_link_service_reset();
    ble_link_service_init(BOOT_ID, _capture, NULL, NULL, 32U);
    memset(s_capture, 0, sizeof(s_capture));
    memset(s_capture_lens, 0, sizeof(s_capture_lens));
    memset(s_capture_channels, 0, sizeof(s_capture_channels));
    s_capture_count = 0U;
    ble_link_session_init(BOOT_ID);
    _establish_session();
    _set_facts(true, true, true);
    _feed_single_channel(prepare, sizeof(prepare),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_AUTHORIZE_PREPARE,
                      response.body);
    TEST_ASSERT_TRUE(response.body_len >= 8U);
}

static void test_session_channel_reassembly(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[512];

    _reset();
    /* A partial frame on the session channel does not disturb control. */
    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 1U;
    framed[2] = 0x01U;
    framed[4] = 40U;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_service_feed(
                          &s_facts,
                          BLE_LINK_SERVICE_RX_SESSION,
                          framed, 20U));
    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x01U;
    framed[4] = (uint8_t)((sizeof(request) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(request) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, 9U + sizeof(request)));
    TEST_ASSERT_EQUAL(1U, s_capture_count);
}

static void test_low_mtu_multi_fragment(void)
{
    /* MTU 23: write value 20 bytes, framing payload 12 bytes. A
     * capabilities response (~70 bytes) splits into several fragments. */
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };

    _reset();
    s_facts.preferred_att_mtu = 23U;
    uint8_t framed[512];

    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x01U;
    framed[4] = (uint8_t)((sizeof(request) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(request) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, 9U + sizeof(request)));
    /* Multiple fragments, each within the 20-byte write value bound. */
    TEST_ASSERT_TRUE(s_capture_count > 1U);
    for (size_t i = 0U; i < s_capture_count; ++i)
    {
        TEST_ASSERT_TRUE(s_capture_lens[i] <= 20U);
    }
    /* The fragments reassemble into one valid response. */
    _reassemble_captured();
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_CAPABILITIES, response.body);
}

static void test_idle_timeout_clears_state(void)
{
    ble_link_state_snapshot_t link_state;

    _reset();
    static const uint8_t subscribe[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x00,
    };

    _feed_single(subscribe, sizeof(subscribe));
    ble_link_service_idle_timeout(GEN);
    memset(&link_state, 0, sizeof(link_state));
    link_state.boot_id = BOOT_ID;
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    /* After the timeout the subscription is gone. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_publish_link_state(
                          &s_facts, &link_state));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
}

int main(void)
{
    test_authorize_commit_wrong_credential();
    test_authorize_commit_truncated_rejected();
    test_capabilities_request();
    test_capabilities_response_bytes();
    test_snapshot_request();
    test_authorize_flow();
    test_subscribe_then_publish();
    test_no_subscriber_no_output();
    test_intermediate_fragment();
    test_admission_denied();
    test_bad_fragment_rejected();
    test_dispatch_error_encoded();
    test_bootstrap_admission_for_authorize();
    test_generation_change_resets_state();
    test_event_wire_structure();
    test_publish_after_revoke_no_output();
    test_committed_replay_idempotent();
    test_timeout_closes_security2();
    test_stale_timeout_ignored();
    test_stale_feed_ignored();
    test_channel_mismatch_rejected();
    test_boot_mismatch_closes_session();
    test_one_transaction_at_a_time();
    test_authorize_prepare_produces_transaction();
    test_session_channel_reassembly();
    test_low_mtu_multi_fragment();
    test_idle_timeout_clears_state();

    return 0;
    return 0;
}
