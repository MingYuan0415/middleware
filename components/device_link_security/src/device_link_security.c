#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"

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

typedef enum
{
    DEVICE_LINK_SECURITY_VERIFIER_NONE = 0,
    DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP,
    DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM,
} device_link_security_verifier_kind_t;

typedef struct device_link_security
{
    device_link_security_config_t config;
    protocomm_security_handle_t sec_inst;
    protocomm_security2_params_t sec_params;
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
    if (record->magic != DEVICE_LINK_SECURITY_AUTH_MAGIC ||
            record->schema_version != DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION)
    {
        return false;
    }
    /* Persisted security material must be structurally sound: a legal
     * peer identity and nonzero identifiers, salt, and verifier. */
    if (record->peer_addr_type > 2U)
    {
        return false;
    }
    bool peer_nonzero = false;
    bool credential_nonzero = false;
    bool auth_id_nonzero = false;
    bool salt_nonzero = false;
    bool verifier_nonzero = false;

    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES; ++i)
    {
        peer_nonzero = peer_nonzero || record->peer_addr[i] != 0U;
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES; ++i)
    {
        credential_nonzero = credential_nonzero ||
                             record->credential_id[i] != 0U;
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_ID_BYTES; ++i)
    {
        auth_id_nonzero = auth_id_nonzero || record->device_auth_id[i] != 0U;
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_SALT_BYTES; ++i)
    {
        salt_nonzero = salt_nonzero || record->salt[i] != 0U;
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES; ++i)
    {
        verifier_nonzero = verifier_nonzero || record->verifier[i] != 0U;
    }
    return peer_nonzero && credential_nonzero && auth_id_nonzero &&
           salt_nonzero && verifier_nonzero;
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

static void _device_link_security_teardown_sec(void)
{
    if (s_security.sec_inst != NULL)
    {
        if (protocomm_security2.cleanup != NULL)
        {
            (void)protocomm_security2.cleanup(s_security.sec_inst);
        }
        s_security.sec_inst = NULL;
    }
    memset(&s_security.sec_params, 0, sizeof(s_security.sec_params));
    s_security.session_open = false;
    s_security.authenticated = false;
}

static esp_err_t _device_link_security_rebuild(void)
{
    if (s_security.verifier_kind == DEVICE_LINK_SECURITY_VERIFIER_NONE)
    {
        /* No verifier: no session can be established (fail closed). */
        _device_link_security_teardown_sec();
        return ESP_OK;
    }
    _device_link_security_teardown_sec();
    if (protocomm_security2.init == NULL)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t result = protocomm_security2.init(&s_security.sec_inst);

    if (result != ESP_OK)
    {
        s_security.sec_inst = NULL;
        return result;
    }
    s_security.sec_params.salt = (const char *)s_security.salt;
    s_security.sec_params.salt_len = (uint16_t)s_security.salt_len;
    s_security.sec_params.verifier = s_security.verifier;
    s_security.sec_params.verifier_len = (uint16_t)s_security.verifier_len;
    return ESP_OK;
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
        _device_link_security_teardown_sec();
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
        _device_link_security_teardown_sec();
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
    /* Tear the old security instance down before freeing the verifier it
     * references: the SRP context shallow-copies the params, so the salt
     * and verifier buffers must outlive every instance using them. */
    _device_link_security_teardown_sec();
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
        _device_link_security_teardown_sec();
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
    if (!s_security.initialized || s_security.sec_inst == NULL ||
            protocomm_security2.security_req_handler == NULL)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_security.session_open)
    {
        if (protocomm_security2.new_transport_session == NULL)
        {
            _device_link_security_unlock();
            return ESP_ERR_NOT_SUPPORTED;
        }
        const esp_err_t result = protocomm_security2.new_transport_session(
                                     s_security.sec_inst,
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
    const esp_err_t result = protocomm_security2.security_req_handler(
                                 s_security.sec_inst,
                                 &s_security.sec_params,
                                 s_security.config.session_id,
                                 input, (ssize_t)input_len,
                                 &response, &response_len, NULL);

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
            s_security.sec_inst == NULL ||
            protocomm_security2.decrypt == NULL ||
            protocomm_security2.encrypt == NULL)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *plain = NULL;
    ssize_t plain_len = 0;
    esp_err_t result = protocomm_security2.decrypt(
                           s_security.sec_inst, s_security.config.session_id,
                           input, (ssize_t)input_len, &plain, &plain_len);

    if (result != ESP_OK)
    {
        free(plain);
        /* Malformed ciphertext or a failed tag closes the session. */
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return result;
    }
    /* The request callback runs without the adapter lock: it may invoke
     * adapter operations (protect, close_session, verifier transitions)
     * without re-entering the mutex. The session state is revalidated
     * before the response is encrypted, so a concurrent close fails
     * closed. */
    _device_link_security_unlock();
    uint8_t *plain_response = NULL;
    size_t plain_response_len = 0U;

    result = s_security.config.request_cb(
                 plain, (size_t)plain_len,
                 &plain_response, &plain_response_len,
                 s_security.config.request_arg);
    free(plain);
    _device_link_security_lock();
    if (result != ESP_OK)
    {
        free(plain_response);
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return result;
    }
    /* The callback may consume the request itself and emit the response
     * through the transport (ESP_OK with a NULL response); otherwise the
     * plaintext response is encrypted and returned. */
    if (plain_response == NULL || plain_response_len == 0U)
    {
        free(plain_response);
        if (!s_security.session_open || s_security.sec_inst == NULL)
        {
            _device_link_security_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        s_security.authenticated = true;
        *output = NULL;
        *output_len = 0U;
        _device_link_security_unlock();
        return ESP_OK;
    }
    uint8_t *cipher = NULL;
    ssize_t cipher_len = 0;

    if (!s_security.session_open || s_security.sec_inst == NULL ||
            protocomm_security2.encrypt == NULL)
    {
        free(plain_response);
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    result = protocomm_security2.encrypt(
                 s_security.sec_inst, s_security.config.session_id,
                 plain_response, (ssize_t)plain_response_len,
                 &cipher, &cipher_len);
    free(plain_response);
    if (result != ESP_OK)
    {
        free(cipher);
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return result;
    }
    /* A successful decrypt proves the SRP proof already verified. */
    s_security.authenticated = true;
    *output = cipher;
    *output_len = (size_t)cipher_len;
    _device_link_security_unlock();
    return ESP_OK;
}

esp_err_t device_link_security_protect(
    const uint8_t *plain, size_t plain_len,
    uint8_t **cipher, size_t *cipher_len)
{
    if (plain == NULL || cipher == NULL || cipher_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *cipher = NULL;
    *cipher_len = 0U;
    _device_link_security_lock();
    if (!s_security.initialized || !s_security.session_open ||
            !s_security.authenticated || s_security.sec_inst == NULL ||
            protocomm_security2.encrypt == NULL)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ssize_t out_len = 0;
    const esp_err_t result = protocomm_security2.encrypt(
                                 s_security.sec_inst,
                                 s_security.config.session_id,
                                 plain, (ssize_t)plain_len,
                                 cipher, &out_len);

    if (result != ESP_OK)
    {
        free(*cipher);
        *cipher = NULL;
        _device_link_security_unlock();
        return result;
    }
    *cipher_len = (size_t)out_len;
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
            s_security.sec_inst != NULL &&
            protocomm_security2.close_transport_session != NULL)
    {
        (void)protocomm_security2.close_transport_session(
            s_security.sec_inst, s_security.config.session_id);
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
    _device_link_security_teardown_sec();
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
