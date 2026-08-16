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
#include "session.pb-c.h"

#ifdef UNIT_TEST_HOST
    #include "tinycrypt/sha256.h"
#else
    #include "mbedtls/md.h"
#endif

#include "nvs.h"

#include "nv_storage.h"

#include "device_link_security.h"
#include "device_link_security_auth.h"

#define DBG_TAG "device_link_security"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define DEVICE_LINK_SECURITY_ENDPOINT_SECURITY "sec2"
#define DEVICE_LINK_SECURITY_ENDPOINT_APP "dl"
#define DEVICE_LINK_SECURITY_SALT_BYTES 16U
#define DEVICE_LINK_SECURITY_STATIC_RANDOM_MASK 0xc0U
#define DEVICE_LINK_SECURITY_RANDOM_PART_MASK 0x3fU
/* Public-discovery derivation version byte (frozen in security.json). */
#define DEVICE_LINK_SECURITY_PUBLIC_SRP_VERSION 0x02U
#define DEVICE_LINK_SECURITY_PUBLIC_SRP_SEPARATOR 0x00U

/* RFC 4122 network-order bytes of the v2 service UUID. */
static const uint8_t s_public_srp_service_uuid_bytes[16] =
{
    0x2c, 0x77, 0xe4, 0x8c, 0xc5, 0x10, 0x42, 0x30,
    0x8d, 0x05, 0x63, 0xd0, 0x36, 0xdc, 0x03, 0x8b,
};

typedef struct device_link_security
{
    device_link_security_config_t config;
    protocomm_security_handle_t sec_inst;
    protocomm_security2_params_t sec_params;
    /* Long-term slot: committed authorization record. */
    char *lt_salt;
    size_t lt_salt_len;
    char *lt_verifier;
    size_t lt_verifier_len;
    /* Bootstrap slot: POP of the currently open pairing window. */
    char *bt_salt;
    size_t bt_salt_len;
    char *bt_verifier;
    size_t bt_verifier_len;
    /* Public slot: derived password of the current public advertisement. */
    char *pb_salt;
    size_t pb_salt_len;
    char *pb_verifier;
    size_t pb_verifier_len;
    /* Selection pinned for the current Security 2 session. */
    device_link_security_verifier_kind_t selected_kind;
    uint8_t selected_peer_type;
    uint8_t selected_peer_addr[DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES];
    bool session_open;
    bool authenticated;
    bool initialized;
} device_link_security_t;

static void _device_link_security_release_request_response(
    uint8_t *response, size_t response_len,
    device_link_security_response_release_fn release_cb, void *release_arg)
{
    if (response == NULL)
    {
        return;
    }
    if (release_cb != NULL)
    {
        release_cb(response, response_len, release_arg);
        return;
    }
    free(response);
}

/* Boot-lifetime session generation: survives init/deinit resets (which
 * memset the state) and never wraps, so a retired session can never be
 * mistaken for a new one (address ABA included). */
static uint32_t s_session_epoch;

/** @brief Storage key of the committed authorization record. */
#define DEVICE_LINK_SECURITY_AUTH_STORAGE_KEY "dls.auth"

/** @brief Storage key of the local-revoke journal marker. */
#define DEVICE_LINK_SECURITY_REVOKE_STORAGE_KEY "dls.revoke"

bool device_link_security_normalized_identity_valid(
    uint8_t peer_addr_type,
    const uint8_t peer_addr[DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES])
{
    if (peer_addr == NULL)
    {
        return false;
    }
    bool nonzero = false;

    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES; ++i)
    {
        nonzero = nonzero || peer_addr[i] != 0U;
    }
    if (peer_addr_type == DEVICE_LINK_SECURITY_PEER_ADDR_PUBLIC ||
            peer_addr_type == DEVICE_LINK_SECURITY_PEER_ADDR_PUBLIC_ID)
    {
        return nonzero;
    }
    if (peer_addr_type != DEVICE_LINK_SECURITY_PEER_ADDR_RANDOM &&
            peer_addr_type != DEVICE_LINK_SECURITY_PEER_ADDR_RANDOM_ID)
    {
        return false;
    }
    if ((peer_addr[DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES - 1U] &
            DEVICE_LINK_SECURITY_STATIC_RANDOM_MASK) !=
            DEVICE_LINK_SECURITY_STATIC_RANDOM_MASK)
    {
        return false;
    }
    bool random_part_nonzero =
        (peer_addr[DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES - 1U] &
         DEVICE_LINK_SECURITY_RANDOM_PART_MASK) != 0U;
    bool random_part_not_all_one =
        (peer_addr[DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES - 1U] &
         DEVICE_LINK_SECURITY_RANDOM_PART_MASK) !=
        DEVICE_LINK_SECURITY_RANDOM_PART_MASK;

    for (size_t i = 0U;
            i < DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES - 1U; ++i)
    {
        random_part_nonzero = random_part_nonzero || peer_addr[i] != 0U;
        random_part_not_all_one = random_part_not_all_one ||
                                  peer_addr[i] != UINT8_MAX;
    }
    return random_part_nonzero && random_part_not_all_one;
}

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
    /* Persisted security material must carry a normalized identity, never an
     * OTA private address, plus nonzero identifiers, salt, and verifier. */
    if (!device_link_security_normalized_identity_valid(
                record->peer_addr_type, record->peer_addr))
    {
        return false;
    }
    bool credential_nonzero = false;
    bool auth_id_nonzero = false;
    bool salt_nonzero = false;
    bool verifier_nonzero = false;

    if (record->granted_permission_count == 0U ||
            record->granted_permission_count >
            DEVICE_LINK_SECURITY_AUTH_MAX_GRANTS)
    {
        return false;
    }
    for (size_t i = 0U; i < record->granted_permission_count; ++i)
    {
        if (record->granted_permissions[i] == 0U ||
                (i > 0U && record->granted_permissions[i - 1U] >=
                 record->granted_permissions[i]))
        {
            return false;
        }
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
    return credential_nonzero && auth_id_nonzero && salt_nonzero &&
           verifier_nonzero;
}

static device_link_security_t s_security;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_control;

static void _device_link_security_close_session_locked(void);

static esp_err_t _device_link_security_parse_handshake(
    const uint8_t *data, size_t data_len, bool response,
    device_link_security_handshake_stage_t *stage)
{
    if (data == NULL || data_len == 0U || stage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    SessionData *session = session_data__unpack(NULL, data_len, data);

    if (session == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_ERR_INVALID_ARG;
    const Sec2Payload *payload = session->sec2;

    if (session->sec_ver != SEC_SCHEME_VERSION__SecScheme2 ||
            session->proto_case != SESSION_DATA__PROTO_SEC2 ||
            payload == NULL)
    {
        goto cleanup;
    }
    if (!response &&
            payload->msg == SEC2_MSG_TYPE__S2Session_Command0 &&
            payload->payload_case == SEC2_PAYLOAD__PAYLOAD_SC0 &&
            payload->sc0 != NULL)
    {
        *stage = DEVICE_LINK_SECURITY_HANDSHAKE_CMD0;
        result = ESP_OK;
    }
    else if (!response &&
             payload->msg == SEC2_MSG_TYPE__S2Session_Command1 &&
             payload->payload_case == SEC2_PAYLOAD__PAYLOAD_SC1 &&
             payload->sc1 != NULL)
    {
        *stage = DEVICE_LINK_SECURITY_HANDSHAKE_CMD1;
        result = ESP_OK;
    }
    else if (response &&
             payload->msg == SEC2_MSG_TYPE__S2Session_Response0 &&
             payload->payload_case == SEC2_PAYLOAD__PAYLOAD_SR0 &&
             payload->sr0 != NULL && payload->sr0->status == STATUS__Success)
    {
        *stage = DEVICE_LINK_SECURITY_HANDSHAKE_CMD0;
        result = ESP_OK;
    }
    else if (response &&
             payload->msg == SEC2_MSG_TYPE__S2Session_Response1 &&
             payload->payload_case == SEC2_PAYLOAD__PAYLOAD_SR1 &&
             payload->sr1 != NULL && payload->sr1->status == STATUS__Success)
    {
        *stage = DEVICE_LINK_SECURITY_HANDSHAKE_CMD1;
        result = ESP_OK;
    }

cleanup:
    session_data__free_unpacked(session, NULL);
    return result;
}

esp_err_t device_link_security_classify_handshake(
    const uint8_t *input, size_t input_len,
    device_link_security_handshake_stage_t *stage)
{
    return _device_link_security_parse_handshake(
               input, input_len, false, stage);
}

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

static esp_err_t _device_link_security_sha256(
    const uint8_t *data, size_t length, uint8_t digest[32U])
{
    if ((data == NULL && length != 0U) || digest == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
#ifdef UNIT_TEST_HOST
    struct tc_sha256_state_struct state;

    memset(&state, 0, sizeof(state));
    if (tc_sha256_init(&state) != 1 ||
            tc_sha256_update(&state, data, length) != 1 ||
            tc_sha256_final(digest, &state) != 1)
    {
        memset(&state, 0, sizeof(state));
        memset(digest, 0, 32U);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&state, 0, sizeof(state));
    return ESP_OK;
#else
    mbedtls_md_context_t ctx;

    mbedtls_md_init(&ctx);
    const int setup_result = mbedtls_md_setup(
                                 &ctx,
                                 mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                 0);

    if (setup_result != 0 ||
            mbedtls_md_starts(&ctx) != 0 ||
            mbedtls_md_update(&ctx, data, length) != 0 ||
            mbedtls_md_finish(&ctx, digest) != 0)
    {
        mbedtls_md_free(&ctx);
        memset(digest, 0, 32U);
        return ESP_ERR_INVALID_STATE;
    }
    mbedtls_md_free(&ctx);
    return ESP_OK;
#endif
}

static void _device_link_security_base64url(
    const uint8_t *in, size_t in_len, char *out)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t out_pos = 0U;
    size_t in_pos = 0U;

    while (in_len - in_pos >= 3U)
    {
        const uint32_t v = ((uint32_t)in[in_pos] << 16U) |
                           ((uint32_t)in[in_pos + 1U] << 8U) |
                           (uint32_t)in[in_pos + 2U];

        out[out_pos++] = alphabet[(v >> 18U) & 0x3fU];
        out[out_pos++] = alphabet[(v >> 12U) & 0x3fU];
        out[out_pos++] = alphabet[(v >> 6U) & 0x3fU];
        out[out_pos++] = alphabet[v & 0x3fU];
        in_pos += 3U;
    }
    const size_t remaining = in_len - in_pos;

    if (remaining == 1U)
    {
        const uint32_t v = (uint32_t)in[in_pos] << 16U;

        out[out_pos++] = alphabet[(v >> 18U) & 0x3fU];
        out[out_pos++] = alphabet[(v >> 12U) & 0x3fU];
    }
    else if (remaining == 2U)
    {
        const uint32_t v = ((uint32_t)in[in_pos] << 16U) |
                           ((uint32_t)in[in_pos + 1U] << 8U);

        out[out_pos++] = alphabet[(v >> 18U) & 0x3fU];
        out[out_pos++] = alphabet[(v >> 12U) & 0x3fU];
        out[out_pos++] = alphabet[(v >> 6U) & 0x3fU];
    }
    out[out_pos] = '\0';
}

static esp_err_t _device_link_security_derive_public_password(
    const uint8_t instance_id[DEVICE_LINK_SECURITY_PUBLIC_INSTANCE_BYTES],
    char password[DEVICE_LINK_SECURITY_PUBLIC_PASSWORD_BYTES + 1U])
{
    if (instance_id == NULL || password == NULL ||
            (instance_id[0] == 0U && instance_id[1] == 0U &&
             instance_id[2] == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const char *const label = DEVICE_LINK_SECURITY_PUBLIC_SRP_LABEL;
    const size_t label_len = strlen(label);
    uint8_t input[128];
    size_t pos = 0U;
    uint8_t digest[32U];
    esp_err_t result;

    memcpy(&input[pos], label, label_len);
    pos += label_len;
    input[pos++] = DEVICE_LINK_SECURITY_PUBLIC_SRP_SEPARATOR;
    memcpy(&input[pos], s_public_srp_service_uuid_bytes,
           sizeof(s_public_srp_service_uuid_bytes));
    pos += sizeof(s_public_srp_service_uuid_bytes);
    input[pos++] = DEVICE_LINK_SECURITY_PUBLIC_SRP_VERSION;
    memcpy(&input[pos], instance_id,
           DEVICE_LINK_SECURITY_PUBLIC_INSTANCE_BYTES);
    pos += DEVICE_LINK_SECURITY_PUBLIC_INSTANCE_BYTES;
    result = _device_link_security_sha256(input, pos, digest);
    _device_link_security_zeroize(input, pos);
    if (result != ESP_OK)
    {
        return result;
    }
    _device_link_security_base64url(
        digest, sizeof(digest), password);
    _device_link_security_zeroize(digest, sizeof(digest));
    return ESP_OK;
}

static void _device_link_security_free_long_term(void)
{
    if (s_security.lt_salt != NULL)
    {
        _device_link_security_zeroize(s_security.lt_salt,
                                      s_security.lt_salt_len);
        free(s_security.lt_salt);
    }
    if (s_security.lt_verifier != NULL)
    {
        _device_link_security_zeroize(s_security.lt_verifier,
                                      s_security.lt_verifier_len);
        free(s_security.lt_verifier);
    }
    s_security.lt_salt = NULL;
    s_security.lt_salt_len = 0U;
    s_security.lt_verifier = NULL;
    s_security.lt_verifier_len = 0U;
}

static void _device_link_security_free_bootstrap(void)
{
    if (s_security.bt_salt != NULL)
    {
        _device_link_security_zeroize(s_security.bt_salt,
                                      s_security.bt_salt_len);
        free(s_security.bt_salt);
    }
    if (s_security.bt_verifier != NULL)
    {
        _device_link_security_zeroize(s_security.bt_verifier,
                                      s_security.bt_verifier_len);
        free(s_security.bt_verifier);
    }
    s_security.bt_salt = NULL;
    s_security.bt_salt_len = 0U;
    s_security.bt_verifier = NULL;
    s_security.bt_verifier_len = 0U;
}

static void _device_link_security_free_public(void)
{
    if (s_security.pb_salt != NULL)
    {
        _device_link_security_zeroize(s_security.pb_salt,
                                      s_security.pb_salt_len);
        free(s_security.pb_salt);
    }
    if (s_security.pb_verifier != NULL)
    {
        _device_link_security_zeroize(s_security.pb_verifier,
                                      s_security.pb_verifier_len);
        free(s_security.pb_verifier);
    }
    s_security.pb_salt = NULL;
    s_security.pb_salt_len = 0U;
    s_security.pb_verifier = NULL;
    s_security.pb_verifier_len = 0U;
}

/**
 * @brief Whether the slot backing the pinned selection has material.
 */
static bool _device_link_security_selection_loaded(void)
{
    switch (s_security.selected_kind)
    {
    case DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP:
        return s_security.bt_salt != NULL && s_security.bt_verifier != NULL;
    case DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM:
        return s_security.lt_salt != NULL && s_security.lt_verifier != NULL;
    case DEVICE_LINK_SECURITY_VERIFIER_PUBLIC:
        return s_security.pb_salt != NULL && s_security.pb_verifier != NULL;
    default:
        return false;
    }
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
    /* A teardown retires the session generation: the epoch advances so
     * any in-flight unlocked callback of the old session fails closed,
     * even if a rebuild reallocates the same instance address (ABA). The
     * epoch saturates at the exhausted maximum. */
    if (s_session_epoch < UINT32_MAX)
    {
        s_session_epoch++;
    }
}

static esp_err_t _device_link_security_rebuild(void)
{
    if (s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_NONE)
    {
        /* No verifier: no session can be established (fail closed). */
        _device_link_security_teardown_sec();
        return ESP_OK;
    }
    if (!_device_link_security_selection_loaded())
    {
        /* A non-NONE selection without slot material is an invariant
         * violation (e.g. a load that failed after the selection was
         * pinned): never report success for a session that cannot
         * handshake. */
        _device_link_security_teardown_sec();
        return ESP_ERR_INVALID_STATE;
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
    if (s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP)
    {
        s_security.sec_params.salt = (const char *)s_security.bt_salt;
        s_security.sec_params.salt_len = (uint16_t)s_security.bt_salt_len;
        s_security.sec_params.verifier = s_security.bt_verifier;
        s_security.sec_params.verifier_len =
            (uint16_t)s_security.bt_verifier_len;
    }
    else if (s_security.selected_kind ==
             DEVICE_LINK_SECURITY_VERIFIER_PUBLIC)
    {
        s_security.sec_params.salt = (const char *)s_security.pb_salt;
        s_security.sec_params.salt_len = (uint16_t)s_security.pb_salt_len;
        s_security.sec_params.verifier = s_security.pb_verifier;
        s_security.sec_params.verifier_len =
            (uint16_t)s_security.pb_verifier_len;
    }
    else
    {
        s_security.sec_params.salt = (const char *)s_security.lt_salt;
        s_security.sec_params.salt_len = (uint16_t)s_security.lt_salt_len;
        s_security.sec_params.verifier = s_security.lt_verifier;
        s_security.sec_params.verifier_len =
            (uint16_t)s_security.lt_verifier_len;
    }
    return ESP_OK;
}

esp_err_t device_link_security_init(const device_link_security_config_t *config)
{
    if (config == NULL || config->request_cb == NULL)
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
        _device_link_security_free_long_term();
        _device_link_security_free_bootstrap();
        _device_link_security_free_public();
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
        _device_link_security_free_long_term();
        _device_link_security_free_bootstrap();
        _device_link_security_free_public();
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
     * and verifier buffers must outlive every instance using them. The
     * long-term slot is preserved so a bound peer keeps its credential. */
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
    if (s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP)
    {
        /* The instance references the old bootstrap buffers: replace them
         * only after the teardown. */
        _device_link_security_teardown_sec();
    }
    _device_link_security_free_bootstrap();
    s_security.bt_salt = salt;
    s_security.bt_salt_len = DEVICE_LINK_SECURITY_SALT_BYTES;
    s_security.bt_verifier = verifier;
    s_security.bt_verifier_len = (size_t)verifier_len;
    if (s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP ||
            s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_NONE)
    {
        result = _device_link_security_rebuild();
    }
    _device_link_security_unlock();
    return result;
}

esp_err_t device_link_security_close_bootstrap(void)
{
    esp_err_t result = ESP_OK;

    _device_link_security_lock();
    if (!s_security.initialized)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP)
    {
        _device_link_security_teardown_sec();
    }
    _device_link_security_free_bootstrap();
    s_security.selected_kind = DEVICE_LINK_SECURITY_VERIFIER_NONE;
    if (s_security.lt_salt != NULL || s_security.lt_verifier != NULL)
    {
        s_security.selected_kind = DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM;
    }
    else if (s_security.pb_salt != NULL || s_security.pb_verifier != NULL)
    {
        /* Closing the QR window returns to public discovery. */
        s_security.selected_kind = DEVICE_LINK_SECURITY_VERIFIER_PUBLIC;
    }
    result = _device_link_security_rebuild();
    _device_link_security_unlock();
    return result;
}

esp_err_t device_link_security_close_public(void)
{
    esp_err_t result = ESP_OK;

    _device_link_security_lock();
    if (!s_security.initialized)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_PUBLIC)
    {
        _device_link_security_teardown_sec();
    }
    _device_link_security_free_public();
    s_security.selected_kind = DEVICE_LINK_SECURITY_VERIFIER_NONE;
    if (s_security.lt_salt != NULL || s_security.lt_verifier != NULL)
    {
        s_security.selected_kind = DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM;
    }
    result = _device_link_security_rebuild();
    _device_link_security_unlock();
    return result;
}

#ifdef UNIT_TEST_HOST
esp_err_t device_link_security_test_derive_public_password(
    const uint8_t instance_id[DEVICE_LINK_SECURITY_PUBLIC_INSTANCE_BYTES],
    char password[DEVICE_LINK_SECURITY_PUBLIC_PASSWORD_BYTES + 1U])
{
    if (instance_id == NULL || password == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return _device_link_security_derive_public_password(
               instance_id, password);
}
#endif

esp_err_t device_link_security_open_public(
    const uint8_t instance_id[DEVICE_LINK_SECURITY_PUBLIC_INSTANCE_BYTES])
{
    if (instance_id == NULL ||
            (instance_id[0] == 0U && instance_id[1] == 0U &&
             instance_id[2] == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    _device_link_security_lock();
    if (!s_security.initialized)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char password[DEVICE_LINK_SECURITY_PUBLIC_PASSWORD_BYTES + 1U];
    const esp_err_t derive_result =
        _device_link_security_derive_public_password(instance_id, password);

    if (derive_result != ESP_OK)
    {
        _device_link_security_unlock();
        return derive_result;
    }
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
                           password, (int)strlen(password),
                           &salt, DEVICE_LINK_SECURITY_SALT_BYTES,
                           &verifier, &verifier_len);

    _device_link_security_zeroize(password, sizeof(password));
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
    if (s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_PUBLIC)
    {
        /* The instance references the old public buffers: replace them
         * only after the teardown. */
        _device_link_security_teardown_sec();
    }
    _device_link_security_free_public();
    s_security.pb_salt = salt;
    s_security.pb_salt_len = DEVICE_LINK_SECURITY_SALT_BYTES;
    s_security.pb_verifier = verifier;
    s_security.pb_verifier_len = (size_t)verifier_len;
    if (s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_PUBLIC ||
            s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_NONE)
    {
        result = _device_link_security_rebuild();
    }
    _device_link_security_unlock();
    return result;
}

esp_err_t device_link_security_select_verifier(
    uint8_t peer_addr_type, const uint8_t *peer_addr, size_t peer_addr_len,
    bool pairing_window_open)
{
    if (peer_addr_len != DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES ||
            !device_link_security_normalized_identity_valid(
                peer_addr_type, peer_addr))
    {
        return ESP_ERR_INVALID_ARG;
    }
    _device_link_security_lock();
    if (!s_security.initialized)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    /* A pending local revoke invalidates every verifier, persistent
     * fail-closed defense: between the journal write and the port's
     * deletion the record may already be erased, but a stale in-memory
     * slot must not keep authenticating the revoked peer. ANY failure to
     * determine the journal state is equally fail-closed: only a definite
     * "no journal" allows a verifier selection. */
    bool revoke_pending = false;

    if (device_link_security_revoke_pending(&revoke_pending) != ESP_OK ||
            revoke_pending)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    device_link_security_verifier_kind_t kind =
        DEVICE_LINK_SECURITY_VERIFIER_NONE;

    /* A committed record whose identity matches the peer always selects
     * the long-term verifier, window open or not: a bound peer keeps its
     * credential during a replacement window. */
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    const esp_err_t load_result =
        device_link_security_load_auth_record(&record);

    if (load_result != ESP_OK && load_result != ESP_ERR_NOT_FOUND)
    {
        _device_link_security_zeroize(&record, sizeof(record));
        _device_link_security_unlock();
        return load_result;
    }
    if (load_result == ESP_OK &&
            record.peer_addr_type == peer_addr_type &&
            memcmp(record.peer_addr, peer_addr,
                   DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES) == 0)
    {
        kind = DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM;
    }
    else if (pairing_window_open && s_security.bt_salt != NULL &&
             s_security.bt_verifier != NULL)
    {
        kind = DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP;
    }
    else if (!pairing_window_open && s_security.pb_salt != NULL &&
             s_security.pb_verifier != NULL)
    {
        /* Public discovery: the advertisement's derived password is the
         * only bootstrap credential outside the QR window. */
        kind = DEVICE_LINK_SECURITY_VERIFIER_PUBLIC;
    }
    _device_link_security_zeroize(&record, sizeof(record));

    const bool changed = kind != s_security.selected_kind;

    s_security.selected_kind = kind;
    s_security.selected_peer_type = peer_addr_type;
    memcpy(s_security.selected_peer_addr, peer_addr,
           DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES);
    if (kind == DEVICE_LINK_SECURITY_VERIFIER_NONE)
    {
        _device_link_security_teardown_sec();
    }
    else if (changed || s_security.sec_inst == NULL)
    {
        const esp_err_t rebuild_result = _device_link_security_rebuild();

        if (rebuild_result != ESP_OK)
        {
            s_security.selected_kind = DEVICE_LINK_SECURITY_VERIFIER_NONE;
            _device_link_security_teardown_sec();
            _device_link_security_unlock();
            return rebuild_result;
        }
    }
    _device_link_security_unlock();
    return ESP_OK;
}

device_link_security_verifier_kind_t device_link_security_selected_verifier(void)
{
    device_link_security_verifier_kind_t kind =
        DEVICE_LINK_SECURITY_VERIFIER_NONE;

    _device_link_security_lock();
    kind = s_security.selected_kind;
    _device_link_security_unlock();
    return kind;
}

esp_err_t device_link_security_handshake(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len)
{
    device_link_security_handshake_result_t handshake_result;

    return device_link_security_handshake_ex(
               input, input_len, output, output_len, &handshake_result);
}

esp_err_t device_link_security_handshake_ex(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len,
    device_link_security_handshake_result_t *handshake_result)
{
    if (input == NULL || output == NULL || output_len == NULL ||
            handshake_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *output = NULL;
    *output_len = 0U;
    memset(handshake_result, 0, sizeof(*handshake_result));
    device_link_security_handshake_stage_t request_stage;
    esp_err_t result = _device_link_security_parse_handshake(
                           input, input_len, false, &request_stage);

    if (result != ESP_OK)
    {
        _device_link_security_lock();
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return result;
    }
    _device_link_security_lock();
    if (!s_security.initialized || s_security.sec_inst == NULL ||
            protocomm_security2.security_req_handler == NULL)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (request_stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD0 &&
            s_security.session_open)
    {
        /* Cmd0 replaces the current session (retire + new epoch), the
         * documented AUTHENTICATED + CMD0 -> HANDSHAKING transition. The
         * adapter is a single global session: true concurrent handshakes
         * from two ACLs cannot occur because the transport admits only
         * one connection, so "concurrent_handshake: BUSY" from the
         * security_adapter fixture is covered by the single-connection
         * GATT gate rather than by a per-ACL session table. */
        _device_link_security_close_session_locked();
    }
    else if (request_stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD1 &&
             (!s_security.session_open || s_security.authenticated))
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_security.session_open)
    {
        if (request_stage != DEVICE_LINK_SECURITY_HANDSHAKE_CMD0)
        {
            _device_link_security_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        if (s_session_epoch >= UINT32_MAX - 1U)
        {
            /* The session generation space is exhausted: UINT32_MAX is
             * reserved as the retired sentinel, so a session opened at
             * the maximum could never be retired safely. */
            _device_link_security_unlock();
            return ESP_ERR_INVALID_STATE;
        }
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
        s_session_epoch++;
    }
    uint8_t *response = NULL;
    ssize_t response_len = 0;
    result = protocomm_security2.security_req_handler(
                 s_security.sec_inst,
                 &s_security.sec_params,
                 s_security.config.session_id,
                 input, (ssize_t)input_len,
                 &response, &response_len, NULL);

    if (result != ESP_OK || response == NULL || response_len <= 0)
    {
        if (response != NULL && response_len > 0)
        {
            _device_link_security_zeroize(response, (size_t)response_len);
        }
        free(response);
        /* A failed handshake (e.g. wrong POP or proof) closes the
         * session so no stale state accumulates. */
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result;
    }
    device_link_security_handshake_stage_t response_stage;

    result = _device_link_security_parse_handshake(
                 response, (size_t)response_len, true, &response_stage);
    if (result != ESP_OK || response_stage != request_stage)
    {
        _device_link_security_zeroize(response, (size_t)response_len);
        free(response);
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result;
    }
    const uint32_t session_epoch = s_session_epoch;
    const protocomm_security_handle_t session_instance = s_security.sec_inst;
    const device_link_security_authenticated_fn authenticated_cb =
        s_security.config.authenticated_cb;
    void *const authenticated_arg = s_security.config.authenticated_arg;

    if (request_stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD1)
    {
        s_security.authenticated = true;
    }
    handshake_result->stage = request_stage;
    handshake_result->authenticated = s_security.authenticated;
    *output = response;
    *output_len = (size_t)response_len;
    _device_link_security_unlock();

    if (request_stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD1 &&
            authenticated_cb != NULL)
    {
        result = authenticated_cb(authenticated_arg);
        _device_link_security_lock();
        const bool session_current =
            s_session_epoch == session_epoch &&
            s_security.sec_inst == session_instance &&
            s_security.authenticated;

        if (result != ESP_OK || !session_current)
        {
            if (session_current)
            {
                _device_link_security_close_session_locked();
            }
            _device_link_security_zeroize(*output, *output_len);
            free(*output);
            *output = NULL;
            *output_len = 0U;
            handshake_result->authenticated = false;
            _device_link_security_unlock();
            return result != ESP_OK ? result : ESP_ERR_INVALID_STATE;
        }
        _device_link_security_unlock();
    }
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
            !s_security.authenticated ||
            s_security.sec_inst == NULL ||
            protocomm_security2.decrypt == NULL ||
            protocomm_security2.encrypt == NULL)
    {
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *plain = NULL;
    ssize_t plain_len = 0;
    const uint32_t session_epoch = s_session_epoch;
    const protocomm_security_handle_t session_instance = s_security.sec_inst;
    const device_link_security_request_fn request_cb =
        s_security.config.request_cb;
    void *const request_arg = s_security.config.request_arg;
    const device_link_security_response_release_fn release_cb =
        s_security.config.response_release_cb;
    void *const release_arg = s_security.config.response_release_arg;
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
     * without re-entering the mutex. The session identity (instance and
     * epoch) is snapshotted under the lock and revalidated before the
     * response is encrypted, so a session replaced during the callback
     * fails closed instead of encrypting under the new session. */
    _device_link_security_unlock();
    uint8_t *plain_response = NULL;
    size_t plain_response_len = 0U;
    result = request_cb(
                 plain, (size_t)plain_len,
                 &plain_response, &plain_response_len,
                 request_arg);
    /* The decrypted request envelope is sensitive: wipe it before the
     * heap block is returned. */
    _device_link_security_zeroize(plain, (size_t)plain_len);
    free(plain);
    _device_link_security_lock();
    if (s_session_epoch != session_epoch ||
            s_security.sec_inst != session_instance)
    {
        /* The session was replaced while the callback ran. */
        _device_link_security_release_request_response(
            plain_response, plain_response_len, release_cb, release_arg);
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (result != ESP_OK)
    {
        _device_link_security_release_request_response(
            plain_response, plain_response_len, release_cb, release_arg);
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return result;
    }
    /* The callback may consume the request itself and emit the response
     * through the transport (ESP_OK with a NULL response); otherwise the
     * plaintext response is encrypted and returned. */
    if (plain_response == NULL || plain_response_len == 0U)
    {
        _device_link_security_release_request_response(
            plain_response, plain_response_len, release_cb, release_arg);
        if (!s_security.session_open || s_security.sec_inst == NULL)
        {
            _device_link_security_unlock();
            return ESP_ERR_INVALID_STATE;
        }
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
        _device_link_security_release_request_response(
            plain_response, plain_response_len, release_cb, release_arg);
        _device_link_security_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    result = protocomm_security2.encrypt(
                 s_security.sec_inst, s_security.config.session_id,
                 plain_response, (ssize_t)plain_response_len,
                 &cipher, &cipher_len);
    _device_link_security_release_request_response(
        plain_response, plain_response_len, release_cb, release_arg);
    if (result != ESP_OK)
    {
        free(cipher);
        _device_link_security_close_session_locked();
        _device_link_security_unlock();
        return result;
    }
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

bool device_link_security_session_open(void)
{
    bool open = false;

    _device_link_security_lock();
    open = s_security.initialized && s_security.session_open &&
           s_security.sec_inst != NULL;
    _device_link_security_unlock();
    return open;
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
    if (s_security.session_open)
    {
        /* The closure retires the session generation; the epoch
         * saturates at the exhausted maximum. */
        if (s_session_epoch < UINT32_MAX)
        {
            s_session_epoch++;
        }
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

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        /* Normalize the storage not-found class for callers. */
        return ESP_ERR_NOT_FOUND;
    }
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
    const esp_err_t result =
        nv_storage_erase_key(DEVICE_LINK_SECURITY_AUTH_STORAGE_KEY);

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        /* Normalize the storage not-found class for callers. */
        return ESP_ERR_NOT_FOUND;
    }
    return result;
}

static const uint8_t s_revoke_marker[1] = {1U};

esp_err_t device_link_security_begin_revoke(void)
{
    return nv_storage_set_blob(DEVICE_LINK_SECURITY_REVOKE_STORAGE_KEY,
                               s_revoke_marker, sizeof(s_revoke_marker));
}

esp_err_t device_link_security_end_revoke(void)
{
    const esp_err_t result =
        nv_storage_erase_key(DEVICE_LINK_SECURITY_REVOKE_STORAGE_KEY);

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        /* Normalize the storage not-found class for callers. */
        return ESP_OK;
    }
    return result;
}

esp_err_t device_link_security_revoke_pending(bool *pending)
{
    uint8_t marker[sizeof(s_revoke_marker)] = {0U};
    size_t size = sizeof(marker);
    esp_err_t result;

    if (pending == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *pending = false;
    result = nv_storage_get_blob(DEVICE_LINK_SECURITY_REVOKE_STORAGE_KEY,
                                 marker, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (result != ESP_OK)
    {
        return result;
    }
    if (size != sizeof(s_revoke_marker) ||
            memcmp(marker, s_revoke_marker, sizeof(s_revoke_marker)) != 0)
    {
        _device_link_security_zeroize(marker, sizeof(marker));
        return ESP_ERR_INVALID_STATE;
    }
    *pending = true;
    _device_link_security_zeroize(marker, sizeof(marker));
    return ESP_OK;
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

    if (load_result == ESP_ERR_INVALID_STATE)
    {
        /* A present but corrupt or schema-mismatched record (for example
         * written by an older firmware revision) can never authenticate a
         * session. Clear it and treat the device as unbound: startup stays
         * available and a fresh bind overwrites the slot. The recovery
         * path keeps its STORAGE/INTERNAL distinction via load_auth_record,
         * so only this startup-oriented loader normalizes the damage. */
        LOG_W("auth record invalid; erasing and continuing as unbound");
        (void)device_link_security_erase_auth_record();
    }
    if (load_result == ESP_ERR_NOT_FOUND ||
            load_result == ESP_ERR_INVALID_STATE)
    {
        const bool rebuild =
            s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM ||
            s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_NONE;

        if (s_security.selected_kind ==
                DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM)
        {
            _device_link_security_teardown_sec();
        }
        _device_link_security_free_long_term();
        if (rebuild)
        {
            s_security.selected_kind = DEVICE_LINK_SECURITY_VERIFIER_NONE;
            if (s_security.pb_salt != NULL || s_security.pb_verifier != NULL)
            {
                s_security.selected_kind =
                    DEVICE_LINK_SECURITY_VERIFIER_PUBLIC;
            }
            const esp_err_t result = _device_link_security_rebuild();

            _device_link_security_unlock();
            return result == ESP_OK ? ESP_ERR_NOT_FOUND : result;
        }
        _device_link_security_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (load_result != ESP_OK)
    {
        _device_link_security_unlock();
        return load_result;
    }
    /* Replace the long-term slot material. The bootstrap slot and any
     * instance currently configured from it are preserved; the rebuild
     * below only reruns when the long-term slot backs the active
     * selection. */
    char *salt = malloc(DEVICE_LINK_SECURITY_AUTH_SALT_BYTES);
    char *verifier = malloc(DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES);
    if (salt == NULL || verifier == NULL)
    {
        free(salt);
        free(verifier);
        _device_link_security_zeroize(&record, sizeof(record));
        _device_link_security_unlock();
        return ESP_ERR_NO_MEM;
    }
    memcpy(salt, record.salt, DEVICE_LINK_SECURITY_AUTH_SALT_BYTES);
    memcpy(verifier, record.verifier,
           DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES);
    _device_link_security_zeroize(&record, sizeof(record));
    const bool rebuild =
        s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM ||
        s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_NONE;

    if (s_security.selected_kind == DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM)
    {
        /* The instance references the old long-term buffers: replace them
         * only after the teardown. */
        _device_link_security_teardown_sec();
    }
    _device_link_security_free_long_term();
    s_security.lt_salt = salt;
    s_security.lt_salt_len = DEVICE_LINK_SECURITY_AUTH_SALT_BYTES;
    s_security.lt_verifier = verifier;
    s_security.lt_verifier_len = DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES;
    if (rebuild)
    {
        s_security.selected_kind = DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM;
        const esp_err_t result = _device_link_security_rebuild();

        _device_link_security_unlock();
        return result;
    }
    _device_link_security_unlock();
    return ESP_OK;
}
