#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_link_security.h"
#include "protocomm.h"
#include "protocomm_security2.h"

#define TEST_POP "window-pop-secret"
#define TEST_USERNAME "microtech"

const protocomm_security_t protocomm_security2 =
{
    .ver = 2,
};

static unsigned s_new_count;
static unsigned s_delete_count;
static unsigned s_open_session_count;
static unsigned s_close_session_count;
static unsigned s_sec2_handle_count;
static unsigned s_app_handle_count;
static char s_captured_salt[64];
static size_t s_captured_salt_len;
static char s_captured_verifier[256];
static size_t s_captured_verifier_len;
static char s_last_sec2_request[256];
static size_t s_last_sec2_request_len;
static uint32_t s_last_session_id;
static const char *s_last_app_endpoint;
static protocomm_req_handler_t s_app_handler;
static void *s_app_handler_arg;
static bool s_fail_next_sec2;

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
    char *out = malloc((size_t)pass_len + 32U);

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
    (void)snprintf(out, (size_t)pass_len + 32U, "verifier:%s", pass);
    *bytes_salt = salt;
    *verifier = out;
    *verifier_len = (int)strlen(out);
    return ESP_OK;
}

protocomm_t *protocomm_new(void)
{
    s_new_count++;
    return (protocomm_t *)(uintptr_t)0x1000U;
}

void protocomm_delete(protocomm_t *protocomm)
{
    assert(protocomm == (protocomm_t *)(uintptr_t)0x1000U);
    s_delete_count++;
}

esp_err_t protocomm_set_security(
    protocomm_t *protocomm, const char *endpoint,
    const protocomm_security_t *security, const void *parameters)
{
    (void)protocomm;
    (void)endpoint;
    assert(security == &protocomm_security2);
    assert(parameters != NULL);
    const protocomm_security2_params_t *params = parameters;

    assert(params->salt != NULL && params->verifier != NULL);
    s_captured_salt_len = (size_t)params->salt_len;
    memcpy(s_captured_salt, params->salt, s_captured_salt_len);
    s_captured_verifier_len = (size_t)params->verifier_len;
    memcpy(s_captured_verifier, params->verifier, s_captured_verifier_len);
    return ESP_OK;
}

esp_err_t protocomm_add_endpoint(
    protocomm_t *protocomm, const char *endpoint,
    protocomm_req_handler_t handler, void *private_data)
{
    (void)protocomm;
    s_last_app_endpoint = endpoint;
    s_app_handler = handler;
    s_app_handler_arg = private_data;
    return ESP_OK;
}

esp_err_t protocomm_open_session(protocomm_t *protocomm, uint32_t session_id)
{
    (void)protocomm;
    s_open_session_count++;
    s_last_session_id = session_id;
    return ESP_OK;
}

esp_err_t protocomm_close_session(protocomm_t *protocomm, uint32_t session_id)
{
    (void)protocomm;
    s_close_session_count++;
    assert(session_id == s_last_session_id);
    return ESP_OK;
}

esp_err_t protocomm_req_handle(
    protocomm_t *protocomm, const char *endpoint, uint32_t session_id,
    const uint8_t *input, ssize_t input_length,
    uint8_t **output, ssize_t *output_length)
{
    (void)protocomm;
    assert(session_id == s_last_session_id);
    if (strcmp(endpoint, "sec2") == 0)
    {
        s_sec2_handle_count++;
        s_last_sec2_request_len = (size_t)input_length;
        memcpy(s_last_sec2_request, input, s_last_sec2_request_len);
        if (s_fail_next_sec2)
        {
            s_fail_next_sec2 = false;
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t *response = malloc((size_t)input_length + 4U);

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
    if (strcmp(endpoint, s_last_app_endpoint) == 0)
    {
        assert(s_app_handler != NULL);
        s_app_handle_count++;
        return s_app_handler(session_id, input, input_length,
                             output, output_length, s_app_handler_arg);
    }
    return ESP_ERR_INVALID_ARG;
}

static void _reset_fakes(void)
{
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
    s_last_app_endpoint = NULL;
    s_app_handler = NULL;
    s_app_handler_arg = NULL;
    s_fail_next_sec2 = false;
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
    *response = out;
    *response_len = request_len + 1U;
    return ESP_OK;
}

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
               (const uint8_t *)"cmd0", 4U, &out, &out_len) ==
           ESP_ERR_INVALID_ARG);

    /* Open the window: the adapter rebuilds protocomm with the POP
     * derived salt and verifier. */
    assert(device_link_security_open_bootstrap(
               (const uint8_t *)TEST_POP, strlen(TEST_POP)) == ESP_OK);
    assert(s_new_count == 1U);
    assert(s_captured_salt_len == 16U);
    assert(s_captured_salt[0] == 0x40);
    assert(strcmp(s_captured_verifier, "verifier:window-pop-secret") == 0);

    /* Handshake opens the session and routes to the security endpoint. */
    assert(device_link_security_handshake(
               (const uint8_t *)"cmd0", 4U, &out, &out_len) == ESP_OK);
    assert(s_open_session_count == 1U);
    assert(s_last_session_id == 7U);
    assert(s_sec2_handle_count == 1U);
    assert(memcmp(s_last_sec2_request, "cmd0", 4U) == 0);
    assert(out_len == 6U);
    assert(out[4] == 0xaa);
    free(out);

    /* Not authenticated until a protected frame decrypts. */
    assert(!device_link_security_is_authenticated());

    /* A protected frame routes through the app endpoint and the request
     * callback; success marks the session authenticated. */
    assert(device_link_security_unprotect(
               (const uint8_t *)"cipher", 6U, &out, &out_len) == ESP_OK);
    assert(s_app_handle_count == 1U);
    assert(out_len == 7U);
    assert(out[6] == 0x7f);
    assert(device_link_security_is_authenticated());
    free(out);

    /* Closing the window removes the verifier and tears the protocomm
     * instance down; the session is no longer usable. */
    device_link_security_close_bootstrap();
    assert(s_delete_count == 1U);
    assert(device_link_security_is_authenticated() == false);
    assert(device_link_security_handshake(
               (const uint8_t *)"cmd0", 4U, &out, &out_len) ==
           ESP_ERR_INVALID_ARG);
    assert(device_link_security_unprotect(
               (const uint8_t *)"cipher", 6U, &out, &out_len) ==
           ESP_ERR_INVALID_STATE);
    device_link_security_deinit();
    /* The close_bootstrap teardown already deleted the instance. */
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
    uint8_t *out = NULL;
    size_t out_len = 0U;

    assert(device_link_security_handshake(
               (const uint8_t *)"cmd0", 4U, &out, &out_len) == ESP_OK);
    free(out);
    s_fail_next_sec2 = true;
    assert(device_link_security_handshake(
               (const uint8_t *)"cmd1", 4U, &out, &out_len) ==
           ESP_ERR_INVALID_ARG);
    assert(s_close_session_count == 1U);
    assert(!device_link_security_is_authenticated());
    assert(device_link_security_unprotect(
               (const uint8_t *)"cipher", 6U, &out, &out_len) ==
           ESP_ERR_INVALID_STATE);
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
    uint8_t *out = NULL;
    size_t out_len = 0U;

    assert(device_link_security_handshake(
               (const uint8_t *)"cmd0", 4U, &out, &out_len) == ESP_OK);
    free(out);
    assert(device_link_security_unprotect(
               (const uint8_t *)"cipher", 6U, &out, &out_len) == ESP_OK);
    free(out);
    device_link_security_close_session();
    assert(s_close_session_count == 1U);
    assert(!device_link_security_is_authenticated());
    assert(device_link_security_unprotect(
               (const uint8_t *)"cipher", 6U, &out, &out_len) ==
           ESP_ERR_INVALID_STATE);
    device_link_security_deinit();
}

int main(void)
{
    _test_init_validation();
    _test_bootstrap_lifecycle();
    _test_failed_handshake_closes_session();
    _test_explicit_session_close();
    puts("device_link_security host tests passed");
    return 0;
}
