#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "protocomm_security.h"
#include "protocomm_security2.h"
#include "session.pb-c.h"

#define FAKE_SESSION (protocomm_security_handle_t)(uintptr_t)0x2000U
#define FAKE_TAG_BYTES 16U
#define FAKE_PUBLIC_KEY_BYTES 384U
#define FAKE_PROOF_BYTES 64U
#define FAKE_NONCE_BYTES 12U

/* In-memory copy of the SRP params for the last handshake. */
static char s_last_salt[64];
static uint16_t s_last_salt_len;
static char s_last_verifier[512];
static uint16_t s_last_verifier_len;
static uint32_t s_last_session_id;
static uint32_t s_handshake_count;
static uint32_t s_encrypt_count;
static uint32_t s_decrypt_count;
static bool s_fail_next_handshake;

void ble_link_sec_fake_reset(void)
{
    memset(s_last_salt, 0, sizeof(s_last_salt));
    s_last_salt_len = 0U;
    memset(s_last_verifier, 0, sizeof(s_last_verifier));
    s_last_verifier_len = 0U;
    s_last_session_id = 0U;
    s_handshake_count = 0U;
    s_encrypt_count = 0U;
    s_decrypt_count = 0U;
    s_fail_next_handshake = false;
}

void ble_link_sec_fake_fail_next_handshake(void)
{
    s_fail_next_handshake = true;
}

uint32_t ble_link_sec_fake_handshake_count(void)
{
    return s_handshake_count;
}

uint32_t ble_link_sec_fake_encrypt_count(void)
{
    return s_encrypt_count;
}

uint32_t ble_link_sec_fake_decrypt_count(void)
{
    return s_decrypt_count;
}

const char *ble_link_sec_fake_last_salt(size_t *len)
{
    *len = s_last_salt_len;
    return s_last_salt;
}

const char *ble_link_sec_fake_last_verifier(size_t *len)
{
    *len = s_last_verifier_len;
    return s_last_verifier;
}

static esp_err_t _sec2_init(protocomm_security_handle_t *handle)
{
    *handle = FAKE_SESSION;
    return ESP_OK;
}

static esp_err_t _sec2_cleanup(protocomm_security_handle_t handle)
{
    assert(handle == FAKE_SESSION);
    return ESP_OK;
}

static esp_err_t _sec2_new_session(
    protocomm_security_handle_t handle, uint32_t session_id)
{
    assert(handle == FAKE_SESSION);
    s_last_session_id = session_id;
    return ESP_OK;
}

static esp_err_t _sec2_close_session(
    protocomm_security_handle_t handle, uint32_t session_id)
{
    assert(handle == FAKE_SESSION);
    assert(session_id == s_last_session_id);
    return ESP_OK;
}

static esp_err_t _sec2_req_handler(
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
    s_last_salt_len = params->salt_len;
    memcpy(s_last_salt, params->salt, s_last_salt_len);
    s_last_verifier_len = params->verifier_len;
    memcpy(s_last_verifier, params->verifier, s_last_verifier_len);
    s_handshake_count++;
    if (s_fail_next_handshake)
    {
        s_fail_next_handshake = false;
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
    static uint8_t public_key[FAKE_PUBLIC_KEY_BYTES] =
    {0x44U, 0x50U, 0x4bU};
    static uint8_t salt[16U] = {0x53U, 0x41U, 0x4cU, 0x54U};
    static uint8_t proof[FAKE_PROOF_BYTES] =
    {0x50U, 0x52U, 0x4fU, 0x4fU, 0x46U};
    static uint8_t nonce[FAKE_NONCE_BYTES] =
    {0x4eU, 0x4fU, 0x4eU, 0x43U, 0x45U};
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

static esp_err_t _sec2_encrypt(
    protocomm_security_handle_t handle, uint32_t session_id,
    const uint8_t *input, ssize_t input_length,
    uint8_t **output, ssize_t *output_length)
{
    assert(handle == FAKE_SESSION);
    (void)session_id;
    s_encrypt_count++;
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

static esp_err_t _sec2_decrypt(
    protocomm_security_handle_t handle, uint32_t session_id,
    const uint8_t *input, ssize_t input_length,
    uint8_t **output, ssize_t *output_length)
{
    assert(handle == FAKE_SESSION);
    (void)session_id;
    s_decrypt_count++;
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
    .init = _sec2_init,
    .cleanup = _sec2_cleanup,
    .new_transport_session = _sec2_new_session,
    .close_transport_session = _sec2_close_session,
    .security_req_handler = _sec2_req_handler,
    .encrypt = _sec2_encrypt,
    .decrypt = _sec2_decrypt,
};
