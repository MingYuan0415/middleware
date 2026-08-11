#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_link_security.h"
#include "device_link_security_auth.h"
#include "nv_storage.h"
#include "protocomm_security.h"
#include "protocomm_security2.h"
#include "session.pb-c.h"

#define TEST_POP "window-pop-secret"
#define TEST_USERNAME "microtech"
#define TEST_AUTH_KEY "dls.auth"
#define TEST_REVOKE_KEY "dls.revoke"
#define TEST_SEC2_PUBLIC_KEY_BYTES 384U
#define TEST_SEC2_PROOF_BYTES 64U
#define TEST_SEC2_NONCE_BYTES 12U

/* Peer identity used by the committed record in the selection tests. */
static const uint8_t TEST_PEER_ADDR[DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES] =
{0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5};

static uint8_t s_cmd0_wire[512];
static size_t s_cmd0_wire_len;
static uint8_t s_cmd1_wire[128];
static size_t s_cmd1_wire_len;

static size_t _pack_handshake_request(
    device_link_security_handshake_stage_t stage,
    uint8_t *out, size_t capacity)
{
    static uint8_t username[] = TEST_USERNAME;
    static uint8_t public_key[TEST_SEC2_PUBLIC_KEY_BYTES] = {0x10, 0x20, 0x30, 0x40};
    static uint8_t proof[TEST_SEC2_PROOF_BYTES] = {0x50, 0x60, 0x70};
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

    assert(packed_size <= capacity);
    assert(session_data__pack(&session, out) == packed_size);
    return packed_size;
}

static void _prepare_handshake_wires(void)
{
    if (s_cmd0_wire_len == 0U)
    {
        s_cmd0_wire_len = _pack_handshake_request(
                              DEVICE_LINK_SECURITY_HANDSHAKE_CMD0,
                              s_cmd0_wire, sizeof(s_cmd0_wire));
        s_cmd1_wire_len = _pack_handshake_request(
                              DEVICE_LINK_SECURITY_HANDSHAKE_CMD1,
                              s_cmd1_wire, sizeof(s_cmd1_wire));
    }
}

static void _assert_handshake_response(
    const uint8_t *wire, size_t wire_len,
    device_link_security_handshake_stage_t stage)
{
    SessionData *session = session_data__unpack(NULL, wire_len, wire);

    assert(session != NULL);
    assert(session->sec_ver == SEC_SCHEME_VERSION__SecScheme2);
    assert(session->proto_case == SESSION_DATA__PROTO_SEC2);
    assert(session->sec2 != NULL);
    if (stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD0)
    {
        assert(session->sec2->msg ==
               SEC2_MSG_TYPE__S2Session_Response0);
        assert(session->sec2->payload_case ==
               SEC2_PAYLOAD__PAYLOAD_SR0);
        assert(session->sec2->sr0 != NULL);
        assert(session->sec2->sr0->status == STATUS__Success);
    }
    else
    {
        assert(session->sec2->msg ==
               SEC2_MSG_TYPE__S2Session_Response1);
        assert(session->sec2->payload_case ==
               SEC2_PAYLOAD__PAYLOAD_SR1);
        assert(session->sec2->sr1 != NULL);
        assert(session->sec2->sr1->status == STATUS__Success);
    }
    session_data__free_unpacked(session, NULL);
}

static void _select_bootstrap(void)
{
    assert(device_link_security_select_verifier(
               1U, TEST_PEER_ADDR, sizeof(TEST_PEER_ADDR), true) == ESP_OK);
    assert(device_link_security_selected_verifier() ==
           DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP);
}

static void _complete_handshake(void)
{
    uint8_t *out = NULL;
    size_t out_len = 0U;
    device_link_security_handshake_result_t result;

    assert(device_link_security_handshake_ex(
               s_cmd1_wire, s_cmd1_wire_len, &out, &out_len,
               &result) == ESP_OK);
    assert(result.stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD1);
    assert(result.authenticated);
    _assert_handshake_response(
        out, out_len, DEVICE_LINK_SECURITY_HANDSHAKE_CMD1);
    free(out);
}

#define FAKE_SESSION (protocomm_security_handle_t)(uintptr_t)0x2000U
#define FAKE_TAG_BYTES 16U

static unsigned s_new_count;
static unsigned s_delete_count;
static unsigned s_open_session_count;
static unsigned s_close_session_count;
static unsigned s_sec2_handle_count;
static unsigned s_app_handle_count;
static char s_captured_salt[64];
static size_t s_captured_salt_len;
static char s_captured_verifier[512];
static size_t s_captured_verifier_len;
static char s_last_sec2_request[512];
static size_t s_last_sec2_request_len;
static uint32_t s_last_session_id;
static bool s_fail_next_sec2;
static unsigned s_authenticated_count;
static bool s_authenticated_observed_state;
static esp_err_t s_authenticated_result;

static esp_err_t _fake_sec2_init(protocomm_security_handle_t *handle)
{
    s_new_count++;
    *handle = FAKE_SESSION;
    return ESP_OK;
}

static esp_err_t _fake_sec2_cleanup(protocomm_security_handle_t handle)
{
    assert(handle == FAKE_SESSION);
    s_delete_count++;
    return ESP_OK;
}

static esp_err_t _fake_sec2_new_session(
    protocomm_security_handle_t handle, uint32_t session_id)
{
    assert(handle == FAKE_SESSION);
    s_open_session_count++;
    s_last_session_id = session_id;
    return ESP_OK;
}

static esp_err_t _fake_sec2_close_session(
    protocomm_security_handle_t handle, uint32_t session_id)
{
    assert(handle == FAKE_SESSION);
    s_close_session_count++;
    assert(session_id == s_last_session_id);
    return ESP_OK;
}

static esp_err_t _fake_sec2_req_handler(
    protocomm_security_handle_t handle, const void *sec_params,
    uint32_t session_id, const uint8_t *input, ssize_t input_length,
    uint8_t **output, ssize_t *output_length, void *priv_data)
{
    assert(handle == FAKE_SESSION);
    assert(sec_params != NULL);
    (void)priv_data;
    assert(session_id == s_last_session_id);
    const protocomm_security2_params_t *params = sec_params;

    assert(params->salt != NULL && params->verifier != NULL);
    s_captured_salt_len = (size_t)params->salt_len;
    memcpy(s_captured_salt, params->salt, s_captured_salt_len);
    s_captured_verifier_len = (size_t)params->verifier_len;
    memcpy(s_captured_verifier, params->verifier, s_captured_verifier_len);
    s_sec2_handle_count++;
    s_last_sec2_request_len = (size_t)input_length;
    assert(s_last_sec2_request_len <= sizeof(s_last_sec2_request));
    memcpy(s_last_sec2_request, input, s_last_sec2_request_len);
    if (s_fail_next_sec2)
    {
        s_fail_next_sec2 = false;
        return ESP_ERR_INVALID_ARG;
    }
    SessionData *request = session_data__unpack(
                               NULL, (size_t)input_length, input);

    if (request == NULL ||
            request->sec_ver != SEC_SCHEME_VERSION__SecScheme2 ||
            request->proto_case != SESSION_DATA__PROTO_SEC2 ||
            request->sec2 == NULL)
    {
        session_data__free_unpacked(request, NULL);
        return ESP_ERR_INVALID_ARG;
    }
    static uint8_t public_key[TEST_SEC2_PUBLIC_KEY_BYTES] =
    {0x44, 0x50, 0x4b};
    static uint8_t salt[DEVICE_LINK_SECURITY_AUTH_SALT_BYTES] =
    {0x53, 0x41, 0x4c, 0x54};
    static uint8_t proof[TEST_SEC2_PROOF_BYTES] =
    {0x50, 0x52, 0x4f, 0x4f, 0x46};
    static uint8_t nonce[TEST_SEC2_NONCE_BYTES] =
    {0x4e, 0x4f, 0x4e, 0x43, 0x45};
    S2SessionResp0 resp0 = S2_SESSION_RESP0__INIT;
    S2SessionResp1 resp1 = S2_SESSION_RESP1__INIT;
    Sec2Payload payload = SEC2_PAYLOAD__INIT;
    SessionData session = SESSION_DATA__INIT;

    session.sec_ver = SEC_SCHEME_VERSION__SecScheme2;
    session.proto_case = SESSION_DATA__PROTO_SEC2;
    session.sec2 = &payload;
    if (request->sec2->msg == SEC2_MSG_TYPE__S2Session_Command0 &&
            request->sec2->payload_case == SEC2_PAYLOAD__PAYLOAD_SC0)
    {
        resp0.status = STATUS__Success;
        resp0.device_pubkey.data = public_key;
        resp0.device_pubkey.len = sizeof(public_key);
        resp0.device_salt.data = salt;
        resp0.device_salt.len = sizeof(salt);
        payload.msg = SEC2_MSG_TYPE__S2Session_Response0;
        payload.payload_case = SEC2_PAYLOAD__PAYLOAD_SR0;
        payload.sr0 = &resp0;
    }
    else if (request->sec2->msg == SEC2_MSG_TYPE__S2Session_Command1 &&
             request->sec2->payload_case == SEC2_PAYLOAD__PAYLOAD_SC1)
    {
        resp1.status = STATUS__Success;
        resp1.device_proof.data = proof;
        resp1.device_proof.len = sizeof(proof);
        resp1.device_nonce.data = nonce;
        resp1.device_nonce.len = sizeof(nonce);
        payload.msg = SEC2_MSG_TYPE__S2Session_Response1;
        payload.payload_case = SEC2_PAYLOAD__PAYLOAD_SR1;
        payload.sr1 = &resp1;
    }
    else
    {
        session_data__free_unpacked(request, NULL);
        return ESP_ERR_INVALID_ARG;
    }
    session_data__free_unpacked(request, NULL);
    const size_t packed_size = session_data__get_packed_size(&session);
    uint8_t *response = malloc(packed_size);

    if (response == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    assert(session_data__pack(&session, response) == packed_size);
    *output = response;
    *output_length = (ssize_t)packed_size;
    return ESP_OK;
}

static esp_err_t _fake_sec2_encrypt(
    protocomm_security_handle_t handle, uint32_t session_id,
    const uint8_t *input, ssize_t input_length,
    uint8_t **output, ssize_t *output_length)
{
    assert(handle == FAKE_SESSION);
    (void)session_id;
    uint8_t *cipher = malloc((size_t)input_length + FAKE_TAG_BYTES);

    if (cipher == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(cipher, input, (size_t)input_length);
    memset(cipher + input_length, 0x5a, FAKE_TAG_BYTES);
    *output = cipher;
    *output_length = input_length + (ssize_t)FAKE_TAG_BYTES;
    return ESP_OK;
}

static esp_err_t _fake_sec2_decrypt(
    protocomm_security_handle_t handle, uint32_t session_id,
    const uint8_t *input, ssize_t input_length,
    uint8_t **output, ssize_t *output_length)
{
    assert(handle == FAKE_SESSION);
    (void)session_id;
    if (input_length < (ssize_t)FAKE_TAG_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *plain = malloc((size_t)input_length - FAKE_TAG_BYTES);

    if (plain == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(plain, input, (size_t)input_length - FAKE_TAG_BYTES);
    *output = plain;
    *output_length = input_length - (ssize_t)FAKE_TAG_BYTES;
    return ESP_OK;
}

const protocomm_security_t protocomm_security2 =
{
    .ver = 2,
    .init = _fake_sec2_init,
    .cleanup = _fake_sec2_cleanup,
    .new_transport_session = _fake_sec2_new_session,
    .close_transport_session = _fake_sec2_close_session,
    .security_req_handler = _fake_sec2_req_handler,
    .encrypt = _fake_sec2_encrypt,
    .decrypt = _fake_sec2_decrypt,
};

esp_err_t esp_srp_gen_salt_verifier(
    const char *username, int username_len,
    const char *pass, int pass_len,
    char **bytes_salt, int salt_len,
    char **verifier, int *verifier_len)
{
    assert(username != NULL && pass != NULL);
    assert(username_len == (int)strlen(TEST_USERNAME));
    assert(strncmp(username, TEST_USERNAME, (size_t)username_len) == 0);
    if (salt_len <= 0 || pass_len <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char *salt = malloc((size_t)salt_len);
    /* 3072-bit group verifier: fixed 384 bytes, derived deterministically
     * from the password for prediction. */
    char *out = malloc(384U);

    if (salt == NULL || out == NULL)
    {
        free(salt);
        free(out);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < salt_len; ++i)
    {
        salt[i] = (char)(0x40 + i);
    }
    for (int i = 0; i < 384; ++i)
    {
        out[i] = (char)(pass[i % pass_len] + (i % 7));
    }
    *bytes_salt = salt;
    *verifier = out;
    *verifier_len = 384;
    return ESP_OK;
}


static void _reset_fakes(void)
{
    _prepare_handshake_wires();
    nv_storage_fake_reset();
    s_new_count = 0U;
    s_delete_count = 0U;
    s_open_session_count = 0U;
    s_close_session_count = 0U;
    s_sec2_handle_count = 0U;
    s_app_handle_count = 0U;
    memset(s_captured_salt, 0, sizeof(s_captured_salt));
    s_captured_salt_len = 0U;
    memset(s_captured_verifier, 0, sizeof(s_captured_verifier));
    s_captured_verifier_len = 0U;
    memset(s_last_sec2_request, 0, sizeof(s_last_sec2_request));
    s_last_sec2_request_len = 0U;
    s_last_session_id = 0U;
    s_fail_next_sec2 = false;
    s_authenticated_count = 0U;
    s_authenticated_observed_state = false;
    s_authenticated_result = ESP_OK;
}

static esp_err_t _echo_request(
    const uint8_t *request, size_t request_len,
    uint8_t **response, size_t *response_len, void *arg)
{
    (void)arg;
    assert(request != NULL && request_len > 0U);
    uint8_t *out = malloc(request_len + 1U);

    if (out == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(out, request, request_len);
    out[request_len] = 0x7f;
    s_app_handle_count++;
    *response = out;
    *response_len = request_len + 1U;
    return ESP_OK;
}

static esp_err_t _authenticated(void *arg)
{
    assert(arg == &s_authenticated_count);
    s_authenticated_count++;
    s_authenticated_observed_state =
        device_link_security_is_authenticated();
    return s_authenticated_result;
}

static const device_link_security_config_t s_lifecycle_config =
{
    .username = TEST_USERNAME,
    .session_id = 7U,
    .request_cb = _echo_request,
    .request_arg = NULL,
};

static void _test_init_validation(void)
{
    _reset_fakes();
    assert(device_link_security_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(device_link_security_init(
               &(device_link_security_config_t)
    {
        0
    }) == ESP_ERR_INVALID_ARG);
    const device_link_security_config_t config =
    {
        .username = TEST_USERNAME,
        .session_id = 7U,
        .request_cb = _echo_request,
        .request_arg = NULL,
    };
    assert(device_link_security_init(&config) == ESP_OK);
    assert(!device_link_security_is_authenticated());
    device_link_security_deinit();
}

static void _test_real_handshake_wire_validation(void)
{
    _prepare_handshake_wires();
    device_link_security_handshake_stage_t stage;

    assert(s_cmd0_wire_len > 4U);
    assert(s_cmd1_wire_len > 4U);
    assert(device_link_security_classify_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &stage) == ESP_OK);
    assert(stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD0);
    assert(device_link_security_classify_handshake(
               s_cmd1_wire, s_cmd1_wire_len, &stage) == ESP_OK);
    assert(stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD1);

    S2SessionCmd0 cmd0 = S2_SESSION_CMD0__INIT;
    Sec2Payload payload = SEC2_PAYLOAD__INIT;
    SessionData session = SESSION_DATA__INIT;
    uint8_t wire[64];

    payload.msg = SEC2_MSG_TYPE__S2Session_Command0;
    payload.payload_case = SEC2_PAYLOAD__PAYLOAD_SC0;
    payload.sc0 = &cmd0;
    session.sec_ver = SEC_SCHEME_VERSION__SecScheme1;
    session.proto_case = SESSION_DATA__PROTO_SEC2;
    session.sec2 = &payload;
    size_t wire_len = session_data__pack(&session, wire);

    assert(device_link_security_classify_handshake(
               wire, wire_len, &stage) == ESP_ERR_INVALID_ARG);

    session.sec_ver = SEC_SCHEME_VERSION__SecScheme2;
    session.proto_case = SESSION_DATA__PROTO__NOT_SET;
    session.sec2 = NULL;
    wire_len = session_data__pack(&session, wire);
    assert(device_link_security_classify_handshake(
               wire, wire_len, &stage) == ESP_ERR_INVALID_ARG);

    session.proto_case = SESSION_DATA__PROTO_SEC2;
    session.sec2 = &payload;
    payload.msg = SEC2_MSG_TYPE__S2Session_Command1;
    wire_len = session_data__pack(&session, wire);
    assert(device_link_security_classify_handshake(
               wire, wire_len, &stage) == ESP_ERR_INVALID_ARG);
    assert(device_link_security_classify_handshake(
               (const uint8_t *)"not-protobuf", 12U,
               &stage) == ESP_ERR_INVALID_ARG);
}

static void _test_bootstrap_lifecycle(void)
{
    _reset_fakes();
    const device_link_security_config_t config =
    {
        .username = TEST_USERNAME,
        .session_id = 7U,
        .request_cb = _echo_request,
        .request_arg = NULL,
    };
    assert(device_link_security_init(&config) == ESP_OK);

    /* No verifier: handshake is not admitted. */
    uint8_t *out = NULL;
    size_t out_len = 0U;

    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) ==
           ESP_ERR_INVALID_STATE);

    /* Open the window: the adapter keeps the POP-derived salt and
     * verifier in the bootstrap slot; the instance is only configured
     * once the verifier is selected for a handshake. */
    assert(device_link_security_open_bootstrap(
               (const uint8_t *)TEST_POP, strlen(TEST_POP)) == ESP_OK);
    assert(s_new_count == 0U);
    assert(device_link_security_selected_verifier() ==
           DEVICE_LINK_SECURITY_VERIFIER_NONE);

    /* Select the bootstrap verifier for an unknown peer inside the
     * window: the protocomm instance is rebuilt with the POP-derived
     * salt and verifier. */
    _select_bootstrap();
    assert(s_new_count == 1U);

    /* Handshake opens the session, passes the POP-derived salt and
     * verifier to the SRP handler, and routes to the security endpoint. */
    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) == ESP_OK);
    assert(s_open_session_count == 1U);
    assert(s_last_session_id == 7U);
    assert(s_sec2_handle_count == 1U);
    assert(s_captured_salt_len == 16U);
    assert(s_captured_salt[0] == 0x40);
    assert(s_captured_verifier_len == 384U);
    assert(s_captured_verifier[0] == (char)(0x77U)); /* 'w' of the pop */
    assert(s_last_sec2_request_len == s_cmd0_wire_len);
    assert(memcmp(s_last_sec2_request, s_cmd0_wire,
                  s_cmd0_wire_len) == 0);
    _assert_handshake_response(
        out, out_len, DEVICE_LINK_SECURITY_HANDSHAKE_CMD0);
    free(out);

    /* Cmd0 establishes only the handshaking session. */
    assert(!device_link_security_is_authenticated());

    /* A valid Cmd1 proof and Resp1 authenticate immediately. */
    _complete_handshake();
    assert(device_link_security_is_authenticated());

    /* A protected frame routes through the application callback. */
    assert(device_link_security_unprotect(
               (const uint8_t *)"ciphertext-payload-22b", 22U, &out, &out_len) == ESP_OK);
    assert(s_app_handle_count == 1U);
    assert(out_len == 23U);
    /* The plaintext echo ends at byte 6 and the 16-byte fake tag follows. */
    assert(out[6] == 0x7f);
    assert(out[22] == 0x5a);
    free(out);

    /* Closing the window removes the verifier and tears the protocomm
     * instance down; the session is no longer usable. */
    device_link_security_close_bootstrap();
    assert(s_delete_count == 1U);
    assert(device_link_security_is_authenticated() == false);
    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) ==
           ESP_ERR_INVALID_STATE);
    assert(device_link_security_unprotect(
               (const uint8_t *)"cipher", 6U, &out, &out_len) ==
           ESP_ERR_INVALID_ARG);
    device_link_security_deinit();
    /* The close_bootstrap teardown already deleted the instance. */
    assert(s_delete_count == 1U);
    /* After deinit no bootstrap can open; re-init is idempotent and the
     * adapter is usable again. */
    assert(device_link_security_open_bootstrap(
               (const uint8_t *)"pop", 3U) == ESP_ERR_INVALID_STATE);
    assert(device_link_security_init(&s_lifecycle_config) == ESP_OK);
    assert(device_link_security_open_bootstrap(
               (const uint8_t *)"pop", 3U) == ESP_OK);
    assert(device_link_security_is_authenticated() == false);
    device_link_security_deinit();
    /* No instance was configured (no selection), so no teardown is added. */
    assert(s_delete_count == 1U);
    device_link_security_deinit();
    assert(device_link_security_is_authenticated() == false);
    assert(s_delete_count == 1U);
}

static void _test_failed_handshake_closes_session(void)
{
    _reset_fakes();
    const device_link_security_config_t config =
    {
        .username = TEST_USERNAME,
        .session_id = 3U,
        .request_cb = _echo_request,
        .request_arg = NULL,
    };
    assert(device_link_security_init(&config) == ESP_OK);
    assert(device_link_security_open_bootstrap(
               (const uint8_t *)TEST_POP, strlen(TEST_POP)) == ESP_OK);
    _select_bootstrap();
    uint8_t *out = NULL;
    size_t out_len = 0U;

    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) == ESP_OK);
    free(out);
    s_fail_next_sec2 = true;
    assert(device_link_security_handshake(
               s_cmd1_wire, s_cmd1_wire_len, &out, &out_len) ==
           ESP_ERR_INVALID_ARG);
    assert(s_close_session_count == 1U);
    assert(!device_link_security_is_authenticated());
    assert(device_link_security_unprotect(
               (const uint8_t *)"ciphertext-payload-22b", 22U, &out, &out_len) ==
           ESP_ERR_INVALID_STATE);
    device_link_security_deinit();
}

static void _test_cmd1_authentication_transition(void)
{
    _reset_fakes();
    const device_link_security_config_t config =
    {
        .username = TEST_USERNAME,
        .session_id = 5U,
        .request_cb = _echo_request,
        .request_arg = NULL,
        .authenticated_cb = _authenticated,
        .authenticated_arg = &s_authenticated_count,
    };

    assert(device_link_security_init(&config) == ESP_OK);
    assert(device_link_security_open_bootstrap(
               (const uint8_t *)TEST_POP, strlen(TEST_POP)) == ESP_OK);
    _select_bootstrap();
    uint8_t *out = NULL;
    size_t out_len = 0U;
    device_link_security_handshake_result_t result;

    assert(device_link_security_handshake_ex(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len,
               &result) == ESP_OK);
    assert(result.stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD0);
    assert(!result.authenticated);
    assert(s_authenticated_count == 0U);
    free(out);

    assert(device_link_security_handshake_ex(
               s_cmd1_wire, s_cmd1_wire_len, &out, &out_len,
               &result) == ESP_OK);
    assert(result.stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD1);
    assert(result.authenticated);
    assert(s_authenticated_count == 1U);
    assert(s_authenticated_observed_state);
    free(out);

    /* Protected traffic consumes the established session without causing
     * another authentication transition. */
    assert(device_link_security_unprotect(
               (const uint8_t *)"ciphertext-payload-22b", 22U,
               &out, &out_len) == ESP_OK);
    assert(s_authenticated_count == 1U);
    free(out);

    /* A fresh Cmd0 replaces the authenticated epoch. If the Cmd1 transition
     * callback then fails, the response is discarded and the new session is
     * closed before protected traffic can be admitted. */
    assert(device_link_security_handshake_ex(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len,
               &result) == ESP_OK);
    assert(!result.authenticated);
    free(out);
    s_authenticated_result = ESP_FAIL;
    out = (uint8_t *)(uintptr_t)1U;
    out_len = 1U;
    assert(device_link_security_handshake_ex(
               s_cmd1_wire, s_cmd1_wire_len, &out, &out_len,
               &result) == ESP_FAIL);
    assert(out == NULL);
    assert(out_len == 0U);
    assert(!result.authenticated);
    assert(s_authenticated_count == 2U);
    assert(!device_link_security_session_open());
    assert(!device_link_security_is_authenticated());
    device_link_security_deinit();
}

static void _test_explicit_session_close(void)
{
    _reset_fakes();
    const device_link_security_config_t config =
    {
        .username = TEST_USERNAME,
        .session_id = 9U,
        .request_cb = _echo_request,
        .request_arg = NULL,
    };
    assert(device_link_security_init(&config) == ESP_OK);
    assert(device_link_security_open_bootstrap(
               (const uint8_t *)TEST_POP, strlen(TEST_POP)) == ESP_OK);
    _select_bootstrap();
    uint8_t *out = NULL;
    size_t out_len = 0U;

    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) == ESP_OK);
    free(out);
    _complete_handshake();
    assert(device_link_security_unprotect(
               (const uint8_t *)"ciphertext-payload-22b", 22U, &out, &out_len) == ESP_OK);
    free(out);
    device_link_security_close_session();
    assert(s_close_session_count == 1U);
    assert(!device_link_security_is_authenticated());
    assert(device_link_security_unprotect(
               (const uint8_t *)"ciphertext-payload-22b", 22U, &out, &out_len) ==
           ESP_ERR_INVALID_STATE);
    /* A ciphertext of 16 bytes or fewer is malformed even with a session. */
    assert(device_link_security_unprotect(
               (const uint8_t *)"short", 5U, &out, &out_len) ==
           ESP_ERR_INVALID_ARG);
    device_link_security_deinit();
}

static void _test_auth_record_persistence(void)
{
    nv_storage_fake_reset();
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    /* Invalid (zeroed) record is rejected. */
    assert(device_link_security_save_auth_record(&record) ==
           ESP_ERR_INVALID_ARG);
    assert(device_link_security_auth_record_valid(&record) == false);
    assert(device_link_security_load_auth_record(&record) ==
           ESP_ERR_NOT_FOUND);
    /* Commit a valid record. */
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
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
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES; ++i)
    {
        record.peer_addr[i] = (uint8_t)(0xa0U + i);
    }
    assert(device_link_security_auth_record_valid(&record) == true);
    assert(device_link_security_save_auth_record(&record) == ESP_OK);
    device_link_security_auth_record_t loaded;

    memset(&loaded, 0, sizeof(loaded));
    assert(device_link_security_load_auth_record(&loaded) == ESP_OK);
    assert(memcmp(&loaded, &record, sizeof(record)) == 0);
    /* Erase clears the record. */
    assert(device_link_security_erase_auth_record() == ESP_OK);
    assert(device_link_security_load_auth_record(&loaded) ==
           ESP_ERR_NOT_FOUND);
    /* Erasing again is a not-found. */
    assert(device_link_security_erase_auth_record() ==
           ESP_ERR_NOT_FOUND);
}

static void _test_long_term_verifier(void)
{
    nv_storage_fake_reset();
    assert(device_link_security_init(&s_lifecycle_config) == ESP_OK);
    /* No record: long-term load fails closed. */
    assert(device_link_security_load_long_term_verifier() ==
           ESP_ERR_NOT_FOUND);
    assert(device_link_security_is_authenticated() == false);
    /* Derive from an application password and commit a record. */
    static const uint8_t password[24] =
    {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10,
        0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90,
    };
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    assert(device_link_security_derive_long_term_verifier(
               password, sizeof(password),
               record.salt, record.verifier) == ESP_OK);
    assert(nv_storage_fake_blob_len() == 0U);
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES; ++i)
    {
        record.credential_id[i] = (uint8_t)(i + 1U);
        record.device_auth_id[i] = (uint8_t)(0x60U + i);
    }
    record.peer_addr_type = 1U;
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES; ++i)
    {
        record.peer_addr[i] = (uint8_t)(0xa0U + i);
    }
    assert(device_link_security_save_auth_record(&record) == ESP_OK);
    /* Loading the long-term verifier makes the session handshakable and
     * no bootstrap verifier is required. */
    assert(device_link_security_load_long_term_verifier() == ESP_OK);
    uint8_t *out = NULL;
    size_t out_len = 0U;

    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) == ESP_OK);
    free(out);
    assert(device_link_security_is_authenticated() == false);
    assert(device_link_security_erase_auth_record() == ESP_OK);
    assert(device_link_security_load_long_term_verifier() ==
           ESP_ERR_NOT_FOUND);
    assert(device_link_security_is_authenticated() == false);
    device_link_security_deinit();
}

static void _test_verifier_selection(void)
{
    nv_storage_fake_reset();
    assert(device_link_security_init(&s_lifecycle_config) == ESP_OK);
    uint8_t *out = NULL;
    size_t out_len = 0U;

    /* No record, no window: no verifier; the handshake is not admitted. */
    assert(device_link_security_select_verifier(
               1U, TEST_PEER_ADDR, sizeof(TEST_PEER_ADDR), false) == ESP_OK);
    assert(device_link_security_selected_verifier() ==
           DEVICE_LINK_SECURITY_VERIFIER_NONE);
    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) ==
           ESP_ERR_INVALID_STATE);

    /* Open a window: an unknown peer inside the window selects the
     * bootstrap verifier. */
    assert(device_link_security_open_bootstrap(
               (const uint8_t *)TEST_POP, strlen(TEST_POP)) == ESP_OK);
    _select_bootstrap();
    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) == ESP_OK);
    free(out);
    device_link_security_close_session();

    /* Commit a record for TEST_PEER_ADDR: the matching peer keeps the
     * long-term verifier even while the window stays open (replacement
     * window semantics). */
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    static const uint8_t password[24] =
    {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10,
        0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90,
    };
    assert(device_link_security_derive_long_term_verifier(
               password, sizeof(password),
               record.salt, record.verifier) == ESP_OK);
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES; ++i)
    {
        record.credential_id[i] = (uint8_t)(i + 1U);
        record.device_auth_id[i] = (uint8_t)(0x60U + i);
    }
    record.peer_addr_type = 1U;
    memcpy(record.peer_addr, TEST_PEER_ADDR, sizeof(TEST_PEER_ADDR));
    assert(device_link_security_save_auth_record(&record) == ESP_OK);
    /* Load the long-term slot so the record material backs the instance. */
    assert(device_link_security_load_long_term_verifier() == ESP_OK);

    /* Bound peer inside the open window: LONG_TERM. */
    assert(device_link_security_select_verifier(
               1U, TEST_PEER_ADDR, sizeof(TEST_PEER_ADDR), true) == ESP_OK);
    assert(device_link_security_selected_verifier() ==
           DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM);
    /* The long-term record salt/verifier back the instance. */
    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) == ESP_OK);
    assert(s_captured_salt_len == DEVICE_LINK_SECURITY_AUTH_SALT_BYTES);
    assert(memcmp(s_captured_salt, record.salt,
                  DEVICE_LINK_SECURITY_AUTH_SALT_BYTES) == 0);
    free(out);
    device_link_security_close_session();

    /* A different peer inside the open window selects the bootstrap
     * verifier (replacement flow for the new peer). */
    static const uint8_t other_peer[6] = {0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5};

    assert(device_link_security_select_verifier(
               1U, other_peer, sizeof(other_peer), true) == ESP_OK);
    assert(device_link_security_selected_verifier() ==
           DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP);
    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) == ESP_OK);
    free(out);
    device_link_security_close_session();

    /* Window closed, matching record still present: LONG_TERM survives. */
    assert(device_link_security_select_verifier(
               1U, TEST_PEER_ADDR, sizeof(TEST_PEER_ADDR), false) == ESP_OK);
    assert(device_link_security_selected_verifier() ==
           DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM);
    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) == ESP_OK);
    free(out);
    device_link_security_close_session();

    /* Window closed, no matching record: not admitted. */
    assert(device_link_security_select_verifier(
               1U, other_peer, sizeof(other_peer), false) == ESP_OK);
    assert(device_link_security_selected_verifier() ==
           DEVICE_LINK_SECURITY_VERIFIER_NONE);
    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) ==
           ESP_ERR_INVALID_STATE);

    /* Invalid selection arguments fail closed: type 4 is out of range,
     * type 3 (random identity) is a legal identity. */
    assert(device_link_security_select_verifier(
               4U, TEST_PEER_ADDR, sizeof(TEST_PEER_ADDR), true) ==
           ESP_ERR_INVALID_ARG);

    /* A pending revoke journal rejects every selection, even with a
     * matching record and an open window. */
    assert(device_link_security_begin_revoke() == ESP_OK);
    assert(device_link_security_select_verifier(
               1U, TEST_PEER_ADDR, sizeof(TEST_PEER_ADDR), true) ==
           ESP_ERR_INVALID_STATE);
    assert(device_link_security_end_revoke() == ESP_OK);

    /* A journal query failure (storage read error) also fails closed:
     * with the revoke state unknown, no verifier may be selected. */
    nv_storage_fake_fail_next_get(ESP_FAIL);
    assert(device_link_security_select_verifier(
               1U, TEST_PEER_ADDR, sizeof(TEST_PEER_ADDR), true) ==
           ESP_ERR_INVALID_STATE);
    assert(device_link_security_select_verifier(
               1U, NULL, 0U, true) == ESP_ERR_INVALID_ARG);
    device_link_security_deinit();
}

static void _test_protect_requires_authentication(void)
{
    _reset_fakes();
    assert(device_link_security_init(&s_lifecycle_config) == ESP_OK);
    uint8_t *cipher = NULL;
    size_t cipher_len = 0U;

    /* No verifier and no session: protect fails closed. */
    assert(device_link_security_protect(
               (const uint8_t *)"plain", 5U, &cipher, &cipher_len) ==
           ESP_ERR_INVALID_STATE);
    assert(device_link_security_open_bootstrap(
               (const uint8_t *)TEST_POP, strlen(TEST_POP)) == ESP_OK);
    _select_bootstrap();
    uint8_t *out = NULL;
    size_t out_len = 0U;

    assert(device_link_security_handshake(
               s_cmd0_wire, s_cmd0_wire_len, &out, &out_len) == ESP_OK);
    free(out);
    /* Cmd0 alone is not enough: still pending. */
    assert(device_link_security_protect(
               (const uint8_t *)"plain", 5U, &cipher, &cipher_len) ==
           ESP_ERR_INVALID_STATE);
    assert(device_link_security_unprotect(
               (const uint8_t *)"ciphertext-payload-22b", 22U,
               &out, &out_len) == ESP_ERR_INVALID_STATE);
    _complete_handshake();
    /* Authenticated: protect produces ciphertext with a tag. */
    assert(device_link_security_protect(
               (const uint8_t *)"plain", 5U, &cipher, &cipher_len) == ESP_OK);
    assert(cipher_len == 21U);
    assert(memcmp(cipher, "plain", 5U) == 0);
    assert(cipher[20] == 0x5a);
    free(cipher);
    device_link_security_deinit();
}

static void _test_revoke_journal(void)
{
    nv_storage_fake_reset();
    assert(device_link_security_init(&s_lifecycle_config) == ESP_OK);
    bool pending = true;

    /* No marker initially. */
    assert(device_link_security_revoke_pending(&pending) == ESP_OK);
    assert(!pending);
    /* Begin journals the intent; the committed record coexists until the
     * revoke completes (multi-key storage). */
    assert(device_link_security_begin_revoke() == ESP_OK);
    assert(device_link_security_revoke_pending(&pending) == ESP_OK);
    assert(pending);
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES; ++i)
    {
        record.credential_id[i] = (uint8_t)(i + 1U);
        record.device_auth_id[i] = (uint8_t)(0x60U + i);
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
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES; ++i)
    {
        record.peer_addr[i] = (uint8_t)(0xa0U + i);
    }
    assert(device_link_security_save_auth_record(&record) == ESP_OK);
    assert(device_link_security_revoke_pending(&pending) == ESP_OK);
    assert(pending);
    assert(device_link_security_load_auth_record(&record) == ESP_OK);

    /* End clears the marker without touching the record. */
    assert(device_link_security_end_revoke() == ESP_OK);
    assert(device_link_security_revoke_pending(&pending) == ESP_OK);
    assert(!pending);
    assert(device_link_security_load_auth_record(&record) == ESP_OK);
    /* Erasing a missing marker is a no-op success. */
    assert(device_link_security_end_revoke() == ESP_OK);
    assert(device_link_security_erase_auth_record() == ESP_OK);
    assert(device_link_security_end_revoke() == ESP_OK);
    device_link_security_deinit();
}

static void _test_revoke_journal_fail_closed(void)
{
    nv_storage_fake_reset();
    assert(device_link_security_init(&s_lifecycle_config) == ESP_OK);
    bool pending = true;

    /* A failed first commit never creates a durable revoke obligation. The
     * caller must not mutate authorization state when begin_revoke fails. */
    nv_storage_fake_fail_next_commit(ESP_FAIL);
    assert(device_link_security_begin_revoke() == ESP_FAIL);
    nv_storage_fake_power_cycle();
    assert(device_link_security_revoke_pending(&pending) == ESP_OK);
    assert(!pending);

    nv_storage_fake_fail_next_get(ESP_FAIL);
    assert(device_link_security_revoke_pending(&pending) == ESP_FAIL);
    assert(!pending);

    const uint8_t malformed_marker = 0x02U;

    assert(nv_storage_set_blob(TEST_REVOKE_KEY, &malformed_marker,
                               sizeof(malformed_marker)) == ESP_OK);
    assert(device_link_security_revoke_pending(&pending) ==
           ESP_ERR_INVALID_STATE);
    assert(!pending);
    assert(device_link_security_end_revoke() == ESP_OK);

    assert(device_link_security_begin_revoke() == ESP_OK);
    nv_storage_fake_fail_next_erase(ESP_FAIL);
    assert(device_link_security_end_revoke() == ESP_FAIL);
    assert(device_link_security_revoke_pending(&pending) == ESP_OK);
    assert(pending);
    /* A failed erase commit leaves the durable marker intact across a crash,
     * even though the current NVS handle can observe its staged removal. */
    nv_storage_fake_fail_next_commit(ESP_FAIL);
    assert(device_link_security_end_revoke() == ESP_FAIL);
    nv_storage_fake_power_cycle();
    assert(device_link_security_revoke_pending(&pending) == ESP_OK);
    assert(pending);
    nv_storage_fake_fail_next_set(ESP_FAIL);
    assert(device_link_security_begin_revoke() == ESP_FAIL);
    assert(device_link_security_revoke_pending(&pending) == ESP_OK);
    assert(pending);
    assert(device_link_security_end_revoke() == ESP_OK);
    device_link_security_deinit();
}

static void _test_nv_commit_boundary(void)
{
    /* The durable boundary: set_blob stages, the commit inside the same
     * call publishes, a failed commit leaves the staged value readable
     * (NVS handle semantics) but a power cycle restores the previous
     * committed value. */
    uint8_t value[32];
    uint8_t out[32];
    size_t size = sizeof(out);

    nv_storage_fake_reset();
    /* First write succeeds and is durable. */
    memset(value, 0x11, sizeof(value));
    assert(nv_storage_set_blob(TEST_AUTH_KEY, value, sizeof(value)) == ESP_OK);
    size = sizeof(out);
    memset(out, 0, sizeof(out));
    assert(nv_storage_get_blob(TEST_AUTH_KEY, out, &size) == ESP_OK);
    assert(size == sizeof(value) && out[0] == 0x11U);

    /* Overwrite with a failed commit: the staged value is readable... */
    memset(value, 0x22, sizeof(value));
    nv_storage_fake_fail_next_commit(ESP_FAIL);
    assert(nv_storage_set_blob(TEST_AUTH_KEY, value, sizeof(value)) ==
           ESP_FAIL);
    size = sizeof(out);
    memset(out, 0, sizeof(out));
    assert(nv_storage_get_blob(TEST_AUTH_KEY, out, &size) == ESP_OK);
    assert(out[0] == 0x22U);
    assert(nv_storage_fake_commit_pending());
    /* ...but a power cycle restores the previous committed value. */
    nv_storage_fake_power_cycle();
    assert(!nv_storage_fake_commit_pending());
    size = sizeof(out);
    memset(out, 0, sizeof(out));
    assert(nv_storage_get_blob(TEST_AUTH_KEY, out, &size) == ESP_OK);
    assert(out[0] == 0x11U);

    /* A failed commit of an erase stages the removal: reads see the erase
     * (NVS handle semantics), and a power cycle restores the committed
     * value. */
    nv_storage_fake_fail_next_commit(ESP_FAIL);
    assert(nv_storage_erase_key(TEST_AUTH_KEY) == ESP_FAIL);
    size = sizeof(out);
    memset(out, 0, sizeof(out));
    assert(nv_storage_get_blob(TEST_AUTH_KEY, out, &size) ==
           ESP_ERR_NVS_NOT_FOUND);
    nv_storage_fake_power_cycle();
    size = sizeof(out);
    memset(out, 0, sizeof(out));
    assert(nv_storage_get_blob(TEST_AUTH_KEY, out, &size) == ESP_OK);
    assert(out[0] == 0x11U);

    /* A committed erase is durable. */
    assert(nv_storage_erase_key(TEST_AUTH_KEY) == ESP_OK);
    size = sizeof(out);
    assert(nv_storage_get_blob(TEST_AUTH_KEY, out, &size) ==
           ESP_ERR_NVS_NOT_FOUND);

    /* A staged write of a NEW key vanishes on power cycle. */
    memset(value, 0x33, sizeof(value));
    nv_storage_fake_fail_next_commit(ESP_FAIL);
    assert(nv_storage_set_blob(TEST_REVOKE_KEY, value, sizeof(value)) ==
           ESP_FAIL);
    assert(nv_storage_fake_commit_pending());
    nv_storage_fake_power_cycle();
    assert(!nv_storage_fake_commit_pending());
    size = sizeof(out);
    assert(nv_storage_get_blob(TEST_REVOKE_KEY, out, &size) ==
           ESP_ERR_NVS_NOT_FOUND);
}

int main(void)
{
    _test_init_validation();
    _test_real_handshake_wire_validation();
    _test_auth_record_persistence();
    _test_long_term_verifier();
    _test_verifier_selection();
    _test_revoke_journal();
    _test_revoke_journal_fail_closed();
    _test_nv_commit_boundary();
    _test_protect_requires_authentication();
    _test_bootstrap_lifecycle();
    _test_failed_handshake_closes_session();
    _test_cmd1_authentication_transition();
    _test_explicit_session_close();
    puts("device_link_security host tests passed");
    return 0;
}
