#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"

#include "protocomm.h"
#include "protocomm_security2.h"
#include "esp_srp.h"

#include "device_link_security.h"

#define DBG_TAG "device_link_security"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define DEVICE_LINK_SECURITY_ENDPOINT_SECURITY "sec2"
#define DEVICE_LINK_SECURITY_ENDPOINT_APP "dl"
#define DEVICE_LINK_SECURITY_SALT_BYTES 16U
#define DEVICE_LINK_SECURITY_USERNAME "microtech"

typedef enum
{
    DEVICE_LINK_SECURITY_VERIFIER_NONE = 0,
    DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP,
} device_link_security_verifier_kind_t;

typedef struct device_link_security
{
    device_link_security_config_t config;
    protocomm_t *protocomm;
    device_link_security_verifier_kind_t verifier_kind;
    char *salt;
    size_t salt_len;
    char *verifier;
    size_t verifier_len;
    bool session_open;
    bool authenticated;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_control;
} device_link_security_t;

static device_link_security_t s_security;

static void _device_link_security_close_session_locked(void);

static void _device_link_security_lock(void)
{
    if (s_security.mutex != NULL)
    {
        (void)xSemaphoreTake(s_security.mutex, portMAX_DELAY);
    }
}

static void _device_link_security_unlock(void)
{
    if (s_security.mutex != NULL)
    {
        (void)xSemaphoreGive(s_security.mutex);
    }
}

static void _device_link_security_zeroize(void *data, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    for (size_t i = 0U; i < size; ++i)
    {
        bytes[i] = 0U;
    }
}

static void _device_link_security_free_verifier(void)
{
    if (s_security.salt != NULL)
    {
        _device_link_security_zeroize(s_security.salt, s_security.salt_len);
        free(s_security.salt);
    }
    if (s_security.verifier != NULL)
    {
        _device_link_security_zeroize(s_security.verifier,
                                      s_security.verifier_len);
        free(s_security.verifier);
    }
    s_security.salt = NULL;
    s_security.salt_len = 0U;
    s_security.verifier = NULL;
    s_security.verifier_len = 0U;
    s_security.verifier_kind = DEVICE_LINK_SECURITY_VERIFIER_NONE;
}

static void _device_link_security_teardown_protocomm(void)
{
    if (s_security.protocomm != NULL)
    {
        protocomm_delete(s_security.protocomm);
        s_security.protocomm = NULL;
    }
    s_security.session_open = false;
    s_security.authenticated = false;
}

/**
 * @brief Rebuild the Protocomm instance with the current verifier.
 *
 * Called after a verifier change so a new handshake always uses the
 * current window POP or long-term credential; any previous session is
 * replaced.
 */
static esp_err_t _device_link_security_app_handler(
    uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
    uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    (void)session_id;
    (void)priv_data;
    if (inbuf == NULL || inlen <= 0 || outbuf == NULL || outlen == NULL ||
            s_security.config.request_cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *response = NULL;
    size_t response_len = 0U;
    const esp_err_t result = s_security.config.request_cb(
                                 inbuf, (size_t)inlen,
                                 &response, &response_len,
                                 s_security.config.request_arg);

    if (result != ESP_OK)
    {
        /* The callback may have allocated a response before failing. */
        free(response);
        return result;
    }
    if (response == NULL || response_len == 0U)
    {
        free(response);
        return ESP_ERR_INVALID_STATE;
    }
    *outbuf = response;
    *outlen = (ssize_t)response_len;
    return ESP_OK;
}

static esp_err_t _device_link_security_rebuild(void)
{
    if (s_security.verifier_kind == DEVICE_LINK_SECURITY_VERIFIER_NONE)
    {
        /* No verifier: no session can be established (fail closed). */
        _device_link_security_teardown_protocomm();
        return ESP_OK;
    }
    _device_link_security_teardown_protocomm();
    s_security.protocomm = protocomm_new();
    if (s_security.protocomm == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    const protocomm_security2_params_t params =
    {
        .salt = (const char *)s_security.salt,
        .salt_len = (int)s_security.salt_len,
        .verifier = s_security.verifier,
        .verifier_len = (uint16_t)s_security.verifier_len,
    };
    esp_err_t result = protocomm_set_security(
                           s_security.protocomm,
                           DEVICE_LINK_SECURITY_ENDPOINT_SECURITY,
                           &protocomm_security2, &params);

    if (result != ESP_OK)
    {
        goto fail;
    }
    result = protocomm_add_endpoint(
                 s_security.protocomm, DEVICE_LINK_SECURITY_ENDPOINT_APP,
                 _device_link_security_app_handler, NULL);
    if (result != ESP_OK)
    {
        goto fail;
    }
    return ESP_OK;

fail:
    protocomm_delete(s_security.protocomm);
    s_security.protocomm = NULL;
    return result;
}

esp_err_t device_link_security_init(const device_link_security_config_t *config)
{
    if (config == NULL || config->username == NULL ||
            config->request_cb == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_security, 0, sizeof(s_security));
    s_security.config = *config;
    s_security.mutex = xSemaphoreCreateMutexStatic(&s_security.mutex_control);
    if (s_security.mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void device_link_security_deinit(void)
{
    _device_link_security_lock();
    _device_link_security_teardown_protocomm();
    _device_link_security_free_verifier();
    _device_link_security_unlock();
    if (s_security.mutex != NULL)
    {
        vSemaphoreDelete(s_security.mutex);
        s_security.mutex = NULL;
    }
    memset(&s_security, 0, sizeof(s_security));
}

esp_err_t device_link_security_open_bootstrap(
    const uint8_t *pop, size_t pop_len)
{
    if (pop == NULL || pop_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _device_link_security_lock();
    /* Tear the old Protocomm instance down before freeing the verifier it
     * references: protocomm_set_security shallow-copies the params, so the
     * salt and verifier buffers must outlive every instance using them. */
    _device_link_security_teardown_protocomm();
    _device_link_security_free_verifier();
    char *salt = NULL;
    char *verifier = NULL;
    int verifier_len = 0;
    esp_err_t result = esp_srp_gen_salt_verifier(
                           s_security.config.username != NULL ?
                           s_security.config.username :
                           DEVICE_LINK_SECURITY_USERNAME,
                           (int)strlen(s_security.config.username != NULL ?
                                       s_security.config.username :
                                       DEVICE_LINK_SECURITY_USERNAME),
                           (const char *)pop, (int)pop_len,
                           &salt, DEVICE_LINK_SECURITY_SALT_BYTES,
                           &verifier, &verifier_len);

    if (result != ESP_OK)
    {
        _device_link_security_unlock();
        return result;
    }
    if (salt == NULL || verifier == NULL || verifier_len <= 0)
    {
        free(salt);
        free(verifier);
        _device_link_security_unlock();
        return ESP_ERR_NO_MEM;
    }
    s_security.salt = salt;
    s_security.salt_len = DEVICE_LINK_SECURITY_SALT_BYTES;
    s_security.verifier = verifier;
    s_security.verifier_len = (size_t)verifier_len;
    s_security.verifier_kind = DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP;
    result = _device_link_security_rebuild();
    if (result != ESP_OK)
    {
        _device_link_security_free_verifier();
    }
    _device_link_security_unlock();
    return result;
}

void device_link_security_close_bootstrap(void)
{
    _device_link_security_lock();
    _device_link_security_teardown_protocomm();
    _device_link_security_free_verifier();
    (void)_device_link_security_rebuild();
    _device_link_security_unlock();
}

esp_err_t device_link_security_handshake(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len)
{
    if (input == NULL || output == NULL || output_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _device_link_security_lock();
    if (s_security.protocomm == NULL)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_security.session_open)
    {
        const esp_err_t result = protocomm_open_session(
                                     s_security.protocomm,
                                     s_security.config.session_id);

        if (result != ESP_OK)
        {
            _device_link_security_unlock();
            return result;
        }
        s_security.session_open = true;
        s_security.authenticated = false;
    }
    uint8_t *response = NULL;
    ssize_t response_len = 0;
    const esp_err_t result = protocomm_req_handle(
                                 s_security.protocomm,
                                 DEVICE_LINK_SECURITY_ENDPOINT_SECURITY,
                                 s_security.config.session_id,
                                 input, (ssize_t)input_len,
                                 &response, &response_len);

    if (result != ESP_OK)
    {
        if (response != NULL)
        {
            free(response);
        }
        /* A failed handshake (e.g. wrong POP or proof) closes the
         * session so no stale state accumulates. */
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return result;
    }
    /* The handshake sequence may still be mid-flight (cmd0 only); the
     * session becomes AUTHENTICATED once a protected frame decrypts
     * successfully, which the Security 2 scheme only allows after the
     * SRP proof verified. */
    *output = response;
    *output_len = (size_t)response_len;
    _device_link_security_unlock();
    return ESP_OK;
}

esp_err_t device_link_security_unprotect(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len)
{
    if (input == NULL || output == NULL || output_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* Ciphertext must exceed the 16-byte GCM tag; anything shorter is
     * malformed and closes the session (upstream would underflow). */
    if (input_len <= 16U)
    {
        _device_link_security_lock();
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    _device_link_security_lock();
    if (!s_security.session_open || s_security.protocomm == NULL)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *response = NULL;
    ssize_t response_len = 0;
    const esp_err_t result = protocomm_req_handle(
                                 s_security.protocomm,
                                 DEVICE_LINK_SECURITY_ENDPOINT_APP,
                                 s_security.config.session_id,
                                 input, (ssize_t)input_len,
                                 &response, &response_len);

    if (result != ESP_OK)
    {
        if (response != NULL)
        {
            free(response);
        }
        /* Malformed ciphertext or a failed tag closes the session. */
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return result;
    }
    if (response == NULL || response_len <= 0)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    /* A successful decrypt proves the SRP proof already verified. */
    s_security.authenticated = true;
    *output = response;
    *output_len = (size_t)response_len;
    _device_link_security_unlock();
    return ESP_OK;
}

bool device_link_security_is_authenticated(void)
{
    bool authenticated = false;

    _device_link_security_lock();
    authenticated = s_security.authenticated && s_security.session_open;
    _device_link_security_unlock();
    return authenticated;
}

static void _device_link_security_close_session_locked(void)
{
    if (s_security.session_open && s_security.protocomm != NULL)
    {
        (void)protocomm_close_session(s_security.protocomm,
                                      s_security.config.session_id);
    }
    s_security.session_open = false;
    s_security.authenticated = false;
}

void device_link_security_close_session(void)
{
    _device_link_security_lock();
    _device_link_security_close_session_locked();
    _device_link_security_unlock();
}
