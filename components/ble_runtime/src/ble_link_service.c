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
#ifndef UNIT_TEST_HOST
    #include "esp_heap_caps.h"
#endif
#include "esp_random.h"

#include "ble_link_codec.h"
#include "ble_link_dispatcher.h"
#include "ble_link_events.h"
#include "ble_link_reassembler.h"
#include "ble_link_service.h"
#include "ble_link_session.h"
#include "ble_link_state.h"

#include "device_link_security_auth.h"

#define DBG_TAG "ble_link_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define BLE_LINK_SERVICE_PROTOCOL_MAJOR 1U
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
        uint8_t *payload;            /**< Owned PSRAM payload copy. */
        size_t payload_len;
        size_t next_offset;          /**< Next fragment start offset. */
        uint32_t att_mtu;
        ble_link_service_tx_channel_t channel;
        uint16_t frame_id;
        uint32_t flow_id;
    } stream;
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
        bool active; /**< Never set in v1: events are not advertised. */
        uint32_t generation;
    } subscriber;
    struct
    {
        ble_link_authorization_phase_t phase;
        uint64_t authorization_txn_id;
        uint64_t confirmation_token;
        uint32_t operation_token;
        uint32_t connection_generation;
        uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
        uint8_t application_password[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
        uint8_t device_auth_id[BLE_LINK_SERVICE_AUTH_ID_BYTES];
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
} ble_link_service_t;

typedef struct ble_link_ingress
{
    ble_link_reassembler_t reassembler[2];
    ble_link_reassembly_disposition_t last_disposition[2];
    uint8_t session_buffer[BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES];
    uint8_t control_buffer[BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES];
    uint32_t generation;
    uint32_t epoch;
    bool exhausted;
} ble_link_ingress_t;

struct ble_link_work
{
    ble_link_service_facts_t facts;
    ble_link_service_rx_channel_t channel;
    uint32_t epoch;
    uint8_t transport_type;
    size_t message_len;
    uint8_t message[];
};

static SemaphoreHandle_t s_service_mutex;
static StaticSemaphore_t s_service_mutex_control;
static uint64_t s_confirmation_token_sequence;
static uint32_t s_operation_token_sequence;

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
static void _ble_link_service_clear_session_state_locked(bool retire_acl);
static esp_err_t _ble_link_service_retire_logical_session(
    uint32_t generation, bool clear_response);
static esp_err_t _ble_link_service_process_handshake_locked(
    const ble_link_service_facts_t *facts,
    const uint8_t *message, size_t message_len,
    device_link_security_handshake_stage_t stage,
    bool session_already_retired);
static esp_err_t _ble_link_service_take_response(
    uint8_t **response, size_t *response_len);
static void _ble_link_service_build_response(
    uint64_t request_id, uint32_t error,
    ble_link_codec_response_tag_t body_tag,
    const uint8_t *body, size_t body_len);
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

static void *_ble_link_service_stream_alloc(size_t size)
{
#ifdef UNIT_TEST_HOST
    return malloc(size);
#else
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

static void _ble_link_service_stream_free(void)
{
    if (s_service.stream.payload != NULL)
    {
        _ble_link_service_zeroize(s_service.stream.payload,
                                  s_service.stream.payload_len);
        free(s_service.stream.payload);
    }
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
 * because there is nothing to protect. Only a generation older than both
 * recorded generations is stale and ignored.
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
           generation == ingress_generation;
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
    ble_link_dispatcher_clear_session();
    if (s_service.security != NULL)
    {
        s_service.security->close_session();
    }
}

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
    s_service.auth_txn.deadline_ms = 0U;
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
        s_ingress.exhausted = true;
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
    _ble_link_service_reset_ingress();
    s_service.subscriber.active = false;
    s_service.handshake_active = false;
    s_service.sec2_opened = false;
    s_service.lt_switch.active = false;
    s_service.close_after_encrypt.active = false;
    _ble_link_service_clear_auth_txn();
    ble_link_dispatcher_clear_session();
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
    const size_t size = sizeof(ble_link_work_t) + message_len;

#ifdef UNIT_TEST_HOST
    return malloc(size);
#else
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

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

static size_t _ble_link_service_varint_size(uint64_t value)
{
    size_t size = 1U;

    while (value >= 0x80U)
    {
        value >>= 7U;
        size++;
    }
    return size;
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

/**
 * @brief Encode a Snapshot message (event_sequence + LinkState).
 */
static size_t _ble_link_service_encode_snapshot(
    uint8_t *out, size_t capacity, uint64_t event_sequence,
    const ble_link_state_snapshot_t *link_state)
{
    uint8_t link_state_bytes[64];
    size_t link_state_len = 0U;

    _ble_link_service_encode_link_state(link_state_bytes, &link_state_len,
                                        link_state);
    const size_t size = 1U + 8U + 1U +
                        _ble_link_service_varint_size(link_state_len) +
                        link_state_len;

    if (capacity < size)
    {
        return 0U;
    }
    size_t pos = 0U;

    _ble_link_service_write_tag(out, &pos, 1U, 1U);
    _ble_link_service_write_fixed64(out, &pos, event_sequence);
    _ble_link_service_write_tag(out, &pos, 2U, 2U);
    _ble_link_service_write_bytes(out, &pos, link_state_bytes,
                                  link_state_len);
    return size;
}

static void _ble_link_service_encode_capabilities(uint8_t *out, size_t *pos)
{
    /* protocol_version {major=1} */
    static const uint8_t protocol_version[] = {0x08, 0x01};
    /* profile_version {major=1} */
    static const uint8_t profile_version[] = {0x08, 0x01};
    /* security {sc_only, key=16, max_bonds=1, protocomm 2, patch 1,
     *          local_confirmation, application_credential} */
    static const uint8_t security[] =
    {
        0x08, 0x01, 0x10, 0x10, 0x18, 0x01, 0x20, 0x02,
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
    _ble_link_service_write_bytes(out, pos, protocol_version,
                                  sizeof(protocol_version));
    _ble_link_service_write_tag(out, pos, 2U, 2U);
    _ble_link_service_write_bytes(out, pos, profile_version,
                                  sizeof(profile_version));
    _ble_link_service_write_tag(out, pos, 3U, 2U);
    _ble_link_service_write_bytes(out, pos, framing, framing_pos);
    _ble_link_service_write_tag(out, pos, 4U, 2U);
    _ble_link_service_write_bytes(out, pos, security, sizeof(security));
    /* No features are advertised until they are implemented. */
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
}

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
 * The payload is copied into an owned PSRAM buffer and only the first
 * fragment is handed to the transport; each following fragment is emitted
 * when the previous indication confirms (ble_link_service_response_
 * completed), so a response completes at any negotiated ATT MTU down to
 * 23 independent of the local TX queue depth.
 */
static bool _ble_link_service_emit_fragments(
    const uint8_t *payload, size_t payload_len, uint32_t att_mtu,
    ble_link_service_tx_channel_t channel)
{
    if (payload == NULL || payload_len == 0U ||
            _ble_link_service_max_fragment_payload(att_mtu) == 0U)
    {
        return false;
    }
    uint8_t *copy = _ble_link_service_stream_alloc(payload_len);

    if (copy == NULL)
    {
        return false;
    }
    memcpy(copy, payload, payload_len);
    if (s_service.stream.payload != NULL ||
            (channel != BLE_LINK_SERVICE_TX_CONTROL_EVENT &&
             s_service.pending_transactions == 0U))
    {
        _ble_link_service_zeroize(copy, payload_len);
        free(copy);
        return false;
    }
    const uint32_t flow_id = (channel == BLE_LINK_SERVICE_TX_CONTROL_EVENT) ?
                             0U : _ble_link_service_next_flow_id();

    if (channel != BLE_LINK_SERVICE_TX_CONTROL_EVENT && flow_id == 0U)
    {
        _ble_link_service_zeroize(copy, payload_len);
        free(copy);
        return false;
    }
    s_service.stream.payload = copy;
    s_service.stream.payload_len = payload_len;
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
}

/**
 * @brief Build and emit one response envelope.
 */
/**
 * @brief Encode a response Envelope into the service response buffer.
 *
 * Every handler builds the plaintext response here; the transport layer
 * (feed for a request, publish for an event) encrypts and emits it.
 */
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
    uint8_t framed[1U + BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES];
    size_t framed_len = 0U;

    framed[0] = transport_type;
    if (transport_type == BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED &&
            s_service.security != NULL &&
            s_service.security->protect != NULL)
    {
        uint8_t *cipher = NULL;
        size_t cipher_len = 0U;

        if (s_service.security->protect(message, message_len,
                                        &cipher, &cipher_len) != ESP_OK ||
                cipher == NULL ||
                cipher_len > sizeof(framed) - 1U)
        {
            free(cipher);
            return false;
        }
        memcpy(&framed[1], cipher, cipher_len);
        framed_len = 1U + cipher_len;
        free(cipher);
    }
    else
    {
        if (message_len > sizeof(framed) - 1U)
        {
            return false;
        }
        memcpy(&framed[1], message, message_len);
        framed_len = 1U + message_len;
    }
    return _ble_link_service_emit_fragments(framed, framed_len,
                                            att_mtu, channel);
}

static void _ble_link_service_emit_response(
    uint64_t request_id, uint32_t error, ble_link_codec_response_tag_t body_tag,
    const uint8_t *body, size_t body_len, uint32_t att_mtu,
    ble_link_service_tx_channel_t channel)
{
    (void)att_mtu;
    (void)channel;
    /* The response envelope is built here; the transport (feed, inside the
     * adapter's unprotect, or the plaintext harness) encrypts and emits it
     * after the request callback returns. This keeps every Security 2
     * operation on the adapter lock without re-entry. */
    _ble_link_service_build_response(request_id, error, body_tag,
                                     body, body_len);
}

/**
 * @brief Response channel for the current RX channel.
 */
static ble_link_service_tx_channel_t _ble_link_service_response_channel(void)
{
    return (s_service.current_channel == BLE_LINK_SERVICE_RX_SESSION) ?
           BLE_LINK_SERVICE_TX_SESSION : BLE_LINK_SERVICE_TX_CONTROL_RESPONSE;
}

static uint32_t _ble_link_service_handle_capabilities(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)facts;
    (void)arg;
    uint8_t body[128];
    size_t body_len = 0U;

    _ble_link_service_encode_capabilities(body, &body_len);
    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_CAPABILITIES, body, body_len,

        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}

static uint32_t _ble_link_service_handle_snapshot(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)facts;
    (void)arg;
    ble_link_state_snapshot_t link_state;
    uint8_t body[64];

    _ble_link_service_build_link_state(&s_service.current_facts,
                                       &link_state);
    const size_t body_len = _ble_link_service_encode_snapshot(
                                body, sizeof(body), ble_link_events_baseline(),
                                &link_state);

    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_SNAPSHOT, body, body_len,

        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}

static uint32_t _ble_link_service_handle_authorize_prepare(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)facts;
    (void)arg;
    _ble_link_service_lock();
    bool txn_active =
        s_service.auth_txn.phase != BLE_LINK_AUTH_PHASE_IDLE;
    const bool txn_expired = _ble_link_service_auth_txn_expired();

    if (txn_expired)
    {
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
            esp_fill_random(s_service.auth_txn.application_password,
                            sizeof(s_service.auth_txn.application_password));
            s_service.auth_txn.connection_generation =
                s_service.current_facts.connection_generation;
            s_service.auth_txn.deadline_ms =
                (uint32_t)xTaskGetTickCount() +
                (uint32_t)pdMS_TO_TICKS(BLE_LINK_SERVICE_AUTH_EXPIRES_MS);
        }
    }
    _ble_link_service_unlock();
    uint8_t body[64];
    size_t body_len = 0U;

    if (operation_token_unavailable)
    {
        _ble_link_service_emit_response(
            request->request_id, BLE_LINK_ERROR_UNAVAILABLE,
            BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,
            s_service.current_facts.preferred_att_mtu,
            _ble_link_service_response_channel());
        return BLE_LINK_ERROR_OK;
    }

    _ble_link_service_encode_authorize_prepare(body, &body_len);
    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_AUTHORIZE_PREPARE, body, body_len,

        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
}

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
        const uint64_t field = tag >> 3U;
        const uint64_t wire = tag & 7U;

        if (field == 1U && wire == 1U) /* fixed64 */
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
        else if (field == 2U && wire == 2U) /* length-delimited */
        {
            if (pos >= body_len || body[pos] >= 0x80U)
            {
                return false;
            }
            const size_t len = body[pos];

            pos++;
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

static uint32_t _ble_link_service_handle_authorize_commit(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)arg;
    uint64_t txn_id = 0U;
    uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
    size_t credential_len = 0U;
    bool ok = false;
    bool replay = false;
    bool confirmed = false;
    bool probe_unavailable = false;
    bool txn_active = false;
    const bool parsed = _ble_link_service_parse_authorize_commit(
                            request->body_data, request->body_len, &txn_id,
                            credential_id, &credential_len);

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
    if (txn_active && parsed)
    {
        ok = (txn_id == s_service.auth_txn.authorization_txn_id &&
              facts->connection_generation ==
              s_service.auth_txn.connection_generation &&
              credential_len == BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES &&
              memcmp(credential_id, s_service.auth_txn.credential_id,
                     credential_len) == 0);
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
    if (!replay && parsed && s_service.committed_replay.active &&
            credential_len == BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES &&
            s_service.committed_replay.authorization_txn_id == txn_id &&
            s_service.committed_replay.connection_generation ==
            facts->connection_generation &&
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
    uint8_t replay_credential[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];
    uint8_t replay_auth_id[BLE_LINK_SERVICE_AUTH_ID_BYTES];

    if (replay)
    {
        /* Idempotent replay of a committed transaction: the credential
         * and auth id are snapshotted while the lock is still held, so a
         * concurrent clear cannot tear them between the eligibility
         * check and the copy. */
        memcpy(replay_credential, s_service.committed_replay.credential_id,
               sizeof(replay_credential));
        memcpy(replay_auth_id, s_service.committed_replay.device_auth_id,
               sizeof(replay_auth_id));
    }
    _ble_link_service_unlock();
    if (replay)
    {
        /* Idempotent replay of a committed transaction. */
        uint8_t replay_body[64];
        size_t replay_body_len = 0U;

        _ble_link_service_encode_authorization_result(
            replay_body, &replay_body_len,
            replay_credential, replay_auth_id);
        _ble_link_service_zeroize(replay_credential,
                                  sizeof(replay_credential));
        _ble_link_service_zeroize(replay_auth_id, sizeof(replay_auth_id));
        _ble_link_service_emit_response(
            request->request_id, BLE_LINK_ERROR_OK,
            BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT, replay_body,
            replay_body_len,

            s_service.current_facts.preferred_att_mtu,
            _ble_link_service_response_channel());
        return BLE_LINK_ERROR_OK;
    }
    if (probe_unavailable)
    {
        _ble_link_service_lock();
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            facts->connection_generation;
        _ble_link_service_clear_auth_txn();
        _ble_link_service_unlock();
        _ble_link_service_discard_provisional_bond(
            facts->connection_generation, true);
        _ble_link_service_emit_response(
            request->request_id, BLE_LINK_ERROR_INTERNAL,
            BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,
            s_service.current_facts.preferred_att_mtu,
            _ble_link_service_response_channel());
        return BLE_LINK_ERROR_OK;
    }
    if (!ok || !txn_active)
    {
        /* Terminal pre-durable error: close the Security 2 session after
         * the stable encrypted error response is emitted. */
        _ble_link_service_lock();
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            s_service.current_facts.connection_generation;
        _ble_link_service_unlock();
        _ble_link_service_discard_provisional_bond(
            s_service.current_facts.connection_generation, true);
        _ble_link_service_clear_auth_txn();
        _ble_link_service_emit_response(
            request->request_id, BLE_LINK_ERROR_INVALID_ARGUMENT,
            BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,

            s_service.current_facts.preferred_att_mtu,
            _ble_link_service_response_channel());
        return BLE_LINK_ERROR_OK;
    }
    if (!ok)
    {
        _ble_link_service_clear_auth_txn();
        _ble_link_service_emit_response(
            request->request_id, BLE_LINK_ERROR_INVALID_ARGUMENT,
            BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,

            s_service.current_facts.preferred_att_mtu,
            _ble_link_service_response_channel());
        return BLE_LINK_ERROR_OK;
    }
    if (!confirmed)
    {
        /* The user has not confirmed this binding on the device. */
        _ble_link_service_emit_response(
            request->request_id, BLE_LINK_ERROR_CONFIRMATION_REQUIRED,
            BLE_LINK_CODEC_RESPONSE_NONE, NULL, 0U,

            s_service.current_facts.preferred_att_mtu,
            _ble_link_service_response_channel());
        return BLE_LINK_ERROR_OK;
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
    uint32_t commit_error = BLE_LINK_ERROR_OK;

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
            facts->connection_generation;
        commit_error = BLE_LINK_ERROR_UNAVAILABLE;
        _ble_link_service_unlock();
        goto commit_exit;
    }
    esp_fill_random(local_device_auth_id, sizeof(local_device_auth_id));
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
            facts->connection_generation;
        commit_error = BLE_LINK_ERROR_UNAVAILABLE;
        _ble_link_service_unlock();
        goto commit_exit;
    }
    memcpy(record.credential_id, local_credential,
           DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES);
    memcpy(record.device_auth_id, local_device_auth_id,
           DEVICE_LINK_SECURITY_AUTH_ID_BYTES);
    /* The mutex stays held across derivation, persistence, and the state
     * publication: a window close or disconnect cannot clear the
     * transaction underneath a durable commit. */
    /* The committed record must carry the normalized SMP identity of the
     * authenticated connection; an unknown or invalid identity is
     * refused. */
    bool peer_valid = s_service.current_facts.peer_addr_type <= 3U;

    if (peer_valid)
    {
        peer_valid = false;
        for (size_t i = 0U; i < 6U; ++i)
        {
            peer_valid = peer_valid ||
                         s_service.current_facts.peer_addr[i] != 0U;
        }
    }
    if (!s_service.current_facts.identity_known || !peer_valid)
    {
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            facts->connection_generation;
        commit_error = BLE_LINK_ERROR_INVALID_ARGUMENT;
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
            facts->connection_generation;
        commit_error = BLE_LINK_ERROR_INTERNAL;
        _ble_link_service_unlock();
        goto commit_exit;
    }
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    if (device_link_security_save_auth_record(&record) != ESP_OK)
    {
        s_service.close_after_encrypt.active = true;
        s_service.close_after_encrypt.generation =
            facts->connection_generation;
        commit_error = BLE_LINK_ERROR_STORAGE;
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
        facts->connection_generation);
    s_service.auth_txn.phase = BLE_LINK_AUTH_PHASE_COMMITTED;
    memcpy(s_service.auth_txn.device_auth_id, local_device_auth_id,
           sizeof(s_service.auth_txn.device_auth_id));
    _ble_link_service_clear_committed_replay();
    s_service.committed_replay.active = true;
    s_service.committed_replay.authorization_txn_id =
        s_service.auth_txn.authorization_txn_id;
    s_service.committed_replay.connection_generation =
        facts->connection_generation;
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
    /* The bootstrap response is still encrypted under the bootstrap
     * session; the long-term verifier switch is deferred until the
     * protected response has been handed to the transport (consumed in
     * the feed), generation-scoped so a retired flow can never trigger
     * it. Both the flag and the external authorization are published
     * while the transaction mutex is held. */
    s_service.lt_switch.active = true;
    s_service.lt_switch.generation = facts->connection_generation;
    if (ble_link_session_set_authorization(true, 0U) != ESP_OK ||
            ble_link_session_report_session_match_current(
                facts->connection_generation, 0U) != ESP_OK)
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
            facts->connection_generation);
        s_service.sec2_opened = false;
    }
    _ble_link_service_unlock();
    uint8_t body_bytes[64];
    size_t body_len_bytes = 0U;

    _ble_link_service_encode_authorization_result(body_bytes,
            &body_len_bytes, local_credential, local_device_auth_id);
    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT, body_bytes,
        body_len_bytes,
        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());

commit_exit:
    _ble_link_service_zeroize(&record, sizeof(record));
    _ble_link_service_zeroize(local_password, sizeof(local_password));
    _ble_link_service_zeroize(local_credential, sizeof(local_credential));
    _ble_link_service_zeroize(local_device_auth_id,
                              sizeof(local_device_auth_id));
    if (commit_error != BLE_LINK_ERROR_OK)
    {
        /* Pre-durable failure: the record was never persisted, so the
         * provisional bond is discarded and the transaction cleared.
         * The Security 2 session stays open until the stable error
         * response is encrypted (close_after_encrypt). */
        _ble_link_service_discard_provisional_bond(
            facts->connection_generation, true);
        _ble_link_service_lock();
        if (s_service.auth_txn.phase == BLE_LINK_AUTH_PHASE_COMMITTING)
        {
            _ble_link_service_clear_auth_txn();
        }
        _ble_link_service_unlock();
        return commit_error;
    }
    return BLE_LINK_ERROR_OK;
}

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
        const uint64_t field = tag >> 3U;
        const uint64_t wire = tag & 7U;

        if (field == 1U && wire == 2U) /* length-delimited credential_id */
        {
            if (pos >= body_len || body[pos] >= 0x80U)
            {
                return false;
            }
            const size_t len = body[pos];

            pos++;
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

static uint32_t _ble_link_service_handle_get_authorization(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg)
{
    (void)arg;
    /* The recovery query is only meaningful under the RECOVERY_QUERY
     * envelope flag; the dispatcher surfaced it in the facts. */
    if (!facts->recovery_query || !facts->authorized ||
            !facts->session_authenticated)
    {
        return BLE_LINK_ERROR_INVALID_ARGUMENT;
    }
    uint8_t credential_id[BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES];

    memset(credential_id, 0, sizeof(credential_id));
    if (!_ble_link_service_parse_get_authorization(
                request->body_data, request->body_len, credential_id))
    {
        return BLE_LINK_ERROR_INVALID_ARGUMENT;
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
        return BLE_LINK_ERROR_NOT_FOUND;
    }
    if (load_result == ESP_ERR_INVALID_STATE ||
            (load_result == ESP_OK &&
             !device_link_security_auth_record_valid(&record)))
    {
        _ble_link_service_zeroize(&record, sizeof(record));
        return BLE_LINK_ERROR_INTERNAL;
    }
    if (load_result != ESP_OK)
    {
        _ble_link_service_zeroize(&record, sizeof(record));
        return BLE_LINK_ERROR_STORAGE;
    }
    if (memcmp(record.credential_id, credential_id,
               BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES) != 0 ||
            record.peer_addr_type != s_service.current_facts.peer_addr_type ||
            memcmp(record.peer_addr, s_service.current_facts.peer_addr, 6U) != 0)
    {
        _ble_link_service_zeroize(&record, sizeof(record));
        return BLE_LINK_ERROR_NOT_FOUND;
    }
    uint8_t body[64];
    size_t body_len = 0U;

    _ble_link_service_encode_authorization_result(
        body, &body_len, record.credential_id, record.device_auth_id);
    _ble_link_service_zeroize(&record, sizeof(record));
    _ble_link_service_emit_response(
        request->request_id, BLE_LINK_ERROR_OK,
        BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT, body, body_len,

        s_service.current_facts.preferred_att_mtu,
        _ble_link_service_response_channel());
    return BLE_LINK_ERROR_OK;
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
    if (kind == DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP)
    {
        /* Bootstrap session: mark only the Security 2 authentication;
         * authorization is established by the commit. */
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
            s_service.current_facts.connection_generation)
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
    ble_link_dispatcher_register_request(
        BLE_LINK_CODEC_REQUEST_GET_CAPABILITIES,
        _ble_link_service_handle_capabilities, NULL);
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
    /* SubscribeEvents is deliberately not registered: v1 does not
     * advertise encrypted events, so the dispatcher answers with
     * LINK_ERROR_UNSUPPORTED_OPERATION. */
}

void ble_link_service_reset(void)
{
    ble_link_dispatcher_reset();
    _ble_link_service_clear_delayed_cmd0();
    _ble_link_service_stream_free();
    memset(&s_service, 0, sizeof(s_service));
    memset(&s_ingress, 0, sizeof(s_ingress));
}

static void _ble_link_service_clear_session_state_locked(bool retire_acl)
{
    const uint32_t generation =
        s_service.current_facts.connection_generation;

    _ble_link_service_discard_provisional_bond(
        generation, true);
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
    ble_link_dispatcher_clear_session();
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
    bool current =
        s_service.current_facts.connection_generation ==
        identity->generation &&
        s_service.current_facts.conn_handle == identity->conn_handle;

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
            (channel != BLE_LINK_SERVICE_RX_SESSION &&
             channel != BLE_LINK_SERVICE_RX_CONTROL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ingress.exhausted)
    {
        return ESP_ERR_INVALID_STATE;
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
    /* Transport type routing (device-link-session-transport-v1): the
     * reassembled message begins with a type byte. 0x00 is the Security 2
     * handshake wire and is accepted only on session_rx; 0x01 is the
     * AES-GCM ciphertext of an Envelope and is accepted on either channel
     * while a Security 2 session is wired. Without a session (host
     * harness) the plaintext Envelope is processed directly. */
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
        _ble_link_service_zeroize(work, sizeof(*work) + work->message_len);
        free(work);
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

    if (result != ESP_OK || out == NULL || handshake_result.stage != stage)
    {
        free(out);
        _ble_link_service_abort_session(facts->connection_generation);
        return result != ESP_OK ? result : ESP_ERR_INVALID_RESPONSE;
    }
    if (handshake_result.authenticated)
    {
        s_service.handshake_active = false;
    }
    uint8_t framed[1U + BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES];

    framed[0] = BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE;
    const size_t framed_len = (out_len <= sizeof(framed) - 1U) ?
                              1U + out_len : 0U;

    if (framed_len != 0U)
    {
        memcpy(&framed[1], out, out_len);
    }
    free(out);
    if (framed_len == 0U)
    {
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_NO_MEM;
    }
    s_service.pending_transactions++;
    if (!_ble_link_service_emit_fragments(
                framed, framed_len, facts->preferred_att_mtu,
                BLE_LINK_SERVICE_TX_SESSION))
    {
        s_service.pending_transactions = 0U;
        _ble_link_service_abort_session(facts->connection_generation);
        return ESP_ERR_NO_MEM;
    }
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
        s_service.lt_switch.active = false;
        s_service.close_after_encrypt.active = false;
        _ble_link_service_stream_free();
        ble_link_dispatcher_clear_session();
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
            free(out);
            _ble_link_service_abort_session(facts->connection_generation);
            return unprotect_result;
        }
        if (out != NULL)
        {
            uint8_t framed[1U + BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES];

            if (out_len > sizeof(framed) - 1U)
            {
                free(out);
                _ble_link_service_abort_session(
                    facts->connection_generation);
                return ESP_ERR_NO_MEM;
            }
            framed[0] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
            memcpy(&framed[1], out, out_len);
            free(out);
            s_service.pending_transactions++;
            if (!_ble_link_service_emit_fragments(
                        framed, 1U + out_len, facts->preferred_att_mtu,
                        _ble_link_service_response_channel()))
            {
                /* The response could not be handed to the transport: return
                 * the failure so the adapter closes the request session. */
                emitted = false;
                s_service.pending_transactions = 0U;
                _ble_link_service_abort_session(
                    facts->connection_generation);
                return ESP_ERR_NO_MEM;
            }
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
            free(plain_response);
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
                free(plain_response);
                return ESP_ERR_NO_MEM;
            }
            free(plain_response);
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
    if (envelope.body != BLE_LINK_CODEC_BODY_REQUEST ||
            ble_link_codec_decode_request(
                envelope.body_data, envelope.body_len, &request) != ESP_OK)
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
     * before authorization; capabilities and snapshot are admitted on the
     * session channel after authentication too (and on the control
     * channel when authorized). Every other request is a control request
     * and requires authorization. A request on the wrong channel is
     * rejected. */
    const bool session_admitted =
        (request.body == BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE ||
         request.body == BLE_LINK_CODEC_REQUEST_AUTHORIZE_COMMIT ||
         request.body == BLE_LINK_CODEC_REQUEST_GET_AUTHORIZATION ||
         request.body == BLE_LINK_CODEC_REQUEST_GET_CAPABILITIES ||
         request.body == BLE_LINK_CODEC_REQUEST_GET_LINK_SNAPSHOT);
    const ble_link_service_rx_channel_t channel = s_service.current_channel;
    ble_link_session_channel_t admission_channel;

    if (session_admitted && channel == BLE_LINK_SERVICE_RX_SESSION)
    {
        admission_channel = BLE_LINK_SESSION_CHANNEL_SESSION;
    }
    else if (session_admitted && channel == BLE_LINK_SERVICE_RX_CONTROL)
    {
        /* Capabilities and snapshot may also be read on the control
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

    if (ble_link_session_query_admission(
                s_service.current_facts.connection_generation,
                admission_channel, &admission_error) != ESP_OK ||
            admission_error != BLE_LINK_ERROR_OK)
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
}

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
    /* v1 events are independent notifications, but this service owns one
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
    if (!_ble_link_service_emit_protected(
                envelope_bytes, envelope_len,
                BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED,
                s_service.current_facts.preferred_att_mtu,
                BLE_LINK_SERVICE_TX_CONTROL_EVENT))
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
