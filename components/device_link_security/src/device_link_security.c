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

#include "nv_storage.h"

#include "device_link_security.h"
#include "device_link_security_auth.h"

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
    DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM,
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
    bool initialized;
} device_link_security_t;

/** @brief Storage key of the committed authorization record. */
#define DEVICE_LINK_SECURITY_AUTH_STORAGE_KEY "dls.auth"

static bool _device_link_security_record_valid(
    const device_link_security_auth_record_t *record)
{
    if (record == NULL)
    {
        return false;
    }
    return record->magic == DEVICE_LINK_SECURITY_AUTH_MAGIC &&
           record->schema_version == DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
}

static device_link_security_t s_security;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_control;

static void _device_link_security_close_session_locked(void);

static void _device_link_security_lock(void)
{
    if (s_mutex != NULL)
    {
        (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void _device_link_security_unlock(void)
{
    if (s_mutex != NULL)
    {
        (void)xSemaphoreGive(s_mutex);
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
    if (s_mutex == NULL)
    {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_control);
        if (s_mutex == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    _device_link_security_lock();
    if (s_security.initialized)
    {
        /* Idempotent re-init: tear the previous instance down first so
         * no verifier or session state survives across configurations. */
        _device_link_security_teardown_protocomm();
        _device_link_security_free_verifier();
    }
    memset(&s_security, 0, sizeof(s_security));
    s_security.config = *config;
    s_security.initialized = true;
    _device_link_security_unlock();
    return ESP_OK;
}

void device_link_security_deinit(void)
{
    _device_link_security_lock();
    if (s_security.initialized)
    {
        _device_link_security_teardown_protocomm();
        _device_link_security_free_verifier();
        memset(&s_security, 0, sizeof(s_security));
    }
    _device_link_security_unlock();
}

esp_err_t device_link_security_open_bootstrap(
    const uint8_t *pop, size_t pop_len)
{
    if (pop == NULL || pop_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _device_link_security_lock();
    if (!s_security.initialized)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
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
    if (s_security.initialized)
    {
        _device_link_security_teardown_protocomm();
        _device_link_security_free_verifier();
        (void)_device_link_security_rebuild();
    }
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
    if (!s_security.initialized || s_security.protocomm == NULL)
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
    if (!s_security.initialized || !s_security.session_open ||
            s_security.protocomm == NULL)
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
    authenticated = s_security.initialized &&
                    s_security.authenticated && s_security.session_open;
    _device_link_security_unlock();
    return authenticated;
}

static void _device_link_security_close_session_locked(void)
{
    if (s_security.initialized && s_security.session_open &&
            s_security.protocomm != NULL)
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

bool device_link_security_auth_record_valid(
    const device_link_security_auth_record_t *record)
{
    return _device_link_security_record_valid(record);
}

esp_err_t device_link_security_save_auth_record(
    const device_link_security_auth_record_t *record)
{
    if (!_device_link_security_record_valid(record))
    {
        return ESP_ERR_INVALID_ARG;
    }
    return nv_storage_set_blob(DEVICE_LINK_SECURITY_AUTH_STORAGE_KEY,
                               record, sizeof(*record));
}

esp_err_t device_link_security_load_auth_record(
    device_link_security_auth_record_t *record)
{
    if (record == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    size_t size = sizeof(*record);
    const esp_err_t result =
        nv_storage_get_blob(DEVICE_LINK_SECURITY_AUTH_STORAGE_KEY,
                            record, &size);

    if (result != ESP_OK)
    {
        return result;
    }
    if (size != sizeof(*record) || !_device_link_security_record_valid(record))
    {
        _device_link_security_zeroize(record, sizeof(*record));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t device_link_security_erase_auth_record(void)
{
    return nv_storage_erase_key(DEVICE_LINK_SECURITY_AUTH_STORAGE_KEY);
}

esp_err_t device_link_security_derive_long_term_verifier(
    const uint8_t *password, size_t password_len,
    uint8_t salt[DEVICE_LINK_SECURITY_AUTH_SALT_BYTES],
    uint8_t verifier[DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES])
{
    if (password == NULL || password_len == 0U ||
            salt == NULL || verifier == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char *srp_salt = NULL;
    char *srp_verifier = NULL;
    int verifier_len = 0;
    const char *username = s_security.config.username != NULL ?
                           s_security.config.username :
                           DEVICE_LINK_SECURITY_USERNAME;
    const esp_err_t result = esp_srp_gen_salt_verifier(
                                 username, (int)strlen(username),
                                 (const char *)password, (int)password_len,
                                 &srp_salt, DEVICE_LINK_SECURITY_SALT_BYTES,
                                 &srp_verifier, &verifier_len);

    if (result != ESP_OK)
    {
        return result;
    }
    if (srp_salt == NULL || srp_verifier == NULL ||
            verifier_len != DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES)
    {
        free(srp_salt);
        free(srp_verifier);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(salt, srp_salt, DEVICE_LINK_SECURITY_AUTH_SALT_BYTES);
    memcpy(verifier, srp_verifier,
           DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES);
    _device_link_security_zeroize(srp_salt, DEVICE_LINK_SECURITY_SALT_BYTES);
    _device_link_security_zeroize(srp_verifier,
                                  (size_t)verifier_len);
    free(srp_salt);
    free(srp_verifier);
    return ESP_OK;
}

esp_err_t device_link_security_load_long_term_verifier(void)
{
    _device_link_security_lock();
    if (!s_security.initialized)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    const esp_err_t load_result =
        device_link_security_load_auth_record(&record);

    if (load_result != ESP_OK)
    {
        _device_link_security_unlock();
        return load_result;
    }
    /* Tear the old Protocomm instance down before freeing the verifier it
     * references, then install the long-term salt and verifier. */
    _device_link_security_teardown_protocomm();
    _device_link_security_free_verifier();
    s_security.salt = malloc(DEVICE_LINK_SECURITY_AUTH_SALT_BYTES);
    s_security.verifier = malloc(DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES);
    if (s_security.salt == NULL || s_security.verifier == NULL)
    {
        _device_link_security_free_verifier();
        _device_link_security_zeroize(&record, sizeof(record));
        _device_link_security_unlock();
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_security.salt, record.salt,
           DEVICE_LINK_SECURITY_AUTH_SALT_BYTES);
    memcpy(s_security.verifier, record.verifier,
           DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES);
    s_security.salt_len = DEVICE_LINK_SECURITY_AUTH_SALT_BYTES;
    s_security.verifier_len = DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES;
    s_security.verifier_kind = DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM;
    _device_link_security_zeroize(&record, sizeof(record));
    const esp_err_t result = _device_link_security_rebuild();

    _device_link_security_unlock();
    return result;
}
