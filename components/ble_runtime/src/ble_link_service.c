#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_random.h"

#ifdef UNIT_TEST_HOST
    #include "ble_link_codec.h"
    #include "ble_link_dispatcher.h"
#endif
#include "ble_link_events.h"
#include "ble_link_reassembler.h"
#include "ble_link_service.h"
#include "ble_link_session.h"
#include "ble_link_state.h"

#include "device_link_core.h"
#include "device_link_digest.h"
#include "device_link_operation.h"
#include "device_link_protocol.h"
#include "device_link_router.h"
#include "device_link_tlv.h"
#include "device_link_wire.h"
#include "device_link_security_auth.h"

#define DBG_TAG "ble_link_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define BLE_LINK_SERVICE_PROTOCOL_MAJOR 2U
#define BLE_LINK_SERVICE_PREFERRED_ATT_MTU 498U
#define BLE_LINK_SERVICE_CONTROL_MAX_BYTES 4096U
#define BLE_LINK_SERVICE_SESSION_MAX_BYTES 1024U
#define BLE_LINK_SERVICE_DEVICE_AUTH_ID_BYTES \
    DEVICE_LINK_SECURITY_AUTH_ID_BYTES
#define BLE_LINK_SERVICE_AUTH_ID_BYTES DEVICE_LINK_SECURITY_AUTH_ID_BYTES
#define BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES \
    DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES
#define BLE_LINK_SERVICE_PROTOCOMM_PATCH_VERSION 1U
#define BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS 4U
#define BLE_LINK_SERVICE_RETRY_BASE_MS 100U
#define BLE_LINK_SERVICE_RETRY_MAX_MS 1000U
/* A completion bridge event may arrive after the manager admitted the
 * operation but before the Core v2 table admission ran (SMP publisher
 * context). Terminal completions are retained for this window and merged
 * by the next async_operation_start for the same owner id. */
#define BLE_LINK_SERVICE_DEFERRED_COMPLETION_TTL_MS 1000U
#define BLE_LINK_SERVICE_MAX_DOMAINS 4U
/* OperationSummary (core v2) freezes maximum_encoded_bytes at 128. */
#define BLE_LINK_SERVICE_OPERATION_SUMMARY_MAX_BYTES 128U

typedef enum ble_link_service_cleanup_action
{
    BLE_LINK_SERVICE_CLEANUP_DISCARD = 0,
    BLE_LINK_SERVICE_CLEANUP_PROMOTE,
} ble_link_service_cleanup_action_t;

typedef struct ble_link_service_retry
{
    uint8_t attempts;
    TickType_t retry_not_before;
} ble_link_service_retry_t;

typedef enum ble_link_authorization_phase
{
    BLE_LINK_AUTH_PHASE_IDLE = 0,
    BLE_LINK_AUTH_PHASE_PREPARED,
    BLE_LINK_AUTH_PHASE_COMMIT_PROBED,
    BLE_LINK_AUTH_PHASE_LOCALLY_CONFIRMED,
    BLE_LINK_AUTH_PHASE_COMMITTING,
    BLE_LINK_AUTH_PHASE_COMMITTED,
} ble_link_authorization_phase_t;

typedef struct ble_link_service
{
    uint64_t boot_id;
    ble_link_service_output_t output;
    void *output_arg;
    uint32_t execution_generation;
    uint16_t outbound_frame_id;
    struct
    {
        bool active;                 /**< A multi-fragment stream is pending. */
        uint8_t *payload;            /**< Points into stream_storage while used. */
        size_t payload_len;
        size_t next_offset;          /**< Next fragment start offset. */
        uint32_t att_mtu;
        ble_link_service_tx_channel_t channel;
        uint16_t frame_id;
        uint32_t flow_id;
    } stream;
    uint8_t stream_storage[BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES];
    struct
    {
        bool pending;
        uint32_t flow_id;
        bool is_last;
    } completion;
    struct
    {
        bool active;
        uint64_t request_id;
        uint32_t generation;
        uint32_t att_mtu;
        ble_link_service_tx_channel_t channel;
    } deferred_busy;
    struct
    {
        bool active; /**< Reserved for a future event capability; never published in Core v2. */
        uint32_t generation;
    } subscriber;
    struct
    {
        ble_link_authorization_phase_t phase;
        uint64_t authorization_txn_id;
        uint64_t confirmation_token;
        uint32_t operation_token;
        uint32_t connection_generation;
        uint32_t security_epoch; /**< Security 2 epoch at Prepare; a Commit
                                  *  from another epoch is INVALID_ARGUMENT. */
        uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
        uint8_t application_password[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
        uint8_t device_auth_id[BLE_LINK_SERVICE_AUTH_ID_BYTES];
        uint16_t requested_permissions[DEVICE_LINK_MAX_PERMISSIONS];
        size_t requested_permission_count;
        uint32_t deadline_ms; /**< Absolute expiry in the adapter clock. */
    } auth_txn;
    struct
    {
        bool active; /**< Consumed once, after the commit response is
                      *  encrypted; generation-scoped. */
        uint32_t generation;
    } lt_switch;
    bool lt_install_pending; /**< Survives session teardown: a committed
                              *  record must install its verifier even if
                              *  the immediate load failed. */
    struct
    {
        bool active; /**< Close both Security 2 layers after the response
                      *  of this generation is encrypted (terminal
                      *  pre-durable error). */
        uint32_t generation;
    } close_after_encrypt;
    struct
    {
        bool active; /**< One complete Cmd0 waits for the old indication. */
        uint32_t generation;
        uint32_t old_flow_id;
        ble_link_service_facts_t facts;
        size_t message_len;
        uint8_t message[BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES];
    } delayed_cmd0;
    struct
    {
        ble_link_work_t *work; /**< Accepted handshake awaiting its worker. */
    } queued_handshake;
    struct
    {
        bool active;
        ble_link_service_cleanup_action_t action;
        ble_link_operation_identity_t identity;
        bool terminate_conn;
        ble_link_service_retry_t retry;
    } cleanup[BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS];
    struct
    {
        bool active;
        ble_link_operation_identity_t identity;
        ble_link_service_retry_t retry;
    } remote_replacement;
    struct
    {
        bool active; /**< Terminal Commit replay retained for this ACL. */
        uint64_t authorization_txn_id;
        uint32_t connection_generation;
        uint16_t conn_handle;
        uint8_t peer_addr_type;
        uint8_t peer_addr[6];
        uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
        uint8_t device_auth_id[BLE_LINK_SERVICE_AUTH_ID_BYTES];
    } committed_replay;
    ble_link_service_facts_t current_facts;
    ble_link_service_rx_channel_t current_channel;
    const ble_link_security_ops_t *security;
    bool handshake_active;
    bool sec2_opened;
    unsigned int pending_transactions;
    ble_link_service_wake_fn_t wake;
    void *wake_arg;
    uint8_t response_envelope[BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES];
    size_t response_envelope_len;
    uint8_t v2_response[BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES];
    bool v2_response_in_use;
    device_link_core_t v2_core;
    device_link_domain_descriptor_t v2_domains[BLE_LINK_SERVICE_MAX_DOMAINS];
    size_t v2_domain_count;
    device_link_router_t v2_router;
    device_link_operation_table_t v2_operations;
    struct
    {
        bool active;
        uint64_t owner_id;
        uint64_t deadline_ms;
        device_link_operation_state_t state;
        device_link_status_t status;
        uint8_t result[DEVICE_LINK_OPERATION_RESULT_BYTES];
        size_t result_len;
    } deferred_completion;
    device_link_call_replay_t v2_replay[DEVICE_LINK_REPLAY_SLOTS];
    uint8_t v2_replay_response[DEVICE_LINK_REPLAY_SLOTS]
    [BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES];
    bool v2_ready;
    bool v2_dispatch_active;
} ble_link_service_t;

typedef struct ble_link_ingress
{
    ble_link_reassembler_t reassembler[2];
    ble_link_reassembly_disposition_t last_disposition[2];
    uint8_t session_buffer[BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES];
    uint8_t control_buffer[BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES];
    uint32_t generation;
    uint16_t conn_handle;
    uint32_t epoch;
    bool retired;
    bool exhausted;
} ble_link_ingress_t;

struct ble_link_work
{
    bool in_use;
    ble_link_service_facts_t facts;
    ble_link_service_rx_channel_t channel;
    uint32_t epoch;
    uint8_t transport_type;
    size_t message_len;
    uint8_t message[BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES];
};

static ble_link_work_t s_work_pool[BLE_LINK_SERVICE_WORK_SLOTS];

static SemaphoreHandle_t s_service_mutex;
static StaticSemaphore_t s_service_mutex_control;
static uint64_t s_confirmation_token_sequence;
static uint32_t s_operation_token_sequence;
static device_link_domain_descriptor_t s_optional_domains[
    BLE_LINK_SERVICE_MAX_DOMAINS - 1U];
static size_t s_optional_domain_count;

static device_link_status_t _ble_link_service_v2_method(
    const device_link_request_context_t *context,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len,
    void *arg);

static void _ble_link_service_lock(void)
{
    if (s_service_mutex != NULL)
    {
        (void)xSemaphoreTakeRecursive(s_service_mutex, portMAX_DELAY);
    }
}

static void _ble_link_service_unlock(void)
{
    if (s_service_mutex != NULL)
    {
        (void)xSemaphoreGiveRecursive(s_service_mutex);
    }
}

static void _ble_link_service_clear_auth_txn(void);
static void _ble_link_service_abort_session(uint32_t generation);
static void _ble_link_service_discard_provisional_bond(
    uint32_t generation, bool terminate_conn);
static void _ble_link_service_promote_provisional_bond(uint32_t generation);
static esp_err_t _ble_link_service_pump_cleanup_locked(void);
static esp_err_t _ble_link_service_process_remote_replacement_locked(void);
static void _ble_link_service_zeroize(void *data, size_t size);
static void _ble_link_service_reset_ingress(void);
static void _ble_link_service_clear_delayed_cmd0(void);
static void _ble_link_service_clear_committed_replay(void);
static void _ble_link_service_reset_v2_replay(void);
static void _ble_link_service_clear_session_state_locked(bool retire_acl);
static esp_err_t _ble_link_service_retire_logical_session(
    uint32_t generation, bool clear_response);
static esp_err_t _ble_link_service_process_handshake_locked(
    const ble_link_service_facts_t *facts,
    const uint8_t *message, size_t message_len,
    device_link_security_handshake_stage_t stage,
    bool session_already_retired);
#ifdef UNIT_TEST_HOST
static esp_err_t _ble_link_service_take_response(
    uint8_t **response, size_t *response_len);
static void _ble_link_service_build_response(
    uint64_t request_id, uint32_t error,
    ble_link_codec_response_tag_t body_tag,
    const uint8_t *body, size_t body_len);
#endif
static bool _ble_link_service_emit_protected(
    const uint8_t *message, size_t message_len, uint8_t transport_type,
    uint32_t att_mtu, ble_link_service_tx_channel_t channel);
static esp_err_t _ble_link_service_accept_locked(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const uint8_t *value, size_t len,
    ble_link_work_t **out_work);
static esp_err_t _ble_link_service_execute_locked(ble_link_work_t *work);
static esp_err_t _ble_link_service_publish_link_state_locked(
    const ble_link_service_facts_t *facts,
    const ble_link_state_snapshot_t *link_state);
static uint64_t _ble_link_service_v2_now_ms(void);

static ble_link_service_t s_service;
static ble_link_ingress_t s_ingress;
static uint32_t s_flow_id_counter;

static bool _ble_link_service_retry_due(
    const ble_link_service_retry_t *retry)
{
    return retry == NULL || retry->attempts == 0U ||
           (int32_t)(xTaskGetTickCount() - retry->retry_not_before) >= 0;
}

static void _ble_link_service_retry_failed(
    ble_link_service_retry_t *retry)
{
    if (retry == NULL)
    {
        return;
    }
    uint32_t delay_ms = BLE_LINK_SERVICE_RETRY_BASE_MS;

    for (uint8_t shift = 0U;
            shift < retry->attempts &&
            delay_ms < BLE_LINK_SERVICE_RETRY_MAX_MS;
            ++shift)
    {
        delay_ms *= 2U;
        if (delay_ms > BLE_LINK_SERVICE_RETRY_MAX_MS)
        {
            delay_ms = BLE_LINK_SERVICE_RETRY_MAX_MS;
        }
    }
    if (retry->attempts < UINT8_MAX)
    {
        ++retry->attempts;
    }
    retry->retry_not_before = xTaskGetTickCount() +
                              pdMS_TO_TICKS(delay_ms);
}

static uint32_t _ble_link_service_retry_remaining_ms(
    const ble_link_service_retry_t *retry)
{
    if (retry == NULL || retry->attempts == 0U)
    {
        return 0U;
    }
    const TickType_t now = xTaskGetTickCount();

    if ((int32_t)(now - retry->retry_not_before) >= 0)
    {
        return 0U;
    }
    const TickType_t remaining_ticks = retry->retry_not_before - now;

    return (uint32_t)(((uint64_t)remaining_ticks * 1000U +
                       configTICK_RATE_HZ - 1U) /
                      configTICK_RATE_HZ);
}

static bool _ble_link_service_same_operation(
    const ble_link_operation_identity_t *left,
    const ble_link_operation_identity_t *right)
{
    return left != NULL && right != NULL &&
           left->generation == right->generation &&
           left->security_epoch == right->security_epoch &&
           left->flow_id == right->flow_id &&
           left->token == right->token &&
           left->conn_handle == right->conn_handle;
}

static uint32_t _ble_link_service_next_operation_token(void)
{
    if (s_operation_token_sequence == UINT32_MAX)
    {
        return 0U;
    }
    s_operation_token_sequence++;
    return s_operation_token_sequence;
}

static ble_link_operation_identity_t _ble_link_service_cleanup_identity(
    uint32_t generation, ble_link_operation_kind_t kind)
{
    uint32_t token = 0U;

    if (s_service.auth_txn.connection_generation == generation)
    {
        token = s_service.auth_txn.operation_token;
    }
    if (token == 0U)
    {
        token = _ble_link_service_next_operation_token();
    }
    return (ble_link_operation_identity_t)
    {
        .generation = generation,
        .security_epoch = s_service.current_facts.security_epoch,
        .token = token,
        .kind = kind,
        .conn_handle = s_service.current_facts.conn_handle,
    };
}

static esp_err_t _ble_link_service_dispatch_cleanup_locked(
    size_t index)
{
    if (index >= BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS ||
            !s_service.cleanup[index].active || s_service.security == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_ble_link_service_retry_due(&s_service.cleanup[index].retry))
    {
        return ESP_ERR_NOT_FINISHED;
    }
    esp_err_t result = ESP_ERR_NOT_SUPPORTED;

    if (s_service.cleanup[index].action ==
            BLE_LINK_SERVICE_CLEANUP_DISCARD &&
            s_service.security->discard_provisional_bond != NULL)
    {
        result = s_service.security->discard_provisional_bond(
                     &s_service.cleanup[index].identity,
                     s_service.cleanup[index].terminate_conn);
    }
    else if (s_service.cleanup[index].action ==
             BLE_LINK_SERVICE_CLEANUP_PROMOTE &&
             s_service.security->promote_provisional_bond != NULL)
    {
        result = s_service.security->promote_provisional_bond(
                     &s_service.cleanup[index].identity);
    }
    if (result == ESP_OK || result == ESP_ERR_NOT_FOUND)
    {
        memset(&s_service.cleanup[index], 0,
               sizeof(s_service.cleanup[index]));
        return ESP_OK;
    }
    _ble_link_service_retry_failed(&s_service.cleanup[index].retry);
    return result;
}

static esp_err_t _ble_link_service_retain_cleanup_locked(
    ble_link_service_cleanup_action_t action,
    const ble_link_operation_identity_t *identity, bool terminate_conn)
{
    if (identity == NULL || identity->generation == 0U ||
            identity->token == 0U || s_service.security == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    size_t selected = BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS;
    size_t empty = BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS;
    bool coalesced_discard = false;

    for (size_t i = 0U; i < BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS; ++i)
    {
        if (action == BLE_LINK_SERVICE_CLEANUP_DISCARD &&
                s_service.cleanup[i].active &&
                s_service.cleanup[i].action ==
                BLE_LINK_SERVICE_CLEANUP_DISCARD &&
                s_service.cleanup[i].identity.generation ==
                identity->generation &&
                s_service.cleanup[i].identity.conn_handle ==
                identity->conn_handle)
        {
            /* Repeated terminal paths may run after auth_txn was cleared and
             * therefore mint a new token. They still name the same physical
             * provisional bond. Keep the first immutable identity so one
             * failing delete cannot fill every retained slot. */
            selected = i;
            coalesced_discard = true;
            break;
        }
        if (s_service.cleanup[i].active &&
                _ble_link_service_same_operation(
                    &s_service.cleanup[i].identity, identity))
        {
            selected = i;
            break;
        }
        if (!s_service.cleanup[i].active &&
                empty == BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS)
        {
            empty = i;
        }
    }
    if (selected == BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS)
    {
        selected = empty;
    }
    if (selected == BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS)
    {
        return ESP_ERR_NO_MEM;
    }
    const bool obligation_changed =
        !s_service.cleanup[selected].active ||
        s_service.cleanup[selected].action != action ||
        (!coalesced_discard &&
         !ble_link_operation_identity_equal(
             &s_service.cleanup[selected].identity, identity)) ||
        (!s_service.cleanup[selected].terminate_conn && terminate_conn);

    if (obligation_changed)
    {
        memset(&s_service.cleanup[selected].retry, 0,
               sizeof(s_service.cleanup[selected].retry));
    }
    s_service.cleanup[selected].active = true;
    if (!coalesced_discard)
    {
        s_service.cleanup[selected].action = action;
        s_service.cleanup[selected].identity = *identity;
    }
    s_service.cleanup[selected].terminate_conn =
        s_service.cleanup[selected].terminate_conn || terminate_conn;
    const esp_err_t result =
        _ble_link_service_dispatch_cleanup_locked(selected);

    if (obligation_changed && result != ESP_OK && s_service.wake != NULL)
    {
        /* One deadline-change hint is enough. A failed retry never wakes the
         * owner again; its retained absolute deadline drives the next try. */
        s_service.wake(s_service.wake_arg);
    }
    return result;
}

static void _ble_link_service_discard_provisional_bond(
    uint32_t generation, bool terminate_conn)
{
    if (s_service.security == NULL ||
            s_service.security->discard_provisional_bond == NULL)
    {
        return;
    }
    const ble_link_operation_identity_t identity =
        _ble_link_service_cleanup_identity(
            generation, BLE_LINK_OPERATION_PROVISIONAL_DISCARD);
    const esp_err_t result = _ble_link_service_retain_cleanup_locked(
                                 BLE_LINK_SERVICE_CLEANUP_DISCARD,
                                 &identity, terminate_conn);

    if (result != ESP_OK)
    {
        LOG_W("provisional bond cleanup retained result=%d", result);
    }
}

static void _ble_link_service_promote_provisional_bond(uint32_t generation)
{
    if (s_service.security == NULL ||
            s_service.security->promote_provisional_bond == NULL)
    {
        return;
    }
    const ble_link_operation_identity_t identity =
        _ble_link_service_cleanup_identity(
            generation, BLE_LINK_OPERATION_PROVISIONAL_PROMOTE);
    const esp_err_t result = _ble_link_service_retain_cleanup_locked(
                                 BLE_LINK_SERVICE_CLEANUP_PROMOTE,
                                 &identity, false);

    if (result != ESP_OK)
    {
        LOG_W("provisional bond promotion retained result=%d", result);
    }
}

static esp_err_t _ble_link_service_pump_cleanup_locked(void)
{
    esp_err_t first_error = ESP_OK;

    for (size_t i = 0U; i < BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS; ++i)
    {
        if (!s_service.cleanup[i].active)
        {
            continue;
        }
        const esp_err_t result =
            _ble_link_service_dispatch_cleanup_locked(i);

        if (result != ESP_OK && first_error == ESP_OK)
        {
            first_error = result;
        }
    }
    return first_error;
}

static esp_err_t _ble_link_service_process_remote_replacement_locked(void)
{
    if (!s_service.remote_replacement.active)
    {
        return ESP_OK;
    }
    if (s_service.security == NULL ||
            s_service.security->replace_authorization == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_ble_link_service_retry_due(&s_service.remote_replacement.retry))
    {
        return ESP_ERR_NOT_FINISHED;
    }
    const ble_link_operation_identity_t identity =
        s_service.remote_replacement.identity;
    const esp_err_t result = s_service.security->replace_authorization(
                                 &identity);

    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND)
    {
        _ble_link_service_retry_failed(&s_service.remote_replacement.retry);
        return result;
    }
    memset(&s_service.remote_replacement, 0,
           sizeof(s_service.remote_replacement));
    _ble_link_service_abort_session(identity.generation);
    return ESP_OK;
}

static uint32_t _ble_link_service_next_flow_id(void)
{
    if (s_flow_id_counter == UINT32_MAX)
    {
        return 0U;
    }
    s_flow_id_counter++;
    return s_flow_id_counter;
}

bool ble_link_service_response_in_flight(void)
{
    _ble_link_service_lock();
    const bool in_flight = s_service.pending_transactions > 0U;

    _ble_link_service_unlock();
    return in_flight;
}

static void _ble_link_service_stream_free(void)
{
    _ble_link_service_zeroize(s_service.stream_storage,
                              sizeof(s_service.stream_storage));
    s_service.stream.payload = NULL;
    s_service.stream.payload_len = 0U;
    s_service.stream.next_offset = 0U;
    s_service.stream.active = false;
    s_service.stream.flow_id = 0U;
}

static bool _ble_link_service_stream_emit_locked(void);
static bool _ble_link_service_emit_deferred_busy_locked(void);

esp_err_t ble_link_service_response_completed(uint32_t flow_id, bool is_last)
{
    esp_err_t result = ESP_ERR_NOT_FOUND;

    _ble_link_service_lock();
    if (flow_id != 0U && s_service.stream.flow_id == flow_id &&
            s_service.pending_transactions > 0U &&
            !s_service.completion.pending)
    {
        s_service.completion.pending = true;
        s_service.completion.flow_id = flow_id;
        s_service.completion.is_last = is_last;
        result = ESP_OK;
    }
    if (result == ESP_OK && s_service.wake != NULL)
    {
        /* Prompt the owner to emit the next fragment immediately. The
         * callback is non-blocking; only the owner submits the next frame.
         * Invoke it under the service lock so disabling the callback is a
         * teardown barrier: once set_worker_wake(NULL) returns, no stale
         * callback can still target an already deleted owner task. */
        s_service.wake(s_service.wake_arg);
    }
    _ble_link_service_unlock();
    return result;
}

void ble_link_service_set_worker_wake(
    ble_link_service_wake_fn_t wake, void *arg)
{
    _ble_link_service_lock();
    s_service.wake = wake;
    s_service.wake_arg = arg;
    _ble_link_service_unlock();
}

void ble_link_service_wake_owner(void)
{
    _ble_link_service_lock();
    const ble_link_service_wake_fn_t wake = s_service.wake;
    void *const wake_arg = s_service.wake_arg;

    _ble_link_service_unlock();
    if (wake != NULL)
    {
        wake(wake_arg);
    }
}

esp_err_t ble_link_service_pump_tx(void)
{
    esp_err_t result = ESP_OK;

    _ble_link_service_lock();
    const esp_err_t replacement_result =
        _ble_link_service_process_remote_replacement_locked();
    const esp_err_t cleanup_result =
        _ble_link_service_pump_cleanup_locked();

    if (replacement_result != ESP_OK)
    {
        result = replacement_result;
    }
    else if (cleanup_result != ESP_OK)
    {
        result = cleanup_result;
    }
    for (;;)
    {
        if (!s_service.completion.pending)
        {
            break;
        }
        const uint32_t flow_id = s_service.completion.flow_id;
        const bool is_last = s_service.completion.is_last;

        s_service.completion.pending = false;
        s_service.completion.flow_id = 0U;
        s_service.completion.is_last = false;
        if (flow_id == 0U || s_service.stream.flow_id != flow_id ||
                s_service.pending_transactions == 0U)
        {
            continue;
        }
        if (s_service.delayed_cmd0.active &&
                flow_id == s_service.delayed_cmd0.old_flow_id)
        {
            /* Any successful confirmation of the submitted old fragment
             * releases the one indication slot. The retired response is
             * never continued, even when that fragment was not its last. */
            const ble_link_service_facts_t facts =
                s_service.delayed_cmd0.facts;
            const size_t message_len =
                s_service.delayed_cmd0.message_len;
            const uint8_t *message = s_service.delayed_cmd0.message;

            s_service.delayed_cmd0.active = false;
            s_service.pending_transactions = 0U;
            _ble_link_service_stream_free();
            result = _ble_link_service_process_handshake_locked(
                         &facts, message, message_len,
                         DEVICE_LINK_SECURITY_HANDSHAKE_CMD0, true);
            _ble_link_service_clear_delayed_cmd0();
            if (result != ESP_OK)
            {
                break;
            }
            continue;
        }
        if (s_service.stream.active)
        {
            if (is_last || !_ble_link_service_stream_emit_locked())
            {
                result = ESP_ERR_INVALID_STATE;
                _ble_link_service_abort_session(
                    s_service.current_facts.connection_generation);
                break;
            }
            continue;
        }
        if (!is_last)
        {
            result = ESP_ERR_INVALID_STATE;
            _ble_link_service_abort_session(
                s_service.current_facts.connection_generation);
            break;
        }
        _ble_link_service_stream_free();
        s_service.pending_transactions--;
        if (s_service.pending_transactions == 0U &&
                s_service.deferred_busy.active &&
                !_ble_link_service_emit_deferred_busy_locked())
        {
            result = ESP_ERR_INVALID_STATE;
            _ble_link_service_abort_session(
                s_service.current_facts.connection_generation);
            break;
        }
    }
    _ble_link_service_unlock();
    return result;
}

uint32_t ble_link_service_retained_retry_remaining_ms(void)
{
    uint32_t remaining_ms = UINT32_MAX;

    _ble_link_service_lock();
    for (size_t i = 0U; i < BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS; ++i)
    {
        if (!s_service.cleanup[i].active)
        {
            continue;
        }
        const uint32_t slot_remaining =
            _ble_link_service_retry_remaining_ms(
                &s_service.cleanup[i].retry);

        if (slot_remaining < remaining_ms)
        {
            remaining_ms = slot_remaining;
        }
    }
    if (s_service.remote_replacement.active)
    {
        const uint32_t replacement_remaining =
            _ble_link_service_retry_remaining_ms(
                &s_service.remote_replacement.retry);

        if (replacement_remaining < remaining_ms)
        {
            remaining_ms = replacement_remaining;
        }
    }
    _ble_link_service_unlock();
    return remaining_ms;
}

bool ble_link_service_retained_cleanup_pending(void)
{
    bool pending = false;

    _ble_link_service_lock();
    for (size_t i = 0U; i < BLE_LINK_SERVICE_CLEANUP_OBLIGATIONS; ++i)
    {
        if (s_service.cleanup[i].active)
        {
            pending = true;
            break;
        }
    }
    pending = pending || s_service.remote_replacement.active;
    _ble_link_service_unlock();
    return pending;
}

esp_err_t ble_link_service_register_remote_replacement(
    const ble_link_operation_identity_t *identity)
{
    if (identity == NULL || identity->generation == 0U ||
            identity->token == 0U ||
            identity->kind != BLE_LINK_OPERATION_REMOTE_REPLACEMENT ||
            identity->conn_handle == UINT16_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_service_lock();
    if (s_service.security == NULL ||
            s_service.security->replace_authorization == NULL)
    {
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.auth_txn.phase == BLE_LINK_AUTH_PHASE_COMMIT_PROBED ||
            s_service.auth_txn.phase ==
            BLE_LINK_AUTH_PHASE_LOCALLY_CONFIRMED ||
            s_service.auth_txn.phase == BLE_LINK_AUTH_PHASE_COMMITTING)
    {
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.current_facts.connection_generation != 0U &&
            (s_service.current_facts.connection_generation !=
             identity->generation ||
             s_service.current_facts.conn_handle != identity->conn_handle))
    {
        _ble_link_service_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (s_service.remote_replacement.active &&
            !ble_link_operation_identity_equal(
                &s_service.remote_replacement.identity, identity))
    {
        _ble_link_service_unlock();
        return ESP_ERR_NO_MEM;
    }
    /* A durable Commit is terminal: it no longer outranks a later remote
     * replacement attempt on the same ACL. The replacement registration is
     * the cutover point, so the old Commit result must not remain replayable
     * while the retained replacement obligation is retried. */
    _ble_link_service_clear_committed_replay();
    if (s_service.auth_txn.phase == BLE_LINK_AUTH_PHASE_COMMITTED)
    {
        _ble_link_service_clear_auth_txn();
    }
    const bool obligation_changed = !s_service.remote_replacement.active;

    if (obligation_changed)
    {
        memset(&s_service.remote_replacement, 0,
               sizeof(s_service.remote_replacement));
        s_service.remote_replacement.active = true;
        s_service.remote_replacement.identity = *identity;
    }
    const ble_link_service_wake_fn_t wake = s_service.wake;
    void *const wake_arg = s_service.wake_arg;

    _ble_link_service_unlock();
    if (obligation_changed && wake != NULL)
    {
        wake(wake_arg);
    }
    return ESP_OK;
}

void ble_link_service_abort_transactions(void)
{
    _ble_link_service_lock();
    _ble_link_service_discard_provisional_bond(
        s_service.current_facts.connection_generation, true);
    s_service.pending_transactions = 0U;
    s_service.completion.pending = false;
    s_service.completion.flow_id = 0U;
    s_service.completion.is_last = false;
    s_service.deferred_busy.active = false;
    _ble_link_service_clear_delayed_cmd0();
    _ble_link_service_stream_free();
    s_service.queued_handshake.work = NULL;
    _ble_link_service_unlock();
}

bool ble_link_service_delayed_replacement_pending(uint32_t generation)
{
    bool pending;

    _ble_link_service_lock();
    pending = s_service.delayed_cmd0.active &&
              s_service.delayed_cmd0.generation == generation;
    _ble_link_service_unlock();
    return pending;
}

/**
 * @brief Whether a generation may clear the current session state.
 *
 * A generation is authoritative when it matches the executed facts OR the
 * ingress slot: a new generation's first partial frame advances the
 * ingress generation before any work executes, so its idle timeout must be
 * able to abort the stale session state it carries. With no facts at all
 * (nothing ever fed or executed) every generation is treated as current
 * because there is nothing to protect. A terminally retired ingress is never
 * current again. Only a generation older than both active recorded
 * generations is stale and ignored.
 */
static bool _ble_link_service_generation_current(uint32_t generation)
{
    const uint32_t facts_generation =
        s_service.current_facts.connection_generation;
    const uint32_t ingress_generation = s_ingress.generation;

    if (facts_generation == 0U && ingress_generation == 0U)
    {
        return true;
    }
    return generation == facts_generation ||
           (!s_ingress.retired && generation == ingress_generation);
}

/**
 * @brief Abort the current control/session flow: clear the reassembly
 * slot, subscriber, authorization transaction, and close the Security 2
 * session (framing contract).
 */
static void _ble_link_service_abort_session(uint32_t generation)
{
    if (!_ble_link_service_generation_current(generation))
    {
        return;
    }
    _ble_link_service_discard_provisional_bond(generation, true);
    (void)ble_link_session_security2_close_current(generation);
    s_service.pending_transactions = 0U;
    s_service.completion.pending = false;
    s_service.completion.flow_id = 0U;
    s_service.completion.is_last = false;
    s_service.deferred_busy.active = false;
    _ble_link_service_clear_delayed_cmd0();
    _ble_link_service_reset_ingress();
    s_service.subscriber.active = false;
    s_service.handshake_active = false;
    s_service.sec2_opened = false;
    s_service.lt_switch.active = false;
    s_service.close_after_encrypt.active = false;
    _ble_link_service_clear_auth_txn();
    _ble_link_service_stream_free();
#ifdef UNIT_TEST_HOST
    ble_link_dispatcher_clear_session();
#endif
    if (s_service.security != NULL)
    {
        s_service.security->close_session();
    }
}

#ifdef UNIT_TEST_HOST
static esp_err_t _ble_link_service_take_response(
    uint8_t **response, size_t *response_len)
{
    if (s_service.response_envelope_len == 0U)
    {
        *response = NULL;
        *response_len = 0U;
        return ESP_OK;
    }
    uint8_t *copy = malloc(s_service.response_envelope_len);

    if (copy == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, s_service.response_envelope,
           s_service.response_envelope_len);
    *response = copy;
    *response_len = s_service.response_envelope_len;
    return ESP_OK;
}
#endif

void ble_link_service_release_plaintext(
    uint8_t *response, size_t response_len)
{
    if (response == NULL)
    {
        return;
    }
    if (response == s_service.v2_response)
    {
        _ble_link_service_lock();
        _ble_link_service_zeroize(s_service.v2_response,
                                  sizeof(s_service.v2_response));
        s_service.v2_response_in_use = false;
        _ble_link_service_unlock();
        return;
    }
    _ble_link_service_zeroize(response, response_len);
    free(response);
}

static void _ble_link_service_clear_auth_txn(void)
{
    _ble_link_service_zeroize(&s_service.auth_txn.application_password,
                              sizeof(s_service.auth_txn.application_password));
    _ble_link_service_zeroize(&s_service.auth_txn.credential_id,
                              sizeof(s_service.auth_txn.credential_id));
    _ble_link_service_zeroize(&s_service.auth_txn.device_auth_id,
                              sizeof(s_service.auth_txn.device_auth_id));
    s_service.auth_txn.phase = BLE_LINK_AUTH_PHASE_IDLE;
    s_service.auth_txn.authorization_txn_id = 0U;
    s_service.auth_txn.confirmation_token = 0U;
    s_service.auth_txn.operation_token = 0U;
    s_service.auth_txn.connection_generation = 0U;
    s_service.auth_txn.security_epoch = 0U;
    s_service.auth_txn.deadline_ms = 0U;
    ble_link_session_set_authorization_transitioning(false);
}

static uint64_t _ble_link_service_next_confirmation_token(void)
{
    if (s_confirmation_token_sequence == UINT64_MAX)
    {
        return 0U;
    }
    s_confirmation_token_sequence++;
    if (s_confirmation_token_sequence == 0U)
    {
        s_confirmation_token_sequence++;
    }
    return s_confirmation_token_sequence;
}

/**
 * @brief Whether the active authorize transaction expired.
 *
 * Caller holds the service lock. A committed transaction never expires:
 * its replay window outlives the prepare deadline so a client can always
 * retrieve the AuthorizationResult for the exact Commit it sent.
 */
static bool _ble_link_service_auth_txn_expired(void)
{
    return s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_IDLE &&
           s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_COMMITTED &&
           s_service.auth_txn.deadline_ms != 0U &&
           (int32_t)(xTaskGetTickCount() -
                     s_service.auth_txn.deadline_ms) >= 0;
}

/**
 * @brief Expire the active authorize transaction when its deadline passed.
 *
 * Returns true when a transaction was cleared, so the device-link worker
 * can republish the snapshot. Caller holds the service lock.
 */
static bool _ble_link_service_auth_expiry_tick_locked(void)
{
    if (_ble_link_service_auth_txn_expired())
    {
        _ble_link_service_abort_session(
            s_service.current_facts.connection_generation);
        return true;
    }
    return false;
}

#ifdef UNIT_TEST_HOST
void ble_link_service_test_set_auth_deadline_ticks(uint32_t ticks)
{
    _ble_link_service_lock();
    s_service.auth_txn.deadline_ms = ticks;
    _ble_link_service_unlock();
}
#endif

static void _ble_link_service_zeroize(void *data, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    for (size_t i = 0U; i < size; ++i)
    {
        bytes[i] = 0U;
    }
}

static bool _ble_link_service_all_zero(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;

    for (size_t i = 0U; i < size; ++i)
    {
        if (bytes[i] != 0U)
        {
            return false;
        }
    }
    return true;
}

static void _ble_link_service_clear_delayed_cmd0(void)
{
    _ble_link_service_zeroize(&s_service.delayed_cmd0,
                              sizeof(s_service.delayed_cmd0));
}

static void _ble_link_service_clear_committed_replay(void)
{
    _ble_link_service_zeroize(&s_service.committed_replay,
                              sizeof(s_service.committed_replay));
}

static void _ble_link_service_reset_v2_replay(void)
{
    if (s_service.v2_ready)
    {
        device_link_router_replay_reset(&s_service.v2_router);
    }
}

static void _ble_link_service_reset_ingress(void)
{
    /* Any queued work carries the previous epoch and will fail execution.
     * Drop only the admission reservation; the worker still owns and frees
     * the work item. */
    s_service.queued_handshake.work = NULL;
    ble_link_reassembler_reset(&s_ingress.reassembler[0]);
    ble_link_reassembler_reset(&s_ingress.reassembler[1]);
    if (s_ingress.epoch < UINT32_MAX)
    {
        s_ingress.epoch++;
    }
    else
    {
        /* The boot-scoped ingress epoch space is exhausted: the runtime
         * cannot admit or execute any further transaction this boot. */
        s_ingress.exhausted = true;
        ble_link_session_set_error(true);
    }
}

/**
 * @brief Retire only the logical Security 2 epoch.
 *
 * A delayed Cmd0 leaves the old indication payload and flow identity alive
 * until confirmation, while every authorization/session fact is invalidated
 * immediately. The provisional bond belongs to the same ACL and is not
 * discarded by a logical re-handshake.
 */
static esp_err_t _ble_link_service_retire_logical_session(
    uint32_t generation, bool clear_response)
{
    uint32_t epoch = 0U;
    const esp_err_t begin_result = ble_link_session_security2_begin(
                                       generation, &epoch);

    if (begin_result == ESP_OK)
    {
        s_service.current_facts.security_epoch = epoch;
    }
    _ble_link_service_reset_v2_replay();
    _ble_link_service_reset_ingress();
    s_service.subscriber.active = false;
    s_service.handshake_active = false;
    s_service.sec2_opened = false;
    s_service.lt_switch.active = false;
    s_service.close_after_encrypt.active = false;
    _ble_link_service_clear_auth_txn();
#ifdef UNIT_TEST_HOST
    ble_link_dispatcher_clear_session();
#endif
    s_service.completion.pending = false;
    s_service.completion.flow_id = 0U;
    s_service.completion.is_last = false;
    s_service.deferred_busy.active = false;
    if (clear_response)
    {
        s_service.pending_transactions = 0U;
        _ble_link_service_stream_free();
    }
    else
    {
        /* The submitted fragment is the only retained transport
         * obligation. Never emit the rest of the retired response. */
        s_service.stream.active = false;
    }
    if (s_service.security != NULL &&
            s_service.security->close_session != NULL)
    {
        s_service.security->close_session();
    }
    return begin_result;
}

static ble_link_work_t *_ble_link_service_allocate_work(size_t message_len)
{
    if (message_len > BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES)
    {
        return NULL;
    }
    for (size_t i = 0U; i < BLE_LINK_SERVICE_WORK_SLOTS; ++i)
    {
        ble_link_work_t *work = &s_work_pool[i];
        if (!work->in_use)
        {
            memset(work, 0, sizeof(*work));
            work->in_use = true;
            return work;
        }
    }
    return NULL;
}

#ifdef UNIT_TEST_HOST
static void _ble_link_service_write_varint(uint8_t *out, size_t *pos,
        uint64_t value)
{
    while (value >= 0x80U)
    {
        out[(*pos)++] = (uint8_t)(value & 0x7fU) | 0x80U;
        value >>= 7U;
    }
    out[(*pos)++] = (uint8_t)value;
}

static void _ble_link_service_write_tag(uint8_t *out, size_t *pos,
                                        uint32_t field, uint32_t wire)
{
    _ble_link_service_write_varint(out, pos,
                                   ((uint64_t)field << 3U) | wire);
}

static void _ble_link_service_write_fixed64(uint8_t *out, size_t *pos,
        uint64_t value)
{
    for (unsigned int i = 0U; i < 8U; ++i)
    {
        out[(*pos)++] = (uint8_t)(value >> (8U * i));
    }
}

static void _ble_link_service_write_bytes(uint8_t *out, size_t *pos,
        const uint8_t *data, size_t len)
{
    _ble_link_service_write_varint(out, pos, len);
    memcpy(&out[*pos], data, len);
    *pos += len;
}

/**
 * @brief Build the current LinkState from session and connection facts.
 */
static void _ble_link_service_build_link_state(
    const ble_link_service_facts_t *facts,
    ble_link_state_snapshot_t *out)
{
    const uint32_t flags = ble_link_session_get_state_flags();

    memset(out, 0, sizeof(*out));
    out->boot_id = facts->active_boot_id;
    if ((flags & BLE_LINK_STATE_FLAG_BOUND) != 0U)
    {
        out->binding_state = BLE_LINK_BINDING_BOUND;
    }
    else if ((flags & BLE_LINK_STATE_FLAG_BINDABLE) != 0U)
    {
        out->binding_state = BLE_LINK_BINDING_PAIRING_WINDOW;
    }
    else
    {
        out->binding_state = BLE_LINK_BINDING_UNBOUND;
    }
    if (facts->authorized)
    {
        out->authorization_state = BLE_LINK_AUTHORIZATION_AUTHORIZED;
    }
    else if (facts->session_authenticated)
    {
        /* Bootstrap or long-term authenticated: refine with the local
         * authorize transaction state. A prepared transaction that is
         * not yet confirmed is CONFIRMATION_PENDING; a confirmed
         * transaction that has not committed is PREPARED. */
        _ble_link_service_lock();
        if (s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_IDLE &&
                !_ble_link_service_auth_txn_expired())
        {
            if (s_service.auth_txn.phase ==
                    BLE_LINK_AUTH_PHASE_COMMIT_PROBED)
            {
                out->authorization_state =
                    BLE_LINK_AUTHORIZATION_CONFIRMATION_PENDING;
            }
            else if (s_service.auth_txn.phase >=
                     BLE_LINK_AUTH_PHASE_LOCALLY_CONFIRMED)
            {
                out->authorization_state = BLE_LINK_AUTHORIZATION_PREPARED;
            }
            else
            {
                out->authorization_state =
                    BLE_LINK_AUTHORIZATION_BOOTSTRAP_AUTHENTICATED;
            }
        }
        else
        {
            out->authorization_state =
                BLE_LINK_AUTHORIZATION_BOOTSTRAP_AUTHENTICATED;
        }
        _ble_link_service_unlock();
    }
    else
    {
        out->authorization_state = BLE_LINK_AUTHORIZATION_UNAUTHORIZED;
    }
    out->encrypted = facts->encrypted;
    out->secure_connections_bond_verified =
        facts->secure_connections_bond_verified;
    out->identity_known = facts->identity_known;
}

static void _ble_link_service_encode_link_state(
    uint8_t *out, size_t *pos, const ble_link_state_snapshot_t *link_state)
{
    _ble_link_service_write_tag(out, pos, 1U, 1U);
    _ble_link_service_write_fixed64(out, pos, link_state->boot_id);
    if (link_state->binding_state != BLE_LINK_BINDING_UNSPECIFIED)
    {
        _ble_link_service_write_tag(out, pos, 2U, 0U);
        _ble_link_service_write_varint(out, pos, link_state->binding_state);
    }
    if (link_state->authorization_state !=
            BLE_LINK_AUTHORIZATION_UNSPECIFIED)
    {
        _ble_link_service_write_tag(out, pos, 3U, 0U);
        _ble_link_service_write_varint(out, pos,
                                       link_state->authorization_state);
    }
    if (link_state->encrypted)
    {
        _ble_link_service_write_tag(out, pos, 4U, 0U);
        _ble_link_service_write_varint(out, pos, 1U);
    }
    if (link_state->secure_connections_bond_verified)
    {
        _ble_link_service_write_tag(out, pos, 5U, 0U);
        _ble_link_service_write_varint(out, pos, 1U);
    }
    if (link_state->identity_known)
    {
        _ble_link_service_write_tag(out, pos, 6U, 0U);
        _ble_link_service_write_varint(out, pos, 1U);
    }
}

static void _ble_link_service_encode_manifest(uint8_t *out, size_t *pos)
{
    /* protocol_version/profile_version {major=2}. */
    static const uint8_t version[] = {0x08, 0x02};
    /* security {sc_only, key=32, max_bonds=1, protocomm 2, patch 1,
     *          local_confirmation, application_credential} */
    static const uint8_t security[] =
    {
        0x08, 0x01, 0x10, 0x20, 0x18, 0x01, 0x20, 0x02,
        0x28, 0x01, 0x30, 0x01, 0x38, 0x01,
    };
    /* framing {framing_version=1, header_bytes=8, preferred_att_mtu=498,
     *          max_control=4096, max_session=1024} */
    uint8_t framing[32];
    size_t framing_pos = 0U;

    _ble_link_service_write_tag(framing, &framing_pos, 1U, 0U);
    _ble_link_service_write_varint(framing, &framing_pos, 1U);
    _ble_link_service_write_tag(framing, &framing_pos, 2U, 0U);
    _ble_link_service_write_varint(framing, &framing_pos,
                                   BLE_LINK_FRAMING_HEADER_BYTES);
    _ble_link_service_write_tag(framing, &framing_pos, 3U, 0U);
    _ble_link_service_write_varint(framing, &framing_pos,
                                   BLE_LINK_SERVICE_PREFERRED_ATT_MTU);
    _ble_link_service_write_tag(framing, &framing_pos, 4U, 0U);
    _ble_link_service_write_varint(framing, &framing_pos,
                                   BLE_LINK_SERVICE_CONTROL_MAX_BYTES);
    _ble_link_service_write_tag(framing, &framing_pos, 5U, 0U);
    _ble_link_service_write_varint(framing, &framing_pos,
                                   BLE_LINK_SERVICE_SESSION_MAX_BYTES);

    _ble_link_service_write_tag(out, pos, 1U, 2U);
    _ble_link_service_write_bytes(out, pos, version, sizeof(version));
    _ble_link_service_write_tag(out, pos, 2U, 2U);
    _ble_link_service_write_bytes(out, pos, version, sizeof(version));
    _ble_link_service_write_tag(out, pos, 3U, 2U);
    _ble_link_service_write_bytes(out, pos, framing, framing_pos);
    _ble_link_service_write_tag(out, pos, 4U, 2U);
    _ble_link_service_write_bytes(out, pos, security, sizeof(security));
    /* Core domain 0, version 2.0. Only methods 1-5 are implemented. */
    uint8_t domain[96];
    size_t domain_pos = 0U;

    _ble_link_service_write_tag(domain, &domain_pos, 2U, 0U);
    _ble_link_service_write_varint(domain, &domain_pos, 2U);
    for (uint32_t method_id = 1U; method_id <= 5U; ++method_id)
    {
        uint8_t method[24];
        size_t method_pos = 0U;

        _ble_link_service_write_tag(method, &method_pos, 1U, 0U);
        _ble_link_service_write_varint(method, &method_pos, method_id);
        _ble_link_service_write_tag(method, &method_pos, 2U, 0U);
        _ble_link_service_write_varint(method, &method_pos,
                                       method_id >= 3U && method_id <= 4U ?
                                       2U : 1U);
        if (method_id == 3U || method_id == 4U)
        {
            _ble_link_service_write_tag(method, &method_pos, 4U, 0U);
            _ble_link_service_write_varint(method, &method_pos, 1U);
        }
        _ble_link_service_write_tag(domain, &domain_pos, 4U, 2U);
        _ble_link_service_write_bytes(domain, &domain_pos,
                                      method, method_pos);
    }
    _ble_link_service_write_tag(out, pos, 5U, 2U);
    _ble_link_service_write_bytes(out, pos, domain, domain_pos);
}

static void _ble_link_service_encode_authorize_prepare(
    uint8_t *out, size_t *pos)
{
    _ble_link_service_write_tag(out, pos, 1U, 1U);
    _ble_link_service_write_fixed64(out, pos,
                                    s_service.auth_txn.authorization_txn_id);
    _ble_link_service_write_tag(out, pos, 2U, 2U);
    _ble_link_service_write_bytes(out, pos, s_service.auth_txn.credential_id,
                                  BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
    _ble_link_service_write_tag(out, pos, 3U, 2U);
    _ble_link_service_write_bytes(out, pos,
                                  s_service.auth_txn.application_password,
                                  BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
    _ble_link_service_write_tag(out, pos, 4U, 0U);
    _ble_link_service_write_varint(out, pos,
                                   BLE_LINK_SERVICE_AUTH_EXPIRES_MS);
    for (size_t i = 0U;
            i < s_service.auth_txn.requested_permission_count; ++i)
    {
        _ble_link_service_write_tag(out, pos, 5U, 0U);
        _ble_link_service_write_varint(
            out, pos, s_service.auth_txn.requested_permissions[i]);
    }
}

static void _ble_link_service_encode_authorization_result(
    uint8_t *out, size_t *pos,
    const uint8_t *credential_id, const uint8_t *device_auth_id)
{
    _ble_link_service_write_tag(out, pos, 1U, 2U);
    _ble_link_service_write_bytes(out, pos, credential_id,
                                  BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
    _ble_link_service_write_tag(out, pos, 2U, 2U);
    _ble_link_service_write_bytes(out, pos, device_auth_id,
                                  BLE_LINK_SERVICE_AUTH_ID_BYTES);
    _ble_link_service_write_tag(out, pos, 3U, 0U);
    _ble_link_service_write_varint(out, pos,
                                   BLE_LINK_AUTHORIZATION_AUTHORIZED);
    for (uint32_t permission = 1U; permission <= 3U; ++permission)
    {
        _ble_link_service_write_tag(out, pos, 4U, 0U);
        _ble_link_service_write_varint(out, pos, permission);
    }
}
#endif

/**
 * @brief Max payload of one fragment for a negotiated ATT MTU.
 */
static size_t _ble_link_service_max_fragment_payload(uint32_t att_mtu)
{
    size_t max_payload = 0U;

    if (att_mtu >= BLE_LINK_FRAMING_HEADER_BYTES + 3U)
    {
        max_payload = att_mtu - 3U - BLE_LINK_FRAMING_HEADER_BYTES;
    }
    /* The fragment buffer only needs to hold one ATT value plus the
     * framing header; the source message lives in the stream buffer. */
    if (max_payload > BLE_LINK_SERVICE_PREFERRED_ATT_MTU)
    {
        max_payload = BLE_LINK_SERVICE_PREFERRED_ATT_MTU;
    }
    return max_payload;
}

/**
 * @brief Build and emit the fragment at the stream's next offset.
 *
 * Caller holds the service lock. Returns false when the output sink
 * rejected the fragment (the caller fails the transaction closed).
 */
static bool _ble_link_service_stream_emit_locked(void)
{
    uint8_t fragment[BLE_LINK_FRAMING_HEADER_BYTES +
                     BLE_LINK_SERVICE_PREFERRED_ATT_MTU];
    const size_t max_payload = _ble_link_service_max_fragment_payload(
                                   s_service.stream.att_mtu);

    if (max_payload == 0U || s_service.stream.next_offset >=
            s_service.stream.payload_len)
    {
        return false;
    }
    const size_t remaining = s_service.stream.payload_len -
                             s_service.stream.next_offset;
    const size_t chunk = (remaining > max_payload) ? max_payload : remaining;
    const size_t offset = s_service.stream.next_offset;
    const uint8_t *payload = s_service.stream.payload;
    const uint16_t frame_id = s_service.stream.frame_id;
    uint8_t flags = 0U;

    if (offset == 0U)
    {
        flags |= BLE_LINK_FRAMING_FLAG_START;
    }
    if (offset + chunk == s_service.stream.payload_len)
    {
        flags |= BLE_LINK_FRAMING_FLAG_END;
    }
    fragment[0] = 1U;
    fragment[1] = flags;
    fragment[2] = (uint8_t)(frame_id & 0xffU);
    fragment[3] = (uint8_t)(frame_id >> 8U);
    fragment[4] = (uint8_t)(s_service.stream.payload_len & 0xffU);
    fragment[5] = (uint8_t)((s_service.stream.payload_len >> 8U) & 0xffU);
    fragment[6] = (uint8_t)(offset & 0xffU);
    fragment[7] = (uint8_t)((offset >> 8U) & 0xffU);
    memcpy(&fragment[BLE_LINK_FRAMING_HEADER_BYTES], &payload[offset],
           chunk);
    s_service.stream.next_offset = offset + chunk;
    if (s_service.stream.next_offset == s_service.stream.payload_len)
    {
        /* The final fragment is in flight: the stream completes when its
         * indication confirms (the transaction gate then releases). */
        s_service.stream.active = false;
    }
    return s_service.output(
               fragment, BLE_LINK_FRAMING_HEADER_BYTES + chunk,
               s_service.stream.channel,
               s_service.stream.next_offset == s_service.stream.payload_len,
               s_service.stream.flow_id,
               s_service.output_arg) == ESP_OK;
}

/**
 * @brief Start one outbound message streamed fragment by fragment.
 *
 * The payload is copied into a fixed service-owned buffer and only the first
 * fragment is handed to the transport; each following fragment is emitted
 * when the previous indication confirms (ble_link_service_response_
 * completed), so a response completes at any negotiated ATT MTU down to
 * 23 independent of the local TX queue depth.
 */
static bool _ble_link_service_emit_fragments(
    const uint8_t *payload, size_t payload_len, uint8_t transport_type,
    uint32_t att_mtu, ble_link_service_tx_channel_t channel)
{
    if (payload == NULL || payload_len == 0U ||
            _ble_link_service_max_fragment_payload(att_mtu) == 0U)
    {
        return false;
    }
    if (1U + payload_len > sizeof(s_service.stream_storage))
    {
        return false;
    }
    uint8_t *const copy = s_service.stream_storage;
    if (s_service.stream.payload != NULL ||
            s_service.pending_transactions == 0U)
    {
        return false;
    }
    const uint32_t flow_id = _ble_link_service_next_flow_id();

    if (flow_id == 0U)
    {
        return false;
    }
    copy[0] = transport_type;
    memcpy(&copy[1], payload, payload_len);
    s_service.stream.payload = copy;
    s_service.stream.payload_len = 1U + payload_len;
    s_service.stream.next_offset = 0U;
    s_service.stream.att_mtu = att_mtu;
    s_service.stream.channel = channel;
    s_service.outbound_frame_id++;
    if (s_service.outbound_frame_id == 0U)
    {
        s_service.outbound_frame_id = 1U;
    }
    s_service.stream.frame_id = s_service.outbound_frame_id;
    s_service.stream.flow_id = flow_id;
    s_service.stream.active = true;
    if (!_ble_link_service_stream_emit_locked())
    {
        _ble_link_service_stream_free();
        return false;
    }
    return true;
}

static bool _ble_link_service_emit_deferred_busy_locked(void)
{
#ifndef UNIT_TEST_HOST
    s_service.deferred_busy.active = false;
    return true;
#else
    if (!s_service.deferred_busy.active ||
            s_service.pending_transactions != 0U ||
            s_service.stream.payload != NULL)
    {
        return !s_service.deferred_busy.active;
    }
    _ble_link_service_build_response(
        s_service.deferred_busy.request_id, BLE_LINK_ERROR_BUSY,
        BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U);
    if (s_service.response_envelope_len == 0U)
    {
        return false;
    }
    const uint32_t generation = s_service.deferred_busy.generation;
    const uint32_t att_mtu = s_service.deferred_busy.att_mtu;
    const ble_link_service_tx_channel_t channel =
        s_service.deferred_busy.channel;

    if (generation != s_service.current_facts.connection_generation)
    {
        s_service.deferred_busy.active = false;
        return true;
    }
    s_service.deferred_busy.active = false;
    s_service.pending_transactions++;
    if (!_ble_link_service_emit_protected(
                s_service.response_envelope, s_service.response_envelope_len,
                BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED, att_mtu, channel))
    {
        s_service.pending_transactions = 0U;
        return false;
    }
    return true;
#endif
}

/**
 * @brief Build and emit one response for the retired host-only adapter path.
 *
 * The production v2 path writes a status and Typed-TLV payload directly;
 * this helper remains only for legacy regression fixtures and business
 * adapter coverage.
 */
#ifdef UNIT_TEST_HOST
static void _ble_link_service_build_response(
    uint64_t request_id, uint32_t error, ble_link_codec_response_tag_t body_tag,
    const uint8_t *body, size_t body_len)
{
    uint8_t response_bytes[512];
    size_t response_len = 0U;
    ble_link_codec_response_t response;

    memset(&response, 0, sizeof(response));
    response.request_id = request_id;
    response.error = error;
    response.body = body_tag;
    response.body_data = body;
    response.body_len = body_len;
    if (ble_link_codec_encode_response(&response, response_bytes,
                                       sizeof(response_bytes),
                                       &response_len) != ESP_OK)
    {
        s_service.response_envelope_len = 0U;
        return;
    }
    ble_link_codec_envelope_t envelope;

    memset(&envelope, 0, sizeof(envelope));
    envelope.protocol_major = BLE_LINK_SERVICE_PROTOCOL_MAJOR;
    envelope.boot_id = s_service.boot_id;
    envelope.body = BLE_LINK_CODEC_BODY_RESPONSE;
    envelope.body_data = response_bytes;
    envelope.body_len = response_len;
    if (ble_link_codec_encode_envelope(
                &envelope, s_service.response_envelope,
                sizeof(s_service.response_envelope),
                &s_service.response_envelope_len) != ESP_OK)
    {
        s_service.response_envelope_len = 0U;
    }
}
#endif /* UNIT_TEST_HOST */

/**
 * @brief Encrypt (when wired) and fragment one outbound message.
 *
 * The protected transport prepends the type byte; without a Security 2
 * session (host harness) the message is emitted plaintext with the same
 * type byte. Returns false when a fragment is rejected; the caller then
 * fails the transaction closed.
 */
static bool _ble_link_service_emit_protected(
    const uint8_t *message, size_t message_len, uint8_t transport_type,
    uint32_t att_mtu, ble_link_service_tx_channel_t channel)
{
    if (transport_type == BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED &&
            s_service.security != NULL &&
            s_service.security->protect != NULL)
    {
        uint8_t *cipher = NULL;
        size_t cipher_len = 0U;

        if (s_service.security->protect(message, message_len,
                                        &cipher, &cipher_len) != ESP_OK ||
                cipher == NULL)
        {
            free(cipher);
            return false;
        }
        const bool emitted = _ble_link_service_emit_fragments(
                                 cipher, cipher_len, transport_type,
                                 att_mtu, channel);

        free(cipher);
        return emitted;
    }
    return _ble_link_service_emit_fragments(message, message_len,
                                            transport_type, att_mtu,
                                            channel);
}

#ifdef UNIT_TEST_HOST
static void _ble_link_service_emit_response(
    uint64_t request_id, uint32_t error, ble_link_codec_response_tag_t body_tag,
    const uint8_t *body, size_t body_len, uint32_t att_mtu,
    ble_link_service_tx_channel_t channel)
{
    (void)att_mtu;
    (void)channel;
    if (s_service.v2_dispatch_active)
    {
        /* v2 callers encode the response directly as status + Typed-TLV.
         * The legacy callback shape is retained for host-only regression
         * tests, but must never build an application Envelope on v2. */
        s_service.response_envelope_len = 0U;
        return;
    }
    /* The response envelope is built here; the transport (feed, inside the
     * adapter's unprotect, or the plaintext harness) encrypts and emits it
     * after the request callback returns. This keeps every Security 2
     * operation on the adapter lock without re-entry. */
    _ble_link_service_build_response(request_id, error, body_tag,
                                     body, body_len);
}
#endif

/**
 * @brief Response channel for the current RX channel.
 */
static ble_link_service_tx_channel_t _ble_link_service_response_channel(void)
{
    return (s_service.current_channel == BLE_LINK_SERVICE_RX_SESSION) ?
           BLE_LINK_SERVICE_TX_SESSION : BLE_LINK_SERVICE_TX_CONTROL_RESPONSE;
}

#ifdef UNIT_TEST_HOST
static uint32_t _ble_link_service_handle_manifest(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)facts;
    (void)arg;
    uint8_t body[128];
    size_t body_len = 0U;

    _ble_link_service_encode_manifest(body, &body_len);
    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_MANIFEST, body, body_len,

        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}
#endif

#ifdef UNIT_TEST_HOST
static uint32_t _ble_link_service_handle_snapshot(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)facts;
    (void)arg;
    ble_link_snapshot_t snapshot;
    uint8_t body[64];
    size_t body_len = 0U;

    memset(&snapshot, 0, sizeof(snapshot));
    _ble_link_service_build_link_state(&s_service.current_facts,
                                       &snapshot.link_state);
    snapshot.event_sequence = ble_link_events_baseline();
    if (ble_link_snapshot_encode(&snapshot, body, sizeof(body),
                                 &body_len) != ESP_OK)
    {
        return BLE_LINK_ERROR_INTERNAL;
    }

    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_SNAPSHOT, body, body_len,

        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}
#endif

/**
 * @brief Whether a permission id is requestable on this device.
 *
 * The contract registry (registry/permissions.yaml) freezes the known
 * permission ids; an unknown id is rejected outright. Non-core domains
 * are requestable only while their startup-frozen domain descriptor is
 * registered with the router: unadvertised domains must not be granted
 * (their methods could never be admitted, and the durable grant would be
 * dead weight at best).
 */
static bool _ble_link_service_permission_admissible(uint16_t permission)
{
    const uint16_t domain_id = (uint16_t)(permission >> 8U);
    const uint16_t sequence = (uint16_t)(permission & 0x00ffU);

    switch (domain_id)
    {
    case DEVICE_LINK_DOMAIN_CORE:
        if (sequence < 1U || sequence > 3U)
        {
            return false;
        }
        break;
    case DEVICE_LINK_DOMAIN_WIFI:
        if (sequence < 1U || sequence > 3U)
        {
            return false;
        }
        break;
    case DEVICE_LINK_DOMAIN_CLOUD:
        if (sequence < 1U || sequence > 2U)
        {
            return false;
        }
        break;
    case DEVICE_LINK_DOMAIN_LOCATION:
        if (sequence < 1U || sequence > 2U)
        {
            return false;
        }
        break;
    default:
        return false;
    }
    if (domain_id == DEVICE_LINK_DOMAIN_CORE)
    {
        /* Core is always registered. */
        return true;
    }
    for (size_t i = 0U; i < s_service.v2_domain_count; ++i)
    {
        if (s_service.v2_domains[i].domain_id == domain_id)
        {
            return true;
        }
    }
    return false;
}

static device_link_status_t _ble_link_service_authorize_prepare(
    uint32_t connection_generation, uint32_t security_epoch,
    const uint16_t *requested_permissions,
    size_t requested_permission_count)
{
    if (!device_link_permission_set_valid(
                requested_permissions, requested_permission_count) ||
            requested_permission_count == 0U)
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    for (size_t i = 0U; i < requested_permission_count; ++i)
    {
        if (!_ble_link_service_permission_admissible(
                    requested_permissions[i]))
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
    }
    _ble_link_service_lock();
    bool txn_active =
        s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_IDLE &&
        s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_COMMITTED;
    const bool txn_expired = _ble_link_service_auth_txn_expired();

    if (txn_expired)
    {
        _ble_link_service_clear_auth_txn();
        txn_active = false;
    }
    else if (txn_active)
    {
        /* A new Prepare retires the previous pending transaction: its
         * transaction ID, credential ID, application password,
         * confirmation token, and any pending Commit matching it are all
         * invalidated (core v2 security contract). The durable committed
         * record is untouched: COMMITTED is not a pending transaction. */
        _ble_link_service_clear_auth_txn();
        txn_active = false;
    }
    bool operation_token_unavailable = false;

    if (!txn_active)
    {
        const uint32_t operation_token =
            _ble_link_service_next_operation_token();

        if (operation_token == 0U)
        {
            operation_token_unavailable = true;
        }
        else
        {
            s_service.auth_txn.phase = BLE_LINK_AUTH_PHASE_PREPARED;
            s_service.auth_txn.operation_token = operation_token;
            s_service.lt_switch.active = false;
            s_service.close_after_encrypt.active = false;
            s_service.auth_txn.authorization_txn_id =
                ((uint64_t)esp_random() << 32U) | (uint64_t)esp_random();
            if (s_service.auth_txn.authorization_txn_id == 0U)
            {
                s_service.auth_txn.authorization_txn_id = 1U;
            }
            esp_fill_random(s_service.auth_txn.credential_id,
                            sizeof(s_service.auth_txn.credential_id));
            if (_ble_link_service_all_zero(
                        s_service.auth_txn.credential_id,
                        sizeof(s_service.auth_txn.credential_id)))
            {
                s_service.auth_txn.credential_id[0] = 1U;
            }
            esp_fill_random(s_service.auth_txn.application_password,
                            sizeof(s_service.auth_txn.application_password));
            esp_fill_random(s_service.auth_txn.device_auth_id,
                            sizeof(s_service.auth_txn.device_auth_id));
            if (_ble_link_service_all_zero(
                        s_service.auth_txn.device_auth_id,
                        sizeof(s_service.auth_txn.device_auth_id)))
            {
                s_service.auth_txn.device_auth_id[0] = 1U;
            }
            s_service.auth_txn.connection_generation =
                connection_generation;
            s_service.auth_txn.security_epoch = security_epoch;
            s_service.auth_txn.deadline_ms =
                (uint32_t)xTaskGetTickCount() +
                (uint32_t)pdMS_TO_TICKS(BLE_LINK_SERVICE_AUTH_EXPIRES_MS);
            ble_link_session_set_authorization_transitioning(true);
            s_service.auth_txn.requested_permission_count =
                requested_permission_count;
            memcpy(s_service.auth_txn.requested_permissions,
                   requested_permissions,
                   requested_permission_count *
                   sizeof(requested_permissions[0]));
        }
    }
    _ble_link_service_unlock();

    if (operation_token_unavailable)
    {
        return DEVICE_LINK_STATUS_UNAVAILABLE;
    }
    return DEVICE_LINK_STATUS_OK;
}

#ifdef UNIT_TEST_HOST
static uint32_t _ble_link_service_handle_authorize_prepare(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)arg;
    static const uint16_t permissions[] =
    {
        DEVICE_LINK_PERMISSION_CORE_READ,
        DEVICE_LINK_PERMISSION_CORE_BIND,
        DEVICE_LINK_PERMISSION_CORE_OPERATE,
    };
    const device_link_status_t status =
        _ble_link_service_authorize_prepare(
            facts->connection_generation,
            s_service.current_facts.security_epoch, permissions,
            sizeof(permissions) / sizeof(permissions[0]));

    if (status != DEVICE_LINK_STATUS_OK && status != DEVICE_LINK_STATUS_BUSY)
    {
        _ble_link_service_emit_response(
            request->request_id, status,
            BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,
            s_service.current_facts.preferred_att_mtu,
            _ble_link_service_response_channel());
        return BLE_LINK_ERROR_OK;
    }
    uint8_t body[64];
    size_t body_len = 0U;

    _ble_link_service_encode_authorize_prepare(body, &body_len);
    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_AUTHORIZE_PREPARE, body, body_len,
        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}
#endif

/**
 * @brief Parse an AuthorizeCommitRequest body with strict bounds.
 *
 * Fields: 1 fixed64 authorization_txn_id, 2 bytes credential_id. The whole
 * message must parse and the credential length must be exactly
 * BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES.
 */
static bool _ble_link_service_parse_authorize_commit(
    const uint8_t *body, size_t body_len, uint64_t *out_txn_id,
    uint8_t *out_credential, size_t *out_credential_len)
{
    uint64_t txn_id = 0U;
    uint8_t credential[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
    size_t credential_len = 0U;
    size_t pos = 0U;
    bool saw_txn = false;
    bool saw_credential = false;

    while (pos < body_len)
    {
        const uint64_t tag = body[pos];

        if (tag >= 0x80U)
        {
            return false;
        }
        pos++;
        const uint64_t wire = tag & 7U;

        if ((tag == 0x07U || tag == 0x09U) && (wire == 1U || tag == 0x07U))
        {
            if (body_len - pos < 8U)
            {
                return false;
            }
            uint64_t value = 0U;

            for (unsigned int i = 0U; i < 8U; ++i)
            {
                value |= (uint64_t)body[pos + i] << (8U * i);
            }
            pos += 8U;
            txn_id = value;
            saw_txn = true;
        }
        else if ((tag == 0x0aU || tag == 0x12U) && (wire == 2U || tag == 0x0aU))
        {
            if (pos >= body_len || body[pos] >= 0x80U)
            {
                return false;
            }
            const size_t len = body[pos++];
            if (body_len - pos < len)
            {
                return false;
            }
            if (len == BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES)
            {
                memcpy(credential, &body[pos], len);
                credential_len = len;
                saw_credential = true;
            }
            pos += len;
        }
        else
        {
            return false;
        }
    }
    if (!saw_txn || !saw_credential)
    {
        return false;
    }
    *out_txn_id = txn_id;
    memcpy(out_credential, credential, credential_len);
    *out_credential_len = credential_len;
    return true;
}

static device_link_status_t _ble_link_service_authorize_commit(
    uint32_t connection_generation, uint32_t security_epoch, uint64_t txn_id,
    const uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES])
{
    if (txn_id == 0U || credential_id == NULL)
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    const size_t credential_len = BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES;
    bool ok = false;
    bool txn_known = false;
    bool replay = false;
    bool confirmed = false;
    bool probe_unavailable = false;
    bool txn_active = false;
    _ble_link_service_lock();
    /* Terminal Commit replay is owned by committed_replay, independently of
     * the logical Security 2 transaction that produced it. */
    txn_active = s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_IDLE &&
                 s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_COMMITTED;
    if (txn_active && _ble_link_service_auth_txn_expired())
    {
        /* The prepare deadline passed: the transaction no longer exists
         * and must not be committed. */
        _ble_link_service_clear_auth_txn();
        txn_active = false;
    }
    if (txn_active)
    {
        /* The referenced transaction exists only when its ID matches the
         * one pending transaction of this boot. A mismatch is an unknown
         * (or already retired) transaction and maps to NOT_FOUND. */
        txn_known = txn_id == s_service.auth_txn.authorization_txn_id;
        if (txn_known)
        {
            ok = (connection_generation ==
                  s_service.auth_txn.connection_generation &&
                  security_epoch == s_service.auth_txn.security_epoch &&
                  credential_len == BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES &&
                  memcmp(credential_id, s_service.auth_txn.credential_id,
                         credential_len) == 0);
        }
        if (ok && s_service.auth_txn.phase ==
                BLE_LINK_AUTH_PHASE_PREPARED)
        {
            const uint64_t token =
                _ble_link_service_next_confirmation_token();

            if (token == 0U)
            {
                probe_unavailable = true;
            }
            else
            {
                s_service.auth_txn.confirmation_token = token;
                s_service.auth_txn.phase =
                    BLE_LINK_AUTH_PHASE_COMMIT_PROBED;
            }
        }
    }
    if (!replay && s_service.committed_replay.active &&
            credential_len == BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES &&
            s_service.committed_replay.authorization_txn_id == txn_id &&
            s_service.committed_replay.connection_generation ==
            connection_generation &&
            s_service.committed_replay.connection_generation ==
            s_service.current_facts.connection_generation &&
            s_service.committed_replay.conn_handle ==
            s_service.current_facts.conn_handle &&
            s_service.current_facts.identity_known &&
            s_service.committed_replay.peer_addr_type ==
            s_service.current_facts.peer_addr_type &&
            memcmp(s_service.committed_replay.peer_addr,
                   s_service.current_facts.peer_addr,
                   sizeof(s_service.committed_replay.peer_addr)) == 0 &&
            memcmp(s_service.committed_replay.credential_id,
                   credential_id, credential_len) == 0)
    {
        replay = true;
        ok = true;
    }
    confirmed = s_service.auth_txn.phase ==
                BLE_LINK_AUTH_PHASE_LOCALLY_CONFIRMED;
    uint8_t local_password[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
    uint8_t local_credential[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
    _ble_link_service_unlock();
    if (replay)
    {
        return DEVICE_LINK_STATUS_OK;
    }
    if (probe_unavailable)
    {
        _ble_link_service_lock();
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            connection_generation;
        _ble_link_service_clear_auth_txn();
        _ble_link_service_unlock();
        _ble_link_service_discard_provisional_bond(
            connection_generation, true);
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    if (!txn_active || !txn_known)
    {
        /* Unknown, expired, or retired transaction: NOT_FOUND with an
         * empty body. The client must Prepare again on this session. */
        _ble_link_service_clear_auth_txn();
        return DEVICE_LINK_STATUS_NOT_FOUND;
    }
    if (!ok)
    {
        /* The transaction exists but its credential, connection
         * generation, or Security 2 epoch does not match. Terminal
         * pre-durable error: close the Security 2 session after the
         * stable encrypted error response is emitted. */
        _ble_link_service_lock();
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            s_service.current_facts.connection_generation;
        _ble_link_service_unlock();
        _ble_link_service_discard_provisional_bond(
            s_service.current_facts.connection_generation, true);
        _ble_link_service_clear_auth_txn();
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    if (!confirmed)
    {
        /* The user has not confirmed this binding on the device. */
        return DEVICE_LINK_STATUS_CONFIRMATION_REQUIRED;
    }
    /* Real commit: persist the authorization record with a long-term
     * verifier derived from the application password (never persisted in
     * plaintext) and switch the active session to it. The credential,
     * password, and the generated device authorization id are local
     * copies made under the service mutex, so a concurrent deny, window
     * close, or disconnect cannot tear the transaction out from under
     * the persistence. All exits share one cleanup path. */
    device_link_security_auth_record_t record;
    uint8_t local_device_auth_id[BLE_LINK_SERVICE_AUTH_ID_BYTES];
    device_link_status_t commit_error = DEVICE_LINK_STATUS_OK;

    memset(&record, 0, sizeof(record));
    _ble_link_service_lock();
    if (s_service.auth_txn.phase !=
            BLE_LINK_AUTH_PHASE_LOCALLY_CONFIRMED)
    {
        /* Terminal pre-durable failure: the client must see a stable
         * LinkError, and the bootstrap Security 2 session is closed once
         * that encrypted response has been handed to the transport. */
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            connection_generation;
        commit_error = DEVICE_LINK_STATUS_UNAVAILABLE;
        _ble_link_service_unlock();
        goto commit_exit;
    }
    memcpy(local_device_auth_id, s_service.auth_txn.device_auth_id,
           sizeof(local_device_auth_id));
    memcpy(local_credential, s_service.auth_txn.credential_id,
           sizeof(local_credential));
    memcpy(local_password, s_service.auth_txn.application_password,
           sizeof(local_password));
    s_service.auth_txn.phase = BLE_LINK_AUTH_PHASE_COMMITTING;
    /* The authorization revision space is checked before anything is
     * persisted (non-mutating): at exhaustion a commit fails closed
     * without installing durable credentials. */
    if (ble_link_session_authorization_exhausted())
    {
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            connection_generation;
        commit_error = DEVICE_LINK_STATUS_UNAVAILABLE;
        _ble_link_service_unlock();
        goto commit_exit;
    }
    {
        device_link_security_auth_record_t existing;

        memset(&existing, 0, sizeof(existing));
        const esp_err_t existing_result =
            device_link_security_load_auth_record(&existing);

        _ble_link_service_zeroize(&existing, sizeof(existing));
        if (existing_result == ESP_ERR_INVALID_STATE)
        {
            /* F-5 fail-closed: a malformed durable record maps to
             * INTERNAL and must never be silently overwritten by a fresh
             * bind. Only an explicit factory reset / revoke journal may
             * delete it. */
            s_service.close_after_encrypt.active = true;
            s_service.close_after_encrypt.generation =
                connection_generation;
            commit_error = DEVICE_LINK_STATUS_INTERNAL;
            _ble_link_service_unlock();
            goto commit_exit;
        }
        if (existing_result != ESP_OK &&
                existing_result != ESP_ERR_NOT_FOUND)
        {
            s_service.close_after_encrypt.active = true;
            s_service.close_after_encrypt.generation =
                connection_generation;
            commit_error = DEVICE_LINK_STATUS_STORAGE;
            _ble_link_service_unlock();
            goto commit_exit;
        }
    }
    memcpy(record.credential_id, local_credential,
           DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES);
    memcpy(record.device_auth_id, local_device_auth_id,
           DEVICE_LINK_SECURITY_AUTH_ID_BYTES);
    if (s_service.auth_txn.requested_permission_count == 0U ||
            s_service.auth_txn.requested_permission_count >
            DEVICE_LINK_SECURITY_AUTH_MAX_GRANTS)
    {
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            connection_generation;
        commit_error = DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        _ble_link_service_unlock();
        goto commit_exit;
    }
    record.granted_permission_count =
        (uint8_t)s_service.auth_txn.requested_permission_count;
    memcpy(record.granted_permissions,
           s_service.auth_txn.requested_permissions,
           s_service.auth_txn.requested_permission_count *
           sizeof(record.granted_permissions[0]));
    /* The mutex stays held across derivation, persistence, and the state
     * publication: a window close or disconnect cannot clear the
     * transaction underneath a durable commit. */
    /* The committed record must carry the normalized SMP identity of the
     * authenticated connection; an unknown or invalid identity is
     * refused. */
    if (!s_service.current_facts.identity_known ||
            !device_link_security_normalized_identity_valid(
                s_service.current_facts.peer_addr_type,
                s_service.current_facts.peer_addr))
    {
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            connection_generation;
        commit_error = DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        _ble_link_service_unlock();
        goto commit_exit;
    }
    record.peer_addr_type = s_service.current_facts.peer_addr_type;
    memcpy(record.peer_addr, s_service.current_facts.peer_addr,
           DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES);
    const esp_err_t derive_result =
        device_link_security_derive_long_term_verifier(
            local_password, sizeof(local_password),
            record.salt, record.verifier);

    if (derive_result != ESP_OK)
    {
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            connection_generation;
        commit_error = DEVICE_LINK_STATUS_INTERNAL;
        _ble_link_service_unlock();
        goto commit_exit;
    }
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    if (device_link_security_save_auth_record(&record) != ESP_OK)
    {
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            connection_generation;
        commit_error = DEVICE_LINK_STATUS_STORAGE;
        _ble_link_service_unlock();
        goto commit_exit;
    }
    /* Durable boundary reached: nvs_commit() succeeded, so the record is
     * irrevocably committed. From this point no failure may surface as a
     * definitive Commit error, no authorization may be rolled back, and
     * the provisional bond must never be deleted: the client that misses
     * the response recovers through the long-term credential and the
     * Recovery Query. The verifier-install obligation is armed NOW: a
     * later response allocation/encryption/emit failure must not lose it
     * (abort only clears the generation-scoped lt_switch). It is cleared
     * only when the long-term verifier is confirmed loaded. */
    s_service.lt_install_pending = true;
    _ble_link_service_promote_provisional_bond(
        connection_generation);
    s_service.auth_txn.phase = BLE_LINK_AUTH_PHASE_COMMITTED;
    memcpy(s_service.auth_txn.device_auth_id, local_device_auth_id,
           sizeof(s_service.auth_txn.device_auth_id));
    _ble_link_service_clear_committed_replay();
    s_service.committed_replay.active = true;
    s_service.committed_replay.authorization_txn_id =
        s_service.auth_txn.authorization_txn_id;
    s_service.committed_replay.connection_generation =
        connection_generation;
    s_service.committed_replay.conn_handle =
        s_service.current_facts.conn_handle;
    s_service.committed_replay.peer_addr_type =
        s_service.current_facts.peer_addr_type;
    memcpy(s_service.committed_replay.peer_addr,
           s_service.current_facts.peer_addr,
           sizeof(s_service.committed_replay.peer_addr));
    memcpy(s_service.committed_replay.credential_id, local_credential,
           sizeof(s_service.committed_replay.credential_id));
    memcpy(s_service.committed_replay.device_auth_id, local_device_auth_id,
           sizeof(s_service.committed_replay.device_auth_id));
    /* The durable boundary is crossed: the transaction secrets are no
     * longer needed (the long-term verifier was derived, the response is
     * encoded from the persisted record, and committed_replay carries its
     * own identity copies). Clear the RAM copies now so "cleared on
     * success" holds; the txn identity fields stay for replay matching. */
    _ble_link_service_zeroize(&s_service.auth_txn.application_password,
                              sizeof(s_service.auth_txn.application_password));
    _ble_link_service_zeroize(&s_service.auth_txn.credential_id,
                              sizeof(s_service.auth_txn.credential_id));
    _ble_link_service_zeroize(&s_service.auth_txn.device_auth_id,
                              sizeof(s_service.auth_txn.device_auth_id));
    s_service.auth_txn.confirmation_token = 0U;
    /* The bootstrap response is still encrypted under the bootstrap
     * session; the long-term verifier switch is deferred until the
     * protected response has been handed to the transport (consumed in
     * the feed), generation-scoped so a retired flow can never trigger
     * it. Both the flag and the external authorization are published
     * while the transaction mutex is held. */
    s_service.lt_switch.active = true;
    s_service.lt_switch.generation = connection_generation;
    if (ble_link_session_set_authorization(true, 0U) != ESP_OK ||
            ble_link_session_report_session_match_current(
                connection_generation, 0U) != ESP_OK)
    {
        /* The record is durable; publication trouble only means the
         * current session must re-handshake under the long-term
         * verifier. Close the link-session reducer now, but NOT the
         * external adapter: closing it inside the request callback would
         * advance the adapter epoch and make unprotect() discard the
         * already-built success response. The lt_switch post-response
         * action closes both layers after the response is encrypted and
         * handed to the transport, and lt_install_pending guarantees the
         * verifier install. */
        LOG_E("commit durable but session publication failed");
        (void)ble_link_session_security2_close_current(
            connection_generation);
        s_service.sec2_opened = false;
    }
    _ble_link_service_unlock();
commit_exit:
    _ble_link_service_zeroize(&record, sizeof(record));
    _ble_link_service_zeroize(local_password, sizeof(local_password));
    _ble_link_service_zeroize(local_credential, sizeof(local_credential));
    _ble_link_service_zeroize(local_device_auth_id,
                              sizeof(local_device_auth_id));
    if (commit_error != DEVICE_LINK_STATUS_OK)
    {
        /* Pre-durable failure: the record was never persisted, so the
         * provisional bond is discarded and the transaction cleared.
         * The Security 2 session stays open until the stable error
         * response is encrypted (close_after_encrypt). */
        _ble_link_service_discard_provisional_bond(
            connection_generation, true);
        _ble_link_service_lock();
        if (s_service.auth_txn.phase == BLE_LINK_AUTH_PHASE_COMMITTING)
        {
            _ble_link_service_clear_auth_txn();
        }
        _ble_link_service_unlock();
        return commit_error;
    }
    return DEVICE_LINK_STATUS_OK;
}

#ifdef UNIT_TEST_HOST
static uint32_t _ble_link_service_handle_authorize_commit(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)arg;
    uint64_t txn_id = 0U;
    uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES] = {0};
    size_t credential_len = 0U;
    device_link_status_t status = DEVICE_LINK_STATUS_INVALID_ARGUMENT;

    if (_ble_link_service_parse_authorize_commit(
                request->body_data, request->body_len, &txn_id,
                credential_id, &credential_len) &&
            credential_len == sizeof(credential_id))
    {
        status = _ble_link_service_authorize_commit(
                     facts->connection_generation,
                     s_service.current_facts.security_epoch, txn_id,
                     credential_id);
    }
    uint8_t body[64];
    size_t body_len = 0U;
    ble_link_codec_response_tag_t body_tag = BLE_LINK_CODEC_RESPONSE_NONE;

    if (status == DEVICE_LINK_STATUS_OK)
    {
        device_link_security_auth_record_t record;

        memset(&record, 0, sizeof(record));
        if (device_link_security_load_auth_record(&record) == ESP_OK &&
                device_link_security_auth_record_valid(&record))
        {
            _ble_link_service_encode_authorization_result(
                body, &body_len, record.credential_id,
                record.device_auth_id);
            body_tag = BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT;
        }
        else
        {
            status = DEVICE_LINK_STATUS_INTERNAL;
        }
        _ble_link_service_zeroize(&record, sizeof(record));
    }
    _ble_link_service_emit_response(
        request->request_id, status, body_tag,
        body_len != 0U ? body : NULL, body_len,
        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    _ble_link_service_zeroize(credential_id, sizeof(credential_id));
    _ble_link_service_zeroize(body, sizeof(body));
    return BLE_LINK_ERROR_OK;
}
#endif

/**
 * @brief Parse a GetAuthorizationRequest body with strict bounds.
 *
 * Fields: 1 bytes credential_id of exactly
 * BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES.
 */
static bool _ble_link_service_parse_get_authorization(
    const uint8_t *body, size_t body_len, uint8_t *out_credential)
{
    size_t pos = 0U;
    bool saw_credential = false;

    while (pos < body_len)
    {
        const uint64_t tag = body[pos];

        if (tag >= 0x80U)
        {
            return false;
        }
        pos++;
        const uint64_t wire = tag & 7U;

        if ((tag == 0x06U || tag == 0x0aU) &&
                (wire == 2U || tag == 0x06U))
        {
            if (pos >= body_len || body[pos] >= 0x80U)
            {
                return false;
            }
            const size_t len = body[pos++];
            if (body_len - pos < len)
            {
                return false;
            }
            if (len == BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES)
            {
                memcpy(out_credential, &body[pos], len);
                saw_credential = true;
            }
            pos += len;
        }
        else
        {
            return false;
        }
    }
    return saw_credential;
}

static device_link_status_t _ble_link_service_get_authorization(
    const uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES])
{
    if (credential_id == NULL)
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    /* The committed record must match the credential ID and the resolved
     * peer identity; the session was already authenticated with the
     * long-term credential of the record (on_authenticated verified the
     * identity), so this is a consistency re-check against a record that
     * could have been revoked mid-session. */
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    const esp_err_t load_result =
        device_link_security_load_auth_record(&record);

    if (load_result == ESP_ERR_NOT_FOUND)
    {
        _ble_link_service_zeroize(&record, sizeof(record));
        return DEVICE_LINK_STATUS_NOT_FOUND;
    }
    if (load_result == ESP_ERR_INVALID_STATE ||
            (load_result == ESP_OK &&
             !device_link_security_auth_record_valid(&record)))
    {
        _ble_link_service_zeroize(&record, sizeof(record));
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    if (load_result != ESP_OK)
    {
        _ble_link_service_zeroize(&record, sizeof(record));
        return DEVICE_LINK_STATUS_STORAGE;
    }
    if (memcmp(record.credential_id, credential_id,
               BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES) != 0 ||
            record.peer_addr_type != s_service.current_facts.peer_addr_type ||
            memcmp(record.peer_addr, s_service.current_facts.peer_addr, 6U) != 0)
    {
        _ble_link_service_zeroize(&record, sizeof(record));
        return DEVICE_LINK_STATUS_NOT_FOUND;
    }
    _ble_link_service_zeroize(&record, sizeof(record));
    return DEVICE_LINK_STATUS_OK;
}

#ifdef UNIT_TEST_HOST
static uint32_t _ble_link_service_handle_get_authorization(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)arg;
    uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES] = {0};
    device_link_status_t status = DEVICE_LINK_STATUS_INVALID_ARGUMENT;

    if (facts->recovery_query && facts->authorized &&
            facts->session_authenticated &&
            _ble_link_service_parse_get_authorization(
                request->body_data, request->body_len, credential_id))
    {
        status = _ble_link_service_get_authorization(credential_id);
    }
    uint8_t body[64];
    size_t body_len = 0U;
    ble_link_codec_response_tag_t body_tag = BLE_LINK_CODEC_RESPONSE_NONE;

    if (status == DEVICE_LINK_STATUS_OK)
    {
        device_link_security_auth_record_t record;

        memset(&record, 0, sizeof(record));
        if (device_link_security_load_auth_record(&record) == ESP_OK &&
                device_link_security_auth_record_valid(&record))
        {
            _ble_link_service_encode_authorization_result(
                body, &body_len, record.credential_id,
                record.device_auth_id);
            body_tag = BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT;
        }
        else
        {
            status = DEVICE_LINK_STATUS_INTERNAL;
        }
        _ble_link_service_zeroize(&record, sizeof(record));
    }
    _ble_link_service_emit_response(
        request->request_id, status, body_tag,
        body_len != 0U ? body : NULL, body_len,
        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    _ble_link_service_zeroize(credential_id, sizeof(credential_id));
    _ble_link_service_zeroize(body, sizeof(body));
    return BLE_LINK_ERROR_OK;
}
#endif

static esp_err_t _ble_link_service_v2_put_nested(
    device_link_tlv_writer_t *writer, uint8_t field_id,
    const uint8_t *nested, size_t nested_len)
{
    return device_link_tlv_put_bytes(writer, field_id, nested, nested_len);
}

static device_link_status_t _ble_link_service_v2_encode_manifest(
    uint8_t *response, size_t capacity, size_t *response_len)
{
    uint8_t version[16];
    uint8_t profile_version[16];
    uint8_t framing[64];
    uint8_t security[64];
    device_link_tlv_writer_t writer;
    size_t version_len = 0U;
    size_t profile_version_len = 0U;
    size_t framing_len = 0U;
    size_t security_len = 0U;

    if (s_service.v2_domain_count == 0U)
    {
        /* Manifest requires at least the frozen Core domain. */
        return DEVICE_LINK_STATUS_UNAVAILABLE;
    }
    device_link_tlv_writer_init(&writer, version, sizeof(version));
    (void)device_link_tlv_put_uint(&writer, 1U, DEVICE_LINK_CORE_MAJOR);
    (void)device_link_tlv_put_uint(&writer, 2U, DEVICE_LINK_CORE_MINOR);
    if (device_link_tlv_writer_finish(&writer, &version_len) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    /* profile_version is a frozen protocol-version-shaped field of its own:
     * the profile may bump independently of the core protocol, so it must
     * never be emitted from the protocol version buffer. */
    device_link_tlv_writer_init(&writer, profile_version,
                                sizeof(profile_version));
    (void)device_link_tlv_put_uint(&writer, 1U, DEVICE_LINK_PROFILE_MAJOR);
    (void)device_link_tlv_put_uint(&writer, 2U, DEVICE_LINK_PROFILE_MINOR);
    if (device_link_tlv_writer_finish(&writer, &profile_version_len) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    device_link_tlv_writer_init(&writer, framing, sizeof(framing));
    (void)device_link_tlv_put_uint(&writer, 1U,
                                   DEVICE_LINK_WIRE_HEADER_VERSION);
    (void)device_link_tlv_put_uint(&writer, 2U,
                                   DEVICE_LINK_WIRE_HEADER_BYTES);
    (void)device_link_tlv_put_uint(&writer, 3U,
                                   BLE_LINK_SERVICE_PREFERRED_ATT_MTU);
    (void)device_link_tlv_put_uint(&writer, 4U,
                                   BLE_LINK_SERVICE_CONTROL_MAX_BYTES);
    (void)device_link_tlv_put_uint(&writer, 5U,
                                   BLE_LINK_SERVICE_SESSION_MAX_BYTES);
    (void)device_link_tlv_put_uint(&writer, 6U,
                                   DEVICE_LINK_MAX_DOMAIN_PAYLOAD_BYTES);
    if (device_link_tlv_writer_finish(&writer, &framing_len) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    device_link_tlv_writer_init(&writer, security, sizeof(security));
    (void)device_link_tlv_put_bool(&writer, 1U, true);
    /* AES-256-GCM session key: the first 32 bytes of the 64-byte SRP
     * session-key digest (core v2 security.md). */
    (void)device_link_tlv_put_uint(&writer, 2U, 32U);
    (void)device_link_tlv_put_uint(&writer, 3U, 1U);
    (void)device_link_tlv_put_uint(&writer, 4U, 2U);
    (void)device_link_tlv_put_uint(&writer, 5U,
                                   BLE_LINK_SERVICE_PROTOCOMM_PATCH_VERSION);
    (void)device_link_tlv_put_bool(&writer, 6U, true);
    (void)device_link_tlv_put_bool(&writer, 7U, true);
    (void)device_link_tlv_put_bool(&writer, 8U, true);
    if (device_link_tlv_writer_finish(&writer, &security_len) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    /* One DomainDescriptor per startup-frozen registered domain. The Core
     * domain is always first; optional domains may be added through
     * ble_link_service_set_domain_descriptors() before init seals them. */
    device_link_tlv_writer_init(&writer, response, capacity);
    (void)_ble_link_service_v2_put_nested(&writer, 1U, version, version_len);
    (void)_ble_link_service_v2_put_nested(
        &writer, 2U, profile_version, profile_version_len);
    (void)_ble_link_service_v2_put_nested(&writer, 3U, framing, framing_len);
    (void)_ble_link_service_v2_put_nested(&writer, 4U, security, security_len);
    for (size_t d = 0U; d < s_service.v2_domain_count; ++d)
    {
        const device_link_domain_descriptor_t *domain =
            &s_service.v2_domains[d];
        /* Contract DomainDescriptor.max_encoded_bytes is 768. */
        uint8_t domain_bytes[768];
        size_t domain_len = 0U;
        device_link_tlv_writer_t domain_writer;

        device_link_tlv_writer_init(&domain_writer, domain_bytes,
                                    sizeof(domain_bytes));
        (void)device_link_tlv_put_uint(&domain_writer, 1U,
                                       domain->domain_id);
        (void)device_link_tlv_put_uint(&domain_writer, 2U, domain->major);
        (void)device_link_tlv_put_uint(&domain_writer, 3U, domain->minor);
        for (size_t m = 0U; m < domain->method_count; ++m)
        {
            const device_link_method_descriptor_t *desc =
                &domain->methods[m];
            uint8_t method[64];
            size_t method_len = 0U;
            device_link_tlv_writer_t method_writer;

            device_link_tlv_writer_init(&method_writer, method,
                                        sizeof(method));
            (void)device_link_tlv_put_uint(&method_writer, 1U,
                                           desc->method_id);
            (void)device_link_tlv_put_uint(&method_writer, 2U,
                                           desc->permission_id);
            (void)device_link_tlv_put_bool(
                &method_writer, 3U,
                (desc->flags & DEVICE_LINK_METHOD_ASYNCHRONOUS) != 0U);
            (void)device_link_tlv_put_uint(&method_writer, 4U,
                                           desc->maximum_request_bytes);
            (void)device_link_tlv_put_uint(&method_writer, 5U,
                                           desc->maximum_response_bytes);
            if (device_link_tlv_writer_finish(&method_writer,
                                              &method_len) != ESP_OK ||
                    device_link_tlv_put_bytes(
                        &domain_writer, 4U, method, method_len) != ESP_OK)
            {
                return DEVICE_LINK_STATUS_INTERNAL;
            }
        }
        if (device_link_tlv_writer_finish(&domain_writer,
                                          &domain_len) != ESP_OK ||
                device_link_tlv_put_bytes(
                    &writer, 5U, domain_bytes, domain_len) != ESP_OK)
        {
            return DEVICE_LINK_STATUS_INTERNAL;
        }
    }
    if (device_link_tlv_writer_finish(&writer, response_len) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
    }
    return DEVICE_LINK_STATUS_OK;
}

static device_link_status_t _ble_link_service_v2_encode_snapshot(
    uint8_t *response, size_t capacity, size_t *response_len)
{
    uint8_t link_state[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    size_t link_state_len = 0U;
    ble_link_state_t state;
    device_link_tlv_writer_t writer;

    memset(&state, 0, sizeof(state));
    state.protocol_major = BLE_LINK_STATE_PROTOCOL_MAJOR;
    state.profile_major = BLE_LINK_STATE_PROFILE_MAJOR;
    state.protocol_minor = DEVICE_LINK_CORE_MINOR;
    state.profile_minor = DEVICE_LINK_PROFILE_MINOR;
    state.boot_id = s_service.current_facts.active_boot_id;
    state.state_flags = ble_link_session_get_state_flags();
    if (ble_link_events_baseline() == 0U ||
            ble_link_state_encode(&state, link_state, sizeof(link_state),
                                  &link_state_len) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    device_link_tlv_writer_init(&writer, response, capacity);
    (void)device_link_tlv_put_fixed64(&writer, 1U,
                                      ble_link_events_baseline());
    (void)_ble_link_service_v2_put_nested(&writer, 2U, link_state,
                                          link_state_len);
    /* Operation summaries: the snapshot carries only the compact summary
     * (id, domain, method, state, error), never a result payload. The
     * table is swept first so expired terminal records are excluded, and
     * the whole iteration runs under the service mutex so the bridge
     * writer (connectivity publisher context) cannot interleave. */
    _ble_link_service_lock();
    device_link_operation_sweep(&s_service.v2_operations,
                                _ble_link_service_v2_now_ms());
    for (size_t i = 0U; i < DEVICE_LINK_MAX_OPERATIONS; ++i)
    {
        const device_link_operation_t *operation =
            &s_service.v2_operations.slots[i];
        uint8_t summary[BLE_LINK_SERVICE_OPERATION_SUMMARY_MAX_BYTES];
        size_t summary_len = 0U;
        device_link_tlv_writer_t summary_writer;

        if (operation->id == 0U)
        {
            continue;
        }
        device_link_tlv_writer_init(&summary_writer, summary,
                                    sizeof(summary));
        if (device_link_tlv_put_fixed64(&summary_writer, 1U,
                                        operation->id) != ESP_OK ||
                device_link_tlv_put_uint(&summary_writer, 2U,
                                         operation->domain_id) != ESP_OK ||
                device_link_tlv_put_uint(&summary_writer, 3U,
                                         operation->method_id) != ESP_OK ||
                device_link_tlv_put_uint(&summary_writer, 4U,
                                         operation->state) != ESP_OK ||
                device_link_tlv_put_uint(&summary_writer, 5U,
                                         operation->status) != ESP_OK ||
                device_link_tlv_writer_finish(&summary_writer,
                                              &summary_len) != ESP_OK ||
                device_link_tlv_put_bytes(&writer, 3U, summary,
                                          summary_len) != ESP_OK)
        {
            _ble_link_service_unlock();
            return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
        }
    }
    _ble_link_service_unlock();
    if (device_link_tlv_writer_finish(&writer, response_len) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
    }
    return DEVICE_LINK_STATUS_OK;
}

static device_link_status_t _ble_link_service_v2_encode_authorization_result(
    const uint8_t *credential_id, const uint8_t *device_auth_id,
    uint32_t state, const uint16_t *permissions, size_t permission_count,
    uint64_t confirmation_token, uint8_t *response, size_t capacity,
    size_t *response_len)
{
    device_link_tlv_writer_t writer;

    if (credential_id == NULL || device_auth_id == NULL || response == NULL ||
            response_len == NULL || permission_count > DEVICE_LINK_MAX_PERMISSIONS ||
            (permissions == NULL && permission_count != 0U))
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    device_link_tlv_writer_init(&writer, response, capacity);
    if (device_link_tlv_put_bytes(
                &writer, 1U, credential_id,
                BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES) != ESP_OK ||
            device_link_tlv_put_bytes(
                &writer, 2U, device_auth_id,
                BLE_LINK_SERVICE_AUTH_ID_BYTES) != ESP_OK ||
            device_link_tlv_put_uint(&writer, 3U, state) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
    }
    for (size_t i = 0U; i < permission_count; ++i)
    {
        if (device_link_tlv_put_uint(
                    &writer, 4U, permissions[i]) != ESP_OK)
        {
            return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
        }
    }
    if (confirmation_token != 0U &&
            device_link_tlv_put_fixed64(
                &writer, 5U, confirmation_token) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
    }
    return device_link_tlv_writer_finish(&writer, response_len) == ESP_OK ?
           DEVICE_LINK_STATUS_OK : DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
}

static bool _ble_link_service_v2_parse_permissions(
    const uint8_t *request, size_t request_len,
    uint16_t permissions[DEVICE_LINK_MAX_PERMISSIONS],
    size_t *permission_count)
{
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has_field = false;
    size_t count = 0U;
    uint16_t previous = 0U;

    if (permissions == NULL || permission_count == NULL ||
            device_link_tlv_reader_init(&reader, request, request_len) !=
            ESP_OK)
    {
        return false;
    }
    while (device_link_tlv_reader_next(&reader, &field, &has_field) == ESP_OK &&
            has_field)
    {
        if (field.id != 1U || field.wire_type != DEVICE_LINK_TLV_UNSIGNED ||
                field.value.unsigned_value == 0U ||
                field.value.unsigned_value > UINT16_MAX || count >=
                DEVICE_LINK_MAX_PERMISSIONS ||
                (count != 0U && previous >= field.value.unsigned_value))
        {
            return false;
        }
        permissions[count++] = (uint16_t)field.value.unsigned_value;
        previous = permissions[count - 1U];
    }
    if (reader.offset != reader.len || count == 0U)
    {
        return false;
    }
    *permission_count = count;
    return true;
}

static bool _ble_link_service_v2_parse_authorize_commit(
    const uint8_t *request, size_t request_len, uint64_t *txn_id,
    uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES])
{
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has_field = false;

    if (txn_id == NULL || credential_id == NULL ||
            device_link_tlv_reader_init(&reader, request, request_len) !=
            ESP_OK ||
            device_link_tlv_reader_next(&reader, &field, &has_field) !=
            ESP_OK || !has_field || field.id != 1U ||
            field.wire_type != DEVICE_LINK_TLV_FIXED64 ||
            field.value.fixed64_value == 0U)
    {
        return false;
    }
    *txn_id = field.value.fixed64_value;
    if (device_link_tlv_reader_next(&reader, &field, &has_field) != ESP_OK ||
            !has_field || field.id != 2U ||
            field.wire_type != DEVICE_LINK_TLV_LENGTH ||
            field.value.bytes.len != BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES)
    {
        return false;
    }
    memcpy(credential_id, field.value.bytes.data,
           BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
    if (device_link_tlv_reader_next(&reader, &field, &has_field) != ESP_OK ||
            has_field || reader.offset != reader.len)
    {
        _ble_link_service_zeroize(
            credential_id, BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
        return false;
    }
    return true;
}

static bool _ble_link_service_v2_parse_credential_id(
    const uint8_t *request, size_t request_len,
    uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES])
{
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has_field = false;

    if (credential_id == NULL ||
            device_link_tlv_reader_init(&reader, request, request_len) !=
            ESP_OK ||
            device_link_tlv_reader_next(&reader, &field, &has_field) !=
            ESP_OK || !has_field || field.id != 1U ||
            field.wire_type != DEVICE_LINK_TLV_LENGTH ||
            field.value.bytes.len != BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES)
    {
        return false;
    }
    memcpy(credential_id, field.value.bytes.data,
           BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
    if (device_link_tlv_reader_next(&reader, &field, &has_field) != ESP_OK ||
            has_field || reader.offset != reader.len)
    {
        _ble_link_service_zeroize(
            credential_id, BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
        return false;
    }
    return true;
}

static device_link_status_t _ble_link_service_v2_encode_current_auth_result(
    uint32_t state, uint64_t confirmation_token, uint8_t *response,
    size_t capacity, size_t *response_len)
{
    uint8_t credential[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
    uint8_t device_auth_id[BLE_LINK_SERVICE_AUTH_ID_BYTES];
    uint16_t permissions[DEVICE_LINK_MAX_PERMISSIONS];
    size_t permission_count = 0U;
    device_link_status_t result;

    memset(credential, 0, sizeof(credential));
    memset(device_auth_id, 0, sizeof(device_auth_id));
    memset(permissions, 0, sizeof(permissions));
    _ble_link_service_lock();
    memcpy(credential, s_service.auth_txn.credential_id, sizeof(credential));
    memcpy(device_auth_id, s_service.auth_txn.device_auth_id,
           sizeof(device_auth_id));
    permission_count = s_service.auth_txn.requested_permission_count;
    if (permission_count > DEVICE_LINK_MAX_PERMISSIONS)
    {
        permission_count = 0U;
    }
    memcpy(permissions, s_service.auth_txn.requested_permissions,
           permission_count * sizeof(permissions[0]));
    _ble_link_service_unlock();
    if (permission_count == 0U)
    {
        result = DEVICE_LINK_STATUS_INTERNAL;
        goto cleanup;
    }
    result = _ble_link_service_v2_encode_authorization_result(
                 credential, device_auth_id, state, permissions,
                 permission_count, confirmation_token, response, capacity,
                 response_len);
cleanup:
    _ble_link_service_zeroize(credential, sizeof(credential));
    _ble_link_service_zeroize(device_auth_id, sizeof(device_auth_id));
    _ble_link_service_zeroize(permissions, sizeof(permissions));
    return result;
}

static uint64_t _ble_link_service_v2_now_ms(void)
{
    const TickType_t ticks = xTaskGetTickCount();

    return ((uint64_t)ticks * 1000U) / configTICK_RATE_HZ;
}

static bool _ble_link_service_v2_parse_operation_id(
    const uint8_t *request, size_t request_len, uint64_t *operation_id)
{
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has_field = false;

    if (operation_id == NULL ||
            device_link_tlv_reader_init(&reader, request, request_len) !=
            ESP_OK ||
            device_link_tlv_reader_next(&reader, &field, &has_field) !=
            ESP_OK || !has_field || field.id != 1U ||
            field.wire_type != DEVICE_LINK_TLV_FIXED64 ||
            field.value.fixed64_value == 0U || reader.offset != reader.len)
    {
        return false;
    }
    *operation_id = field.value.fixed64_value;
    return true;
}

/**
 * @brief Whether an asynchronous method declares a non-empty operation
 * result payload in its startup-frozen descriptor.
 *
 * The contract freezes the operation result per method (e.g. start_scan
 * declares core.v2.Empty, the profile-mutating Wi-Fi methods declare
 * WifiStatus). A SUCCEEDED record may only carry the payload of a method
 * that declares a non-empty result message: a declared Empty is still a
 * declared result, but it must never be encoded, so it is excluded here
 * explicitly rather than by payload presence.
 */
static const device_link_method_descriptor_t *
_ble_link_service_v2_operation_descriptor(
    uint8_t domain_id, uint8_t method_id)
{
    for (size_t d = 0U; d < s_service.v2_domain_count; ++d)
    {
        const device_link_domain_descriptor_t *domain =
            &s_service.v2_domains[d];

        if (domain->domain_id != domain_id)
        {
            continue;
        }
        for (size_t m = 0U; m < domain->method_count; ++m)
        {
            const device_link_method_descriptor_t *desc =
                &domain->methods[m];

            if (desc->method_id == method_id)
            {
                return (desc->flags & DEVICE_LINK_METHOD_ASYNCHRONOUS) != 0U ?
                       desc : NULL;
            }
        }
    }
    return NULL;
}

static device_link_status_t _ble_link_service_v2_encode_operation(
    const device_link_operation_t *operation, uint8_t *response,
    size_t capacity, size_t *response_len)
{
    device_link_tlv_writer_t writer;

    if (operation == NULL || response == NULL || response_len == NULL)
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    const device_link_method_descriptor_t *descriptor =
        _ble_link_service_v2_operation_descriptor(
            operation->domain_id, operation->method_id);
    if (descriptor == NULL || descriptor->operation_result_schema == NULL)
    {
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    const bool succeeded =
        operation->state == DEVICE_LINK_OPERATION_SUCCEEDED;
    const bool empty_result =
        descriptor->operation_result_schema->field_count == 0U;
    if ((!succeeded && operation->result_len != 0U) ||
            (succeeded && empty_result && operation->result_len != 0U) ||
            (succeeded && !empty_result &&
             (operation->result_len == 0U ||
              device_link_tlv_validate_message(
                  operation->result, operation->result_len,
                  descriptor->operation_result_schema) != ESP_OK)))
    {
        return DEVICE_LINK_STATUS_INTERNAL;
    }
    device_link_tlv_writer_init(&writer, response, capacity);
    if (device_link_tlv_put_fixed64(
                &writer, 1U, operation->id) != ESP_OK ||
            device_link_tlv_put_uint(
                &writer, 2U, operation->domain_id) != ESP_OK ||
            device_link_tlv_put_uint(
                &writer, 3U, operation->method_id) != ESP_OK ||
            device_link_tlv_put_uint(
                &writer, 4U, operation->state) != ESP_OK ||
            device_link_tlv_put_uint(
                &writer, 5U, operation->status) != ESP_OK)
    {
        return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
    }
    if (succeeded && !empty_result &&
            device_link_tlv_put_bytes(
                &writer, 6U, operation->result, operation->result_len) !=
            ESP_OK)
    {
        return DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
    }
    return device_link_tlv_writer_finish(&writer, response_len) == ESP_OK ?
           DEVICE_LINK_STATUS_OK : DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
}

static device_link_status_t _ble_link_service_v2_method(
    const device_link_request_context_t *context,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len,
    void *arg)
{
    (void)arg;
    *response_len = 0U;
    if (context == NULL || response == NULL || response_len == NULL)
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    if (context->header.method_id >= 1U &&
            context->header.method_id <= 5U &&
            s_service.current_channel != BLE_LINK_SERVICE_RX_SESSION)
    {
        return DEVICE_LINK_STATUS_UNAVAILABLE;
    }
    if (context->header.method_id == 1U)
    {
        return _ble_link_service_v2_encode_manifest(
                   response, response_capacity, response_len);
    }
    if (context->header.method_id == 2U)
    {
        return _ble_link_service_v2_encode_snapshot(
                   response, response_capacity, response_len);
    }
    if (context->header.method_id == 6U || context->header.method_id == 7U)
    {
        if (s_service.current_channel != BLE_LINK_SERVICE_RX_CONTROL)
        {
            return DEVICE_LINK_STATUS_UNAVAILABLE;
        }
        uint64_t operation_id = 0U;
        const device_link_operation_t *operation = NULL;

        if (!_ble_link_service_v2_parse_operation_id(
                    request, request_len, &operation_id))
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        const uint64_t now_ms = _ble_link_service_v2_now_ms();
        esp_err_t operation_result;
        device_link_status_t op_status = DEVICE_LINK_STATUS_OK;

        /* The completion bridge writes the operation table from the
         * connectivity publisher context under _ble_link_service_lock()
         * (ble_link_service_async_operation_update), and GetLinkSnapshot
         * reads it under the same lock. Cancel and GetOperation must hold
         * it too, otherwise a terminal bridge write can race the
         * cancel/get read and publish a torn or stale OperationStatus. The
         * get returns the table slot pointer, so the encode below must stay
         * inside the same critical section. */
        _ble_link_service_lock();
        if (context->header.method_id == 7U)
        {
            operation_result = device_link_operation_cancel(
                                   &s_service.v2_operations, now_ms,
                                   operation_id);
            if (operation_result != ESP_OK &&
                    operation_result != ESP_ERR_INVALID_STATE)
            {
                op_status = operation_result == ESP_ERR_NOT_FOUND ?
                            DEVICE_LINK_STATUS_NOT_FOUND :
                            DEVICE_LINK_STATUS_UNAVAILABLE;
            }
        }
        if (op_status == DEVICE_LINK_STATUS_OK)
        {
            operation_result = device_link_operation_get(
                                   &s_service.v2_operations, now_ms,
                                   operation_id, &operation);
            if (operation_result != ESP_OK)
            {
                op_status = DEVICE_LINK_STATUS_NOT_FOUND;
            }
        }
        if (op_status == DEVICE_LINK_STATUS_OK)
        {
            op_status = _ble_link_service_v2_encode_operation(
                            operation, response, response_capacity,
                            response_len);
        }
        _ble_link_service_unlock();
        return op_status;
    }

    uint16_t requested_permissions[DEVICE_LINK_MAX_PERMISSIONS] = {0};
    size_t requested_permission_count = 0U;

    if (context->header.method_id < 3U || context->header.method_id > 5U)
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    if (context->header.method_id == 3U &&
            !_ble_link_service_v2_parse_permissions(
                request, request_len, requested_permissions,
                &requested_permission_count))
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    if (context->header.method_id == 3U)
    {
        const device_link_status_t prepare_status =
            _ble_link_service_authorize_prepare(
                context->connection_generation, context->security_epoch,
                requested_permissions, requested_permission_count);

        if (prepare_status != DEVICE_LINK_STATUS_OK)
        {
            _ble_link_service_zeroize(requested_permissions,
                                      sizeof(requested_permissions));
            return prepare_status;
        }
        device_link_tlv_writer_t writer;

        device_link_tlv_writer_init(&writer, response, response_capacity);
        _ble_link_service_lock();
        const uint64_t txn_id = s_service.auth_txn.authorization_txn_id;
        const uint8_t *credential = s_service.auth_txn.credential_id;
        const uint8_t *password = s_service.auth_txn.application_password;
        (void)device_link_tlv_put_fixed64(&writer, 1U, txn_id);
        (void)device_link_tlv_put_bytes(
            &writer, 2U, credential,
            BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
        (void)device_link_tlv_put_bytes(
            &writer, 3U, password,
            BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES);
        (void)device_link_tlv_put_uint(
            &writer, 4U, BLE_LINK_SERVICE_AUTH_EXPIRES_MS);
        for (size_t i = 0U;
                i < s_service.auth_txn.requested_permission_count; ++i)
        {
            (void)device_link_tlv_put_uint(
                &writer, 5U,
                s_service.auth_txn.requested_permissions[i]);
        }
        const esp_err_t result = device_link_tlv_writer_finish(
                                     &writer, response_len);
        _ble_link_service_unlock();
        _ble_link_service_zeroize(requested_permissions,
                                  sizeof(requested_permissions));
        return result == ESP_OK ? DEVICE_LINK_STATUS_OK :
               DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED;
    }
    if (context->header.method_id == 4U)
    {
        uint64_t txn_id = 0U;
        uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES] = {0};

        if (!_ble_link_service_v2_parse_authorize_commit(
                    request, request_len, &txn_id, credential_id))
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        device_link_status_t commit_status =
            _ble_link_service_authorize_commit(
                context->connection_generation, context->security_epoch,
                txn_id, credential_id);

        _ble_link_service_zeroize(credential_id, sizeof(credential_id));
        if (commit_status == DEVICE_LINK_STATUS_CONFIRMATION_REQUIRED)
        {
            const uint64_t token = ble_link_service_confirmation_token();

            if (token == 0U ||
                    _ble_link_service_v2_encode_current_auth_result(
                        BLE_LINK_AUTHORIZATION_CONFIRMATION_PENDING,
                        token, response, response_capacity,
                        response_len) != DEVICE_LINK_STATUS_OK)
            {
                return DEVICE_LINK_STATUS_INTERNAL;
            }
            return commit_status;
        }
        if (commit_status != DEVICE_LINK_STATUS_OK)
        {
            return commit_status;
        }
        device_link_security_auth_record_t record;

        memset(&record, 0, sizeof(record));
        const esp_err_t load_result =
            device_link_security_load_auth_record(&record);

        if (load_result != ESP_OK ||
                !device_link_security_auth_record_valid(&record))
        {
            _ble_link_service_zeroize(&record, sizeof(record));
            return load_result == ESP_ERR_NOT_FOUND ?
                   DEVICE_LINK_STATUS_NOT_FOUND :
                   load_result == ESP_ERR_INVALID_STATE ?
                   DEVICE_LINK_STATUS_INTERNAL : DEVICE_LINK_STATUS_STORAGE;
        }
        commit_status = _ble_link_service_v2_encode_authorization_result(
                            record.credential_id, record.device_auth_id,
                            BLE_LINK_AUTHORIZATION_AUTHORIZED,
                            record.granted_permissions,
                            record.granted_permission_count, 0U,
                            response, response_capacity, response_len);
        _ble_link_service_zeroize(&record, sizeof(record));
        return commit_status;
    }
    if (context->header.method_id == 5U)
    {
        uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES] = {0};

        /* GetAuthorization is recovery-only: a request without the
         * recovery bit misuses the header (MALFORMED_FRAME, a global
         * error), while a malformed body stays INVALID_ARGUMENT. */
        if (!context->header.recovery_query)
        {
            return DEVICE_LINK_STATUS_MALFORMED_FRAME;
        }
        if (!_ble_link_service_v2_parse_credential_id(
                    request, request_len, credential_id))
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        device_link_status_t auth_status =
            _ble_link_service_get_authorization(credential_id);

        _ble_link_service_zeroize(credential_id, sizeof(credential_id));
        if (auth_status != DEVICE_LINK_STATUS_OK)
        {
            return auth_status;
        }
        device_link_security_auth_record_t record;

        memset(&record, 0, sizeof(record));
        const esp_err_t load_result =
            device_link_security_load_auth_record(&record);

        if (load_result != ESP_OK ||
                !device_link_security_auth_record_valid(&record))
        {
            _ble_link_service_zeroize(&record, sizeof(record));
            return load_result == ESP_ERR_NOT_FOUND ?
                   DEVICE_LINK_STATUS_NOT_FOUND :
                   load_result == ESP_ERR_INVALID_STATE ?
                   DEVICE_LINK_STATUS_INTERNAL : DEVICE_LINK_STATUS_STORAGE;
        }
        auth_status = _ble_link_service_v2_encode_authorization_result(
                          record.credential_id, record.device_auth_id,
                          BLE_LINK_AUTHORIZATION_AUTHORIZED,
                          record.granted_permissions,
                          record.granted_permission_count, 0U,
                          response, response_capacity, response_len);
        _ble_link_service_zeroize(&record, sizeof(record));
        return auth_status;
    }
#ifdef UNIT_TEST_HOST
    ble_link_codec_request_t legacy_request;
    ble_link_dispatcher_facts_t legacy_facts;
    uint8_t legacy_body[32];
    size_t legacy_body_len = 0U;
    uint32_t error = BLE_LINK_ERROR_INTERNAL;

    memset(&legacy_request, 0, sizeof(legacy_request));
    legacy_request.request_id = context->header.call_id;
    legacy_request.body = (ble_link_codec_request_tag_t)(
                              BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE +
                              context->header.method_id - 3U);
    if (context->header.method_id == 5U)
    {
        legacy_request.body = BLE_LINK_CODEC_REQUEST_GET_AUTHORIZATION;
    }
    if (context->header.method_id == 4U)
    {
        device_link_tlv_reader_t reader;
        device_link_tlv_field_t field;
        bool has_field = false;
        uint64_t txn_id = 0U;

        if (device_link_tlv_reader_init(&reader, request, request_len) !=
                ESP_OK ||
                device_link_tlv_reader_next(&reader, &field, &has_field) !=
                ESP_OK || !has_field || field.id != 1U ||
                field.wire_type != DEVICE_LINK_TLV_FIXED64 ||
                (txn_id = field.value.fixed64_value) == 0U ||
                device_link_tlv_reader_next(&reader, &field, &has_field) !=
                ESP_OK || !has_field || field.id != 2U ||
                field.wire_type != DEVICE_LINK_TLV_LENGTH ||
                field.value.bytes.len !=
                BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES ||
                reader.offset != reader.len)
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        legacy_body[legacy_body_len++] = 0x09U;
        for (size_t i = 0U; i < 8U; ++i)
        {
            legacy_body[legacy_body_len++] =
                (uint8_t)(txn_id >> (8U * i));
        }
        legacy_body[legacy_body_len++] = 0x12U;
        legacy_body[legacy_body_len++] = 0x10U;
        memcpy(&legacy_body[legacy_body_len], field.value.bytes.data,
               field.value.bytes.len);
        legacy_body_len += field.value.bytes.len;
    }
    else if (context->header.method_id == 5U)
    {
        device_link_tlv_reader_t reader;
        device_link_tlv_field_t field;
        bool has_field = false;

        if (device_link_tlv_reader_init(&reader, request, request_len) !=
                ESP_OK ||
                device_link_tlv_reader_next(&reader, &field, &has_field) !=
                ESP_OK || !has_field || field.id != 1U ||
                field.wire_type != DEVICE_LINK_TLV_LENGTH ||
                field.value.bytes.len !=
                BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES ||
                reader.offset != reader.len)
        {
            return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
        }
        legacy_body[0] = 0x0aU;
        legacy_body[1] = 0x10U;
        memcpy(&legacy_body[2], field.value.bytes.data,
               field.value.bytes.len);
        legacy_body_len = 2U + field.value.bytes.len;
    }
    legacy_request.body_data = legacy_body;
    legacy_request.body_len = legacy_body_len;
    memset(&legacy_facts, 0, sizeof(legacy_facts));
    legacy_facts.active_boot_id = context->header.boot_id;
    legacy_facts.connection_generation = context->connection_generation;
    legacy_facts.encrypted = context->security_authenticated;
    legacy_facts.session_authenticated = context->security_authenticated;
    legacy_facts.authorized = context->authorized;
    legacy_facts.recovery_query = context->header.recovery_query;
    s_service.response_envelope_len = 0U;
    uint32_t (*handler)(const ble_link_codec_request_t *,
                        const ble_link_dispatcher_facts_t *, void *);

    if (context->header.method_id == 4U)
    {
        handler = _ble_link_service_handle_authorize_commit;
    }
    else
    {
        handler = _ble_link_service_handle_get_authorization;
    }
    s_service.v2_dispatch_active = true;
    error = handler(&legacy_request, &legacy_facts, NULL);
    s_service.v2_dispatch_active = false;
    device_link_status_t mapped_error =
        (error >= BLE_LINK_ERROR_OK && error <= BLE_LINK_ERROR_INTERNAL) ?
        (device_link_status_t)error : DEVICE_LINK_STATUS_INTERNAL;
    if (mapped_error == DEVICE_LINK_STATUS_CONFIRMATION_REQUIRED)
    {
        uint64_t token = ble_link_service_confirmation_token();
        if (token == 0U ||
                _ble_link_service_v2_encode_current_auth_result(
                    BLE_LINK_AUTHORIZATION_CONFIRMATION_PENDING, token,
                    response, response_capacity, response_len) !=
                DEVICE_LINK_STATUS_OK)
        {
            return DEVICE_LINK_STATUS_INTERNAL;
        }
    }
    else if (mapped_error == DEVICE_LINK_STATUS_OK && context->header.method_id == 4U)
    {
        if (_ble_link_service_v2_encode_current_auth_result(
                    BLE_LINK_AUTHORIZATION_AUTHORIZED, 0U, response,
                    response_capacity, response_len) != DEVICE_LINK_STATUS_OK)
        {
            return DEVICE_LINK_STATUS_INTERNAL;
        }
    }
    else if (mapped_error == DEVICE_LINK_STATUS_OK && context->header.method_id == 5U)
    {
        device_link_security_auth_record_t record;
        memset(&record, 0, sizeof(record));
        if (device_link_security_load_auth_record(&record) != ESP_OK ||
                !device_link_security_auth_record_valid(&record))
        {
            _ble_link_service_zeroize(&record, sizeof(record));
            return DEVICE_LINK_STATUS_INTERNAL;
        }
        const device_link_status_t result =
            _ble_link_service_v2_encode_authorization_result(
                record.credential_id, record.device_auth_id,
                BLE_LINK_AUTHORIZATION_AUTHORIZED,
                record.granted_permissions,
                record.granted_permission_count, 0U,
                response, response_capacity, response_len);
        _ble_link_service_zeroize(&record, sizeof(record));
        if (result != DEVICE_LINK_STATUS_OK)
        {
            return result;
        }
    }
    _ble_link_service_zeroize(requested_permissions,
                              sizeof(requested_permissions));
    return mapped_error;
#else
    _ble_link_service_zeroize(requested_permissions,
                              sizeof(requested_permissions));
    return DEVICE_LINK_STATUS_UNSUPPORTED_OPERATION;
#endif
}

esp_err_t ble_link_service_auth_expiry_tick(void)
{
    _ble_link_service_lock();
    (void)_ble_link_service_auth_expiry_tick_locked();
    _ble_link_service_unlock();
    return ESP_OK;
}

uint32_t ble_link_service_auth_expiry_remaining_ms(void)
{
    uint32_t remaining_ms = UINT32_MAX;

    _ble_link_service_lock();
    if (s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_IDLE &&
            s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_COMMITTED &&
            s_service.auth_txn.deadline_ms != 0U)
    {
        const TickType_t now = xTaskGetTickCount();

        if ((int32_t)(now - s_service.auth_txn.deadline_ms) >= 0)
        {
            remaining_ms = 0U;
        }
        else
        {
            const TickType_t remaining_ticks =
                s_service.auth_txn.deadline_ms - now;

            remaining_ms = (uint32_t)(((uint64_t)remaining_ticks * 1000U +
                                       configTICK_RATE_HZ - 1U) /
                                      configTICK_RATE_HZ);
        }
    }
    _ble_link_service_unlock();
    return remaining_ms;
}

esp_err_t ble_link_service_on_authenticated(void *arg)
{
    (void)arg;
    if (s_service.sec2_opened)
    {
        return ESP_OK;
    }
    /* The verifier kind was pinned by select_verifier when the handshake
     * started: it, not the pairing-window flag, decides bootstrap versus
     * long-term recovery. A bound peer keeps the long-term credential
     * during a replacement window; an unknown peer only ever reaches
     * bootstrap inside an open window. */
    device_link_security_verifier_kind_t kind =
        DEVICE_LINK_SECURITY_VERIFIER_NONE;
    const uint32_t epoch = s_service.current_facts.security_epoch;

    if (s_service.security != NULL &&
            s_service.security->selected_verifier != NULL)
    {
        kind = s_service.security->selected_verifier();
    }
    if (kind == DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP ||
            kind == DEVICE_LINK_SECURITY_VERIFIER_PUBLIC)
    {
        /* Bootstrap/public session: mark only the Security 2
         * authentication; authorization is established by the commit
         * (or stays absent on the public read-only path). */
        if (epoch == 0U ||
                ble_link_session_security2_authenticate_current(
                    s_service.current_facts.connection_generation,
                    epoch) != ESP_OK)
        {
            return ESP_ERR_INVALID_STATE;
        }
        s_service.sec2_opened = true;
        return ESP_OK;
    }
    if (kind != DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM)
    {
        /* No verifier was selected (window closed, no record): the
         * handshake should not have succeeded; fail closed. */
        return ESP_ERR_INVALID_STATE;
    }
    /* Long-term reconnect: the committed record must match the resolved
     * identity, and the record restores the bound/authorized state
     * before the session match is reported. */
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    if (device_link_security_load_auth_record(&record) != ESP_OK ||
            !device_link_security_auth_record_valid(&record) ||
            record.peer_addr_type != s_service.current_facts.peer_addr_type ||
            memcmp(record.peer_addr, s_service.current_facts.peer_addr, 6U) != 0)
    {
        _ble_link_service_zeroize(&record, sizeof(record));
        return ESP_ERR_INVALID_STATE;
    }
    _ble_link_service_zeroize(&record, sizeof(record));
    if (epoch == 0U ||
            ble_link_session_set_authorization(true, 0U) != ESP_OK ||
            ble_link_session_security2_authenticate_current(
                s_service.current_facts.connection_generation,
                epoch) != ESP_OK ||
            ble_link_session_report_session_match_current(
                s_service.current_facts.connection_generation, 0U) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_service.sec2_opened = true;
    return ESP_OK;
}

esp_err_t ble_link_service_confirm_binding(uint64_t token, bool accept)
{
    _ble_link_service_lock();
    if (token == 0U ||
            s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_COMMIT_PROBED ||
            s_service.auth_txn.confirmation_token != token ||
            s_service.auth_txn.connection_generation !=
            s_service.current_facts.connection_generation ||
            s_service.auth_txn.security_epoch !=
            s_service.current_facts.security_epoch)
    {
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!accept)
    {
        /* A denial is only legal in COMMIT_PROBED, before mutation. */
        _ble_link_service_abort_session(
            s_service.current_facts.connection_generation);
        _ble_link_service_unlock();
        return ESP_OK;
    }
    s_service.auth_txn.phase = BLE_LINK_AUTH_PHASE_LOCALLY_CONFIRMED;
    _ble_link_service_unlock();
    return ESP_OK;
}

bool ble_link_service_pending_confirmation(void)
{
    bool pending = false;

    _ble_link_service_lock();
    pending = s_service.auth_txn.phase == BLE_LINK_AUTH_PHASE_COMMIT_PROBED;
    _ble_link_service_unlock();
    return pending;
}

uint64_t ble_link_service_confirmation_token(void)
{
    uint64_t token = 0U;

    _ble_link_service_lock();
    if (s_service.auth_txn.phase == BLE_LINK_AUTH_PHASE_COMMIT_PROBED)
    {
        token = s_service.auth_txn.confirmation_token;
    }
    _ble_link_service_unlock();
    return token;
}

static void _ble_link_service_sweep_deferred_completion_locked(
    uint64_t now_ms)
{
    if (!s_service.deferred_completion.active)
    {
        return;
    }
    if ((int32_t)(now_ms - s_service.deferred_completion.deadline_ms) < 0)
    {
        return;
    }
    _ble_link_service_zeroize(
        s_service.deferred_completion.result,
        sizeof(s_service.deferred_completion.result));
    s_service.deferred_completion.active = false;
    s_service.deferred_completion.owner_id = 0U;
    s_service.deferred_completion.deadline_ms = 0U;
    s_service.deferred_completion.result_len = 0U;
}

static bool _ble_link_service_deferred_completion_params_valid(
    device_link_operation_state_t state, device_link_status_t status,
    const uint8_t *result, size_t result_len)
{
    if (state < DEVICE_LINK_OPERATION_PENDING ||
            state > DEVICE_LINK_OPERATION_CANCELED ||
            status < DEVICE_LINK_STATUS_OK ||
            status > DEVICE_LINK_STATUS_INTERNAL ||
            (result == NULL && result_len != 0U) ||
            result_len > DEVICE_LINK_OPERATION_RESULT_BYTES)
    {
        return false;
    }
    /* Mirror device_link_operation_update(): in-flight records report OK,
     * SUCCEEDED reports OK, FAILED requires a non-OK error, and non-success
     * terminal records never carry a result payload. */
    const bool in_flight = state == DEVICE_LINK_OPERATION_PENDING ||
                           state == DEVICE_LINK_OPERATION_RUNNING;
    const bool non_success_terminal =
        state == DEVICE_LINK_OPERATION_FAILED ||
        state == DEVICE_LINK_OPERATION_CANCELED;

    return !in_flight &&
           !((state == DEVICE_LINK_OPERATION_SUCCEEDED &&
              status != DEVICE_LINK_STATUS_OK) ||
             (state == DEVICE_LINK_OPERATION_FAILED &&
              status == DEVICE_LINK_STATUS_OK) ||
             (state == DEVICE_LINK_OPERATION_CANCELED &&
              status != DEVICE_LINK_STATUS_OK) ||
             (non_success_terminal && result_len != 0U));
}

esp_err_t ble_link_service_async_operation_start(
    uint8_t domain_id, uint8_t method_id, uint64_t owner_id,
    device_link_operation_cancel_t cancel, void *cancel_arg,
    uint64_t *out_operation_id)
{
    if (out_operation_id == NULL || owner_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_service_lock();
    if (!s_service.v2_ready)
    {
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint64_t now_ms = _ble_link_service_v2_now_ms();
    const device_link_method_descriptor_t *descriptor =
        _ble_link_service_v2_operation_descriptor(domain_id, method_id);

    if (descriptor == NULL || descriptor->operation_result_schema == NULL)
    {
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    _ble_link_service_sweep_deferred_completion_locked(now_ms);
    const esp_err_t result = device_link_operation_start_with_schema(
                                 &s_service.v2_operations, now_ms,
                                 domain_id, method_id, owner_id,
                                 descriptor->operation_result_schema,
                                 cancel, cancel_arg, out_operation_id);

    if (result != ESP_OK)
    {
        _ble_link_service_unlock();
        return result;
    }
    if (s_service.deferred_completion.active &&
            s_service.deferred_completion.owner_id == owner_id)
    {
        /* A completion bridge event for this manager operation arrived
         * before the table admission ran. Merge it now: the freshly
         * admitted PENDING record receives the retained terminal state
         * instead of leaking as a never-completing slot. The deferred
         * payload was validated when it was stored, so this update can
         * only fail on an internal invariant violation. */
        const esp_err_t merge_result = device_link_operation_update(
                                           &s_service.v2_operations, now_ms,
                                           *out_operation_id,
                                           s_service.deferred_completion.state,
                                           s_service.deferred_completion.status,
                                           s_service.deferred_completion.result,
                                           s_service.deferred_completion.result_len);
        _ble_link_service_zeroize(
            s_service.deferred_completion.result,
            sizeof(s_service.deferred_completion.result));
        s_service.deferred_completion.active = false;
        s_service.deferred_completion.owner_id = 0U;
        s_service.deferred_completion.deadline_ms = 0U;
        s_service.deferred_completion.result_len = 0U;
        if (merge_result != ESP_OK)
        {
            /* Admission already succeeded. A malformed retained terminal
             * must not leave the freshly allocated record PENDING; expose a
             * terminal internal failure so the caller can still observe and
             * retire the operation deterministically. */
            const esp_err_t failure_result = device_link_operation_update(
                                                 &s_service.v2_operations,
                                                 now_ms, *out_operation_id,
                                                 DEVICE_LINK_OPERATION_FAILED,
                                                 DEVICE_LINK_STATUS_INTERNAL,
                                                 NULL, 0U);
            _ble_link_service_unlock();
            return failure_result == ESP_OK ? ESP_OK : merge_result;
        }
    }
    _ble_link_service_unlock();
    return result;
}

esp_err_t ble_link_service_async_operation_update(
    uint64_t owner_id, device_link_operation_state_t state,
    device_link_status_t status, const uint8_t *result, size_t result_len)
{
    if (owner_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_service_lock();
    const uint64_t now_ms = _ble_link_service_v2_now_ms();
    device_link_operation_t operation;

    _ble_link_service_sweep_deferred_completion_locked(now_ms);
    const esp_err_t find_result = device_link_operation_find_by_owner(
                                      &s_service.v2_operations, owner_id,
                                      &operation);

    if (find_result != ESP_OK)
    {
        _ble_link_service_unlock();
        return find_result;
    }
    const esp_err_t result_status = device_link_operation_update(
                                        &s_service.v2_operations, now_ms,
                                        operation.id, state, status,
                                        result, result_len);

    _ble_link_service_unlock();
    return result_status;
}

esp_err_t ble_link_service_async_operation_defer_update(
    uint64_t owner_id, device_link_operation_state_t state,
    device_link_status_t status, const uint8_t *result, size_t result_len)
{
    if (owner_id == 0U ||
            !_ble_link_service_deferred_completion_params_valid(
                state, status, result, result_len))
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_service_lock();
    if (!s_service.v2_ready)
    {
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint64_t now_ms = _ble_link_service_v2_now_ms();

    _ble_link_service_sweep_deferred_completion_locked(now_ms);
    device_link_operation_t operation;
    const esp_err_t find_result = device_link_operation_find_by_owner(
                                      &s_service.v2_operations, owner_id,
                                      &operation);

    if (find_result == ESP_OK)
    {
        /* The table admission landed between the caller's failed update
         * and this deferral: apply the terminal directly. */
        const esp_err_t result_status = device_link_operation_update(
                                            &s_service.v2_operations,
                                            now_ms, operation.id, state,
                                            status, result, result_len);

        _ble_link_service_unlock();
        return result_status;
    }
    if (s_service.deferred_completion.active)
    {
        /* One manager operation id is unique per boot, so a second
         * terminal for a still-unmatched owner can only be a superseded
         * snapshot; keep the newest and never leak the old payload. */
        _ble_link_service_zeroize(
            s_service.deferred_completion.result,
            sizeof(s_service.deferred_completion.result));
    }
    s_service.deferred_completion.active = true;
    s_service.deferred_completion.owner_id = owner_id;
    s_service.deferred_completion.deadline_ms =
        now_ms + BLE_LINK_SERVICE_DEFERRED_COMPLETION_TTL_MS;
    s_service.deferred_completion.state = state;
    s_service.deferred_completion.status = status;
    s_service.deferred_completion.result_len = result_len;
    _ble_link_service_zeroize(
        s_service.deferred_completion.result,
        sizeof(s_service.deferred_completion.result));
    if (result_len != 0U)
    {
        memcpy(s_service.deferred_completion.result, result, result_len);
    }
    _ble_link_service_unlock();
    return ESP_OK;
}

bool ble_link_service_async_operation_in_flight(uint8_t domain_id)
{
    bool in_flight = false;

    _ble_link_service_lock();
    for (size_t i = 0U; i < DEVICE_LINK_MAX_OPERATIONS; ++i)
    {
        const device_link_operation_t *operation =
            &s_service.v2_operations.slots[i];

        if (operation->id != 0U &&
                operation->domain_id == domain_id &&
                operation->state != DEVICE_LINK_OPERATION_SUCCEEDED &&
                operation->state != DEVICE_LINK_OPERATION_FAILED &&
                operation->state != DEVICE_LINK_OPERATION_CANCELED)
        {
            in_flight = true;
            break;
        }
    }
    _ble_link_service_unlock();
    return in_flight;
}

#ifdef UNIT_TEST_HOST
esp_err_t ble_link_service_test_copy_operation(
    uint64_t operation_id, device_link_operation_t *operation)
{
    if (operation_id == 0U || operation == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_service_lock();
    const device_link_operation_t *slot = NULL;
    const esp_err_t result = device_link_operation_get(
                                 &s_service.v2_operations,
                                 _ble_link_service_v2_now_ms(),
                                 operation_id, &slot);

    if (result == ESP_OK)
    {
        *operation = *slot;
    }
    _ble_link_service_unlock();
    return result;
}

device_link_status_t ble_link_service_test_encode_operation(
    uint64_t operation_id, uint8_t domain_id, uint8_t method_id,
    device_link_operation_state_t state, device_link_status_t status,
    const uint8_t *result, size_t result_len,
    uint8_t *response, size_t capacity, size_t *response_len)
{
    device_link_operation_t operation;

    memset(&operation, 0, sizeof(operation));
    operation.id = operation_id;
    operation.domain_id = domain_id;
    operation.method_id = method_id;
    operation.state = state;
    operation.status = status;
    if (result_len > sizeof(operation.result))
    {
        return DEVICE_LINK_STATUS_INVALID_ARGUMENT;
    }
    if (result_len != 0U && result != NULL)
    {
        memcpy(operation.result, result, result_len);
    }
    operation.result_len = result_len;
    return _ble_link_service_v2_encode_operation(
               &operation, response, capacity, response_len);
}
#endif

esp_err_t ble_link_service_set_domain_descriptors(
    const device_link_domain_descriptor_t *domains, size_t domain_count)
{
    if (s_service.boot_id != 0U || domain_count >
            BLE_LINK_SERVICE_MAX_DOMAINS - 1U ||
            (domains == NULL && domain_count != 0U))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (domain_count != 0U &&
            device_link_domain_descriptors_validate(domains, domain_count) !=
            ESP_OK)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < domain_count; ++i)
    {
        if (domains[i].domain_id == DEVICE_LINK_DOMAIN_CORE ||
                (i != 0U && domains[i - 1U].domain_id >= domains[i].domain_id))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    memset(s_optional_domains, 0, sizeof(s_optional_domains));
    if (domain_count != 0U)
    {
        memcpy(s_optional_domains, domains,
               domain_count * sizeof(s_optional_domains[0]));
    }
    s_optional_domain_count = domain_count;
    return ESP_OK;
}

void ble_link_service_init(
    uint64_t boot_id, ble_link_service_output_t output, void *arg,
    const ble_link_security_ops_t *security, size_t max_pending_frames)
{
    if (s_service_mutex == NULL)
    {
        s_service_mutex = xSemaphoreCreateRecursiveMutexStatic(
                              &s_service_mutex_control);
    }
    memset(&s_service, 0, sizeof(s_service));
    memset(&s_ingress, 0, sizeof(s_ingress));
    memset(s_work_pool, 0, sizeof(s_work_pool));
    s_service.boot_id = boot_id;
    s_service.output = output;
    s_service.output_arg = arg;
    s_service.security = security;
    (void)max_pending_frames;
    ble_link_reassembler_init(&s_ingress.reassembler[0],
                              s_ingress.session_buffer,
                              BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES);
    ble_link_reassembler_init(&s_ingress.reassembler[1],
                              s_ingress.control_buffer,
                              BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES);
    s_ingress.epoch = 1U;
    const device_link_core_callbacks_t v2_callbacks =
    {
        .method = _ble_link_service_v2_method,
        .arg = &s_service,
    };
    const esp_err_t core_result = device_link_core_init(
                                      &s_service.v2_core, &v2_callbacks);

    s_service.v2_domains[0] = s_service.v2_core.domain;
    s_service.v2_domain_count = core_result == ESP_OK ? 1U : 0U;
    if (core_result == ESP_OK && s_optional_domain_count != 0U)
    {
        memcpy(&s_service.v2_domains[1], s_optional_domains,
               s_optional_domain_count * sizeof(s_optional_domains[0]));
        s_service.v2_domain_count += s_optional_domain_count;
    }

    for (size_t i = 0U; i < DEVICE_LINK_REPLAY_SLOTS; ++i)
    {
        s_service.v2_replay[i].response = s_service.v2_replay_response[i];
        s_service.v2_replay[i].response_capacity =
            sizeof(s_service.v2_replay_response[i]);
    }
    const esp_err_t router_result = core_result == ESP_OK ?
                                    device_link_router_init(
                                        &s_service.v2_router, boot_id,
                                        s_service.v2_domains,
                                        s_service.v2_domain_count,
                                        s_service.v2_replay,
                                        DEVICE_LINK_REPLAY_SLOTS,
                                        device_link_digest_sha256, NULL) :
                                    core_result;
    s_service.v2_ready = router_result == ESP_OK;
    if (s_service.v2_ready && device_link_operation_table_init(
                &s_service.v2_operations, boot_id) != ESP_OK)
    {
        s_service.v2_ready = false;
        LOG_E("Typed-TLV operation table init failed");
    }
    if (!s_service.v2_ready)
    {
        LOG_E("Typed-TLV Core v2 init failed result=%d", router_result);
    }
#ifdef UNIT_TEST_HOST
    /* Legacy Envelope dispatch is retired in production: the v2 typed-TLV
     * router owns all application methods. The adapters below exist solely
     * for host regression fixtures. */
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_GET_MANIFEST,
        _ble_link_service_handle_manifest, NULL);
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_GET_LINK_SNAPSHOT,
        _ble_link_service_handle_snapshot, NULL);
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE,
        _ble_link_service_handle_authorize_prepare, NULL);
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_AUTHORIZE_COMMIT,
        _ble_link_service_handle_authorize_commit, NULL);
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_GET_AUTHORIZATION,
        _ble_link_service_handle_get_authorization, NULL);
#endif
    /* Domain and operation methods are deliberately not registered until a
     * startup-frozen descriptor with a complete owner adapter enables them. */
}

void ble_link_service_reset(void)
{
#ifdef UNIT_TEST_HOST
    ble_link_dispatcher_reset();
#endif
    _ble_link_service_reset_v2_replay();
    _ble_link_service_clear_delayed_cmd0();
    _ble_link_service_stream_free();
    memset(&s_service, 0, sizeof(s_service));
    memset(&s_ingress, 0, sizeof(s_ingress));
    memset(s_work_pool, 0, sizeof(s_work_pool));
}

static void _ble_link_service_clear_session_state_locked(bool retire_acl)
{
    const uint32_t generation =
        s_service.current_facts.connection_generation;

    if (generation != 0U)
    {
        _ble_link_service_discard_provisional_bond(generation, true);
    }
    s_service.pending_transactions = 0U;
    s_service.completion.pending = false;
    s_service.completion.flow_id = 0U;
    s_service.completion.is_last = false;
    s_service.deferred_busy.active = false;
    _ble_link_service_clear_delayed_cmd0();
    _ble_link_service_reset_v2_replay();
    _ble_link_service_reset_ingress();
    s_service.subscriber.active = false;
    s_service.handshake_active = false;
    s_service.sec2_opened = false;
    s_service.lt_switch.active = false;
    s_service.close_after_encrypt.active = false;
    _ble_link_service_clear_auth_txn();
    _ble_link_service_stream_free();
#ifdef UNIT_TEST_HOST
    ble_link_dispatcher_clear_session();
#endif
    /* Sync the external link-session facts with the adapter teardown.
     * The generation snapshot and the close run inside the same service
     * critical section: a worker executing a new generation's handshake
     * cannot interleave a current_facts update between the snapshot and
     * the close, so a stale clear can never close a newer session. The
     * committed authorization record survives a disconnect: only the
     * session-level authorization (security2_open/authorized) is
     * cleared, never the persistent bound fact. The close itself
     * validates the generation and is a no-op for a retired one. */
    if (generation != 0U)
    {
        (void)ble_link_session_security2_close_current(generation);
        if (s_service.security != NULL &&
                s_service.security->close_session != NULL)
        {
            s_service.security->close_session();
        }
    }
    if (retire_acl)
    {
        _ble_link_service_clear_committed_replay();
    }
    memset(&s_service.current_facts, 0,
           sizeof(s_service.current_facts));
}

void ble_link_service_clear_session_state(void)
{
    _ble_link_service_lock();
    _ble_link_service_clear_session_state_locked(true);
    _ble_link_service_unlock();
}

esp_err_t ble_link_service_clear_session_state_if_current(
    const ble_link_operation_identity_t *identity)
{
    if (identity == NULL || identity->generation == 0U ||
            identity->conn_handle == UINT16_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    switch (identity->kind)
    {
    case BLE_LINK_OPERATION_DISCONNECT:
    case BLE_LINK_OPERATION_RESET:
    case BLE_LINK_OPERATION_ENCRYPT_CHANGE:
        if (identity->flow_id != 0U || identity->token != 0U)
        {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case BLE_LINK_OPERATION_TX_INDICATE:
        if (identity->flow_id == 0U || identity->token == 0U)
        {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case BLE_LINK_OPERATION_TERMINATE:
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    _ble_link_service_lock();
    const bool retire_acl =
        identity->kind == BLE_LINK_OPERATION_DISCONNECT ||
        identity->kind == BLE_LINK_OPERATION_RESET ||
        identity->kind == BLE_LINK_OPERATION_TERMINATE;
    const bool current_facts =
        s_service.current_facts.connection_generation ==
        identity->generation &&
        s_service.current_facts.conn_handle == identity->conn_handle;
    const bool current_ingress =
        !s_ingress.retired &&
        s_ingress.generation == identity->generation &&
        s_ingress.conn_handle == identity->conn_handle;
    bool current = current_facts || (retire_acl && current_ingress);

    /* An ACL terminal event has generation+handle authority over every
     * Security 2 epoch on that ACL. This closes a Cmd0 that advanced the
     * epoch while DISCONNECT/RESET waited for the service mutex. Session-only
     * failures remain epoch-scoped and cannot retire a newer handshake. */
    if (current && !retire_acl)
    {
        current = s_service.current_facts.security_epoch ==
                  identity->security_epoch;
    }

    if (current && identity->flow_id != 0U)
    {
        current = s_service.stream.flow_id == identity->flow_id ||
                  (s_service.delayed_cmd0.active &&
                   s_service.delayed_cmd0.old_flow_id == identity->flow_id);
    }
    if (!current)
    {
        _ble_link_service_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    _ble_link_service_clear_session_state_locked(retire_acl);
    if (retire_acl && current_ingress)
    {
        /* Preserve the generation floor while ensuring a repeated terminal
         * callback and any same-generation ingress are permanent no-ops. */
        s_ingress.retired = true;
    }
    _ble_link_service_unlock();
    return ESP_OK;
}

bool ble_link_service_has_partial_frame(
    ble_link_service_rx_channel_t channel)
{
    if (channel != BLE_LINK_SERVICE_RX_SESSION &&
            channel != BLE_LINK_SERVICE_RX_CONTROL)
    {
        return false;
    }
    _ble_link_service_lock();
    const bool partial = s_ingress.reassembler[channel].started;

    _ble_link_service_unlock();
    return partial;
}

esp_err_t ble_link_service_get_reassembly_state(
    ble_link_service_rx_channel_t channel,
    bool *out_partial, uint32_t *out_epoch)
{
    ble_link_reassembly_disposition_t disposition;

    return ble_link_service_get_reassembly_state_ex(
               channel, out_partial, out_epoch, &disposition);
}

esp_err_t ble_link_service_get_reassembly_state_ex(
    ble_link_service_rx_channel_t channel,
    bool *out_partial, uint32_t *out_epoch,
    ble_link_reassembly_disposition_t *out_disposition)
{
    if ((channel != BLE_LINK_SERVICE_RX_SESSION &&
            channel != BLE_LINK_SERVICE_RX_CONTROL) ||
            out_partial == NULL || out_epoch == NULL ||
            out_disposition == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_service_lock();
    *out_partial = s_ingress.reassembler[channel].started;
    *out_epoch = s_ingress.epoch;
    *out_disposition = s_ingress.last_disposition[channel];
    _ble_link_service_unlock();
    return ESP_OK;
}

void ble_link_service_idle_timeout(uint32_t generation)
{
    _ble_link_service_lock();
    /* close_current validates the generation; a stale timeout has no
     * effect. */
    _ble_link_service_abort_session(generation);
    _ble_link_service_unlock();
}

void ble_link_service_idle_timeout_epoch(
    uint32_t generation, uint32_t epoch)
{
    _ble_link_service_lock();
    if (!s_ingress.exhausted && epoch == s_ingress.epoch)
    {
        _ble_link_service_abort_session(generation);
    }
    _ble_link_service_unlock();
}

esp_err_t ble_link_service_feed(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const uint8_t *value, size_t len)
{
    ble_link_work_t *work = NULL;
    esp_err_t result = ble_link_service_accept(facts, channel, value, len,
                       &work);

    if (result == ESP_OK && work != NULL)
    {
        result = ble_link_service_execute(work);
    }
    ble_link_service_release_work(work);
    return result;
}

esp_err_t ble_link_service_accept(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const uint8_t *value, size_t len,
    ble_link_work_t **out_work)
{
    if (out_work == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_work = NULL;
    _ble_link_service_lock();
    const esp_err_t result = _ble_link_service_accept_locked(
                                 facts, channel, value, len, out_work);

    _ble_link_service_unlock();
    return result;
}

static bool _ble_link_service_fragment_matches_message(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const ble_link_fragment_t *fragment,
    const ble_link_service_facts_t *expected_facts,
    ble_link_service_rx_channel_t expected_channel,
    uint8_t expected_transport_type,
    const uint8_t *expected_message, size_t expected_message_len)
{
    const bool start =
        (fragment->flags & BLE_LINK_FRAMING_FLAG_START) != 0U;
    const bool end =
        (fragment->flags & BLE_LINK_FRAMING_FLAG_END) != 0U;
    const size_t expected_total = expected_message_len + 1U;

    if (facts->connection_generation !=
            expected_facts->connection_generation ||
            facts->conn_handle != expected_facts->conn_handle ||
            channel != expected_channel ||
            fragment->version != BLE_LINK_FRAMING_VERSION ||
            (fragment->flags & ~(BLE_LINK_FRAMING_FLAG_START |
                                 BLE_LINK_FRAMING_FLAG_END)) != 0U ||
            fragment->frame_id == 0U || fragment->payload_len == 0U ||
            fragment->total_length != expected_total ||
            fragment->offset >= expected_total ||
            fragment->payload_len > expected_total - fragment->offset ||
            start != (fragment->offset == 0U) ||
            end != (fragment->offset + fragment->payload_len ==
                    expected_total))
    {
        return false;
    }
    for (size_t i = 0U; i < fragment->payload_len; ++i)
    {
        const size_t offset = fragment->offset + i;
        const uint8_t expected = offset == 0U ? expected_transport_type :
                                 expected_message[offset - 1U];

        if (fragment->payload[i] != expected)
        {
            return false;
        }
    }
    return true;
}

static esp_err_t _ble_link_service_reserved_handshake_admission_locked(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const ble_link_fragment_t *fragment)
{
    const ble_link_work_t *queued = s_service.queued_handshake.work;
    bool duplicate = false;

    if (queued != NULL)
    {
        duplicate = _ble_link_service_fragment_matches_message(
                        facts, channel, fragment, &queued->facts,
                        queued->channel, queued->transport_type,
                        queued->message, queued->message_len);
    }
    else if (s_service.delayed_cmd0.active)
    {
        duplicate = _ble_link_service_fragment_matches_message(
                        facts, channel, fragment,
                        &s_service.delayed_cmd0.facts,
                        BLE_LINK_SERVICE_RX_SESSION,
                        BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE,
                        s_service.delayed_cmd0.message,
                        s_service.delayed_cmd0.message_len);
    }
    if (!duplicate)
    {
        return ESP_ERR_NOT_ALLOWED;
    }
    return (fragment->flags & BLE_LINK_FRAMING_FLAG_END) != 0U ?
           ESP_OK : ESP_ERR_NOT_FINISHED;
}

static esp_err_t _ble_link_service_accept_locked(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const uint8_t *value, size_t len,
    ble_link_work_t **out_work)
{
    if (facts == NULL || value == NULL || s_service.output == NULL ||
            facts->connection_generation == 0U ||
            facts->conn_handle == UINT16_MAX ||
            (channel != BLE_LINK_SERVICE_RX_SESSION &&
             channel != BLE_LINK_SERVICE_RX_CONTROL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ingress.exhausted)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (facts->connection_generation == s_ingress.generation &&
            s_ingress.retired)
    {
        return ESP_OK;
    }
    if (facts->connection_generation != s_ingress.generation)
    {
        if (facts->connection_generation < s_ingress.generation)
        {
            /* A stale feed from a retired generation has no effect. */
            return ESP_OK;
        }
        _ble_link_service_reset_ingress();
        if (s_ingress.exhausted)
        {
            return ESP_ERR_INVALID_STATE;
        }
        s_ingress.generation = facts->connection_generation;
        s_ingress.conn_handle = facts->conn_handle;
        s_ingress.retired = false;
    }
    else if (facts->connection_generation != 0U &&
             facts->conn_handle != s_ingress.conn_handle)
    {
        /* One generation names exactly one ACL. A reused handle receives a
         * new generation before any ingress can be admitted. */
        return ESP_ERR_INVALID_STATE;
    }
    ble_link_reassembler_t *slot = &s_ingress.reassembler[channel];
    const size_t slot_capacity =
        (channel == BLE_LINK_SERVICE_RX_SESSION) ?
        BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES :
        BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES;
    ble_link_fragment_t fragment;

    if (ble_link_reassembler_parse(value, len, &fragment) != ESP_OK)
    {
        if (s_service.queued_handshake.work != NULL ||
                s_service.delayed_cmd0.active)
        {
            return ESP_ERR_NOT_ALLOWED;
        }
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_service.queued_handshake.work != NULL ||
            s_service.delayed_cmd0.active)
    {
        return _ble_link_service_reserved_handshake_admission_locked(
                   facts, channel, &fragment);
    }
    const uint16_t total_length = fragment.total_length;

    ble_link_reassembly_disposition_t disposition;
    esp_err_t result = ble_link_reassembler_accept_ex(
                           slot, &fragment, &disposition);

    if (result == ESP_OK)
    {
        s_ingress.last_disposition[channel] = disposition;
    }
    if (result != ESP_OK)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return result;
    }
    if (disposition != BLE_LINK_REASSEMBLY_COMPLETE)
    {
        return ESP_ERR_NOT_FINISHED;
    }
    if (total_length > slot_capacity)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_NO_MEM;
    }
    /* Transport type routing: the reassembled message begins with a type
     * byte. 0x00 is the Security 2 handshake wire and is accepted only on
     * session_rx; 0x01 is protected Device Link v2 application data and is
     * accepted on either channel while a Security 2 session is wired. */
    if (total_length < 1U)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t transport_type = slot->buffer[0];
    const uint8_t *message = &slot->buffer[1];
    const size_t message_len = total_length - 1U;

    ble_link_work_t *work = _ble_link_service_allocate_work(message_len);

    if (work == NULL)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_NO_MEM;
    }
    work->facts = *facts;
    work->channel = channel;
    work->epoch = s_ingress.epoch;
    work->transport_type = transport_type;
    work->message_len = message_len;
    memcpy(work->message, message, message_len);
    if (transport_type == BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE)
    {
        s_service.queued_handshake.work = work;
    }
    *out_work = work;
    return ESP_OK;
}

esp_err_t ble_link_service_execute(ble_link_work_t *work)
{
    if (work == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_service_lock();
    if (work->transport_type == BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE &&
            s_service.queued_handshake.work != work)
    {
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.queued_handshake.work == work)
    {
        /* Claim and execution share the service lock. A replacement Cmd0
         * therefore moves directly from queued admission ownership into
         * delayed_cmd0 without exposing an unguarded ingress window. */
        s_service.queued_handshake.work = NULL;
    }
    const esp_err_t result = _ble_link_service_execute_locked(work);

    _ble_link_service_unlock();
    return result;
}

void ble_link_service_release_work(ble_link_work_t *work)
{
    if (work != NULL)
    {
        _ble_link_service_lock();
        if (s_service.queued_handshake.work == work)
        {
            /* Queue submission failed or teardown drained an unexecuted
             * work item. Its admission obligation ends with ownership. */
            s_service.queued_handshake.work = NULL;
            ble_link_reassembler_reset(
                &s_ingress.reassembler[work->channel]);
        }
        _ble_link_service_unlock();
        _ble_link_service_zeroize(work, sizeof(*work));
        work->in_use = false;
    }
}

static esp_err_t _ble_link_service_process_handshake_locked(
    const ble_link_service_facts_t *facts,
    const uint8_t *message, size_t message_len,
    device_link_security_handshake_stage_t stage,
    bool session_already_retired)
{
    if (stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD0)
    {
        if (!session_already_retired &&
                _ble_link_service_retire_logical_session(
                    facts->connection_generation, true) != ESP_OK)
        {
            return ESP_ERR_INVALID_STATE;
        }
        if (s_ingress.exhausted)
        {
            return ESP_ERR_INVALID_STATE;
        }
        if (s_service.lt_install_pending)
        {
            if (device_link_security_load_long_term_verifier() != ESP_OK)
            {
                _ble_link_service_abort_session(
                    facts->connection_generation);
                return ESP_ERR_INVALID_STATE;
            }
            s_service.lt_install_pending = false;
        }
        if (s_service.security->select_verifier != NULL)
        {
            const esp_err_t select_result =
                s_service.security->select_verifier(
                    facts->peer_addr_type, facts->peer_addr,
                    sizeof(facts->peer_addr), facts->pairing_window_open);

            if (select_result != ESP_OK)
            {
                _ble_link_service_abort_session(
                    facts->connection_generation);
                return select_result;
            }
        }
        s_service.handshake_active = true;
    }
    else if (stage != DEVICE_LINK_SECURITY_HANDSHAKE_CMD1 ||
             !s_service.handshake_active)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *out = NULL;
    size_t out_len = 0U;
    device_link_security_handshake_result_t handshake_result;
    const esp_err_t result = s_service.security->handshake(
                                 message, message_len, &out, &out_len,
                                 &handshake_result);

    if (result != ESP_OK)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return result;
    }
    if (out == NULL || handshake_result.stage != stage)
    {
        free(out);
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (handshake_result.authenticated)
    {
        s_service.handshake_active = false;
    }
    s_service.pending_transactions++;
    if (!_ble_link_service_emit_fragments(
                out, out_len, BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE,
                facts->preferred_att_mtu, BLE_LINK_SERVICE_TX_SESSION))
    {
        free(out);
        s_service.pending_transactions = 0U;
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_NO_MEM;
    }
    free(out);
    return ESP_OK;
}

static esp_err_t _ble_link_service_execute_locked(ble_link_work_t *work)
{
    if (s_ingress.exhausted || work->epoch != s_ingress.epoch)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const ble_link_service_facts_t *facts = &work->facts;
    const ble_link_service_rx_channel_t channel = work->channel;
    const uint8_t transport_type = work->transport_type;
    const uint8_t *message = work->message;
    const size_t message_len = work->message_len;

    if (facts->connection_generation != s_service.execution_generation)
    {
        if (facts->connection_generation < s_service.execution_generation)
        {
            return ESP_ERR_INVALID_STATE;
        }
        s_service.subscriber.active = false;
        _ble_link_service_clear_auth_txn();
        s_service.pending_transactions = 0U;
        s_service.completion.pending = false;
        s_service.completion.flow_id = 0U;
        s_service.completion.is_last = false;
        s_service.deferred_busy.active = false;
        _ble_link_service_clear_delayed_cmd0();
        _ble_link_service_clear_committed_replay();
        _ble_link_service_reset_v2_replay();
        s_service.lt_switch.active = false;
        s_service.close_after_encrypt.active = false;
        _ble_link_service_stream_free();
#ifdef UNIT_TEST_HOST
        ble_link_dispatcher_clear_session();
#endif
        s_service.execution_generation = facts->connection_generation;
    }

    s_service.current_facts = *facts;
    s_service.current_channel = channel;
    if (s_service.remote_replacement.active &&
            s_service.remote_replacement.identity.generation ==
            facts->connection_generation)
    {
        (void)_ble_link_service_process_remote_replacement_locked();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.delayed_cmd0.active &&
            transport_type != BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE)
    {
        /* The retained Cmd0 owns the next transaction. Other ingress is
         * BUSY and must not disturb either the old indication deadline or
         * the retained replacement. */
        return ESP_ERR_INVALID_STATE;
    }
    if (transport_type == BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE)
    {
        if (channel != BLE_LINK_SERVICE_RX_SESSION ||
                s_service.security == NULL ||
                s_service.security->classify_handshake == NULL ||
                s_service.security->handshake == NULL)
        {
            _ble_link_service_abort_session(facts->connection_generation);
            return ESP_ERR_INVALID_STATE;
        }
        if (s_service.delayed_cmd0.active)
        {
            const bool exact_duplicate =
                facts->connection_generation ==
                s_service.delayed_cmd0.generation &&
                message_len == s_service.delayed_cmd0.message_len &&
                memcmp(message, s_service.delayed_cmd0.message,
                       message_len) == 0;

            return exact_duplicate ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
        device_link_security_handshake_stage_t stage;
        const esp_err_t classify_result =
            s_service.security->classify_handshake(
                message, message_len, &stage);

        if (classify_result != ESP_OK)
        {
            _ble_link_service_abort_session(facts->connection_generation);
            return classify_result;
        }
        if (stage == DEVICE_LINK_SECURITY_HANDSHAKE_CMD0 &&
                s_service.pending_transactions > 0U)
        {
            if (s_service.stream.flow_id == 0U ||
                    s_service.stream.payload == NULL ||
                    message_len > sizeof(s_service.delayed_cmd0.message))
            {
                _ble_link_service_abort_session(
                    facts->connection_generation);
                return ESP_ERR_INVALID_STATE;
            }
            const uint32_t old_flow_id = s_service.stream.flow_id;

            if (_ble_link_service_retire_logical_session(
                        facts->connection_generation, false) != ESP_OK)
            {
                return ESP_ERR_INVALID_STATE;
            }
            s_service.delayed_cmd0.active = true;
            s_service.delayed_cmd0.generation =
                facts->connection_generation;
            s_service.delayed_cmd0.old_flow_id = old_flow_id;
            s_service.delayed_cmd0.facts = *facts;
            s_service.delayed_cmd0.facts.security_epoch =
                s_service.current_facts.security_epoch;
            s_service.delayed_cmd0.message_len = message_len;
            memcpy(s_service.delayed_cmd0.message, message, message_len);
            return ESP_OK;
        }
        if (s_service.pending_transactions > 0U)
        {
            return ESP_ERR_INVALID_STATE;
        }
        return _ble_link_service_process_handshake_locked(
                   facts, message, message_len, stage, false);
    }
    /* Protected traffic only flows after the handshake completed. */
    s_service.handshake_active = false;
    if (transport_type != BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_service.security != NULL &&
            s_service.security->unprotect != NULL)
    {
        uint8_t *out = NULL;
        size_t out_len = 0U;
        bool emitted = true;
        const esp_err_t unprotect_result = s_service.security->unprotect(
                                               message, message_len,
                                               &out, &out_len);

        if (unprotect_result != ESP_OK)
        {
            _ble_link_service_abort_session(facts->connection_generation);
            return unprotect_result;
        }
        if (out != NULL)
        {
            s_service.pending_transactions++;
            if (!_ble_link_service_emit_fragments(
                        out, out_len, BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED,
                        facts->preferred_att_mtu,
                        _ble_link_service_response_channel()))
            {
                free(out);
                /* The response could not be handed to the transport: return
                 * the failure so the adapter closes the request session. */
                emitted = false;
                s_service.pending_transactions = 0U;
                _ble_link_service_abort_session(
                    facts->connection_generation);
                return ESP_ERR_NO_MEM;
            }
            free(out);
        }
        /* One-shot, generation-scoped post-response actions: they consume
         * only after the response was encrypted and handed to the
         * transport, never on a TX outcome that could be lost. */
        if (emitted && s_service.close_after_encrypt.active &&
                s_service.close_after_encrypt.generation ==
                facts->connection_generation)
        {
            /* A terminal pre-durable Commit failure: the stable error
             * response is on its way; close both Security 2 layers so the
             * bootstrap session cannot outlive the flow. */
            s_service.close_after_encrypt.active = false;
            (void)ble_link_session_security2_close_current(
                facts->connection_generation);
            if (s_service.security != NULL)
            {
                s_service.security->close_session();
            }
            s_service.sec2_opened = false;
        }
        if (emitted && s_service.lt_switch.active &&
                s_service.lt_switch.generation ==
                facts->connection_generation)
        {
            /* A commit switched the authorization record: activate the
             * long-term verifier after the bootstrap response was handed
             * to the transport. A load failure keeps the durable install
             * obligation (retried at the next handshake or startup); the
             * session still closes so no GATT path admits traffic the
             * adapter cannot serve, while the persistent bound fact stays
             * set (the record was just committed). */
            s_service.lt_switch.active = false;
            const esp_err_t load_result =
                device_link_security_load_long_term_verifier();

            if (load_result != ESP_OK)
            {
                /* The obligation stays armed for the handshake retry. */
                LOG_E("long-term verifier install failed result=%d",
                      load_result);
            }
            else
            {
                s_service.lt_install_pending = false;
            }
            (void)ble_link_session_security2_close_current(
                facts->connection_generation);
            if (s_service.security != NULL)
            {
                s_service.security->close_session();
            }
            s_service.sec2_opened = false;
        }
        return ESP_OK;
    }
    {
        /* Host harness without a session: plaintext pipeline. */
        uint8_t *plain_response = NULL;
        size_t plain_response_len = 0U;
        const esp_err_t plain_result = ble_link_service_process_plaintext(
                                           message, message_len,
                                           &plain_response,
                                           &plain_response_len);


        if (plain_result != ESP_OK)
        {
            ble_link_service_release_plaintext(
                plain_response, plain_response_len);
            return plain_result;
        }
        if (plain_response != NULL)
        {
            s_service.pending_transactions++;
            if (!_ble_link_service_emit_protected(
                        plain_response, plain_response_len,
                        BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED,
                        facts->preferred_att_mtu,
                        _ble_link_service_response_channel()))
            {
                /* Do not report success after the response was rejected by
                 * the transport; the caller must close the session. */
                s_service.pending_transactions = 0U;
                _ble_link_service_abort_session(
                    facts->connection_generation);
                ble_link_service_release_plaintext(
                    plain_response, plain_response_len);
                return ESP_ERR_NO_MEM;
            }
            ble_link_service_release_plaintext(
                plain_response, plain_response_len);
        }
        return ESP_OK;
    }
}

esp_err_t ble_link_service_process_plaintext(
    const uint8_t *msg, size_t len,
    uint8_t **response, size_t *response_len)
{
    if (response == NULL || response_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *response = NULL;
    *response_len = 0U;
    if (msg == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    device_link_wire_header_t v2_header;

    if (s_service.v2_ready &&
            device_link_wire_decode_header(msg, len, &v2_header) == ESP_OK)
    {
        uint16_t permissions[DEVICE_LINK_SECURITY_AUTH_MAX_GRANTS] = {0};
        size_t permission_count = 0U;
        ble_link_dispatcher_facts_t live_facts;
        device_link_request_context_t context;

        memset(&context, 0, sizeof(context));
        context.header = v2_header;
        context.connection_generation =
            s_service.current_facts.connection_generation;
        context.security_epoch = s_service.current_facts.security_epoch;
        context.channel =
            s_service.current_channel == BLE_LINK_SERVICE_RX_SESSION ?
            DEVICE_LINK_CHANNEL_SESSION : DEVICE_LINK_CHANNEL_CONTROL;
        context.security_authenticated =
            s_service.current_facts.session_authenticated;
        context.authorized = s_service.current_facts.authorized;
        if (ble_link_session_get_facts(
                    context.connection_generation, &live_facts) == ESP_OK)
        {
            context.security_authenticated = live_facts.session_authenticated;
            context.authorized = live_facts.authorized;
        }
        device_link_security_auth_record_t auth_record;

        memset(&auth_record, 0, sizeof(auth_record));
        if (context.authorized &&
                device_link_security_load_auth_record(&auth_record) == ESP_OK &&
                device_link_security_auth_record_valid(&auth_record) &&
                auth_record.peer_addr_type ==
                s_service.current_facts.peer_addr_type &&
                memcmp(auth_record.peer_addr,
                       s_service.current_facts.peer_addr,
                       DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES) == 0)
        {
            permission_count = auth_record.granted_permission_count;
            memcpy(permissions, auth_record.granted_permissions,
                   permission_count * sizeof(permissions[0]));
        }
        _ble_link_service_zeroize(&auth_record, sizeof(auth_record));
        if (context.authorized && permission_count == 0U)
        {
            context.authorized = false;
        }
        if (context.authorized)
        {
            context.admission = DEVICE_LINK_ADMISSION_AUTHORIZED;
        }
        else
        {
            /* A public-password session is a candidate: read-only once the
             * device is bound, bindable otherwise. */
            device_link_security_verifier_kind_t verifier_kind =
                DEVICE_LINK_SECURITY_VERIFIER_NONE;

            if (s_service.security != NULL &&
                    s_service.security->selected_verifier != NULL)
            {
                verifier_kind = s_service.security->selected_verifier();
            }
            if (verifier_kind == DEVICE_LINK_SECURITY_VERIFIER_PUBLIC)
            {
                context.admission =
                    (ble_link_session_get_state_flags() &
                     BLE_LINK_STATE_FLAG_BOUND) != 0U ?
                    DEVICE_LINK_ADMISSION_BOUND_PUBLIC_READ_ONLY :
                    DEVICE_LINK_ADMISSION_UNBOUND_PUBLIC;
            }
            else
            {
                context.admission =
                    DEVICE_LINK_ADMISSION_VERIFIED_UNAUTHORIZED;
            }
        }
        context.permissions = context.authorized ? permissions : NULL;
        context.permission_count = context.authorized ? permission_count : 0U;
        _ble_link_service_lock();
        if (s_service.v2_response_in_use)
        {
            _ble_link_service_unlock();
            return ESP_ERR_NO_MEM;
        }
        s_service.v2_response_in_use = true;
        uint8_t *const typed_response = s_service.v2_response;
        _ble_link_service_unlock();
        size_t typed_response_len = 0U;
        const esp_err_t result = device_link_router_process(
                                     &s_service.v2_router, &context,
                                     msg, len, typed_response,
                                     BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES,
                                     &typed_response_len);

        if (result != ESP_OK)
        {
            ble_link_service_release_plaintext(
                typed_response, BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES);
            return result;
        }
        *response = typed_response;
        *response_len = typed_response_len;
        return ESP_OK;
    }
#ifndef UNIT_TEST_HOST
    /* Device Link v1 and all legacy Envelope variants are retired. The
     * v2 fixed header is the only application wire accepted in production;
     * keep the old business adapters below solely for host regression tests
     * until their test fixtures are migrated. */
    return ESP_ERR_INVALID_RESPONSE;
#else
    ble_link_codec_envelope_t envelope;
    ble_link_codec_request_t request;

    if (ble_link_codec_decode_envelope(msg, len, &envelope) != ESP_OK)
    {
        _ble_link_service_abort_session(
            s_service.current_facts.connection_generation);
        return ESP_ERR_INVALID_STATE;
    }
    /* A boot id mismatch is terminal regardless of the transaction gate
     * or the envelope body: the session is closed without a response. */
    if (envelope.boot_id != s_service.current_facts.active_boot_id)
    {
        _ble_link_service_abort_session(
            s_service.current_facts.connection_generation);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t request_decode_result =
        ble_link_codec_decode_request(envelope.body_data, envelope.body_len,
                                      &request);
    if (envelope.body != BLE_LINK_CODEC_BODY_REQUEST ||
            request_decode_result != ESP_OK)
    {
        _ble_link_service_abort_session(
            s_service.current_facts.connection_generation);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.pending_transactions > 0U)
    {
        /* A second response cannot be sent while the first indication
         * stream owns the service slot. Keep one immutable request identity
         * and emit its BUSY response after the active stream confirms; never
         * replace the active payload with a competing response. Additional
         * requests are deliberately absorbed until the client retries. */
        if (!s_service.deferred_busy.active)
        {
            s_service.deferred_busy.active = true;
            s_service.deferred_busy.request_id = request.request_id;
            s_service.deferred_busy.generation =
                s_service.current_facts.connection_generation;
            s_service.deferred_busy.att_mtu =
                s_service.current_facts.preferred_att_mtu;
            s_service.deferred_busy.channel =
                _ble_link_service_response_channel();
        }
        return ESP_OK;
    }
    /* The bootstrap authorize/recovery flow runs on the session channel
     * before authorization; manifest and snapshot are admitted on the
     * session channel after authentication too (and on the control
     * channel when authorized). Every other request is a control request
     * and requires authorization. A request on the wrong channel is
     * rejected. */
    const bool session_admitted =
        (request.body == BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE ||
         request.body == BLE_LINK_CODEC_REQUEST_AUTHORIZE_COMMIT ||
         request.body == BLE_LINK_CODEC_REQUEST_GET_AUTHORIZATION ||
         request.body == BLE_LINK_CODEC_REQUEST_GET_MANIFEST ||
         request.body == BLE_LINK_CODEC_REQUEST_GET_LINK_SNAPSHOT);
    const ble_link_service_rx_channel_t channel = s_service.current_channel;
    ble_link_session_channel_t admission_channel;

    if (session_admitted && channel == BLE_LINK_SERVICE_RX_SESSION)
    {
        admission_channel = BLE_LINK_SESSION_CHANNEL_SESSION;
    }
    else if (session_admitted && channel == BLE_LINK_SERVICE_RX_CONTROL)
    {
        /* Manifest and snapshot may also be read on the control
         * channel once authorized; prepare/commit/get_authorization on
         * the control channel are rejected below. */
        if (request.body == BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE ||
                request.body == BLE_LINK_CODEC_REQUEST_AUTHORIZE_COMMIT ||
                request.body == BLE_LINK_CODEC_REQUEST_GET_AUTHORIZATION)
        {
            _ble_link_service_abort_session(
                s_service.current_facts.connection_generation);
            return ESP_ERR_INVALID_STATE;
        }
        admission_channel = BLE_LINK_SESSION_CHANNEL_CONTROL;
    }
    else if (channel == BLE_LINK_SERVICE_RX_CONTROL)
    {
        admission_channel = BLE_LINK_SESSION_CHANNEL_CONTROL;
    }
    else
    {
        _ble_link_service_abort_session(
            s_service.current_facts.connection_generation);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t admission_error = 0U;

    const esp_err_t admission_result = ble_link_session_query_admission(
                                           s_service.current_facts.connection_generation,
                                           admission_channel, &admission_error);
    if (admission_result != ESP_OK || admission_error != BLE_LINK_ERROR_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ble_link_dispatcher_facts_t dispatcher_facts;

    memset(&dispatcher_facts, 0, sizeof(dispatcher_facts));
    dispatcher_facts.active_boot_id =
        s_service.current_facts.active_boot_id;
    dispatcher_facts.connection_generation =
        s_service.current_facts.connection_generation;
    dispatcher_facts.encrypted = s_service.current_facts.encrypted;
    dispatcher_facts.session_authenticated =
        s_service.current_facts.session_authenticated;
    dispatcher_facts.authorized = s_service.current_facts.authorized;
    {
        /* Refresh the live session facts: on_authenticated() may have just
         * opened the Security 2 session and restored the authorized state,
         * so the stale current_facts snapshot must not reject the first
         * protected request of a session (e.g. the Recovery Query sent
         * directly after the long-term handshake). */
        ble_link_dispatcher_facts_t live_facts;

        if (ble_link_session_get_facts(
                    s_service.current_facts.connection_generation,
                    &live_facts) == ESP_OK)
        {
            dispatcher_facts.encrypted = live_facts.encrypted;
            dispatcher_facts.session_authenticated =
                live_facts.session_authenticated;
            dispatcher_facts.authorized = live_facts.authorized;
        }
    }
    uint32_t dispatch_error = 0U;

    if (ble_link_dispatcher_handle_request(
                &envelope, &request, &dispatcher_facts, &dispatch_error) != ESP_OK)
    {
        if (dispatch_error != 0U)
        {
            _ble_link_service_emit_response(
                request.request_id, dispatch_error,
                BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,

                s_service.current_facts.preferred_att_mtu,
                _ble_link_service_response_channel());
            return _ble_link_service_take_response(response, response_len);
        }
        return ESP_ERR_NO_MEM;
    }
    if (dispatch_error != BLE_LINK_ERROR_OK)
    {
        /* Encode the stable LinkError as a body-less response. A boot id
         * mismatch is terminal per the lifecycle contract: the session is
         * closed, not merely answered. */
        if (dispatch_error == BLE_LINK_ERROR_UNAVAILABLE &&
                envelope.boot_id != s_service.current_facts.active_boot_id)
        {
            _ble_link_service_abort_session(
                s_service.current_facts.connection_generation);
            return ESP_OK;
        }
        _ble_link_service_emit_response(
            request.request_id, dispatch_error,
            BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,

            s_service.current_facts.preferred_att_mtu,
            _ble_link_service_response_channel());
    }
    return _ble_link_service_take_response(response, response_len);
#endif /* UNIT_TEST_HOST (legacy Envelope dispatch) */
}

#ifdef UNIT_TEST_HOST
esp_err_t ble_link_service_publish_link_state(
    const ble_link_service_facts_t *facts,
    const ble_link_state_snapshot_t *link_state)
{
    _ble_link_service_lock();
    const esp_err_t result = _ble_link_service_publish_link_state_locked(
                                 facts, link_state);

    _ble_link_service_unlock();
    return result;
}

static esp_err_t _ble_link_service_publish_link_state_locked(
    const ble_link_service_facts_t *facts,
    const ble_link_state_snapshot_t *link_state)
{
    if (facts == NULL || link_state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_service.subscriber.active)
    {
        return ESP_OK;
    }
    /* A publish from a retired generation has no effect. */
    if (facts->connection_generation != s_service.subscriber.generation)
    {
        return ESP_OK;
    }
    /* Event publication requires current-generation authorized admission. */
    uint32_t admission_error = 0U;

    if (ble_link_session_query_admission(
                facts->connection_generation,
                BLE_LINK_SESSION_CHANNEL_EVENT, &admission_error) != ESP_OK ||
            admission_error != BLE_LINK_ERROR_OK)
    {
        s_service.subscriber.active = false;
        return ESP_OK;
    }
    /* Events are independent notifications, but this service owns one
     * fragment buffer. Do not consume a sequence or replace a response while
     * that buffer is occupied. */
    if (s_service.pending_transactions > 0U ||
            s_service.stream.payload != NULL)
    {
        return ESP_OK;
    }
    const uint64_t sequence = ble_link_events_next();

    if (sequence == 0U)
    {
        return ESP_OK;
    }
    /* Event { sequence=1; link_state_changed=10 { link_state=1 {...} } } */
    uint8_t event_body[192];
    size_t event_len = 0U;
    uint8_t changed_body[64];
    size_t changed_len = 0U;

    _ble_link_service_encode_link_state(changed_body, &changed_len,
                                        link_state);
    uint8_t changed_msg[64];
    size_t changed_msg_len = 0U;

    _ble_link_service_write_tag(changed_msg, &changed_msg_len, 1U, 2U);
    _ble_link_service_write_varint(changed_msg, &changed_msg_len,
                                   changed_len);
    memcpy(&changed_msg[changed_msg_len], changed_body, changed_len);
    changed_msg_len += changed_len;
    _ble_link_service_write_tag(event_body, &event_len, 1U, 1U);
    _ble_link_service_write_fixed64(event_body, &event_len, sequence);
    _ble_link_service_write_tag(event_body, &event_len, 10U, 2U);
    _ble_link_service_write_bytes(event_body, &event_len, changed_msg,
                                  changed_msg_len);
    uint8_t envelope_bytes[512];
    size_t envelope_len = 0U;
    ble_link_codec_envelope_t envelope;

    memset(&envelope, 0, sizeof(envelope));
    envelope.protocol_major = BLE_LINK_SERVICE_PROTOCOL_MAJOR;
    envelope.boot_id = s_service.boot_id;
    envelope.body = BLE_LINK_CODEC_BODY_EVENT;
    envelope.body_data = event_body;
    envelope.body_len = event_len;
    if (ble_link_codec_encode_envelope(&envelope, envelope_bytes,
                                       sizeof(envelope_bytes),
                                       &envelope_len) != ESP_OK)
    {
        return ESP_ERR_NO_MEM;
    }
    s_service.pending_transactions++;
    const bool emitted = _ble_link_service_emit_protected(
                             envelope_bytes, envelope_len,
                             BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED,
                             s_service.current_facts.preferred_att_mtu,
                             BLE_LINK_SERVICE_TX_CONTROL_RESPONSE);

    if (!emitted)
    {
        s_service.pending_transactions = 0U;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
#endif /* UNIT_TEST_HOST (legacy event publisher) */
