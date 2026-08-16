#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/task.h"
#include "esp_err.h"
#include "host_freertos.h"

#include "ble_link_codec.h"
#include "ble_link_events.h"
#include "ble_link_reassembler.h"
#include "ble_link_service.h"
#include "ble_link_session.h"

#include "esp_random.h"

#include "device_link_security.h"
#include "device_link_operation.h"
#include "device_link_protocol.h"
#include "device_link_wire.h"
#include "device_link_tlv.h"
#include "nv_storage.h"
#include "session.pb-c.h"

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
#define TEST_SEC2_PUBLIC_KEY_BYTES 384U
#define TEST_SEC2_PROOF_BYTES 64U
#define TEST_SEC2_TAG_BYTES 16U

static void _set_test_grants(device_link_security_auth_record_t *record)
{
    record->granted_permission_count = 3U;
    record->granted_permissions[0] = DEVICE_LINK_PERMISSION_CORE_READ;
    record->granted_permissions[1] = DEVICE_LINK_PERMISSION_CORE_BIND;
    record->granted_permissions[2] = DEVICE_LINK_PERMISSION_CORE_OPERATE;
}

static size_t _pack_real_handshake_request(
    device_link_security_handshake_stage_t stage,
    uint8_t *out, size_t capacity)
{
    static uint8_t username[] = "microtech";
    static uint8_t public_key[TEST_SEC2_PUBLIC_KEY_BYTES] =
    {0x10U, 0x20U, 0x30U, 0x40U};
    static uint8_t proof[TEST_SEC2_PROOF_BYTES] =
    {0x50U, 0x60U, 0x70U};
    S2SessionCmd0 cmd0 = S2_SESSION_CMD0__INIT;
    S2SessionCmd1 cmd1 = S2_SESSION_CMD1__INIT;
    Sec2Payload payload = SEC2_PAYLOAD__INIT;
    SessionData session = SESSION_DATA__INIT;

    session.sec_ver = SEC_SCHEME_VERSION__SecScheme2;
    session.proto_case = SESSION_DATA__PROTO_SEC2;
    session.sec2 = &payload;
    if (stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD0)
    {
        cmd0.client_username.data = username;
        cmd0.client_username.len = sizeof(username) - 1U;
        cmd0.client_pubkey.data = public_key;
        cmd0.client_pubkey.len = sizeof(public_key);
        payload.msg = SEC2_MSG_TYPE__S2Session_Command0;
        payload.payload_case = SEC2_PAYLOAD__PAYLOAD_SC0;
        payload.sc0 = &cmd0;
    }
    else
    {
        cmd1.client_proof.data = proof;
        cmd1.client_proof.len = sizeof(proof);
        payload.msg = SEC2_MSG_TYPE__S2Session_Command1;
        payload.payload_case = SEC2_PAYLOAD__PAYLOAD_SC1;
        payload.sc1 = &cmd1;
    }
    const size_t packed_size = session_data__get_packed_size(&session);

    TEST_ASSERT_TRUE(packed_size <= capacity);
    TEST_ASSERT_EQUAL(packed_size, session_data__pack(&session, out));
    return packed_size;
}

/* Capture sink state. */
static uint8_t s_capture[16][512];
static size_t s_capture_lens[16];
static size_t s_capture_count;
static ble_link_service_tx_channel_t s_capture_channels[16];
static uint32_t s_capture_flow_ids[16];
static bool s_capture_last[16];
static bool s_auto_confirm;
static uint16_t s_next_frame_id;
static unsigned int s_provisional_discard_count;
static unsigned int s_provisional_promote_count;
static unsigned int s_security_close_count;
static uint32_t s_provisional_generation;
static bool s_provisional_terminate;
static ble_link_operation_identity_t s_provisional_identity;
static esp_err_t s_provisional_result;
static unsigned int s_replacement_count;
static esp_err_t s_replacement_result;
static ble_link_operation_identity_t s_replacement_identity;
static unsigned int s_handshake_count;
static unsigned int s_owner_wake_count;

static void _owner_wake(void *arg)
{
    (void)arg;
    s_owner_wake_count++;
}

static void _security_close(void)
{
    s_security_close_count++;
}

static esp_err_t _discard_provisional_bond(
    const ble_link_operation_identity_t *identity,
    bool terminate_conn)
{
    s_provisional_discard_count++;
    s_provisional_identity = *identity;
    s_provisional_generation = identity->generation;
    s_provisional_terminate = terminate_conn;
    return s_provisional_result;
}

static esp_err_t _promote_provisional_bond(
    const ble_link_operation_identity_t *identity)
{
    s_provisional_promote_count++;
    s_provisional_identity = *identity;
    s_provisional_generation = identity->generation;
    return s_provisional_result;
}

static esp_err_t _replace_authorization(
    const ble_link_operation_identity_t *identity)
{
    s_replacement_count++;
    s_replacement_identity = *identity;
    return s_replacement_result;
}

static esp_err_t _real_security_request(
    const uint8_t *request, size_t request_len,
    uint8_t **response, size_t *response_len, void *arg)
{
    (void)arg;
    return ble_link_service_process_plaintext(
               request, request_len, response, response_len);
}

static void _real_security_release(
    uint8_t *response, size_t response_len, void *arg)
{
    (void)arg;
    ble_link_service_release_plaintext(response, response_len);
}

static esp_err_t _real_security_authenticated(void *arg)
{
    (void)arg;
    return ble_link_service_on_authenticated(NULL);
}

static void _real_security_close(void)
{
    s_security_close_count++;
    device_link_security_close_session();
}

static const device_link_security_config_t s_real_sec_config =
{
    .username = "microtech",
    .session_id = 1U,
    .request_cb = _real_security_request,
    .request_arg = NULL,
    .response_release_cb = _real_security_release,
    .response_release_arg = NULL,
    .authenticated_cb = _real_security_authenticated,
    .authenticated_arg = NULL,
};

static const ble_link_security_ops_t s_real_security_ops =
{
    .select_verifier = device_link_security_select_verifier,
    .selected_verifier = device_link_security_selected_verifier,
    .classify_handshake = device_link_security_classify_handshake,
    .handshake = device_link_security_handshake_ex,
    .unprotect = device_link_security_unprotect,
    .protect = device_link_security_protect,
    .is_authenticated = device_link_security_is_authenticated,
    .session_open = device_link_security_session_open,
    .close_session = _real_security_close,
    .discard_provisional_bond = _discard_provisional_bond,
    .promote_provisional_bond = _promote_provisional_bond,
    .replace_authorization = _replace_authorization,
};

static const ble_link_security_ops_t s_provisional_security_ops =
{
    .close_session = _security_close,
    .discard_provisional_bond = _discard_provisional_bond,
    .promote_provisional_bond = _promote_provisional_bond,
    .replace_authorization = _replace_authorization,
};

static esp_err_t _classify_handshake(
    const uint8_t *input, size_t input_len,
    device_link_security_handshake_stage_t *stage)
{
    if (input == NULL || stage == NULL || input_len != 4U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (memcmp(input, "cmd0", 4U) == 0)
    {
        *stage = DEVICE_LINK_SECURITY_HANDSHAKE_CMD0;
        return ESP_OK;
    }
    if (memcmp(input, "cmd1", 4U) == 0)
    {
        *stage = DEVICE_LINK_SECURITY_HANDSHAKE_CMD1;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t _process_handshake(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len,
    device_link_security_handshake_result_t *handshake_result)
{
    device_link_security_handshake_stage_t stage;
    const esp_err_t result = _classify_handshake(
                                 input, input_len, &stage);

    if (result != ESP_OK || output == NULL || output_len == NULL ||
            handshake_result == NULL)
    {
        return result != ESP_OK ? result : ESP_ERR_INVALID_ARG;
    }
    uint8_t *response = malloc(5U);

    if (response == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(response, stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD0 ?
           "resp0" : "resp1", 5U);
    *output = response;
    *output_len = 5U;
    handshake_result->stage = stage;
    handshake_result->authenticated =
        stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD1;
    s_handshake_count++;
    return ESP_OK;
}

static esp_err_t _select_handshake_verifier(
    uint8_t peer_addr_type, const uint8_t *peer_addr,
    size_t peer_addr_len, bool pairing_window_open)
{
    (void)peer_addr_type;
    (void)peer_addr;
    (void)peer_addr_len;
    (void)pairing_window_open;
    return ESP_OK;
}

static const ble_link_security_ops_t s_handshake_security_ops =
{
    .select_verifier = _select_handshake_verifier,
    .classify_handshake = _classify_handshake,
    .handshake = _process_handshake,
    .close_session = _security_close,
};

/* Reassembly of the captured outbound fragments. */
static uint8_t s_outbound[1024];
static size_t s_outbound_len;

static ble_link_service_facts_t s_facts;

static esp_err_t _capture(const uint8_t *value, size_t len,
                          ble_link_service_tx_channel_t channel,
                          bool is_last, uint32_t flow_id, void *arg)
{
    (void)arg;
    TEST_ASSERT_TRUE(s_capture_count < 16U);
    TEST_ASSERT_TRUE(len <= sizeof(s_capture[0]));
    memcpy(s_capture[s_capture_count], value, len);
    s_capture_lens[s_capture_count] = len;
    s_capture_channels[s_capture_count] = channel;
    s_capture_flow_ids[s_capture_count] = flow_id;
    s_capture_last[s_capture_count] = is_last;
    s_capture_count++;
    /* Model the transport: every frame confirmation advances the
     * outbound stream (the service emits the next fragment); the final
     * confirmation releases the transaction gate. */
    if (s_auto_confirm && flow_id != 0U)
    {
        TEST_ASSERT_EQUAL(ESP_OK,
                          ble_link_service_response_completed(
                              flow_id, is_last));
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
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
    memset(framed, 0, sizeof(framed));
    const size_t total = payload_len + 1U;

    framed[0] = 1U;
    framed[1] = 3U; /* START|END */
    framed[2] = (uint8_t)(s_next_frame_id & 0xffU);
    framed[3] = (uint8_t)(s_next_frame_id >> 8U);
    framed[4] = (uint8_t)(total & 0xffU);
    framed[5] = (uint8_t)((total >> 8U) & 0xffU);
    framed[6] = 0x00U;
    framed[7] = 0x00U;
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], payload, payload_len);
    s_next_frame_id++;
    if (s_next_frame_id == 0U)
    {
        s_next_frame_id = 1U;
    }
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &s_facts, channel, framed, 8U + total));
}

static void _feed_single(const uint8_t *payload, size_t payload_len)
{
    _feed_single_channel(payload, payload_len,
                         BLE_LINK_SERVICE_RX_CONTROL);
}

static esp_err_t _accept_handshake_single(
    const uint8_t *payload, size_t payload_len,
    ble_link_work_t **out_work)
{
    uint8_t framed[512];
    const size_t total = payload_len + 1U;

    TEST_ASSERT_TRUE(8U + total <= sizeof(framed));
    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = (uint8_t)(s_next_frame_id & 0xffU);
    framed[3] = (uint8_t)(s_next_frame_id >> 8U);
    framed[4] = (uint8_t)(total & 0xffU);
    framed[5] = (uint8_t)((total >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE;
    memcpy(&framed[9], payload, payload_len);
    s_next_frame_id++;
    if (s_next_frame_id == 0U)
    {
        s_next_frame_id = 1U;
    }
    return ble_link_service_accept(
               &s_facts, BLE_LINK_SERVICE_RX_SESSION,
               framed, 8U + total, out_work);
}

static esp_err_t _feed_handshake_single(
    const uint8_t *payload, size_t payload_len)
{
    ble_link_work_t *work = NULL;
    esp_err_t result = _accept_handshake_single(
                           payload, payload_len, &work);

    if (result == ESP_OK && work != NULL)
    {
        result = ble_link_service_execute(work);
    }
    ble_link_service_release_work(work);
    return result;
}

/**
 * @brief Reassemble captured fragments into one message.
 */
static void _reassemble_captured_type(uint8_t expected_transport_type)
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
    TEST_ASSERT_EQUAL(expected_transport_type, s_outbound[0]);
    memmove(s_outbound, &s_outbound[1], s_outbound_len - 1U);
    s_outbound_len -= 1U;
}

static void _reassemble_captured(void)
{
    _reassemble_captured_type(BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED);
}

static void _reassemble_real_protected(void)
{
    _reassemble_captured();
    TEST_ASSERT_TRUE(s_outbound_len >= TEST_SEC2_TAG_BYTES);
    for (size_t i = s_outbound_len - TEST_SEC2_TAG_BYTES;
            i < s_outbound_len; ++i)
    {
        TEST_ASSERT_EQUAL(0x5aU, s_outbound[i]);
    }
    s_outbound_len -= TEST_SEC2_TAG_BYTES;
}

static void _set_facts(bool encrypted, bool authenticated, bool authorized)
{
    memset(&s_facts, 0, sizeof(s_facts));
    s_facts.active_boot_id = BOOT_ID;
    s_facts.connection_generation = GEN;
    s_facts.security_epoch = ble_link_session_security2_epoch();
    s_facts.preferred_att_mtu = 495U;
    s_facts.conn_handle = 7U;
    s_facts.encrypted = encrypted;
    s_facts.session_authenticated = authenticated;
    s_facts.authorized = authorized;
    s_facts.identity_known = true;
    s_facts.peer_addr_type = 1U;
    memset(s_facts.peer_addr, 0, sizeof(s_facts.peer_addr));
    s_facts.peer_addr[0] = 0x11U;
    s_facts.peer_addr[5] = 0xeaU;
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
    memset(s_capture_flow_ids, 0, sizeof(s_capture_flow_ids));
    memset(s_capture_last, 0, sizeof(s_capture_last));
    s_capture_count = 0U;
    s_auto_confirm = true;
    s_next_frame_id = 1U;
    ble_link_service_reset();
    ble_link_service_init(BOOT_ID, _capture, NULL, NULL, 32U);
    ble_link_events_init();
    _establish_session();
    _set_facts(true, true, true);
    (void)device_link_security_init(&s_sec_config);
}

static void _enable_provisional_security_ops(void)
{
    ble_link_service_reset();
    ble_link_service_init(BOOT_ID, _capture, NULL,
                          &s_provisional_security_ops, 32U);
    s_provisional_discard_count = 0U;
    s_provisional_promote_count = 0U;
    s_security_close_count = 0U;
    s_provisional_generation = 0U;
    s_provisional_terminate = false;
    memset(&s_provisional_identity, 0, sizeof(s_provisional_identity));
    s_provisional_result = ESP_OK;
    s_replacement_count = 0U;
    s_replacement_result = ESP_OK;
    memset(&s_replacement_identity, 0, sizeof(s_replacement_identity));
    s_owner_wake_count = 0U;
}

static void _confirm_last_captured_flow(void);

static void _clear_capture(void)
{
    memset(s_capture, 0, sizeof(s_capture));
    memset(s_capture_lens, 0, sizeof(s_capture_lens));
    memset(s_capture_channels, 0, sizeof(s_capture_channels));
    memset(s_capture_flow_ids, 0, sizeof(s_capture_flow_ids));
    memset(s_capture_last, 0, sizeof(s_capture_last));
    s_capture_count = 0U;
}

static void _setup_real_bootstrap_session(void)
{
    static const uint8_t pop[] = "window-pop-secret";

    nv_storage_fake_reset();
    device_link_security_deinit();
    ble_link_service_reset();
    ble_link_session_init(BOOT_ID);
    ble_link_session_set_pairing_window(true);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN, BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(GEN, true));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_session_set_connection_pairing_window(
                          GEN, true));
    ble_link_events_init();
    ble_link_service_init(BOOT_ID, _capture, NULL,
                          &s_real_security_ops, 32U);
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_link_security_init(&s_real_sec_config));
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_link_security_open_bootstrap(
                          pop, sizeof(pop) - 1U));
    _set_facts(true, false, false);
    s_facts.pairing_window_open = true;
    s_provisional_discard_count = 0U;
    s_provisional_promote_count = 0U;
    s_security_close_count = 0U;
    s_provisional_result = ESP_OK;
    s_replacement_count = 0U;
    s_replacement_result = ESP_OK;
    s_next_frame_id = 1U;
    s_auto_confirm = false;
    _clear_capture();
}

static void _assert_real_handshake_response(
    device_link_security_handshake_stage_t stage)
{
    _reassemble_captured_type(BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE);
    SessionData *session = session_data__unpack(
                               NULL, s_outbound_len, s_outbound);

    TEST_ASSERT_TRUE(session != NULL);
    TEST_ASSERT_EQUAL(SEC_SCHEME_VERSION__SecScheme2, session->sec_ver);
    TEST_ASSERT_EQUAL(SESSION_DATA__PROTO_SEC2, session->proto_case);
    TEST_ASSERT_TRUE(session->sec2 != NULL);
    if (stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD0)
    {
        TEST_ASSERT_EQUAL(SEC2_MSG_TYPE__S2Session_Response0,
                          session->sec2->msg);
        TEST_ASSERT_EQUAL(SEC2_PAYLOAD__PAYLOAD_SR0,
                          session->sec2->payload_case);
        TEST_ASSERT_TRUE(session->sec2->sr0 != NULL);
        TEST_ASSERT_EQUAL(STATUS__Success, session->sec2->sr0->status);
    }
    else
    {
        TEST_ASSERT_EQUAL(SEC2_MSG_TYPE__S2Session_Response1,
                          session->sec2->msg);
        TEST_ASSERT_EQUAL(SEC2_PAYLOAD__PAYLOAD_SR1,
                          session->sec2->payload_case);
        TEST_ASSERT_TRUE(session->sec2->sr1 != NULL);
        TEST_ASSERT_EQUAL(STATUS__Success, session->sec2->sr1->status);
    }
    session_data__free_unpacked(session, NULL);
}

static uint32_t _complete_real_handshake(void)
{
    uint8_t handshake[512];
    const uint32_t epoch_before = ble_link_session_security2_epoch();
    size_t handshake_len = _pack_real_handshake_request(
                               DEVICE_LINK_SECURITY_HANDSHAKE_CMD0,
                               handshake, sizeof(handshake));

    _clear_capture();
    TEST_ASSERT_EQUAL(ESP_OK,
                      _feed_handshake_single(handshake, handshake_len));
    const uint32_t epoch = ble_link_session_security2_epoch();

    TEST_ASSERT_EQUAL(epoch_before + 1U, epoch);
    s_facts.security_epoch = epoch;
    _assert_real_handshake_response(DEVICE_LINK_SECURITY_HANDSHAKE_CMD0);
    TEST_ASSERT_TRUE(s_capture_last[s_capture_count - 1U]);
    _confirm_last_captured_flow();

    handshake_len = _pack_real_handshake_request(
                        DEVICE_LINK_SECURITY_HANDSHAKE_CMD1,
                        handshake, sizeof(handshake));
    _clear_capture();
    TEST_ASSERT_EQUAL(ESP_OK,
                      _feed_handshake_single(handshake, handshake_len));
    TEST_ASSERT_EQUAL(epoch, ble_link_session_security2_epoch());
    TEST_ASSERT_TRUE(device_link_security_is_authenticated());
    _assert_real_handshake_response(DEVICE_LINK_SECURITY_HANDSHAKE_CMD1);
    TEST_ASSERT_TRUE(s_capture_last[s_capture_count - 1U]);
    _confirm_last_captured_flow();
    s_facts.security_epoch = epoch;
    s_facts.session_authenticated = true;
    ble_link_dispatcher_facts_t live_facts;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_session_get_facts(GEN, &live_facts));
    s_facts.authorized = live_facts.authorized;
    return epoch;
}

static void _feed_real_protected(
    const uint8_t *plain, size_t plain_len)
{
    uint8_t cipher[512];

    TEST_ASSERT_TRUE(plain_len + TEST_SEC2_TAG_BYTES <= sizeof(cipher));
    memcpy(cipher, plain, plain_len);
    memset(&cipher[plain_len], 0xa5, TEST_SEC2_TAG_BYTES);
    _feed_single_channel(cipher, plain_len + TEST_SEC2_TAG_BYTES,
                         BLE_LINK_SERVICE_RX_SESSION);
}

static void _confirm_last_captured_flow(void)
{
    TEST_ASSERT_TRUE(s_capture_count > 0U);
    const size_t index = s_capture_count - 1U;
    const uint32_t flow_id = s_capture_flow_ids[index];

    TEST_ASSERT_TRUE(flow_id != 0U);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_response_completed(
                          flow_id, s_capture_last[index]));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
}

static void test_response_flow_identity_and_deferred_busy(void)
{
    static const uint8_t request_one[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t request_two[sizeof(request_one)];

    memcpy(request_two, request_one, sizeof(request_two));
    request_two[1] = 0x02U;
    _reset();
    s_facts.preferred_att_mtu = 23U;
    s_auto_confirm = false;
    _feed_single(request_one, sizeof(request_one));
    TEST_ASSERT_EQUAL(1U, s_capture_count);
    TEST_ASSERT_TRUE(s_capture_flow_ids[0] != 0U);
    const uint32_t first_flow = s_capture_flow_ids[0];

    _feed_single(request_two, sizeof(request_two));
    TEST_ASSERT_EQUAL(1U, s_capture_count);
    TEST_ASSERT_TRUE(ble_link_service_response_in_flight());

    const size_t first_start = 0U;
    while (!s_capture_last[s_capture_count - 1U])
    {
        _confirm_last_captured_flow();
        TEST_ASSERT_TRUE(s_capture_count < 16U);
        TEST_ASSERT_EQUAL(first_flow,
                          s_capture_flow_ids[s_capture_count - 1U]);
    }
    const size_t first_count = s_capture_count;
    TEST_ASSERT_TRUE(first_count > first_start + 1U);
    TEST_ASSERT_TRUE(s_capture_flow_ids[first_count - 1U] == first_flow);

    /* Final confirmation starts the deferred BUSY response as a new flow. */
    _confirm_last_captured_flow();
    TEST_ASSERT_TRUE(s_capture_count > first_count);
    const uint32_t busy_flow = s_capture_flow_ids[first_count];

    TEST_ASSERT_TRUE(busy_flow != 0U);
    TEST_ASSERT_TRUE(busy_flow != first_flow);
    TEST_ASSERT_EQUAL(BLE_LINK_SERVICE_TX_CONTROL_RESPONSE,
                      s_capture_channels[first_count]);
    TEST_ASSERT_TRUE(ble_link_service_response_in_flight());
    while (!s_capture_last[s_capture_count - 1U])
    {
        _confirm_last_captured_flow();
        TEST_ASSERT_TRUE(s_capture_count < 16U);
        TEST_ASSERT_EQUAL(busy_flow,
                          s_capture_flow_ids[s_capture_count - 1U]);
    }
    _confirm_last_captured_flow();
    TEST_ASSERT_TRUE(!ble_link_service_response_in_flight());
}

static void test_stale_response_flow_is_ignored(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };

    _reset();
    s_auto_confirm = false;
    _feed_single(request, sizeof(request));
    TEST_ASSERT_EQUAL(1U, s_capture_count);
    const uint32_t old_flow = s_capture_flow_ids[0];

    ble_link_service_abort_transactions();
    _reset();
    s_auto_confirm = false;
    _feed_single(request, sizeof(request));
    TEST_ASSERT_EQUAL(1U, s_capture_count);
    const uint32_t new_flow = s_capture_flow_ids[0];

    TEST_ASSERT_TRUE(old_flow != 0U);
    TEST_ASSERT_TRUE(new_flow != 0U);
    TEST_ASSERT_TRUE(old_flow != new_flow);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_response_completed(old_flow, false));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(1U, s_capture_count);
    ble_link_service_abort_transactions();
}

static void test_handshake_queued_admission(void)
{
    static const uint8_t cmd0[] = "cmd0";
    static const uint8_t competing[] = "cmdX";
    ble_link_work_t *work = NULL;
    ble_link_work_t *duplicate = NULL;

    _reset();
    ble_link_service_reset();
    ble_link_service_init(BOOT_ID, _capture, NULL,
                          &s_handshake_security_ops, 32U);
    s_auto_confirm = false;
    s_handshake_count = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &work));
    TEST_ASSERT_TRUE(work != NULL);
    TEST_ASSERT_EQUAL(0U, s_handshake_count);
    TEST_ASSERT_EQUAL(ESP_OK, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &duplicate));
    TEST_ASSERT_TRUE(duplicate == NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, _accept_handshake_single(
                          competing, sizeof(competing) - 1U,
                          &duplicate));
    TEST_ASSERT_TRUE(duplicate == NULL);

    /* A failed queue submission releases the reservation with the work. */
    ble_link_service_release_work(work);
    work = NULL;
    s_next_frame_id = 1U;
    TEST_ASSERT_EQUAL(ESP_OK, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &work));
    TEST_ASSERT_TRUE(work != NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_EQUAL(1U, s_handshake_count);
    TEST_ASSERT_EQUAL(1U, s_capture_count);
    ble_link_service_abort_transactions();
}

static void test_cmd0_delayed_replacement(void)
{
    static const uint8_t cmd0[] = "cmd0";
    static const uint8_t competing[] = "cmdX";

    _reset();
    ble_link_service_reset();
    ble_link_service_init(BOOT_ID, _capture, NULL,
                          &s_handshake_security_ops, 32U);
    s_auto_confirm = false;
    s_handshake_count = 0U;
    s_security_close_count = 0U;

    TEST_ASSERT_EQUAL(ESP_OK,
                      _feed_handshake_single(cmd0, sizeof(cmd0) - 1U));
    TEST_ASSERT_EQUAL(1U, s_handshake_count);
    TEST_ASSERT_EQUAL(1U, s_capture_count);
    const uint32_t old_flow = s_capture_flow_ids[0];

    TEST_ASSERT_TRUE(old_flow != 0U);
    /* A new Cmd0 retires the old logical session immediately but cannot
     * create its response while the old indication occupies the slot. */
    ble_link_work_t *replacement = NULL;
    ble_link_work_t *duplicate = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &replacement));
    TEST_ASSERT_TRUE(replacement != NULL);
    TEST_ASSERT_TRUE(!ble_link_service_delayed_replacement_pending(GEN));
    TEST_ASSERT_EQUAL(ESP_OK, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &duplicate));
    TEST_ASSERT_TRUE(duplicate == NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, _accept_handshake_single(
                          competing, sizeof(competing) - 1U,
                          &duplicate));
    TEST_ASSERT_TRUE(duplicate == NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(replacement));
    ble_link_service_release_work(replacement);
    TEST_ASSERT_TRUE(ble_link_service_delayed_replacement_pending(GEN));
    TEST_ASSERT_EQUAL(1U, s_handshake_count);
    ble_link_dispatcher_facts_t facts;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_get_facts(GEN, &facts));
    TEST_ASSERT_TRUE(!facts.session_authenticated);
    /* Exact repeats are absorbed and do not process or replace the one
     * retained body; competing ingress reports BUSY/invalid state. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      _feed_handshake_single(cmd0, sizeof(cmd0) - 1U));
    TEST_ASSERT_EQUAL(1U, s_handshake_count);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED,
                      _feed_handshake_single(
                          competing, sizeof(competing) - 1U));
    TEST_ASSERT_TRUE(ble_link_service_delayed_replacement_pending(GEN));

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_response_completed(old_flow, true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(2U, s_handshake_count);
    TEST_ASSERT_EQUAL(2U, s_capture_count);
    TEST_ASSERT_TRUE(!ble_link_service_delayed_replacement_pending(GEN));
    const uint32_t new_flow = s_capture_flow_ids[1];

    TEST_ASSERT_TRUE(new_flow != 0U && new_flow != old_flow);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_response_completed(old_flow, true));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_response_completed(new_flow, true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_TRUE(!ble_link_service_response_in_flight());

    /* Timeout/session-abort discards a retained replacement without ever
     * invoking Protocomm for it. The timer owner applies ACL termination. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      _feed_handshake_single(cmd0, sizeof(cmd0) - 1U));
    TEST_ASSERT_EQUAL(3U, s_handshake_count);
    const uint32_t timeout_flow =
        s_capture_flow_ids[s_capture_count - 1U];

    TEST_ASSERT_EQUAL(ESP_OK,
                      _feed_handshake_single(cmd0, sizeof(cmd0) - 1U));
    TEST_ASSERT_TRUE(ble_link_service_delayed_replacement_pending(GEN));
    ble_link_service_abort_transactions();
    TEST_ASSERT_TRUE(!ble_link_service_delayed_replacement_pending(GEN));
    TEST_ASSERT_EQUAL(3U, s_handshake_count);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_response_completed(
                          timeout_flow, true));
}

static void test_manifest_request(void)
{
    /* Frozen fixture: request_id=1, get_manifest empty. */
    static const uint8_t request[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_MANIFEST, response.body);
}

static void test_manifest_response_bytes(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_MANIFEST, response.body);
    TEST_ASSERT_TRUE(response.body_len > sizeof(expected));
}

static void test_typed_tlv_manifest_request(void)
{
    static const uint8_t request[] =
    {
        0x14U, 0x00U, 0x02U, 0x01U,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
    };
    device_link_wire_header_t header;
    device_link_status_t status;

    _reset();
    _feed_single_channel(request, sizeof(request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_header(
                          s_outbound, s_outbound_len, &header));
    TEST_ASSERT_EQUAL(DEVICE_LINK_MESSAGE_RESPONSE, header.kind);
    TEST_ASSERT_EQUAL(1U, header.call_id);
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_status(
                          &s_outbound[DEVICE_LINK_WIRE_HEADER_BYTES],
                          s_outbound_len - DEVICE_LINK_WIRE_HEADER_BYTES,
                          &status));
    TEST_ASSERT_EQUAL(DEVICE_LINK_STATUS_OK, status);
    TEST_ASSERT_TRUE(s_outbound_len >
                     DEVICE_LINK_WIRE_HEADER_BYTES +
                     DEVICE_LINK_RESPONSE_STATUS_BYTES);
    TEST_ASSERT_TRUE(s_outbound[DEVICE_LINK_WIRE_HEADER_BYTES +
                                DEVICE_LINK_RESPONSE_STATUS_BYTES] != 0U);
}

static void test_typed_tlv_snapshot_has_fixed_link_state(void)
{
    static const uint8_t request[] =
    {
        0x14U, 0x00U, 0x02U, 0x02U,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
    };
    device_link_wire_header_t header;
    device_link_status_t status;
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has_field = false;

    _reset();
    _feed_single_channel(request, sizeof(request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_header(
                          s_outbound, s_outbound_len, &header));
    TEST_ASSERT_EQUAL(DEVICE_LINK_MESSAGE_RESPONSE, header.kind);
    TEST_ASSERT_EQUAL(2U, header.method_id);
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_status(
                          &s_outbound[DEVICE_LINK_WIRE_HEADER_BYTES],
                          s_outbound_len - DEVICE_LINK_WIRE_HEADER_BYTES,
                          &status));
    TEST_ASSERT_EQUAL(DEVICE_LINK_STATUS_OK, status);
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_init(
                          &reader,
                          &s_outbound[DEVICE_LINK_WIRE_HEADER_BYTES +
                                      DEVICE_LINK_RESPONSE_STATUS_BYTES],
                          s_outbound_len - DEVICE_LINK_WIRE_HEADER_BYTES -
                          DEVICE_LINK_RESPONSE_STATUS_BYTES));
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                          &reader, &field, &has_field));
    TEST_ASSERT_TRUE(has_field);
    TEST_ASSERT_EQUAL(1U, field.id);
    TEST_ASSERT_EQUAL(DEVICE_LINK_TLV_FIXED64, field.wire_type);
    TEST_ASSERT_EQUAL(1U, field.value.fixed64_value);
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                          &reader, &field, &has_field));
    TEST_ASSERT_TRUE(has_field);
    TEST_ASSERT_EQUAL(2U, field.id);
    TEST_ASSERT_EQUAL(DEVICE_LINK_TLV_LENGTH, field.wire_type);
    TEST_ASSERT_EQUAL(16U, field.value.bytes.len);
    TEST_ASSERT_EQUAL(2U, field.value.bytes.data[0]);
    TEST_ASSERT_EQUAL(2U, field.value.bytes.data[2]);
    TEST_ASSERT_EQUAL(BOOT_ID,
                      (uint64_t)field.value.bytes.data[8] |
                      (uint64_t)field.value.bytes.data[9] << 8U |
                      (uint64_t)field.value.bytes.data[10] << 16U |
                      (uint64_t)field.value.bytes.data[11] << 24U |
                      (uint64_t)field.value.bytes.data[12] << 32U |
                      (uint64_t)field.value.bytes.data[13] << 40U |
                      (uint64_t)field.value.bytes.data[14] << 48U |
                      (uint64_t)field.value.bytes.data[15] << 56U);
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                          &reader, &field, &has_field));
    TEST_ASSERT_TRUE(!has_field);
}

static void test_v2_snapshot_includes_operation_summaries(void)
{
    /* Core v2: GetLinkSnapshot carries compact operation summaries (field
     * 3) for live operation-table records, and never a result payload. */
    static const uint8_t request[] =
    {
        0x14U, 0x00U, 0x02U, 0x02U,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
    };
    device_link_wire_header_t header;
    device_link_status_t status;
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has_field = false;
    uint64_t operation_id = 0U;

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_async_operation_start(
                          DEVICE_LINK_DOMAIN_WIFI, 4U, 0x1000U,
                          NULL, NULL, &operation_id));
    TEST_ASSERT_TRUE(operation_id != 0U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_async_operation_update(
                          0x1000U, DEVICE_LINK_OPERATION_RUNNING,
                          DEVICE_LINK_STATUS_OK, NULL, 0U));
    _feed_single_channel(request, sizeof(request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_header(
                          s_outbound, s_outbound_len, &header));
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_status(
                          &s_outbound[DEVICE_LINK_WIRE_HEADER_BYTES],
                          s_outbound_len - DEVICE_LINK_WIRE_HEADER_BYTES,
                          &status));
    TEST_ASSERT_EQUAL(DEVICE_LINK_STATUS_OK, status);
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_init(
                          &reader,
                          &s_outbound[DEVICE_LINK_WIRE_HEADER_BYTES +
                                      DEVICE_LINK_RESPONSE_STATUS_BYTES],
                          s_outbound_len - DEVICE_LINK_WIRE_HEADER_BYTES -
                          DEVICE_LINK_RESPONSE_STATUS_BYTES));
    /* Field 1: event sequence; field 2: fixed link_state. */
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                          &reader, &field, &has_field));
    TEST_ASSERT_TRUE(has_field);
    TEST_ASSERT_EQUAL(1U, field.id);
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                          &reader, &field, &has_field));
    TEST_ASSERT_TRUE(has_field);
    TEST_ASSERT_EQUAL(2U, field.id);
    TEST_ASSERT_EQUAL(16U, field.value.bytes.len);
    /* Field 3: one OperationSummary for the live record. */
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                          &reader, &field, &has_field));
    TEST_ASSERT_TRUE(has_field);
    TEST_ASSERT_EQUAL(3U, field.id);
    TEST_ASSERT_EQUAL(DEVICE_LINK_TLV_LENGTH, field.wire_type);
    {
        device_link_tlv_reader_t nested;
        device_link_tlv_field_t nfield;
        bool nhas = false;

        TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_init_nested(
                              &nested, &reader, &field));
        TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                              &nested, &nfield, &nhas));
        TEST_ASSERT_TRUE(nhas);
        TEST_ASSERT_EQUAL(1U, nfield.id);
        TEST_ASSERT_EQUAL(DEVICE_LINK_TLV_FIXED64, nfield.wire_type);
        TEST_ASSERT_EQUAL(operation_id, nfield.value.fixed64_value);
        TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                              &nested, &nfield, &nhas));
        TEST_ASSERT_TRUE(nhas);
        TEST_ASSERT_EQUAL(2U, nfield.id);
        TEST_ASSERT_EQUAL(DEVICE_LINK_DOMAIN_WIFI,
                          nfield.value.unsigned_value);
        TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                              &nested, &nfield, &nhas));
        TEST_ASSERT_TRUE(nhas);
        TEST_ASSERT_EQUAL(3U, nfield.id);
        TEST_ASSERT_EQUAL(4U, nfield.value.unsigned_value);
        TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                              &nested, &nfield, &nhas));
        TEST_ASSERT_TRUE(nhas);
        TEST_ASSERT_EQUAL(4U, nfield.id);
        TEST_ASSERT_EQUAL(DEVICE_LINK_OPERATION_RUNNING,
                          nfield.value.unsigned_value);
        TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                              &nested, &nfield, &nhas));
        TEST_ASSERT_TRUE(nhas);
        TEST_ASSERT_EQUAL(5U, nfield.id);
        TEST_ASSERT_EQUAL(DEVICE_LINK_STATUS_OK,
                          nfield.value.unsigned_value);
        TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                              &nested, &nfield, &nhas));
        TEST_ASSERT_TRUE(!nhas);
    }
    /* No further top-level fields: summaries never carry a result
     * payload. */
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_next(
                          &reader, &field, &has_field));
    TEST_ASSERT_TRUE(!has_field);
}

static void _commit_auth_record(void);

static void test_v2_get_authorization_without_recovery_malformed(void)
{
    /* GetAuthorization is recovery-only: a request without the recovery
     * bit misuses the header (MALFORMED_FRAME, a global error with an
     * empty body); a recovery request with a malformed body stays
     * INVALID_ARGUMENT (schema bounds are rejected by the router). */
    static const uint8_t no_recovery[] =
    {
        0x14U, 0x00U, 0x02U, 0x05U,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x06U, 0x10U, 0x00U,
        0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
        0x18U, 0x19U, 0x1aU, 0x1bU, 0x1cU, 0x1dU, 0x1eU, 0x1fU,
    };
    static const uint8_t recovery_bad_body[] =
    {
        0x15U, 0x00U, 0x02U, 0x05U,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x06U, 0x02U, 0x00U, 0x10U, 0x11U,
    };
    device_link_wire_header_t header;
    device_link_status_t status;

    _reset();
    nv_storage_fake_reset();
    _commit_auth_record();
    _feed_single_channel(no_recovery, sizeof(no_recovery),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_header(
                          s_outbound, s_outbound_len, &header));
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_status(
                          &s_outbound[DEVICE_LINK_WIRE_HEADER_BYTES],
                          s_outbound_len - DEVICE_LINK_WIRE_HEADER_BYTES,
                          &status));
    TEST_ASSERT_EQUAL(DEVICE_LINK_STATUS_MALFORMED_FRAME, status);
    TEST_ASSERT_EQUAL(DEVICE_LINK_WIRE_HEADER_BYTES +
                      DEVICE_LINK_RESPONSE_STATUS_BYTES, s_outbound_len);

    _reset();
    nv_storage_fake_reset();
    _commit_auth_record();
    _feed_single_channel(recovery_bad_body, sizeof(recovery_bad_body),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_header(
                          s_outbound, s_outbound_len, &header));
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_status(
                          &s_outbound[DEVICE_LINK_WIRE_HEADER_BYTES],
                          s_outbound_len - DEVICE_LINK_WIRE_HEADER_BYTES,
                          &status));
    TEST_ASSERT_EQUAL(DEVICE_LINK_STATUS_INVALID_ARGUMENT, status);
    TEST_ASSERT_EQUAL(DEVICE_LINK_WIRE_HEADER_BYTES +
                      DEVICE_LINK_RESPONSE_STATUS_BYTES, s_outbound_len);
}

static void test_snapshot_request(void)
{
    /* request_id=2, get_link_snapshot. */
    static const uint8_t request[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
    TEST_ASSERT_TRUE(response.body_len >= 9U);
    TEST_ASSERT_EQUAL(0x09U, response.body_data[0]);
    TEST_ASSERT_EQUAL(0x01U, response.body_data[1]);
    for (size_t i = 2U; i < 9U; ++i)
    {
        TEST_ASSERT_EQUAL(0U, response.body_data[i]);
    }
}

static device_link_status_t _empty_scan_handler(
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
    return DEVICE_LINK_STATUS_UNSUPPORTED_OPERATION;
}

static void test_operation_declared_empty_result_never_encoded(void)
{
    /* A wifi-like async method whose descriptor declares core.v2.Empty.
     * A SUCCEEDED record for such a method must never carry a result
     * payload even if one was attached: the encoder excludes declared
     * Empty results explicitly instead of trusting payload presence. */
    static const device_link_tlv_schema_t s_test_empty_schema =
    {
        .fields = NULL,
        .field_count = 0U,
        .maximum_encoded_bytes = 0U,
    };
    static const device_link_method_descriptor_t s_test_methods[] =
    {
        {
            .method_id = 2U,
            .permission_id = DEVICE_LINK_PERMISSION_WIFI_SCAN,
            .channel = DEVICE_LINK_CHANNEL_SESSION,
            .maximum_request_bytes = 0U,
            .maximum_response_bytes = 16U,
            .request_schema = &s_test_empty_schema,
            .response_schema = &s_test_empty_schema,
            .operation_result_schema = &s_test_empty_schema,
            .response_body_status_mask =
            DEVICE_LINK_STATUS_MASK(DEVICE_LINK_STATUS_OK),
            .handler = _empty_scan_handler,
        },
    };
    static const device_link_domain_descriptor_t s_test_domain =
    {
        .domain_id = DEVICE_LINK_DOMAIN_WIFI,
        .major = 1U,
        .minor = 0U,
        .methods = s_test_methods,
        .method_count = 1U,
    };
    static const uint8_t bogus_payload[] = {0x08U, 0x01U};
    uint64_t operation_id = 0U;

    /* Register before init (boot_id is still zero), like the service does
     * before the router seals its startup descriptor set. */
    ble_link_service_reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_set_domain_descriptors(
                          &s_test_domain, 1U));
    ble_link_service_init(BOOT_ID, _capture, NULL, NULL, 32U);
    ble_link_events_init();
    _establish_session();
    _set_facts(true, true, true);
    (void)device_link_security_init(&s_sec_config);
    /* GetOperation is a CORE_OPERATE method: the session must be
     * authorized against a matching auth record. */
    device_link_security_auth_record_t auth_record;

    memset(&auth_record, 0, sizeof(auth_record));
    auth_record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    auth_record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    _set_test_grants(&auth_record);
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES; ++i)
    {
        auth_record.credential_id[i] = (uint8_t)(i + 1U);
        auth_record.device_auth_id[i] = (uint8_t)(0x60U + i);
    }
    auth_record.peer_addr_type = 1U;
    auth_record.peer_addr[0] = 0x11U;
    auth_record.peer_addr[5] = 0xeaU;
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_SALT_BYTES; ++i)
    {
        auth_record.salt[i] = (uint8_t)(0x80U + i);
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES; ++i)
    {
        auth_record.verifier[i] = (uint8_t)(0xa0U + i);
    }
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_link_security_save_auth_record(&auth_record));

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_async_operation_start(
                          DEVICE_LINK_DOMAIN_WIFI, 2U, 777U, NULL, NULL,
                          &operation_id));
    TEST_ASSERT_TRUE(operation_id != 0U);
    /* A buggy bridge attaches a payload despite the Empty declaration;
     * update() admits it defensively, the encoder must not expose it. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_async_operation_update(
                          777U, DEVICE_LINK_OPERATION_SUCCEEDED,
                          DEVICE_LINK_STATUS_OK, bogus_payload,
                          sizeof(bogus_payload)));
    uint8_t request[DEVICE_LINK_WIRE_HEADER_BYTES + 16U];
    device_link_wire_header_t header;
    device_link_tlv_writer_t writer;
    size_t body_len = 0U;

    memset(&header, 0, sizeof(header));
    header.kind = DEVICE_LINK_MESSAGE_REQUEST;
    header.domain_id = DEVICE_LINK_DOMAIN_CORE;
    header.domain_major = 2U;
    header.method_id = 6U; /* GetOperation */
    header.call_id = 1U;
    header.boot_id = BOOT_ID;
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_encode_header(
                          &header, request));
    device_link_tlv_writer_init(
        &writer, &request[DEVICE_LINK_WIRE_HEADER_BYTES], 16U);
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_put_fixed64(
                          &writer, 1U, operation_id));
    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_writer_finish(
                          &writer, &body_len));

    _clear_capture();
    _feed_single(request, DEVICE_LINK_WIRE_HEADER_BYTES + body_len);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_header(
                          s_outbound, s_outbound_len, &header));
    TEST_ASSERT_EQUAL(DEVICE_LINK_MESSAGE_RESPONSE, header.kind);

    device_link_status_t status = DEVICE_LINK_STATUS_INTERNAL;
    size_t body_offset = DEVICE_LINK_WIRE_HEADER_BYTES;

    TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_status(
                          &s_outbound[body_offset],
                          s_outbound_len - body_offset, &status));
    TEST_ASSERT_EQUAL(DEVICE_LINK_STATUS_OK, status);
    body_offset += DEVICE_LINK_RESPONSE_STATUS_BYTES;
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has_field = false;
    bool saw_state_succeeded = false;
    bool saw_result_payload = false;

    TEST_ASSERT_EQUAL(ESP_OK, device_link_tlv_reader_init(
                          &reader, &s_outbound[body_offset],
                          s_outbound_len - body_offset));
    while (device_link_tlv_reader_next(&reader, &field, &has_field) ==
            ESP_OK && has_field)
    {
        if (field.id == 4U && field.value.unsigned_value ==
                DEVICE_LINK_OPERATION_SUCCEEDED)
        {
            saw_state_succeeded = true;
        }
        if (field.id == 6U)
        {
            saw_result_payload = true;
        }
    }
    TEST_ASSERT_TRUE(saw_state_succeeded);
    TEST_ASSERT_TRUE(!saw_result_payload);
}

static void test_snapshot_zero_baseline_returns_internal(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0x00,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    ble_link_events_test_set_sequence(0U);
    _feed_single(request, sizeof(request));
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_INTERNAL, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_NONE, response.body);
}

/* authorize_prepare request, request_id=3. */
static const uint8_t s_prepare_request[] =
{
    0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
    out[1] = 0x02U;
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


static void test_authorize_prepare_rejects_unregistered_permissions(void)
{
    /* The contract permission registry freezes the requestable ids; an
     * unknown id and an id of an unadvertised domain must be refused at
     * Prepare, before any transaction or durable grant exists. */
    static const uint8_t unknown_permission_request[] =
    {
        0x14U, 0x00U, 0x02U, 0x03U,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x04U, 0xb4U, 0x24U, /* requested_permissions = 0x1234 (unknown). */
    };
    static const uint8_t unadvertised_domain_request[] =
    {
        0x14U, 0x00U, 0x02U, 0x03U,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x04U, 0x81U, 0x06U, /* location.read (0x0301), domain unregistered. */
    };
    static const uint8_t mixed_request[] =
    {
        0x14U, 0x00U, 0x02U, 0x03U,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x04U, 0x01U, 0x04U, 0x81U, 0x06U, /* core.read + location.read. */
    };
    static const uint8_t valid_request[] =
    {
        0x14U, 0x00U, 0x02U, 0x03U,
        0x01U, 0x00U, 0x00U, 0x00U,
        0x08U, 0x07U, 0x06U, 0x05U, 0x04U, 0x03U, 0x02U, 0x01U,
        0x04U, 0x01U, /* core.read. */
    };
    const uint8_t *cases[] =
    {
        unknown_permission_request,
        unadvertised_domain_request,
        mixed_request,
    };
    const size_t case_lengths[] =
    {
        sizeof(unknown_permission_request),
        sizeof(unadvertised_domain_request),
        sizeof(mixed_request),
    };

    for (size_t i = 0U; i < 3U; ++i)
    {
        device_link_wire_header_t header;
        device_link_status_t status;

        _reset();
        _feed_single_channel(cases[i], case_lengths[i],
                             BLE_LINK_SERVICE_RX_SESSION);
        _reassemble_captured();
        TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_header(
                              s_outbound, s_outbound_len, &header));
        TEST_ASSERT_EQUAL(DEVICE_LINK_MESSAGE_RESPONSE, header.kind);
        TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_status(
                              &s_outbound[DEVICE_LINK_WIRE_HEADER_BYTES],
                              s_outbound_len - DEVICE_LINK_WIRE_HEADER_BYTES,
                              &status));
        TEST_ASSERT_EQUAL(DEVICE_LINK_STATUS_INVALID_ARGUMENT, status);
    }
    /* A registered Core permission still prepares successfully. */
    {
        device_link_wire_header_t header;
        device_link_status_t status;

        _reset();
        _feed_single_channel(valid_request, sizeof(valid_request),
                             BLE_LINK_SERVICE_RX_SESSION);
        _reassemble_captured();
        TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_header(
                              s_outbound, s_outbound_len, &header));
        TEST_ASSERT_EQUAL(ESP_OK, device_link_wire_decode_status(
                              &s_outbound[DEVICE_LINK_WIRE_HEADER_BYTES],
                              s_outbound_len - DEVICE_LINK_WIRE_HEADER_BYTES,
                              &status));
        TEST_ASSERT_EQUAL(DEVICE_LINK_STATUS_OK, status);
    }
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

    /* Prepare alone does not expose a local confirmation. The first
     * valid Commit is the mandatory probe and is refused. */
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    TEST_ASSERT_TRUE(!ble_link_service_pending_confirmation());
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
    TEST_ASSERT_TRUE(ble_link_service_pending_confirmation());
    const uint64_t confirmation_token =
        ble_link_service_confirmation_token();

    TEST_ASSERT_TRUE(confirmation_token != 0U);

    /* Confirm locally, then commit succeeds with a fresh request id
     * (the dispatcher replay guard rejects reused ids). */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_confirm_binding(
                          confirmation_token, true));
    TEST_ASSERT_TRUE(!ble_link_service_pending_confirmation());
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    const size_t commit2_len = _build_commit_body(commit, sizeof(commit), 6U);

    TEST_ASSERT_EQUAL(commit_len, commit2_len);
    _feed_single_channel(commit, commit2_len,
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

static void test_authorize_commit_rejects_private_peer_address(void)
{
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;
    device_link_security_auth_record_t record;
    uint8_t commit[64];

    device_link_security_deinit();
    nv_storage_fake_reset();
    _reset();
    s_pending_captured = false;
    esp_random_fake_reset(0x5eed5eedU);
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    const size_t commit_len = _build_commit_body(
                                  commit, sizeof(commit), 5U);

    _clear_capture();
    _feed_single_channel(commit, commit_len, BLE_LINK_SERVICE_RX_SESSION);
    const uint64_t token = ble_link_service_confirmation_token();

    TEST_ASSERT_TRUE(token != 0U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_confirm_binding(token, true));

    /* Identity facts are revalidated at the durable boundary. An RPA must
     * never become the primary key even if an upstream regression labels it
     * identity_known. */
    s_facts.peer_addr[5] = 0x4aU;
    TEST_ASSERT_EQUAL(commit_len,
                      _build_commit_body(commit, sizeof(commit), 6U));
    _clear_capture();
    _feed_single_channel(commit, commit_len, BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len, &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_INVALID_ARGUMENT, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_NONE, response.body);
    memset(&record, 0, sizeof(record));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      device_link_security_load_auth_record(&record));
}

static void test_authorize_commit_wrong_credential(void)
{
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;
    uint8_t commit[64];

    _reset();
    s_pending_captured = false;
    _set_facts(true, true, true);
    esp_random_fake_reset(0x5eed5eedU);
    /* Prepare first, then commit with the real transaction id but a
     * corrupted credential: the transaction exists, so the mismatch is
     * INVALID_ARGUMENT (not NOT_FOUND). */
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    const size_t commit_len = _build_commit_body(commit, sizeof(commit), 5U);

    /* Corrupt the 16-byte credential (last 16 bytes of the commit body). */
    memset(&commit[commit_len - 16U], 0xffU, 16U);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
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

static void test_authorize_commit_malformed_record_fails_closed(void)
{
    /* F-5 fail-closed (core v2 operations.md): a present but malformed
     * durable auth record rejects the commit probe with INTERNAL and is
     * never silently overwritten by a fresh bind. Only an explicit
     * factory reset / revoke journal may erase it. */
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;
    device_link_security_auth_record_t record;
    uint8_t commit[64];

    _reset();
    s_pending_captured = false;
    _set_facts(true, true, true);
    esp_random_fake_reset(0x5eed5eedU);
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    const size_t first_commit_len = _build_commit_body(
                                        commit, sizeof(commit), 5U);

    /* First commit: CONFIRMATION_REQUIRED (the durable probe runs only
     * after local confirmation). */
    _clear_capture();
    _feed_single_channel(commit, first_commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    const uint64_t token = ble_link_service_confirmation_token();

    TEST_ASSERT_TRUE(token != 0U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_confirm_binding(token, true));
    /* Plant a malformed durable record (bad magic/schema) between the
     * confirmation and the commit so the durable probe must see it. */
    memset(&record, 0, sizeof(record));
    TEST_ASSERT_EQUAL(ESP_OK, nv_storage_set_blob(
                          "dls.auth", &record, sizeof(record)));
    const size_t commit_len = _build_commit_body(commit, sizeof(commit), 6U);

    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_INTERNAL, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_NONE, response.body);
    /* The malformed record survives: no silent overwrite. */
    memset(&record, 0xa5, sizeof(record));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      device_link_security_load_auth_record(&record));
}

static void test_authorize_commit_unknown_transaction_not_found(void)
{
    /* A Commit for an unknown transaction (no Prepare, or a retired one)
     * is NOT_FOUND with an empty body per the core v2 contract. */
    static const uint8_t commit[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
    /* No Prepare: the referenced transaction is unknown. */
    _feed_single_channel(commit, sizeof(commit),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_NOT_FOUND, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_NONE, response.body);
}

static void test_domain_call_unadvertised(void)
{
    /* No domain is published until a complete startup-frozen adapter is
     * installed. A syntactically valid DomainCall is therefore rejected. */
    static const uint8_t domain_call[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7a, 0x00,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    _feed_single(domain_call, sizeof(domain_call));
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNSUPPORTED_OPERATION, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_NONE, response.body);

    /* No subscriber exists: publishing produces no output. */
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
    TEST_ASSERT_EQUAL(0U, s_capture_count);
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
    memset(framed, 0, sizeof(framed));
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };

    _reset();
    /* Not authorized: control admission fails. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          false, 2U));
    _set_facts(true, true, false);
    uint8_t framed[512];
    memset(framed, 0, sizeof(framed));
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
    };
    uint8_t framed[512];
    memset(framed, 0, sizeof(framed));
    _feed_single_channel(prepare, sizeof(prepare),
                         BLE_LINK_SERVICE_RX_SESSION);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x02U;
    framed[3] = 0x00U;
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
    memset(framed, 0, sizeof(framed));
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[512];
    memset(framed, 0, sizeof(framed));
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    /* Start a partial frame in generation 1. */
    uint8_t framed[512];
    memset(framed, 0, sizeof(framed));
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
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_MANIFEST, response.body);
}

static void test_event_wire_structure(void)
{
    /* Events are not advertised in v1: without a subscriber the publish
     * path produces no output, so the event wire structure is covered by
     * the link-events codec tests instead. */
    ble_link_state_snapshot_t link_state;

    _reset();
    memset(&link_state, 0, sizeof(link_state));
    link_state.boot_id = BOOT_ID;
    link_state.binding_state = BLE_LINK_BINDING_BOUND;
    link_state.authorization_state = BLE_LINK_AUTHORIZATION_AUTHORIZED;
    link_state.encrypted = true;
    link_state.secure_connections_bond_verified = true;
    link_state.identity_known = true;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_publish_link_state(
                          &s_facts, &link_state));
    TEST_ASSERT_EQUAL(0U, s_capture_count);
}

static void test_publish_after_revoke_no_output(void)
{
    ble_link_state_snapshot_t link_state;

    _reset();
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
    const size_t commit_len = _build_commit_body(commit, sizeof(commit), 5U);

    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    const uint64_t confirmation_token =
        ble_link_service_confirmation_token();

    TEST_ASSERT_TRUE(confirmation_token != 0U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_confirm_binding(
                          confirmation_token, true));
    (void)_build_commit_body(commit, sizeof(commit), 6U);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    /* Replay the same commit with a fresh request id: idempotent success
     * with the full result. */
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    (void)_build_commit_body(commit, sizeof(commit), 7U);
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
    _enable_provisional_security_ops();
    ble_link_service_idle_timeout(GEN);
    /* The Security 2 session is closed: control admission fails. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_query_admission(
                          GEN, BLE_LINK_SESSION_CHANNEL_CONTROL,
                          &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED, error);
    /* The adapter session is closed through the security ops as well. */
    TEST_ASSERT_EQUAL(1U, s_security_close_count);
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[512];
    memset(framed, 0, sizeof(framed));
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

static void test_channel_routing(void)
{
    static const uint8_t capabilities[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[512];
    memset(framed, 0, sizeof(framed));
    _reset();
    _establish_session();
    _set_facts(true, true, true);
    /* GetCapabilities is admitted on the session channel during the
     * bootstrap flow, before authorization. */
    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x01U;
    framed[4] = (uint8_t)((sizeof(capabilities) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(capabilities) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], capabilities, sizeof(capabilities));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &s_facts,
                          BLE_LINK_SERVICE_RX_SESSION,
                          framed,
                          9U + sizeof(capabilities)));
    TEST_ASSERT_TRUE(s_capture_count > 0U);
    /* A control-only request (subscribe_events) on the session channel is
     * rejected. */
    static const uint8_t subscribe[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x72, 0x00,
    };

    memset(framed, 0, sizeof(framed));
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x02U;
    framed[4] = (uint8_t)((sizeof(subscribe) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(subscribe) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], subscribe, sizeof(subscribe));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_feed(
                          &s_facts,
                          BLE_LINK_SERVICE_RX_SESSION,
                          framed,
                          9U + sizeof(subscribe)));
    TEST_ASSERT_TRUE(s_capture_count > 0U);
    /* The reverse direction: a bootstrap request on the control channel. */
    static const uint8_t prepare[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
    };

    memset(framed, 0, sizeof(framed));
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x03U;
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[512];
    memset(framed, 0, sizeof(framed));
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
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };

    _reset();
    s_facts.preferred_att_mtu = 23U;
    uint8_t framed[512];
    memset(framed, 0, sizeof(framed));
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
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_MANIFEST, response.body);
}

static void test_idle_timeout_clears_state(void)
{
    ble_link_state_snapshot_t link_state;

    _reset();
    /* Core v2 does not expose an event subscription method.  The timeout
     * path must therefore remain harmless when no subscriber exists. */
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

static void test_stale_ingress_epoch_timeout_is_ignored(void)
{
    uint8_t framed[20] = {0};
    bool partial = false;
    uint32_t stale_epoch = 0U;
    uint32_t current_epoch = 0U;
    ble_link_work_t *work = NULL;

    _reset();
    framed[0] = 1U;
    framed[1] = 1U;
    framed[2] = 1U;
    framed[4] = 40U;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_service_accept(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, sizeof(framed), &work));
    TEST_ASSERT_TRUE(work == NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_get_reassembly_state(
                          BLE_LINK_SERVICE_RX_CONTROL,
                          &partial, &stale_epoch));
    TEST_ASSERT_TRUE(partial);

    ble_link_service_clear_session_state();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_service_accept(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, sizeof(framed), &work));
    TEST_ASSERT_TRUE(work == NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_get_reassembly_state(
                          BLE_LINK_SERVICE_RX_CONTROL,
                          &partial, &current_epoch));
    TEST_ASSERT_TRUE(partial);
    TEST_ASSERT_TRUE(current_epoch != stale_epoch);

    ble_link_service_idle_timeout_epoch(GEN, stale_epoch);
    TEST_ASSERT_TRUE(ble_link_service_has_partial_frame(
                         BLE_LINK_SERVICE_RX_CONTROL));
    ble_link_service_idle_timeout_epoch(GEN, current_epoch);
    TEST_ASSERT_TRUE(!ble_link_service_has_partial_frame(
                         BLE_LINK_SERVICE_RX_CONTROL));
}

static void test_completed_work_is_copied_and_deferred(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x21, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[64] = {0};
    const size_t total = sizeof(request) + 1U;

    _reset();
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 1U;
    framed[4] = (uint8_t)total;
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    ble_link_work_t *work = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, 8U + total, &work));
    TEST_ASSERT_TRUE(work != NULL);
    TEST_ASSERT_EQUAL(0U, s_capture_count);
    memset(framed, 0xa5, sizeof(framed));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_EQUAL(1U, s_capture_count);
}

static void test_terminal_clear_retires_queued_protected_work(void)
{
    static const uint8_t request[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x22, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[64] = {0};
    const size_t total = sizeof(request) + 1U;

    _reset();
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 1U;
    framed[4] = (uint8_t)total;
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    ble_link_work_t *work = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &s_facts, BLE_LINK_SERVICE_RX_CONTROL,
                          framed, 8U + total, &work));
    ble_link_operation_identity_t terminal =
    {
        .generation = GEN,
        .security_epoch = s_facts.security_epoch,
        .kind = BLE_LINK_OPERATION_DISCONNECT,
        .conn_handle = s_facts.conn_handle,
    };
    ble_link_operation_identity_t stale = terminal;

    stale.conn_handle++;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(
                          &stale));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_clear_session_state_if_current(
                          &terminal));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_EQUAL(0U, s_capture_count);
}

static void test_terminal_clear_retires_queued_handshake(void)
{
    static const uint8_t cmd0[] = "cmd0";
    ble_link_work_t *work = NULL;

    _reset();
    ble_link_service_reset();
    ble_link_service_init(BOOT_ID, _capture, NULL,
                          &s_handshake_security_ops, 32U);
    s_auto_confirm = false;
    s_handshake_count = 0U;
    s_security_close_count = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &work));
    TEST_ASSERT_TRUE(work != NULL);

    const ble_link_operation_identity_t terminal =
    {
        .generation = GEN,
        .security_epoch = s_facts.security_epoch,
        .kind = BLE_LINK_OPERATION_RESET,
        .conn_handle = s_facts.conn_handle,
    };
    ble_link_operation_identity_t stale = terminal;

    stale.generation++;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(
                          &stale));
    stale = terminal;
    stale.conn_handle++;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(
                          &stale));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_clear_session_state_if_current(
                          &terminal));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_service_execute(work));
    ble_link_service_release_work(work);
    work = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &work));
    TEST_ASSERT_TRUE(work == NULL);
    ble_link_service_idle_timeout(GEN);
    TEST_ASSERT_EQUAL(0U, s_handshake_count);
    TEST_ASSERT_EQUAL(0U, s_capture_count);
    TEST_ASSERT_EQUAL(0U, s_security_close_count);
    TEST_ASSERT_TRUE(!ble_link_service_response_in_flight());
}

static void test_ingress_requires_immutable_acl_identity(void)
{
    static const uint8_t cmd0[] = "cmd0";
    ble_link_work_t *work = NULL;

    _reset();
    s_facts.connection_generation = 0U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &work));
    TEST_ASSERT_TRUE(work == NULL);

    s_facts.connection_generation = GEN;
    s_facts.conn_handle = UINT16_MAX;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &work));
    TEST_ASSERT_TRUE(work == NULL);
}

static void test_retired_generation_allows_handle_reuse(void)
{
    static const uint8_t cmd0[] = "cmd0";
    ble_link_work_t *old_work = NULL;
    ble_link_work_t *new_work = NULL;

    _reset();
    ble_link_service_reset();
    ble_link_service_init(BOOT_ID, _capture, NULL,
                          &s_handshake_security_ops, 32U);
    s_auto_confirm = false;
    s_handshake_count = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &old_work));
    TEST_ASSERT_TRUE(old_work != NULL);
    const ble_link_operation_identity_t old_terminal =
    {
        .generation = GEN,
        .security_epoch = s_facts.security_epoch,
        .kind = BLE_LINK_OPERATION_DISCONNECT,
        .conn_handle = s_facts.conn_handle,
    };

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_clear_session_state_if_current(
                          &old_terminal));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_service_execute(old_work));
    ble_link_service_release_work(old_work);

    ble_link_session_init(BOOT_ID);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN + 1U,
                          BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN + 1U,
                          BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN + 1U,
                          BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(
                          GEN + 1U, true));
    _set_facts(true, false, false);
    s_facts.connection_generation = GEN + 1U;
    s_facts.security_epoch = ble_link_session_security2_epoch();
    TEST_ASSERT_EQUAL(ESP_OK, _accept_handshake_single(
                          cmd0, sizeof(cmd0) - 1U, &new_work));
    TEST_ASSERT_TRUE(new_work != NULL);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(
                          &old_terminal));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(new_work));
    ble_link_service_release_work(new_work);
    TEST_ASSERT_EQUAL(1U, s_handshake_count);
}

static void _commit_auth_record(void)
{
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    _set_test_grants(&record);
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES; ++i)
    {
        record.credential_id[i] = (uint8_t)(i + 1U);
        record.device_auth_id[i] = (uint8_t)(0x40U + i);
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_SALT_BYTES; ++i)
    {
        record.salt[i] = (uint8_t)(0x30U + i);
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES; ++i)
    {
        record.verifier[i] = (uint8_t)(i & 0x7fU);
    }
    record.peer_addr_type = 1U;
    record.peer_addr[0] = 0x11U;
    record.peer_addr[5] = 0xeaU;
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_link_security_save_auth_record(&record));
}

/**
 * @brief Build a GetAuthorizationRequest envelope with RECOVERY_QUERY.
 */
static size_t _build_recovery_query(
    uint8_t *out, size_t capacity, uint64_t request_id,
    const uint8_t *credential, size_t credential_len)
{
    size_t pos = 0U;

    out[pos++] = 0x08U; /* protocol_major=2 */
    out[pos++] = 0x02U;
    out[pos++] = 0x19U; /* boot_id fixed64 */
    for (size_t i = 0U; i < 8U; ++i)
    {
        out[pos++] = (uint8_t)(BOOT_ID >> (8U * i));
    }
    out[pos++] = 0x20U; /* flags=[RECOVERY_QUERY] */
    out[pos++] = 0x01U;
    out[pos++] = 0x52U; /* request submessage */
    const size_t request_start = pos;

    pos++;
    out[pos++] = 0x09U; /* request_id fixed64 */
    for (size_t i = 0U; i < 8U; ++i)
    {
        out[pos++] = (uint8_t)(request_id >> (8U * i));
    }
    out[pos++] = 0x72U; /* get_authorization field 14 */
    out[pos++] = (uint8_t)(2U + credential_len);
    out[pos++] = 0x0aU; /* credential_id field 1 */
    out[pos++] = (uint8_t)credential_len;
    TEST_ASSERT_TRUE(pos < capacity);
    memcpy(&out[pos], credential, credential_len);
    pos += credential_len;
    out[request_start] = (uint8_t)(pos - request_start - 1U);
    return pos;
}

static void test_get_authorization_recovery(void)
{
    uint8_t request[64];
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    nv_storage_fake_reset();
    _commit_auth_record();
    static const uint8_t credential[16] =
    {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    };
    const size_t request_len = _build_recovery_query(
                                   request, sizeof(request), 4U,
                                   credential, sizeof(credential));

    _feed_single_channel(request, request_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(4U, response.request_id);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, response.error);
    TEST_ASSERT_EQUAL(BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT,
                      response.body);
    /* Body: field 1 credential_id (16 bytes), field 2 device_auth_id
     * (16 bytes, 0x40..0x4f of the committed record), field 3 state. */
    TEST_ASSERT_EQUAL(0x0aU, response.body_data[0]);
    TEST_ASSERT_EQUAL(0x10U, response.body_data[1]);
    TEST_ASSERT_EQUAL(0x40U, response.body_data[20]);
    TEST_ASSERT_EQUAL(0x4fU, response.body_data[35]);
    TEST_ASSERT_EQUAL(0x18U, response.body_data[36]);
    TEST_ASSERT_EQUAL(0x05U, response.body_data[37]);

    /* A wrong credential is rejected with NOT_FOUND. */
    static const uint8_t wrong[16] = {0x00};

    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    const size_t wrong_len = _build_recovery_query(
                                 request, sizeof(request), 5U,
                                 wrong, sizeof(wrong));

    _feed_single_channel(request, wrong_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_NOT_FOUND, response.error);

    /* An I/O failure is ambiguous and must not collapse to NOT_FOUND. */
    nv_storage_fake_fail_next_get(ESP_FAIL);
    device_link_security_auth_record_t probe_record;

    memset(&probe_record, 0, sizeof(probe_record));
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      device_link_security_load_auth_record(&probe_record));
    nv_storage_fake_fail_next_get(ESP_FAIL);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    const size_t storage_len = _build_recovery_query(
                                   request, sizeof(request), 6U,
                                   credential, sizeof(credential));

    _feed_single_channel(request, storage_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_STORAGE, response.error);

    /* A present but malformed durable record is an internal consistency
     * failure, also ambiguous to the client. */
    device_link_security_auth_record_t corrupt_record;

    memset(&corrupt_record, 0, sizeof(corrupt_record));
    TEST_ASSERT_EQUAL(ESP_OK, nv_storage_set_blob(
                          "dls.auth", &corrupt_record,
                          sizeof(corrupt_record)));
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    const size_t corrupt_len = _build_recovery_query(
                                   request, sizeof(request), 7U,
                                   credential, sizeof(credential));

    _feed_single_channel(request, corrupt_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_INTERNAL, response.error);
    nv_storage_fake_reset();
}

static void _decode_real_captured_response(
    uint64_t request_id, uint32_t error,
    ble_link_codec_response_tag_t body)
{
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reassemble_real_protected();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(request_id, response.request_id);
    TEST_ASSERT_EQUAL(error, response.error);
    TEST_ASSERT_EQUAL(body, response.body);
}

static void test_real_security2_rehandshake_commit_replay_and_recovery(void)
{
    uint8_t commit[64];
    uint8_t recovery[64];
    device_link_security_auth_record_t record;

    _setup_real_bootstrap_session();
    const uint32_t bootstrap_epoch = _complete_real_handshake();

    TEST_ASSERT_EQUAL(DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP,
                      device_link_security_selected_verifier());
    TEST_ASSERT_TRUE(!s_facts.authorized);

    /* Prepare, Commit probe, local confirmation, and durable Commit all run
     * through the real adapter's decrypt/request/encrypt callbacks. */
    s_pending_captured = false;
    esp_random_fake_reset(0x5eed5eedU);
    _clear_capture();
    _feed_real_protected(s_prepare_request, sizeof(s_prepare_request));
    _reassemble_real_protected();
    _capture_pending_credential();
    _confirm_last_captured_flow();
    const size_t commit_len = _build_commit_body(
                                  commit, sizeof(commit), 5U);

    _clear_capture();
    _feed_real_protected(commit, commit_len);
    _decode_real_captured_response(
        5U, BLE_LINK_ERROR_CONFIRMATION_REQUIRED,
        BLE_LINK_CODEC_RESPONSE_NONE);
    const uint64_t confirmation_token =
        ble_link_service_confirmation_token();

    TEST_ASSERT_TRUE(confirmation_token != 0U);
    _confirm_last_captured_flow();
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_confirm_binding(
                          confirmation_token, true));

    TEST_ASSERT_EQUAL(commit_len,
                      _build_commit_body(commit, sizeof(commit), 6U));
    _clear_capture();
    _feed_real_protected(commit, commit_len);
    _decode_real_captured_response(
        6U, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT);
    TEST_ASSERT_TRUE(ble_link_service_response_in_flight());
    _confirm_last_captured_flow();
    TEST_ASSERT_TRUE(!ble_link_service_response_in_flight());
    TEST_ASSERT_EQUAL(1U, s_provisional_promote_count);
    memset(&record, 0, sizeof(record));
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_link_security_load_auth_record(&record));
    TEST_ASSERT_EQUAL(0, memcmp(record.credential_id,
                                s_pending_credential,
                                sizeof(s_pending_credential)));
    TEST_ASSERT_TRUE(!device_link_security_session_open());

    /* The terminal Commit response retired the bootstrap session, but its
     * cache remains ACL-scoped. A true long-term Cmd0/Cmd1 re-handshake must
     * use one new link epoch (Cmd1 cannot allocate another) and restore the
     * authorized state from the committed peer record. */
    TEST_ASSERT_TRUE(ble_link_session_security2_epoch() > bootstrap_epoch);
    s_facts.security_epoch = ble_link_session_security2_epoch();
    s_facts.session_authenticated = false;
    s_facts.authorized = false;
    const uint32_t long_term_epoch = _complete_real_handshake();

    TEST_ASSERT_TRUE(long_term_epoch > bootstrap_epoch);
    TEST_ASSERT_EQUAL(DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM,
                      device_link_security_selected_verifier());
    TEST_ASSERT_TRUE(s_facts.authorized);

    /* A fresh request id with the exact committed txn+credential replays the
     * terminal success after re-handshake. It is not tied to the retired
     * bootstrap adapter session. */
    TEST_ASSERT_EQUAL(commit_len,
                      _build_commit_body(commit, sizeof(commit), 7U));
    _clear_capture();
    _feed_real_protected(commit, commit_len);
    _decode_real_captured_response(
        7U, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT);
    _confirm_last_captured_flow();

    /* The same real long-term session can perform the contract's ambiguous
     * Commit recovery query using the durable credential. */
    const size_t recovery_len = _build_recovery_query(
                                    recovery, sizeof(recovery), 8U,
                                    s_pending_credential,
                                    sizeof(s_pending_credential));

    _clear_capture();
    _feed_real_protected(recovery, recovery_len);
    _decode_real_captured_response(
        8U, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT);
    _confirm_last_captured_flow();

    device_link_security_deinit();
    ble_link_session_set_pairing_window(false);
    nv_storage_fake_reset();
}

static void test_get_authorization_requires_recovery_flag(void)
{
    uint8_t request[64];
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    _reset();
    nv_storage_fake_reset();
    _commit_auth_record();
    static const uint8_t credential[16] =
    {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    };
    size_t pos = 0U;

    request[pos++] = 0x08U;
    request[pos++] = 0x02U;
    request[pos++] = 0x19U;
    for (size_t i = 0U; i < 8U; ++i)
    {
        request[pos++] = (uint8_t)(BOOT_ID >> (8U * i));
    }
    request[pos++] = 0x52U;
    const size_t request_start = pos;

    pos++;
    request[pos++] = 0x09U;
    {
        const uint64_t request_id = 6U;

        for (size_t i = 0U; i < 8U; ++i)
        {
            request[pos++] = (uint8_t)(request_id >> (8U * i));
        }
    }
    request[pos++] = 0x72U;
    request[pos++] = (uint8_t)(2U + sizeof(credential));
    request[pos++] = 0x0aU;
    request[pos++] = (uint8_t)sizeof(credential);
    memcpy(&request[pos], credential, sizeof(credential));
    pos += sizeof(credential);
    request[request_start] = (uint8_t)(pos - request_start - 1U);

    _feed_single_channel(request, pos,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_INVALID_ARGUMENT, response.error);
    nv_storage_fake_reset();
}

static void test_prepare_retires_previous_transaction(void)
{
    /* Core v2 semantic invariant #31: a new Prepare retires the previous
     * pending transaction -- txn id, credential id, application password,
     * confirmation token, and any pending Commit matching it are all
     * invalidated. A Commit for the retired transaction is NOT_FOUND. */
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t first;
    ble_link_codec_response_t second;
    uint8_t first_txn[8];
    uint8_t first_credential[16];
    uint8_t commit[64];

    _reset();
    esp_random_fake_reset(0x5eed5eedU);
    s_pending_captured = false;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &first));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, first.error);
    /* Remember the first transaction material. */
    TEST_ASSERT_TRUE(first.body_len >= 27U);
    memcpy(first_txn, &first.body_data[1], 8U);
    memcpy(first_credential, &first.body_data[11], 16U);

    /* A second prepare with a fresh request id retires the first
     * transaction and returns fresh material. */
    static const uint8_t prepare2[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x07, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
    };

    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(prepare2, sizeof(prepare2),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &second));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, second.error);
    TEST_ASSERT_TRUE(second.body_len >= 27U);
    TEST_ASSERT_TRUE(memcmp(first_txn, &second.body_data[1], 8U) != 0);
    TEST_ASSERT_TRUE(memcmp(first_credential,
                            &second.body_data[11], 16U) != 0);

    /* A Commit matching the retired transaction is NOT_FOUND and the
     * retired credential never grants. */
    memcpy(s_pending_txn, first_txn, sizeof(s_pending_txn));
    memcpy(s_pending_credential, first_credential,
           sizeof(s_pending_credential));
    s_pending_captured = true;
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    const size_t commit_len = _build_commit_body(commit, sizeof(commit), 5U);

    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &second));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_NOT_FOUND, second.error);
}

static void test_authorize_expiry_clears_transaction(void)
{
    _reset();
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_link_service_auth_expiry_remaining_ms());
    esp_random_fake_reset(0x5eed5eedU);
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    TEST_ASSERT_TRUE(!ble_link_service_pending_confirmation());
    TEST_ASSERT_TRUE(ble_link_service_auth_expiry_remaining_ms() > 0U);
    TEST_ASSERT_TRUE(ble_link_service_auth_expiry_remaining_ms() <
                     UINT32_MAX);

    /* Force the deadline into the past; the tick clears the transaction. */
    const TickType_t now = xTaskGetTickCount();
    const TickType_t past_deadline = now == 1U ? UINT32_MAX : now - 1U;

    ble_link_service_test_set_auth_deadline_ticks(past_deadline);
    TEST_ASSERT_EQUAL(0U, ble_link_service_auth_expiry_remaining_ms());
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_auth_expiry_tick());
    TEST_ASSERT_TRUE(!ble_link_service_pending_confirmation());
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_link_service_auth_expiry_remaining_ms());
}

static void test_provisional_bond_lifecycle(void)
{
    uint8_t commit[64];

    device_link_security_deinit();
    nv_storage_fake_reset();

    _reset();
    _enable_provisional_security_ops();
    s_pending_captured = false;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    const size_t probe_len = _build_commit_body(
                                 commit, sizeof(commit), 5U);

    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, probe_len, BLE_LINK_SERVICE_RX_SESSION);
    TEST_ASSERT_TRUE(ble_link_service_pending_confirmation());
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_confirm_binding(
                          ble_link_service_confirmation_token(), false));
    TEST_ASSERT_EQUAL(1U, s_provisional_discard_count);
    TEST_ASSERT_EQUAL(GEN, s_provisional_generation);
    TEST_ASSERT_TRUE(s_provisional_terminate);
    TEST_ASSERT_EQUAL(1U, s_security_close_count);

    _reset();
    _enable_provisional_security_ops();
    s_pending_captured = false;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    TEST_ASSERT_TRUE(!ble_link_service_pending_confirmation());
    ble_link_service_test_set_auth_deadline_ticks(UINT32_MAX);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_auth_expiry_tick());
    TEST_ASSERT_EQUAL(1U, s_provisional_discard_count);
    TEST_ASSERT_EQUAL(GEN, s_provisional_generation);
    TEST_ASSERT_TRUE(s_provisional_terminate);

    _reset();
    _enable_provisional_security_ops();
    esp_random_fake_reset(0x5eed5eedU);
    s_pending_captured = false;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    const size_t commit_len = _build_commit_body(commit, sizeof(commit), 6U);

    _feed_single_channel(commit, commit_len, BLE_LINK_SERVICE_RX_SESSION);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_confirm_binding(
                          ble_link_service_confirmation_token(), true));
    (void)_build_commit_body(commit, sizeof(commit), 7U);

    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len, BLE_LINK_SERVICE_RX_SESSION);
    TEST_ASSERT_EQUAL(1U, s_provisional_promote_count);
    TEST_ASSERT_EQUAL(0U, s_provisional_discard_count);
    TEST_ASSERT_EQUAL(GEN, s_provisional_generation);
}

static void test_provisional_cleanup_is_retained_until_accepted(void)
{
    _reset();
    _enable_provisional_security_ops();
    s_provisional_result = ESP_ERR_NO_MEM;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    ble_link_service_clear_session_state();
    TEST_ASSERT_EQUAL(1U, s_provisional_discard_count);
    const ble_link_operation_identity_t retained = s_provisional_identity;

    TEST_ASSERT_TRUE(retained.token != 0U);
    TEST_ASSERT_EQUAL(BLE_LINK_OPERATION_PROVISIONAL_DISCARD,
                      retained.kind);
    TEST_ASSERT_TRUE(ble_link_service_retained_cleanup_pending());
    TEST_ASSERT_TRUE(
        ble_link_service_retained_retry_remaining_ms() > 0U);
    TEST_ASSERT_TRUE(
        ble_link_service_retained_retry_remaining_ms() <= 100U);
    s_provisional_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(1U, s_provisional_discard_count);
    host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(2U, s_provisional_discard_count);
    TEST_ASSERT_TRUE(ble_link_operation_identity_equal(
                         &retained, &s_provisional_identity));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(2U, s_provisional_discard_count);
    TEST_ASSERT_TRUE(!ble_link_service_retained_cleanup_pending());
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_link_service_retained_retry_remaining_ms());
}

static void test_clear_without_connection_does_not_forge_cleanup_identity(void)
{
    _reset();
    _enable_provisional_security_ops();

    /* No frame was accepted, so the service has no immutable ACL identity.
     * The port owns the physical provisional tracker and handles its exact
     * DISCONNECT/RESET fallback; generation zero must never be dispatched. */
    ble_link_service_clear_session_state();

    TEST_ASSERT_EQUAL(0U, s_provisional_discard_count);
    TEST_ASSERT_TRUE(!ble_link_service_retained_cleanup_pending());
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_link_service_retained_retry_remaining_ms());
}

static void test_provisional_cleanup_backoff_is_bounded(void)
{
    static const uint32_t delays_ms[] = {100U, 200U, 400U, 800U, 1000U,
                                         1000U
                                        };

    _reset();
    _enable_provisional_security_ops();
    s_provisional_result = ESP_ERR_NO_MEM;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    ble_link_service_set_worker_wake(_owner_wake, NULL);
    ble_link_service_clear_session_state();
    TEST_ASSERT_EQUAL(1U, s_provisional_discard_count);
    TEST_ASSERT_EQUAL(1U, s_owner_wake_count);

    for (size_t attempt = 0U;
            attempt < sizeof(delays_ms) / sizeof(delays_ms[0]); ++attempt)
    {
        const uint32_t remaining_ms =
            ble_link_service_retained_retry_remaining_ms();

        TEST_ASSERT_TRUE(remaining_ms > 0U);
        TEST_ASSERT_TRUE(remaining_ms <= delays_ms[attempt]);
        for (unsigned int notification = 0U; notification < 16U;
                ++notification)
        {
            ble_link_service_wake_owner();
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED,
                              ble_link_service_pump_tx());
        }
        TEST_ASSERT_EQUAL(attempt + 1U, s_provisional_discard_count);
        /* External notifications only wake the owner; failed retry dispatch
         * itself must never add another self-wake. */
        TEST_ASSERT_EQUAL(1U + 16U * (attempt + 1U), s_owner_wake_count);
        host_freertos_advance_ticks(pdMS_TO_TICKS(delays_ms[attempt]));
        TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_link_service_pump_tx());
        TEST_ASSERT_EQUAL(attempt + 2U, s_provisional_discard_count);
    }
    TEST_ASSERT_TRUE(
        ble_link_service_retained_retry_remaining_ms() <= 1000U);
    s_provisional_result = ESP_OK;
    host_freertos_advance_ticks(pdMS_TO_TICKS(1000U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_TRUE(!ble_link_service_retained_cleanup_pending());
}

static void test_repeated_terminal_discard_coalesces_physical_target(void)
{
    _reset();
    _enable_provisional_security_ops();
    s_provisional_result = ESP_ERR_NO_MEM;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);

    /* The timeout retains the first discard and then clears auth_txn. */
    ble_link_service_idle_timeout(GEN);
    const ble_link_operation_identity_t first = s_provisional_identity;

    TEST_ASSERT_EQUAL(1U, s_provisional_discard_count);
    TEST_ASSERT_TRUE(first.token != 0U);
    /* auth_txn is now empty, so this separate terminal path generates another
     * token. The retained slot must still keep the first identity and
     * cooldown. */
    ble_link_service_abort_transactions();
    TEST_ASSERT_EQUAL(1U, s_provisional_discard_count);
    TEST_ASSERT_TRUE(ble_link_service_retained_cleanup_pending());

    s_provisional_result = ESP_OK;
    host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(2U, s_provisional_discard_count);
    TEST_ASSERT_TRUE(ble_link_operation_identity_equal(
                         &first, &s_provisional_identity));
    TEST_ASSERT_TRUE(!ble_link_service_retained_cleanup_pending());
}

static void test_failed_commit_retires_transaction(void)
{
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;
    uint8_t commit[64];
    const uint8_t filler = 1U;

    device_link_security_deinit();
    nv_storage_fake_reset();
    _reset();
    _enable_provisional_security_ops();
    s_pending_captured = false;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    const size_t commit_len = _build_commit_body(
                                  commit, sizeof(commit), 5U);

    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    const uint64_t token = ble_link_service_confirmation_token();

    TEST_ASSERT_TRUE(token != 0U);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_confirm_binding(token, true));

    /* Exhaust the fake NVS slots so the real authorization save fails
     * before its durable boundary. */
    TEST_ASSERT_EQUAL(ESP_OK, nv_storage_set_blob(
                          "fill.1", &filler, sizeof(filler)));
    TEST_ASSERT_EQUAL(ESP_OK, nv_storage_set_blob(
                          "fill.2", &filler, sizeof(filler)));
    TEST_ASSERT_EQUAL(ESP_OK, nv_storage_set_blob(
                          "fill.3", &filler, sizeof(filler)));
    TEST_ASSERT_EQUAL(ESP_OK, nv_storage_set_blob(
                          "fill.4", &filler, sizeof(filler)));

    (void)_build_commit_body(commit, sizeof(commit), 6U);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_STORAGE, response.error);
    TEST_ASSERT_EQUAL(1U, s_provisional_discard_count);
    TEST_ASSERT_TRUE(!ble_link_service_pending_confirmation());
    TEST_ASSERT_EQUAL(0U, ble_link_service_confirmation_token());

    /* The explicit pre-durable failure is terminal. A fresh request id
     * cannot retry the retired transaction while bond cleanup is live:
     * the retired transaction is unknown, so the Commit is NOT_FOUND. */
    (void)_build_commit_body(commit, sizeof(commit), 7U);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len,
                          &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_NOT_FOUND, response.error);
    nv_storage_fake_reset();
}

static void test_remote_replacement_is_owner_serialized(void)
{
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;
    uint8_t commit[64];
    const ble_link_operation_identity_t replacement =
    {
        .generation = GEN,
        .security_epoch = 1U,
        .token = 900U,
        .kind = BLE_LINK_OPERATION_REMOTE_REPLACEMENT,
        .conn_handle = 7U,
    };

    _reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_service_register_remote_replacement(
                          &replacement));
    TEST_ASSERT_TRUE(!ble_link_service_retained_cleanup_pending());
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_link_service_retained_retry_remaining_ms());

    _reset();
    _enable_provisional_security_ops();
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    s_replacement_result = ESP_ERR_NO_MEM;
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_register_remote_replacement(
                          &replacement));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(1U, s_replacement_count);
    s_replacement_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(1U, s_replacement_count);
    host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(2U, s_replacement_count);
    TEST_ASSERT_TRUE(ble_link_operation_identity_equal(
                         &replacement, &s_replacement_identity));

    /* Once the first Commit probe entered, Commit wins the linearization
     * race and the host callback may not register replacement mutation. */
    _reset();
    _enable_provisional_security_ops();
    s_pending_captured = false;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    const size_t commit_len = _build_commit_body(
                                  commit, sizeof(commit), 5U);

    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    TEST_ASSERT_TRUE(ble_link_service_pending_confirmation());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_service_register_remote_replacement(
                          &replacement));
    TEST_ASSERT_EQUAL(0U, s_replacement_count);

    /* A durable Commit is terminal and no longer outranks a later pairing
     * retry. Replacement admission clears the retained terminal result so it
     * cannot be replayed while replacement cleanup is in progress. */
    nv_storage_fake_reset();
    _reset();
    _enable_provisional_security_ops();
    s_pending_captured = false;
    _feed_single_channel(s_prepare_request, sizeof(s_prepare_request),
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    (void)_build_commit_body(commit, sizeof(commit), 5U);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    const uint64_t token = ble_link_service_confirmation_token();

    TEST_ASSERT_TRUE(token != 0U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_confirm_binding(token, true));
    (void)_build_commit_body(commit, sizeof(commit), 6U);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len, &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, response.error);

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_register_remote_replacement(
                          &replacement));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(1U, s_replacement_count);

    /* Re-open the same fake ACL after the cutover. The exact terminal Commit
     * is no longer cached and cannot return the old success: the committed
     * transaction is retired, so the Commit is NOT_FOUND. */
    _establish_session();
    (void)_build_commit_body(commit, sizeof(commit), 7U);
    memset(s_capture, 0, sizeof(s_capture));
    s_capture_count = 0U;
    _feed_single_channel(commit, commit_len,
                         BLE_LINK_SERVICE_RX_SESSION);
    _reassemble_captured();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_envelope(
                          s_outbound, s_outbound_len, &envelope));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_codec_decode_response(
                          envelope.body_data, envelope.body_len, &response));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_NOT_FOUND, response.error);
    nv_storage_fake_reset();
}

static void test_handshaking_teardown_requires_current_identity(void)
{
    static const uint8_t cmd0[] = "cmd0";

    _reset();
    ble_link_service_reset();
    ble_link_service_init(BOOT_ID, _capture, NULL,
                          &s_handshake_security_ops, 32U);
    s_auto_confirm = false;
    s_security_close_count = 0U;
    TEST_ASSERT_EQUAL(ESP_OK,
                      _feed_handshake_single(cmd0, sizeof(cmd0) - 1U));
    const uint32_t epoch = ble_link_session_security2_epoch();
    const ble_link_operation_identity_t current =
    {
        .generation = GEN,
        .security_epoch = epoch,
        .kind = BLE_LINK_OPERATION_DISCONNECT,
        .conn_handle = s_facts.conn_handle,
    };
    ble_link_operation_identity_t stale = current;

    TEST_ASSERT_TRUE(epoch != 0U);
    /* The first Cmd0 retired the adapter's previous logical session. */
    TEST_ASSERT_EQUAL(1U, s_security_close_count);

    stale.generation++;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(
                          &stale));
    stale = current;
    stale.security_epoch--;
    stale.kind = BLE_LINK_OPERATION_ENCRYPT_CHANGE;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(
                          &stale));
    stale = current;
    stale.conn_handle++;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(
                          &stale));
    TEST_ASSERT_TRUE(ble_link_service_response_in_flight());
    TEST_ASSERT_EQUAL(1U, s_security_close_count);

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_clear_session_state_if_current(
                          &current));
    TEST_ASSERT_TRUE(!ble_link_service_response_in_flight());
    TEST_ASSERT_EQUAL(2U, s_security_close_count);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(
                          &current));
    TEST_ASSERT_EQUAL(2U, s_security_close_count);

    /* ACL-terminal teardown deliberately ignores an older Security 2 epoch:
     * a Cmd0 may advance it while DISCONNECT waits for the service mutex. */
    ble_link_service_init(BOOT_ID, _capture, NULL,
                          &s_handshake_security_ops, 32U);
    TEST_ASSERT_EQUAL(ESP_OK,
                      _feed_handshake_single(cmd0, sizeof(cmd0) - 1U));
    ble_link_operation_identity_t terminal =
    {
        .generation = GEN,
        .security_epoch = ble_link_session_security2_epoch() - 1U,
        .kind = BLE_LINK_OPERATION_DISCONNECT,
        .conn_handle = s_facts.conn_handle,
    };

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_clear_session_state_if_current(
                          &terminal));
    TEST_ASSERT_TRUE(!ble_link_service_response_in_flight());
}

int main(void)
{
    test_authorize_commit_wrong_credential();
    test_authorize_commit_malformed_record_fails_closed();
    test_authorize_commit_unknown_transaction_not_found();
    test_authorize_commit_truncated_rejected();
    test_get_authorization_recovery();
    test_real_security2_rehandshake_commit_replay_and_recovery();
    test_get_authorization_requires_recovery_flag();
    test_prepare_retires_previous_transaction();
    test_v2_snapshot_includes_operation_summaries();
    test_v2_get_authorization_without_recovery_malformed();
    test_authorize_expiry_clears_transaction();
    test_response_flow_identity_and_deferred_busy();
    test_stale_response_flow_is_ignored();
    test_handshake_queued_admission();
    test_cmd0_delayed_replacement();
    test_manifest_request();
    test_manifest_response_bytes();
    test_typed_tlv_manifest_request();
    test_typed_tlv_snapshot_has_fixed_link_state();
    test_operation_declared_empty_result_never_encoded();
    test_snapshot_request();
    test_snapshot_zero_baseline_returns_internal();
    test_authorize_flow();
    test_authorize_commit_rejects_private_peer_address();
    test_domain_call_unadvertised();
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
    test_channel_routing();
    test_boot_mismatch_closes_session();
    test_one_transaction_at_a_time();
    test_authorize_prepare_produces_transaction();
    test_authorize_prepare_rejects_unregistered_permissions();
    test_session_channel_reassembly();
    test_low_mtu_multi_fragment();
    test_idle_timeout_clears_state();
    test_stale_ingress_epoch_timeout_is_ignored();
    test_completed_work_is_copied_and_deferred();
    test_terminal_clear_retires_queued_protected_work();
    test_terminal_clear_retires_queued_handshake();
    test_ingress_requires_immutable_acl_identity();
    test_retired_generation_allows_handle_reuse();
    test_provisional_bond_lifecycle();
    test_provisional_cleanup_is_retained_until_accepted();
    test_clear_without_connection_does_not_forge_cleanup_identity();
    test_provisional_cleanup_backoff_is_bounded();
    test_repeated_terminal_discard_coalesces_physical_target();
    test_failed_commit_retires_transaction();
    test_remote_replacement_is_owner_serialized();
    test_handshaking_teardown_requires_current_identity();

    return 0;
}
