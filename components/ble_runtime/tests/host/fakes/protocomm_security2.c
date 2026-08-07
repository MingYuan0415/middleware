#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "protocomm_security.h"
#include "protocomm_security2.h"

#define FAKE_SESSION (protocomm_security_handle_t)(uintptr_t)0x2000U
#define FAKE_TAG_BYTES 16U

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
    uint8_t *response = malloc((size_t)input_length + 2U);

    if (response == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(response, input, (size_t)input_length);
    response[input_length] = 0xaa;
    response[input_length + 1U] = 0xbb;
    *output = response;
    *output_length = input_length + 2;
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
