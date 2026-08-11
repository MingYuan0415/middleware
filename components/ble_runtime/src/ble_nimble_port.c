#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_adv_manager.h"
#include "ble_nimble_adv_start.h"
#include "ble_nimble_pairing_gate.h"
#include "ble_nimble_store_guard.h"
#include "ble_gap_manager.h"
#include "ble_gatt_registry.h"
#include "ble_nimble_port.h"
#include "ble_port_ops.h"

#include "device_link_security.h"
#include "device_link_security_auth.h"

#include "ble_link_security_ops.h"
#include "ble_link_service.h"
#include "ble_link_sec_state.h"
#include "ble_link_gatt.h"
#include "ble_link_cleanup_obligation.h"
#include "ble_nimble_tx_tracker.h"
#include "ble_link_session.h"
#include "ble_link_timer_deadline.h"
#include "ble_response_cache.h"
#include "ble_runtime.h"
#include "ble_tx_scheduler.h"

static const char *const TAG = "ble_nimble_port";

static esp_err_t _ble_nimble_port_queue_provisional_unpair(
    const ble_link_operation_identity_t *identity, bool terminate_conn);
static esp_err_t _ble_nimble_port_promote_provisional_bond(
    const ble_link_operation_identity_t *identity);
static esp_err_t _ble_nimble_port_retain_remote_replacement(
    const ble_link_operation_identity_t *identity);
static void _ble_nimble_port_clear_provisional_tracking(void);

/**
 * @brief Security 2 ops bound to the device_link_security adapter.
 *
 * The adapter owns the session; the port wires it to the GATT transport
 * (type 0x00 handshake, 0x01 protected). The bootstrap/long-term verifier
 * lifecycle is driven by the link service window ops and the per-peer
 * selection made by the link service before each handshake.
 */
static esp_err_t _ble_nimble_port_sec_select_verifier(
    uint8_t peer_addr_type, const uint8_t *peer_addr, size_t peer_addr_len,
    bool pairing_window_open)
{
    return device_link_security_select_verifier(
               peer_addr_type, peer_addr, peer_addr_len, pairing_window_open);
}

static device_link_security_verifier_kind_t
_ble_nimble_port_sec_selected_verifier(void)
{
    return device_link_security_selected_verifier();
}

static esp_err_t _ble_nimble_port_sec_handshake(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len,
    device_link_security_handshake_result_t *handshake_result)
{
    return device_link_security_handshake_ex(
               input, input_len, output, output_len, handshake_result);
}

static esp_err_t _ble_nimble_port_sec_classify_handshake(
    const uint8_t *input, size_t input_len,
    device_link_security_handshake_stage_t *stage)
{
    return device_link_security_classify_handshake(
               input, input_len, stage);
}

static esp_err_t _ble_nimble_port_sec_unprotect(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len)
{
    return device_link_security_unprotect(input, input_len,
                                          output, output_len);
}

static esp_err_t _ble_nimble_port_sec_protect(
    const uint8_t *plain, size_t plain_len,
    uint8_t **cipher, size_t *cipher_len)
{
    return device_link_security_protect(plain, plain_len,
                                        cipher, cipher_len);
}

static bool _ble_nimble_port_sec_is_authenticated(void)
{
    return device_link_security_is_authenticated();
}

static bool _ble_nimble_port_sec_session_open(void)
{
    return device_link_security_session_open();
}

static void _ble_nimble_port_sec_close_session(void)
{
    device_link_security_close_session();
}

static esp_err_t _ble_nimble_port_sec_discard_provisional_bond(
    const ble_link_operation_identity_t *identity, bool terminate_conn)
{
    return _ble_nimble_port_queue_provisional_unpair(
               identity, terminate_conn);
}

static esp_err_t _ble_nimble_port_sec_promote_provisional_bond(
    const ble_link_operation_identity_t *identity)
{
    return _ble_nimble_port_promote_provisional_bond(identity);
}

static esp_err_t _ble_nimble_port_security_request(
    const uint8_t *request, size_t request_len,
    uint8_t **response, size_t *response_len, void *arg)
{
    (void)arg;
    return ble_link_service_process_plaintext(request, request_len,
            response, response_len);
}

static esp_err_t _ble_nimble_port_sec_authenticated(void *arg)
{
    (void)arg;
    const esp_err_t result = ble_link_service_on_authenticated(NULL);

    if (result == ESP_OK)
    {
        ble_link_gatt_authentication_epoch_advance();
    }
    return result;
}

static const ble_link_security_ops_t s_security_ops =
{
    .select_verifier = _ble_nimble_port_sec_select_verifier,
    .selected_verifier = _ble_nimble_port_sec_selected_verifier,
    .classify_handshake = _ble_nimble_port_sec_classify_handshake,
    .handshake = _ble_nimble_port_sec_handshake,
    .unprotect = _ble_nimble_port_sec_unprotect,
    .protect = _ble_nimble_port_sec_protect,
    .is_authenticated = _ble_nimble_port_sec_is_authenticated,
    .session_open = _ble_nimble_port_sec_session_open,
    .close_session = _ble_nimble_port_sec_close_session,
    .discard_provisional_bond =
    _ble_nimble_port_sec_discard_provisional_bond,
    .promote_provisional_bond = _ble_nimble_port_sec_promote_provisional_bond,
    .replace_authorization = _ble_nimble_port_retain_remote_replacement,
};
#define DBG_TAG "ble_nimble_port"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define BLE_NIMBLE_PORT_REASSEMBLY_IDLE_MS 5000U
#define BLE_NIMBLE_PORT_INDICATION_TIMEOUT_MS 2000U
#define BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS 10000U
#define BLE_NIMBLE_PORT_ADV_QUIT_TIMEOUT_MS 2000U
#define BLE_NIMBLE_PORT_ACCESS_BUFFER_BYTES 512U
#define BLE_NIMBLE_PORT_ADV_QUEUE_DEPTH 4U
#define BLE_NIMBLE_PORT_ADV_TASK_STACK 2048U
#define BLE_NIMBLE_PORT_ADV_TASK_PRIORITY 2U
#define BLE_NIMBLE_PORT_LINK_TIMER_STACK 1024U
#define BLE_NIMBLE_PORT_LINK_TIMER_PRIORITY 1U
#define BLE_NIMBLE_PORT_SECURITY_COMMAND_DELAY_MS 50U
#define BLE_NIMBLE_PORT_OWNER_QUIT_SEND_MS 100U
#define BLE_NIMBLE_PORT_REVOKE_RETRIES 3U
#define BLE_NIMBLE_PORT_REVOKE_WAIT_RETRIES 200U
#define BLE_NIMBLE_PORT_REVOKE_WAIT_MS 10U
#define BLE_NIMBLE_PORT_TERMINATE_RETRY_MS 100U
#define BLE_NIMBLE_PORT_TX_TRACKER_CAPACITY \
    (CONFIG_BLE_RUNTIME_TX_QUEUE_DEPTH + 1U)

/**
 * @brief Tear down the link-session security for one failure path.
 *
 * A terminal transmission or session failure must close the Security 2
 * session in every layer: the link-session reducer (generation-scoped),
 * the service transaction gate, and the external device_link_security
 * adapter. Calling only some of these leaves a half-open session that a
 * later request could mistake for authenticated.
 *
 * @param[in] identity Exact connection and Security 2 operation identity.
 */
static void _ble_nimble_port_link_abort(
    const ble_link_operation_identity_t *identity)
{
    if (identity == NULL || identity->generation == 0U)
    {
        return;
    }
    (void)ble_link_service_clear_session_state_if_current(identity);
}

static void _ble_nimble_port_zeroize(void *data, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    for (size_t i = 0U; i < size; ++i)
    {
        bytes[i] = 0U;
    }
}

#if CONFIG_BT_NIMBLE_PINNED_TO_CORE == 0
    #define BLE_NIMBLE_PORT_HOST_CORE 0
#elif CONFIG_BT_NIMBLE_PINNED_TO_CORE == 1
    #define BLE_NIMBLE_PORT_HOST_CORE 1
#else
    #define BLE_NIMBLE_PORT_HOST_CORE tskNO_AFFINITY
#endif

typedef enum
{
    BLE_NIMBLE_PORT_ADV_CMD_START = 0,
    BLE_NIMBLE_PORT_ADV_CMD_STOP,
    BLE_NIMBLE_PORT_ADV_CMD_QUIT,
} ble_nimble_port_adv_cmd_type_t;

typedef struct ble_nimble_port_adv_cmd
{
    ble_nimble_port_adv_cmd_type_t type;
    ble_port_adv_config_t config;
    uint8_t service_data[31U];
} ble_nimble_port_adv_cmd_t;
typedef struct ble_nimble_port
{
    SemaphoreHandle_t sync_semaphore;
    SemaphoreHandle_t exit_semaphore;
    SemaphoreHandle_t stop_done_semaphore;
    SemaphoreHandle_t adv_exit_semaphore;
    SemaphoreHandle_t adv_lock;
    TaskHandle_t host_task;
    TaskHandle_t adv_task;
    QueueHandle_t adv_queue;
    struct ble_gap_event_listener listener;
    bool listener_registered;
    bool link_gatt_initialized;
    const ble_port_ops_t *ops;
    bool started;
    bool deinitialized;
    bool link_security_initialized;
    bool nimble_init_attempted;
    bool quiescing;
    bool deinit_failed;
    esp_err_t deinit_error;
    ble_nimble_store_guard_t storage_guard;
    int stop_result;
} ble_nimble_port_t;

static ble_nimble_port_t s_port;
static struct ble_npl_event s_pairing_gate_event;
static struct ble_npl_event s_terminate_event;
static struct ble_npl_event s_cleanup_drain_event;
static StaticSemaphore_t s_pairing_gate_ack_control;
static StaticSemaphore_t s_pairing_gate_lock_control;
static SemaphoreHandle_t s_pairing_gate_ack;
static SemaphoreHandle_t s_pairing_gate_lock;
static atomic_uint s_pairing_gate_requested_seq = ATOMIC_VAR_INIT(0U);
static atomic_uint s_pairing_gate_applied_seq = ATOMIC_VAR_INIT(0U);
static ble_nimble_pairing_gate_state_t s_pairing_gate_state;
static atomic_bool s_pairing_gate_event_queued = ATOMIC_VAR_INIT(false);
static atomic_uintptr_t s_nimble_host_task = ATOMIC_VAR_INIT(0U);
static atomic_bool s_terminate_event_queued = ATOMIC_VAR_INIT(false);
static atomic_bool s_cleanup_draining = ATOMIC_VAR_INIT(false);
static atomic_bool s_cleanup_drain_event_queued = ATOMIC_VAR_INIT(false);
static atomic_uint s_cleanup_drain_requested_seq = ATOMIC_VAR_INIT(0U);
static atomic_uint s_cleanup_drain_applied_seq = ATOMIC_VAR_INIT(0U);
static StaticSemaphore_t s_cleanup_drain_ack_control;
static StaticSemaphore_t s_cleanup_drain_lock_control;
static SemaphoreHandle_t s_cleanup_drain_ack;
static SemaphoreHandle_t s_cleanup_drain_lock;
/* False from init/reset until host synchronization and durable store
 * reconciliation both complete. The ADV owner checks this immediately before
 * every queued physical command. */
static atomic_bool s_adv_host_ready = ATOMIC_VAR_INIT(false);

static void _ble_nimble_port_pairing_gate_event(
    struct ble_npl_event *event)
{
    (void)event;
    for (;;)
    {
        const uint32_t sequence = atomic_load_explicit(
                                      &s_pairing_gate_requested_seq,
                                      memory_order_acquire);
        const bool open = ble_nimble_pairing_gate_effective_open(
                              &s_pairing_gate_state);

        ble_hs_cfg.sm_sec_lvl = open ? 0 : 1;
        atomic_store_explicit(&s_pairing_gate_applied_seq, sequence,
                              memory_order_release);
        if (s_pairing_gate_ack != NULL)
        {
            (void)xSemaphoreGive(s_pairing_gate_ack);
        }
        atomic_store_explicit(&s_pairing_gate_event_queued, false,
                              memory_order_release);
        if (atomic_load_explicit(&s_pairing_gate_requested_seq,
                                 memory_order_acquire) == sequence)
        {
            return;
        }
        /* A request that observed event_queued=false has already queued
         * this persistent event. Otherwise this callback owns the newer
         * request and applies it before returning. */
        if (atomic_exchange_explicit(&s_pairing_gate_event_queued, true,
                                     memory_order_acq_rel))
        {
            return;
        }
    }
}

static void _ble_nimble_port_cleanup_drain_event(
    struct ble_npl_event *event)
{
    (void)event;
    /* This callback is itself the barrier: it runs on the default host event
     * queue after every callback queued before the shutdown request. */
    ble_nimble_pairing_gate_request(&s_pairing_gate_state, false);
    (void)ble_nimble_pairing_gate_set_hold(
        &s_pairing_gate_state, BLE_NIMBLE_PAIRING_GATE_HOLD_DRAIN, true);
    ble_hs_cfg.sm_sec_lvl = 1;
    const uint32_t sequence = atomic_load_explicit(
                                  &s_cleanup_drain_requested_seq,
                                  memory_order_acquire);

    atomic_store_explicit(&s_cleanup_drain_applied_seq, sequence,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_drain_event_queued, false,
                          memory_order_release);
    if (s_cleanup_drain_ack != NULL)
    {
        (void)xSemaphoreGive(s_cleanup_drain_ack);
    }
}

static esp_err_t _ble_nimble_port_apply_pairing_gate_state(void)
{
    if (s_pairing_gate_ack == NULL || s_pairing_gate_lock == NULL ||
            !s_port.started || s_port.quiescing || !ble_hs_synced())
    {
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreTake(s_pairing_gate_lock, portMAX_DELAY);
    while (xSemaphoreTake(s_pairing_gate_ack, 0U) == pdTRUE)
    {
        /* Drain an acknowledgement left by a timed-out earlier caller. */
    }
    const uint32_t current = atomic_load_explicit(
                                 &s_pairing_gate_requested_seq,
                                 memory_order_acquire);

    if (current == UINT32_MAX)
    {
        (void)xSemaphoreGive(s_pairing_gate_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t sequence = current + 1U;

    atomic_store_explicit(&s_pairing_gate_requested_seq, sequence,
                          memory_order_release);
    if (!atomic_exchange_explicit(&s_pairing_gate_event_queued, true,
                                  memory_order_acq_rel))
    {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(),
                           &s_pairing_gate_event);
    }
    esp_err_t result = ESP_ERR_TIMEOUT;

    while (xSemaphoreTake(s_pairing_gate_ack,
                          pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) ==
            pdTRUE)
    {
        if (atomic_load_explicit(&s_pairing_gate_applied_seq,
                                 memory_order_acquire) == sequence)
        {
            result = ESP_OK;
            break;
        }
    }
    (void)xSemaphoreGive(s_pairing_gate_lock);
    return result;
}

static esp_err_t _ble_nimble_port_set_pairing_gate(bool open)
{
    if (open && atomic_load_explicit(&s_cleanup_draining,
                                     memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }
    ble_nimble_pairing_gate_request(&s_pairing_gate_state, open);
    const esp_err_t result = _ble_nimble_port_apply_pairing_gate_state();

    if (result == ESP_OK && open &&
            !ble_nimble_pairing_gate_effective_open(&s_pairing_gate_state))
    {
        /* Preserve the requested OPEN so the last hold release can restore
         * it, but do not let a bindable ADV START claim that SMP opened. */
        return ESP_ERR_INVALID_STATE;
    }
    return result;
}

static esp_err_t _ble_nimble_port_apply_pairing_gate_context(void)
{
    const TaskHandle_t host_task = (TaskHandle_t)(uintptr_t)
                                   atomic_load_explicit(
                                       &s_nimble_host_task,
                                       memory_order_acquire);

    if (host_task != NULL && xTaskGetCurrentTaskHandle() == host_task)
    {
        /* GAP admission and terminal callbacks already execute on the
         * persistent NimBLE host queue. Apply directly to avoid waiting on an
         * event behind the current callback; queued older events still read
         * the latest effective state. */
        ble_hs_cfg.sm_sec_lvl =
            ble_nimble_pairing_gate_effective_open(&s_pairing_gate_state) ?
            0 : 1;
        return ESP_OK;
    }
    return _ble_nimble_port_apply_pairing_gate_state();
}

static esp_err_t _ble_nimble_port_set_pairing_gate_hold(
    ble_nimble_pairing_gate_hold_t hold, bool active)
{
    if (!ble_nimble_pairing_gate_set_hold(
                &s_pairing_gate_state, hold, active))
    {
        return ESP_ERR_INVALID_ARG;
    }
    return _ble_nimble_port_apply_pairing_gate_context();
}

static bool _ble_nimble_port_bond_store_verified(
    const struct ble_gap_conn_desc *desc);
static bool _ble_nimble_port_pairing_window_open(void);
static void _ble_nimble_port_latch_storage_error(esp_err_t error);
static esp_err_t _ble_nimble_port_storage_error_load(void);
static esp_err_t _ble_nimble_port_gap_handle_event(
    const ble_gap_manager_event_t *event);
static esp_err_t _ble_nimble_port_gap_snapshot(
    ble_gap_manager_snapshot_t *out);
static bool _ble_nimble_port_capture_connection_identity(
    ble_port_event_t *event, ble_link_operation_kind_t kind);
static bool _ble_nimble_port_connection_identity_matches(
    const ble_port_event_t *event, ble_link_operation_kind_t kind,
    uint32_t generation, uint16_t conn_handle, bool match_security_epoch);
static bool _ble_nimble_port_gap_is_subscribed_kind(
    uint16_t conn_handle, uint16_t attr_handle, bool notify);
static esp_err_t _ble_nimble_port_wait_for_adv_stopped(
    ble_adv_manager_pause_reason_t reason);
static esp_err_t _ble_nimble_port_queue_peer_unpair_address(
    const ble_addr_t *peer_id_addr, uint16_t conn_handle,
    bool terminate_conn);
static esp_err_t _ble_nimble_port_queue_peer_unpair(
    uint16_t conn_handle, bool terminate_conn);
static esp_err_t _ble_nimble_port_queue_provisional_unpair(
    const ble_link_operation_identity_t *identity, bool terminate_conn);
static esp_err_t _ble_nimble_port_promote_provisional_bond(
    const ble_link_operation_identity_t *identity);
static int _ble_nimble_port_unpair_peer(const ble_addr_t *peer_id_addr);
static int _ble_nimble_port_store_status(
    struct ble_store_status_event *event, void *arg);
static esp_err_t _ble_nimble_port_reconcile_storage(void);
static esp_err_t _ble_nimble_port_reconcile_storage_locked(void);
static esp_err_t _ble_nimble_port_execute_revoke_locked(void);
static esp_err_t _ble_nimble_port_invalidate_authorization(void);
static esp_err_t _ble_nimble_port_delete_all_bonds(void);
static esp_err_t _ble_nimble_port_collect_residuals(
    int obj_type, ble_addr_t *peers, size_t *count, size_t capacity);
static esp_err_t _ble_nimble_port_execute_revoke(void);
static bool _ble_nimble_port_store_object_is_bond(int object_type);
static esp_err_t _ble_nimble_port_production_adv_start(
    const ble_port_adv_config_t *config);
static esp_err_t _ble_nimble_port_production_adv_stop(uint32_t generation);
static esp_err_t _ble_nimble_port_production_notify(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len);
static esp_err_t _ble_nimble_port_production_indicate(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len);
static bool _ble_nimble_port_arm_indication_timeout(
    bool armed, const ble_link_operation_identity_t *identity);
static bool _ble_nimble_port_retain_terminate(
    const ble_link_operation_identity_t *identity);
static bool _ble_nimble_port_retain_rejected_terminate(uint16_t conn_handle);

static void _ble_nimble_port_tx_completed(
    const ble_tx_scheduler_result_t *result, void *arg)
{
    (void)arg;
    if (result == NULL)
    {
        return;
    }
    if (result->kind == BLE_TX_SCHEDULER_KIND_INDICATE &&
            result->token != 0U)
    {
        /* The scheduler may already have submitted the next frame. The
         * token-qualified disarm therefore retires only this frame's timer. */
        (void)_ble_nimble_port_arm_indication_timeout(
            false, &result->identity);
    }
    /* Only a tracked indication response belongs to the link service. The
     * completion callback records the fact; the Device Link worker performs
     * the next submit outside this host callback. */
    if (result->status == ESP_OK && result->flow_id != 0U)
    {
        (void)ble_link_service_response_completed(
            result->flow_id, result->is_last);
    }
}

static const ble_port_ops_t s_production_ops =
{
    .adv_start = _ble_nimble_port_production_adv_start,
    .adv_stop = _ble_nimble_port_production_adv_stop,
    .notify = _ble_nimble_port_production_notify,
    .indicate = _ble_nimble_port_production_indicate,
};

static void _ble_nimble_port_dispatch(const ble_port_event_t *event)
{
    (void)ble_event_router_dispatch(event);
}

static void _ble_nimble_port_gap_consumer(
    const ble_port_event_t *event, void *arg)
{
    (void)arg;
    ble_gap_manager_event_t manager_event;

    memset(&manager_event, 0, sizeof(manager_event));
    switch (event->type)
    {
    case BLE_PORT_EVENT_CONNECT:
        /* Connection admission runs in the GAP event handler before the
         * event is dispatched, so the manager must never be fed twice for
         * one ACL. A rejected CONNECT is dispatched with accepted=false and
         * must not be admitted here. */
        return;
    case BLE_PORT_EVENT_DISCONNECT:
        manager_event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
        manager_event.identity = event->identity;
        manager_event.conn_handle = event->conn_handle;
        manager_event.reason = event->reason;
        break;
    case BLE_PORT_EVENT_MTU:
        manager_event.type = BLE_GAP_MANAGER_EVENT_MTU;
        manager_event.identity = event->identity;
        manager_event.conn_handle = event->conn_handle;
        manager_event.mtu = event->mtu;
        break;
    case BLE_PORT_EVENT_ENC_CHANGE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE;
        manager_event.identity = event->identity;
        manager_event.conn_handle = event->conn_handle;
        manager_event.encrypted = event->encrypted;
        break;
    case BLE_PORT_EVENT_SUBSCRIBE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE;
        manager_event.identity = event->identity;
        manager_event.conn_handle = event->conn_handle;
        manager_event.attr_handle = event->attr_handle;
        manager_event.subscribed = event->subscribed;
        manager_event.notify = event->notify;
        manager_event.indicate = event->indicate;
        break;
    case BLE_PORT_EVENT_ADV_COMPLETE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_ADV_COMPLETE;
        break;
    case BLE_PORT_EVENT_RESET:
        manager_event.type = BLE_GAP_MANAGER_EVENT_RESET;
        break;
    default:
        return;
    }
    (void)_ble_nimble_port_gap_handle_event(&manager_event);
}

static esp_err_t _ble_nimble_port_publish_link_state(
    const uint8_t *value, size_t len, void *arg)
{
    (void)arg;
    const uint16_t handle = ble_link_gatt_link_state_handle();

    if (handle == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ble_gap_manager_snapshot_t snapshot;

    if (_ble_nimble_port_gap_snapshot(&snapshot) != ESP_OK ||
            !snapshot.connected)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* tx_admission: authorized; the link_state notification CCCD must be
     * enabled. */
    uint32_t admission_error = 0U;

    if (ble_link_session_query_admission(
                snapshot.generation, BLE_LINK_SESSION_CHANNEL_EVENT,
                &admission_error) != ESP_OK ||
            admission_error != BLE_LINK_ERROR_OK ||
            !_ble_nimble_port_gap_is_subscribed_kind(
                snapshot.conn_handle, handle, true))
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* The raw link_state notification is not a service transaction: its
     * completion must not release the service response gate. */
    const ble_link_operation_identity_t identity =
    {
        .generation = snapshot.generation,
        .security_epoch = ble_link_session_security2_epoch(),
        .kind = BLE_LINK_OPERATION_TX_NOTIFY,
        .conn_handle = snapshot.conn_handle,
    };

    return ble_tx_scheduler_submit(
               BLE_TX_SCHEDULER_KIND_NOTIFY, &identity, handle,
               value, len, false);
}

/* One-shot timers for reassembly idle and indication confirmation. Absolute
 * obligations are retained independently of the command queue; esp_timer
 * callbacks are wake hints only. */
#define BLE_NIMBLE_PORT_TIMER_KIND_SESSION 0U
#define BLE_NIMBLE_PORT_TIMER_KIND_CONTROL 1U
#define BLE_NIMBLE_PORT_TIMER_KIND_INDICATION 2U
#define BLE_NIMBLE_PORT_TIMER_KINDS BLE_LINK_TIMER_DEADLINE_SLOT_COUNT

/* A pseudo-kind carried by a host-task command (not a real timer): the
 * timer owner task executes the local binding revoke on the host core,
 * where the NimBLE store may be touched safely. These opcodes are distinct
 * from the real timer kinds so a control command can never be confused
 * with a timer expiry. */
#define BLE_NIMBLE_PORT_TIMER_KIND_REVOKE 3U
#define BLE_NIMBLE_PORT_TIMER_KIND_UNPAIR 4U
#define BLE_NIMBLE_PORT_TIMER_KIND_QUIT 5U

#define BLE_NIMBLE_PORT_TIMER_REVISION_MAX 0x3fffffffU

typedef struct ble_nimble_port_timer_command
{
    bool armed;
    unsigned int kind;
    ble_link_operation_identity_t identity;
    uint32_t generation;
    uint32_t token;
    uint16_t conn_handle;
    ble_addr_t peer_id_addr;
    bool peer_addr_valid;
    bool delete_all_if_unresolved;
    bool provisional;
    bool terminate_conn;
    bool invalidate_authorization;
} ble_nimble_port_timer_command_t;

static bool _ble_nimble_port_execute_unpair(
    const ble_nimble_port_timer_command_t *command);

/* One REVOKE command may be pending at a time: the owner sets this while
 * executing a revoke, so a worker retry cannot enqueue duplicates that
 * would re-run the full bond deletion after the journal is gone. */
static atomic_bool s_revoke_command_pending;

/* The static queue carries retained security-control work only. Timer
 * deadlines never depend on queue capacity. */
static StaticQueue_t s_timer_queue_storage;
static ble_nimble_port_timer_command_t s_timer_queue_items[16];
static QueueHandle_t s_timer_command_queue;
static TaskHandle_t s_timer_owner_task;
static atomic_uintptr_t s_timer_wake_task = ATOMIC_VAR_INIT(0U);
static atomic_uint s_timer_callbacks_active = ATOMIC_VAR_INIT(0U);
static SemaphoreHandle_t s_timer_exit;
static uint32_t s_timer_generation;
static uint16_t s_link_conn_handle;
static uint16_t s_adv_conn_handle;
static uint32_t s_adv_generation;

static ble_link_timer_terminate_state_t s_terminate_obligation;
static ble_link_rejected_terminate_state_t s_rejected_terminate_obligation;
static uint32_t s_rejected_terminate_token;
static bool s_rejected_terminate_exhausted;
static ble_link_cleanup_state_t s_cleanup_obligations;

/* Per-connection link security admission facts, driven by the GAP
 * listener. The reducer converges regardless of whether IDENTITY_RESOLVED
 * precedes or follows ENC_CHANGE, so a legal RPA reconnect is never
 * misclassified as an unknown peer. */
static ble_link_sec_state_t s_link_sec_state;
static uint16_t s_link_sec_conn;
static bool s_provisional_bond;
static bool s_provisional_bond_promoted;
static bool s_provisional_cleanup_queued;
static bool s_provisional_peer_valid;
static uint32_t s_provisional_generation;
static uint16_t s_provisional_conn_handle;
static ble_addr_t s_provisional_peer;

/* Per-kind state is updated under s_link_state_lock. The revision advances
 * synchronously with each accepted identity update, so stale disarms are
 * no-ops and stale callbacks are only redundant wakeups. */
static uint32_t s_timer_epochs[BLE_NIMBLE_PORT_TIMER_KINDS];
static uint32_t s_operation_token;
static bool s_timer_exhausted[BLE_NIMBLE_PORT_TIMER_KINDS];
static esp_timer_handle_t s_timer_handles[BLE_NIMBLE_PORT_TIMER_KINDS];
static uint32_t s_timer_handle_epochs[BLE_NIMBLE_PORT_TIMER_KINDS];
static ble_link_timer_deadline_state_t s_timer_deadlines;

/* Serializes every link session/service access between the NimBLE host
 * task (GATT feed) and the timer owner task (idle/indication expiry). */
static SemaphoreHandle_t s_link_state_lock;

/* NimBLE's raw NOTIFY_TX callback carries no application token. Keep the
 * scheduler identity that was submitted to the host until the corresponding
 * callback arrives, so a late confirmation can never impersonate a newer
 * same-handle indication. Entries stay in submission order. */
static ble_nimble_tx_tracker_entry_t
s_tx_tracker_entries[BLE_NIMBLE_PORT_TX_TRACKER_CAPACITY];
static ble_nimble_tx_tracker_t s_tx_tracker;

/* Serializes the storage reconciliation/revoke transactions between the
 * host task (on_sync resume + reconcile) and the timer owner task (REVOKE
 * command): a revoke must never interleave with a reconcile that read the
 * authorization record before the revoke erased it. */
static SemaphoreHandle_t s_storage_lock;

static bool _ble_nimble_port_terminate_submitted(int result)
{
    return result == 0 || result == BLE_HS_EALREADY;
}

static void _ble_nimble_port_terminate_event(
    struct ble_npl_event *event)
{
    (void)event;
    bool release_rejected_pause = false;

    if (s_link_state_lock != NULL &&
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) == pdTRUE)
    {
        const uint64_t now_us = esp_timer_get_time();
        ble_link_timer_terminate_state_t accepted;

        if (ble_link_timer_terminate_due(
                    &s_terminate_obligation, now_us, &accepted))
        {
            ble_gap_manager_snapshot_t snapshot;
            const bool current =
                ble_gap_manager_get_snapshot(&snapshot) == ESP_OK &&
                snapshot.connected &&
                snapshot.generation == accepted.identity.generation &&
                snapshot.conn_handle == accepted.identity.conn_handle;

            if (!current)
            {
                ble_link_timer_terminate_finish(
                    &s_terminate_obligation, &accepted, true, 0U);
            }
            else
            {
                const int terminate_result = ble_gap_terminate(
                                                 accepted.identity.conn_handle,
                                                 BLE_ERR_CONN_TERM_LOCAL);

                if (_ble_nimble_port_terminate_submitted(terminate_result))
                {
                    /* Submission is not the terminal boundary. Keep the
                     * generation fenced until its DISCONNECT/RESET callback. */
                    ble_link_timer_terminate_submitted(
                        &s_terminate_obligation, &accepted);
                }
                else if (terminate_result == BLE_HS_ENOTCONN)
                {
                    ble_link_timer_terminate_finish(
                        &s_terminate_obligation, &accepted, true, 0U);
                }
                else
                {
                    ble_link_timer_terminate_finish(
                        &s_terminate_obligation, &accepted, false,
                        now_us +
                        (uint64_t)BLE_NIMBLE_PORT_TERMINATE_RETRY_MS * 1000U);
                }
            }
        }
        ble_link_rejected_terminate_state_t rejected;

        if (ble_link_rejected_terminate_due(
                    &s_rejected_terminate_obligation, now_us, &rejected))
        {
            const int terminate_result = ble_gap_terminate(
                                             rejected.conn_handle,
                                             BLE_ERR_CONN_TERM_LOCAL);

            if (_ble_nimble_port_terminate_submitted(terminate_result))
            {
                ble_link_rejected_terminate_submitted(
                    &s_rejected_terminate_obligation, &rejected);
            }
            else if (terminate_result == BLE_HS_ENOTCONN)
            {
                ble_link_rejected_terminate_finish(
                    &s_rejected_terminate_obligation, &rejected, true, 0U);
                release_rejected_pause = true;
            }
            else
            {
                ble_link_rejected_terminate_finish(
                    &s_rejected_terminate_obligation, &rejected, false,
                    now_us +
                    (uint64_t)BLE_NIMBLE_PORT_TERMINATE_RETRY_MS * 1000U);
            }
        }
        if (release_rejected_pause)
        {
            (void)ble_nimble_pairing_gate_set_hold(
                &s_pairing_gate_state,
                BLE_NIMBLE_PAIRING_GATE_HOLD_REJECTED_ACL, false);
            (void)ble_adv_manager_set_pause_reason(
                BLE_ADV_MANAGER_PAUSE_REASON_REJECTED_ACL, false);
        }
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
    if (release_rejected_pause)
    {
        (void)_ble_nimble_port_apply_pairing_gate_context();
    }
    atomic_store_explicit(&s_terminate_event_queued, false,
                          memory_order_release);
    const TaskHandle_t owner = (TaskHandle_t)(uintptr_t)
                               atomic_load_explicit(
                                   &s_timer_wake_task,
                                   memory_order_acquire);

    if (owner != NULL)
    {
        xTaskNotifyGive(owner);
    }
}

static void _ble_nimble_port_queue_terminate_event(void)
{
    if (!atomic_exchange_explicit(&s_terminate_event_queued, true,
                                  memory_order_acq_rel))
    {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(),
                           &s_terminate_event);
    }
}

static esp_err_t _ble_nimble_port_tx_tracker_retain(
    const ble_link_operation_identity_t *identity,
    uint16_t value_handle, bool indication)
{
    if (identity == NULL || identity->generation == 0U ||
            identity->token == 0U || s_link_state_lock == NULL ||
            identity->kind != (indication ?
                               BLE_LINK_OPERATION_TX_INDICATE :
                               BLE_LINK_OPERATION_TX_NOTIFY) ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = ble_nimble_tx_tracker_retain(
                                 &s_tx_tracker, identity,
                                 value_handle, indication);

    xSemaphoreGiveRecursive(s_link_state_lock);
    return result;
}

static void _ble_nimble_port_tx_tracker_remove_identity(
    const ble_link_operation_identity_t *identity)
{
    if (identity == NULL || s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    (void)ble_nimble_tx_tracker_remove_identity(&s_tx_tracker, identity);
    xSemaphoreGiveRecursive(s_link_state_lock);
}

static bool _ble_nimble_port_tx_tracker_retire_identity(
    const ble_link_operation_identity_t *identity)
{
    if (identity == NULL || s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    const bool retired = ble_nimble_tx_tracker_retire_identity(
                             &s_tx_tracker, identity);

    xSemaphoreGiveRecursive(s_link_state_lock);
    return retired;
}

static bool _ble_nimble_port_tx_tracker_translate(
    uint16_t conn_handle, uint16_t value_handle,
    bool indication, bool terminal,
    ble_link_operation_identity_t *identity)
{
    if (identity == NULL || s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    const bool found = ble_nimble_tx_tracker_translate(
                           &s_tx_tracker, conn_handle, value_handle,
                           indication, terminal, identity);

    xSemaphoreGiveRecursive(s_link_state_lock);
    return found;
}

static void _ble_nimble_port_tx_tracker_clear(void)
{
    if (s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    ble_nimble_tx_tracker_clear(&s_tx_tracker);
    xSemaphoreGiveRecursive(s_link_state_lock);
}

static uint32_t _ble_nimble_port_next_operation_token_locked(void)
{
    if (s_operation_token == UINT32_MAX)
    {
        return 0U;
    }
    s_operation_token++;
    return s_operation_token;
}

static esp_err_t _ble_nimble_port_retain_cleanup(
    const ble_link_cleanup_request_t *request)
{
    const TaskHandle_t owner = (TaskHandle_t)(uintptr_t)
                               atomic_load_explicit(
                                   &s_timer_wake_task,
                                   memory_order_acquire);

    if (request == NULL || owner == NULL || s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool retained = ble_link_cleanup_retain(
                              &s_cleanup_obligations, request,
                              esp_timer_get_time());
    const bool live_acl = request->identity.generation ==
                          s_timer_generation &&
                          request->identity.conn_handle == s_link_conn_handle;
    const bool fence_required = request->terminate_conn && live_acl;
    const bool fenced = !fence_required ||
                        ble_link_cleanup_terminal_fence_retain(
                            &s_cleanup_obligations,
                            request->identity.generation,
                            request->identity.conn_handle);
    const bool cleanup_pending = ble_link_cleanup_pending(
                                     &s_cleanup_obligations);
    bool apply_gate = false;

    if (cleanup_pending)
    {
        apply_gate = ble_nimble_pairing_gate_set_hold(
                         &s_pairing_gate_state,
                         BLE_NIMBLE_PAIRING_GATE_HOLD_PEER_CLEANUP, true);
        const esp_err_t pause_result = ble_adv_manager_set_pause_reason(
                                           BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP,
                                           true);

        if (pause_result != ESP_OK)
        {
            /* The reason bit is retained even when physical STOP convergence
             * failed; the cleanup owner retries before touching the store. */
            LOG_W("cleanup advertising pause deferred result=%d", pause_result);
        }
    }
    xSemaphoreGiveRecursive(s_link_state_lock);
    if (apply_gate)
    {
        const esp_err_t gate_result =
            _ble_nimble_port_apply_pairing_gate_context();

        if (gate_result != ESP_OK)
        {
            LOG_W("cleanup pairing gate deferred result=%d", gate_result);
        }
    }
    if (fence_required)
    {
        ble_link_operation_identity_t terminate = request->identity;

        terminate.kind = BLE_LINK_OPERATION_TERMINATE;
        if (!_ble_nimble_port_retain_terminate(&terminate))
        {
            /* The cleanup payload and RX fence remain authoritative even if
             * the independent terminate retry reducer cannot be armed. */
            LOG_W("cleanup terminate retain failed generation=%" PRIu32
                  " handle=%u", terminate.generation,
                  terminate.conn_handle);
        }
    }
    if (retained || (fence_required && fenced))
    {
        xTaskNotifyGive(owner);
    }
    if (!retained)
    {
        return ESP_ERR_NO_MEM;
    }
    return fenced ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool _ble_nimble_port_cleanup_admission(void *arg)
{
    (void)arg;
    if (s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    const bool allowed = ble_link_cleanup_admission_allowed(
                             &s_cleanup_obligations) &&
                         !s_rejected_terminate_obligation.pending &&
                         !s_rejected_terminate_exhausted &&
                         !atomic_load_explicit(&s_cleanup_draining,
                             memory_order_acquire);

    xSemaphoreGiveRecursive(s_link_state_lock);
    return allowed;
}

static void _ble_nimble_port_latch_storage_error(esp_err_t error)
{
    ble_nimble_store_guard_finish(&s_port.storage_guard, error);
}

static esp_err_t _ble_nimble_port_storage_error_load(void)
{
    return ble_nimble_store_guard_error(&s_port.storage_guard);
}

static esp_err_t _ble_nimble_port_gap_handle_event(
    const ble_gap_manager_event_t *event)
{
    esp_err_t result;

    if (s_link_state_lock != NULL &&
            xSemaphoreTakeRecursive(s_link_state_lock, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    result = ble_gap_manager_handle_event(event);
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
    return result;
}

static esp_err_t _ble_nimble_port_gap_snapshot(
    ble_gap_manager_snapshot_t *out)
{
    esp_err_t result;

    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_link_state_lock != NULL &&
            xSemaphoreTakeRecursive(s_link_state_lock, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    result = ble_gap_manager_get_snapshot(out);
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
    return result;
}

static bool _ble_nimble_port_capture_connection_identity(
    ble_port_event_t *event, ble_link_operation_kind_t kind)
{
    if (event == NULL || kind == BLE_LINK_OPERATION_INVALID ||
            s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    ble_gap_manager_snapshot_t snapshot;
    const bool current = ble_gap_manager_get_snapshot(&snapshot) == ESP_OK &&
                         snapshot.connected &&
                         snapshot.conn_handle == event->conn_handle;

    if (current)
    {
        event->identity = (ble_link_operation_identity_t)
        {
            .generation = snapshot.generation,
            .security_epoch = ble_link_session_security2_epoch(),
            .kind = kind,
            .conn_handle = event->conn_handle,
        };
    }
    xSemaphoreGiveRecursive(s_link_state_lock);
    return current;
}

static bool _ble_nimble_port_connection_identity_matches(
    const ble_port_event_t *event, ble_link_operation_kind_t kind,
    uint32_t generation, uint16_t conn_handle, bool match_security_epoch)
{
    if (event == NULL || event->conn_handle != conn_handle ||
            event->identity.generation != generation ||
            event->identity.flow_id != 0U ||
            event->identity.token != 0U ||
            event->identity.kind != kind ||
            event->identity.conn_handle != conn_handle)
    {
        return false;
    }
    return !match_security_epoch ||
           event->identity.security_epoch ==
           ble_link_session_security2_epoch();
}

static bool _ble_nimble_port_gap_is_subscribed_kind(
    uint16_t conn_handle, uint16_t attr_handle, bool notify)
{
    bool subscribed = false;

    if (s_link_state_lock != NULL &&
            xSemaphoreTakeRecursive(s_link_state_lock, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    subscribed = ble_gap_manager_is_subscribed_kind(
                     conn_handle, attr_handle, notify);
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
    return subscribed;
}

static void _ble_nimble_port_storage_lock(void)
{
    if (s_storage_lock != NULL)
    {
        (void)xSemaphoreTake(s_storage_lock, portMAX_DELAY);
    }
}

static void _ble_nimble_port_storage_unlock(void)
{
    if (s_storage_lock != NULL)
    {
        (void)xSemaphoreGive(s_storage_lock);
    }
}


static void _ble_nimble_port_timer_cb(void *arg)
{
    (void)arg;
    /* A callback is only a wake hint. The owner sweeps retained absolute
     * deadlines, so a delayed callback cannot act on a retired identity. */
    atomic_fetch_add_explicit(&s_timer_callbacks_active, 1U,
                              memory_order_acq_rel);
    const TaskHandle_t owner = (TaskHandle_t)(uintptr_t)
                               atomic_load_explicit(
                                   &s_timer_wake_task, memory_order_acquire);

    if (owner != NULL)
    {
        xTaskNotifyGive(owner);
    }
    atomic_fetch_sub_explicit(&s_timer_callbacks_active, 1U,
                              memory_order_acq_rel);
}

static uint32_t _ble_nimble_port_timer_epoch_next(unsigned int kind)
{
    if (s_timer_exhausted[kind])
    {
        return 0U;
    }
    if (s_timer_epochs[kind] >= BLE_NIMBLE_PORT_TIMER_REVISION_MAX)
    {
        /* The epoch no longer fits the immutable timer id: fail closed
         * before the encoded value would wrap. */
        s_timer_exhausted[kind] = true;
        return 0U;
    }
    return s_timer_epochs[kind] + 1U;
}

static void _ble_nimble_port_timer_free_handle(unsigned int kind)
{
    if (s_timer_handles[kind] != NULL)
    {
        esp_timer_stop(s_timer_handles[kind]);
        esp_timer_delete(s_timer_handles[kind]);
        s_timer_handles[kind] = NULL;
    }
    s_timer_handle_epochs[kind] = 0U;
}

static void _ble_nimble_port_timer_fail_closed(
    unsigned int kind, const ble_link_operation_identity_t *identity)
{
    if (identity == NULL)
    {
        return;
    }
    if (kind == BLE_NIMBLE_PORT_TIMER_KIND_INDICATION)
    {
        const bool terminate_for_replacement =
            ble_link_service_delayed_replacement_pending(
                identity->generation);

        /* Retire the raw-callback mapping before the scheduler can submit a
         * later indication. NimBLE provides no application token in
         * NOTIFY_TX, so the tombstone must stay until the old terminal event
         * is consumed (or the connection is reset). */
        ble_link_operation_identity_t in_flight;
        const bool current =
            ble_tx_scheduler_get_in_flight_identity(&in_flight) == ESP_OK &&
            ble_link_operation_identity_equal(&in_flight, identity);

        if (current &&
                _ble_nimble_port_tx_tracker_retire_identity(identity) &&
                ble_tx_scheduler_handle_indication_timeout(
                    identity->token) == ESP_OK)
        {
            _ble_nimble_port_link_abort(identity);
            if (terminate_for_replacement)
            {
                ble_link_operation_identity_t terminate = *identity;

                terminate.kind = BLE_LINK_OPERATION_TERMINATE;
                (void)_ble_nimble_port_retain_terminate(&terminate);
            }
        }
    }
    else
    {
        /* Abort the reassembly slot for this channel. */
        ble_link_gatt_on_reassembly_idle_generation(
            identity->generation, identity->token);
    }
}

static bool _ble_nimble_port_timer_reconcile_handle(unsigned int kind)
{
    const ble_link_timer_deadline_slot_t *const slot =
        ble_link_timer_deadline_get_slot(&s_timer_deadlines, kind);

    if (slot == NULL || !slot->armed)
    {
        _ble_nimble_port_timer_free_handle(kind);
        return true;
    }
    if (s_timer_handles[kind] != NULL &&
            s_timer_handle_epochs[kind] == slot->revision)
    {
        return true;
    }
    const uint64_t now = esp_timer_get_time();
    uint64_t remaining_us = 0U;

    if (slot->deadline_us > now)
    {
        remaining_us = slot->deadline_us - now;
    }
    _ble_nimble_port_timer_free_handle(kind);
    esp_timer_create_args_t args;

    memset(&args, 0, sizeof(args));
    args.callback = _ble_nimble_port_timer_cb;
    args.name = (kind == BLE_NIMBLE_PORT_TIMER_KIND_INDICATION) ?
                "ble_ind_tmo" : "ble_reasm_idle";
    if (remaining_us == 0U ||
            esp_timer_create(&args, &s_timer_handles[kind]) != ESP_OK ||
            esp_timer_start_once(s_timer_handles[kind], remaining_us) != ESP_OK)
    {
        /* The bounded owner wait is still authoritative, but a failed wake
         * timer indicates that the runtime cannot maintain its normal
         * scheduling guarantee. Retire this obligation fail closed. */
        _ble_nimble_port_timer_free_handle(kind);
        return false;
    }
    s_timer_handle_epochs[kind] = slot->revision;
    return true;
}

static TickType_t _ble_nimble_port_timer_wait_ticks(uint64_t remaining_us)
{
    if (remaining_us == UINT64_MAX)
    {
        return portMAX_DELAY;
    }
    const uint64_t wait_ms = (remaining_us + 999U) / 1000U;
    TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)wait_ms);

    if (wait_ticks == 0U)
    {
        wait_ticks = 1U;
    }
    return wait_ticks;
}

static void _ble_nimble_port_timer_teardown(void)
{
    atomic_store_explicit(&s_revoke_command_pending, false,
                          memory_order_release);
    atomic_store_explicit(&s_timer_wake_task, 0U, memory_order_release);
    /* Free live timer handles and clear per-kind runtime state. Safe from
     * the owner task or from teardown when the owner had to be deleted. */
    for (unsigned int kind = 0U; kind < BLE_NIMBLE_PORT_TIMER_KINDS; ++kind)
    {
        _ble_nimble_port_timer_free_handle(kind);
    }
    while (atomic_load_explicit(&s_timer_callbacks_active,
                                memory_order_acquire) != 0U)
    {
        taskYIELD();
    }
    ble_link_timer_deadline_reset(&s_timer_deadlines);
    ble_link_timer_terminate_reset(&s_terminate_obligation);
    ble_link_rejected_terminate_reset(&s_rejected_terminate_obligation);
    ble_link_cleanup_reset(&s_cleanup_obligations);
    ble_nimble_pairing_gate_reset(&s_pairing_gate_state);
    (void)ble_adv_manager_set_pause_reason(
        BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP, false);
    (void)ble_adv_manager_set_pause_reason(
        BLE_ADV_MANAGER_PAUSE_REASON_REVOKE_PORT, false);
    if (!s_rejected_terminate_exhausted)
    {
        (void)ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_REJECTED_ACL, false);
    }
    /* Drain stale commands so a restarted owner cannot act on them. The
     * queue may not exist yet during partial initialization. */
    if (s_timer_command_queue != NULL)
    {
        ble_nimble_port_timer_command_t stale;

        while (xQueueReceive(s_timer_command_queue, &stale, 0U) == pdTRUE)
        {
        }
    }
}

static bool _ble_nimble_port_retain_terminate(
    const ble_link_operation_identity_t *identity)
{
    const TaskHandle_t owner = (TaskHandle_t)(uintptr_t)
                               atomic_load_explicit(
                                   &s_timer_wake_task, memory_order_acquire);

    if (identity == NULL || identity->generation == 0U ||
            identity->kind != BLE_LINK_OPERATION_TERMINATE ||
            identity->conn_handle == BLE_LINK_TIMER_DEADLINE_CONN_ANY ||
            owner == NULL ||
            s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    if (identity->generation != s_timer_generation ||
            identity->conn_handle != s_link_conn_handle)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
        return true;
    }
    const bool retained = ble_link_timer_terminate_request(
                              &s_terminate_obligation, identity,
                              esp_timer_get_time());
    xSemaphoreGiveRecursive(s_link_state_lock);
    if (!retained)
    {
        return false;
    }
    xTaskNotifyGive(owner);
    return true;
}

static bool _ble_nimble_port_retain_rejected_terminate(uint16_t conn_handle)
{
    const TaskHandle_t owner = (TaskHandle_t)(uintptr_t)
                               atomic_load_explicit(
                                   &s_timer_wake_task, memory_order_acquire);

    if (conn_handle == BLE_LINK_TIMER_DEADLINE_CONN_ANY || owner == NULL ||
            s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    bool retained = false;

    if (s_rejected_terminate_obligation.pending)
    {
        retained = s_rejected_terminate_obligation.conn_handle == conn_handle;
    }
    else if (!s_rejected_terminate_exhausted)
    {
        if (s_rejected_terminate_token == UINT32_MAX)
        {
            s_rejected_terminate_exhausted = true;
        }
        else
        {
            s_rejected_terminate_token++;
            retained = ble_link_rejected_terminate_request(
                           &s_rejected_terminate_obligation,
                           s_rejected_terminate_token, conn_handle,
                           esp_timer_get_time());
        }
    }
    const bool hold_gate = retained || s_rejected_terminate_exhausted;

    if (hold_gate)
    {
        /* Keep advertising and admission closed until the rejected physical
         * ACL has reached its terminal host callback. Exhaustion stays
         * fail-closed for the rest of the boot. */
        (void)ble_nimble_pairing_gate_set_hold(
            &s_pairing_gate_state,
            BLE_NIMBLE_PAIRING_GATE_HOLD_REJECTED_ACL, true);
        (void)ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_REJECTED_ACL, true);
    }
    xSemaphoreGiveRecursive(s_link_state_lock);
    if (hold_gate)
    {
        (void)_ble_nimble_port_apply_pairing_gate_context();
    }
    if (retained)
    {
        xTaskNotifyGive(owner);
    }
    return retained;
}

static void _ble_nimble_port_retire_rejected_terminate(uint16_t conn_handle)
{
    if (s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    bool retired = false;

    if (s_rejected_terminate_obligation.pending &&
            s_rejected_terminate_obligation.conn_handle == conn_handle)
    {
        retired = ble_link_rejected_terminate_retire(
                      &s_rejected_terminate_obligation,
                      s_rejected_terminate_obligation.admission_token,
                      conn_handle);
        if (retired)
        {
            (void)ble_nimble_pairing_gate_set_hold(
                &s_pairing_gate_state,
                BLE_NIMBLE_PAIRING_GATE_HOLD_REJECTED_ACL, false);
            (void)ble_adv_manager_set_pause_reason(
                BLE_ADV_MANAGER_PAUSE_REASON_REJECTED_ACL, false);
        }
    }
    xSemaphoreGiveRecursive(s_link_state_lock);
    if (retired)
    {
        (void)_ble_nimble_port_apply_pairing_gate_context();
        const TaskHandle_t owner = (TaskHandle_t)(uintptr_t)
                                   atomic_load_explicit(
                                       &s_timer_wake_task,
                                       memory_order_acquire);

        if (owner != NULL)
        {
            xTaskNotifyGive(owner);
        }
    }
}

static void _ble_nimble_port_execute_terminate_obligation(void)
{
    if (s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    ble_link_timer_terminate_state_t accepted;
    ble_link_rejected_terminate_state_t rejected;
    const uint64_t now_us = esp_timer_get_time();
    const bool due = ble_link_timer_terminate_due(
                         &s_terminate_obligation, now_us, &accepted) ||
                     ble_link_rejected_terminate_due(
                         &s_rejected_terminate_obligation, now_us, &rejected);

    xSemaphoreGiveRecursive(s_link_state_lock);
    if (due)
    {
        /* The exact revalidation and handle-only HCI side effect run in the
         * NimBLE host event queue, serialized with CONNECT/DISCONNECT. */
        _ble_nimble_port_queue_terminate_event();
    }
}

static void _ble_nimble_port_execute_cleanup_obligation(void)
{
    ble_link_cleanup_request_t request;

    if (s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    const bool due = ble_link_cleanup_take_due(
                         &s_cleanup_obligations, esp_timer_get_time(),
                         &request);

    xSemaphoreGiveRecursive(s_link_state_lock);
    if (!due)
    {
        return;
    }
    ble_nimble_port_timer_command_t command;

    memset(&command, 0, sizeof(command));
    command.kind = BLE_NIMBLE_PORT_TIMER_KIND_UNPAIR;
    command.identity = request.identity;
    command.generation = request.identity.generation;
    command.conn_handle = request.identity.conn_handle;
    command.peer_id_addr.type = request.peer_addr_type;
    memcpy(command.peer_id_addr.val, request.peer_addr,
           sizeof(command.peer_id_addr.val));
    command.peer_addr_valid = request.peer_addr_valid;
    command.delete_all_if_unresolved = request.delete_all_if_unresolved;
    command.provisional = request.provisional;
    command.terminate_conn = request.terminate_conn;
    command.invalidate_authorization = request.invalidate_authorization;
    const bool complete = _ble_nimble_port_execute_unpair(&command);

    if (xSemaphoreTakeRecursive(s_link_state_lock,
                                portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    ble_link_cleanup_finish(
        &s_cleanup_obligations, &request, complete,
        esp_timer_get_time() +
        (uint64_t)BLE_NIMBLE_PORT_TERMINATE_RETRY_MS * 1000U);
    const bool provisional_pending =
        ble_link_cleanup_provisional_pending_for_acl(
            &s_cleanup_obligations, request.identity.generation,
            request.identity.conn_handle);
    const bool cleanup_pending = ble_link_cleanup_pending(
                                     &s_cleanup_obligations);
    const bool release_cleanup_gate = complete && !cleanup_pending;

    if (complete && request.provisional && !provisional_pending &&
            s_provisional_generation == request.identity.generation &&
            s_provisional_conn_handle == request.identity.conn_handle)
    {
        _ble_nimble_port_clear_provisional_tracking();
    }
    if (release_cleanup_gate)
    {
        /* Release visibility only after the completed snapshot was retired
         * and no other regular or overflow cleanup remains. The transition
         * stays under the cleanup lock so a concurrent retain cannot be
         * overwritten by this last-finisher release. */
        (void)ble_nimble_pairing_gate_set_hold(
            &s_pairing_gate_state,
            BLE_NIMBLE_PAIRING_GATE_HOLD_PEER_CLEANUP, false);
        (void)ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP, false);
    }
    xSemaphoreGiveRecursive(s_link_state_lock);
    if (release_cleanup_gate)
    {
        (void)_ble_nimble_port_apply_pairing_gate_context();
    }
}

static uint64_t _ble_nimble_port_timer_owner_sweep(void)
{
    ble_link_timer_deadline_expiry_t expiries[
        BLE_NIMBLE_PORT_TIMER_KINDS];
    size_t expiry_count = 0U;
    uint64_t remaining_us = UINT64_MAX;

    if (s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return remaining_us;
    }
    const uint64_t now = esp_timer_get_time();

    expiry_count = ble_link_timer_deadline_collect(
                       &s_timer_deadlines, now, expiries);
    for (unsigned int kind = 0U;
            kind < BLE_NIMBLE_PORT_TIMER_KINDS; ++kind)
    {
        if (!_ble_nimble_port_timer_reconcile_handle(kind) &&
                expiry_count < BLE_NIMBLE_PORT_TIMER_KINDS &&
                ble_link_timer_deadline_retire(
                    &s_timer_deadlines, kind, &expiries[expiry_count]))
        {
            ++expiry_count;
        }
    }
    remaining_us = ble_link_timer_deadline_remaining_us(
                       &s_timer_deadlines, now);
    const uint64_t terminate_remaining =
        ble_link_timer_terminate_remaining_us(
            &s_terminate_obligation, now);

    if (terminate_remaining < remaining_us)
    {
        remaining_us = terminate_remaining;
    }
    const uint64_t rejected_terminate_remaining =
        ble_link_rejected_terminate_remaining_us(
            &s_rejected_terminate_obligation, now);

    if (rejected_terminate_remaining < remaining_us)
    {
        remaining_us = rejected_terminate_remaining;
    }
    const uint64_t cleanup_remaining =
        ble_link_cleanup_remaining_us(&s_cleanup_obligations, now);

    if (cleanup_remaining < remaining_us)
    {
        remaining_us = cleanup_remaining;
    }
    xSemaphoreGiveRecursive(s_link_state_lock);

    for (size_t i = 0U; i < expiry_count; ++i)
    {
        _ble_nimble_port_timer_fail_closed(
            expiries[i].kind, &expiries[i].identity);
    }
    return remaining_us;
}

static void _ble_nimble_port_timer_owner(void *arg)
{
    (void)arg;
    ble_nimble_port_timer_command_t command;

    for (;;)
    {
        const bool command_received =
            xQueueReceive(s_timer_command_queue, &command, 0U) == pdTRUE;
        uint64_t remaining_us = _ble_nimble_port_timer_owner_sweep();
        bool quit = false;

        _ble_nimble_port_execute_terminate_obligation();
        _ble_nimble_port_execute_cleanup_obligation();
        remaining_us = _ble_nimble_port_timer_owner_sweep();

        if (command_received)
        {
            if (command.kind == BLE_NIMBLE_PORT_TIMER_KIND_QUIT)
            {
                quit = true;
            }
            else if (command.kind == BLE_NIMBLE_PORT_TIMER_KIND_REVOKE)
            {
                /* The worker journaled the intent and erased the
                 * authorization; complete the bond/CCCD deletion on the
                 * host core. The retained journal survives queue loss. */
                atomic_store_explicit(&s_revoke_command_pending, true,
                                      memory_order_release);
                esp_err_t revoke_result = ESP_FAIL;

                for (unsigned int attempt = 0U;
                        attempt < BLE_NIMBLE_PORT_REVOKE_RETRIES;
                        ++attempt)
                {
                    revoke_result = _ble_nimble_port_execute_revoke();
                    if (revoke_result == ESP_OK ||
                            attempt + 1U >= BLE_NIMBLE_PORT_REVOKE_RETRIES)
                    {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(
                                   BLE_NIMBLE_PORT_SECURITY_COMMAND_DELAY_MS));
                }
                if (revoke_result != ESP_OK)
                {
                    LOG_W("binding revoke incomplete result=%d",
                          revoke_result);
                }
                atomic_store_explicit(&s_revoke_command_pending, false,
                                      memory_order_release);
            }
            /* Blocking security commands return through the same sweep,
             * so a deadline that elapsed during storage/GAP work is never
             * skipped by an early continue. */
            remaining_us = _ble_nimble_port_timer_owner_sweep();
        }
        if (quit)
        {
            break;
        }
        if (!command_received)
        {
            (void)ulTaskNotifyTake(
                pdTRUE, _ble_nimble_port_timer_wait_ticks(remaining_us));
        }
    }
    _ble_nimble_port_timer_teardown();
    if (s_timer_exit != NULL)
    {
        xSemaphoreGive(s_timer_exit);
    }
    vTaskDelete(NULL);
}

static bool _ble_nimble_port_timer_send(
    bool armed, unsigned int kind,
    const ble_link_operation_identity_t *identity)
{
    const TaskHandle_t owner = (TaskHandle_t)(uintptr_t)
                               atomic_load_explicit(
                                   &s_timer_wake_task, memory_order_acquire);

    if (identity == NULL || kind >= BLE_NIMBLE_PORT_TIMER_KINDS ||
            owner == NULL ||
            (s_link_state_lock != NULL &&
             xSemaphoreTakeRecursive(s_link_state_lock,
                                     portMAX_DELAY) != pdTRUE))
    {
        return false;
    }
    ble_link_operation_identity_t current = *identity;

    if (current.conn_handle == BLE_LINK_TIMER_DEADLINE_CONN_ANY &&
            current.generation != 0U &&
            current.generation == s_timer_generation)
    {
        current.conn_handle = s_link_conn_handle;
    }
    if (armed && (current.generation == 0U || current.token == 0U ||
                  current.kind == BLE_LINK_OPERATION_INVALID ||
                  current.conn_handle == BLE_LINK_TIMER_DEADLINE_CONN_ANY ||
                  current.generation != s_timer_generation))
    {
        if (s_link_state_lock != NULL)
        {
            xSemaphoreGiveRecursive(s_link_state_lock);
        }
        return false;
    }
    const uint32_t revision = _ble_nimble_port_timer_epoch_next(kind);

    if (revision == 0U)
    {
        if (s_link_state_lock != NULL)
        {
            xSemaphoreGiveRecursive(s_link_state_lock);
        }
        if (armed)
        {
            _ble_nimble_port_timer_fail_closed(
                kind, &current);
        }
        return false;
    }
    const uint64_t timeout_us =
        (kind == BLE_NIMBLE_PORT_TIMER_KIND_INDICATION) ?
        (uint64_t)BLE_NIMBLE_PORT_INDICATION_TIMEOUT_MS * 1000U :
        (uint64_t)BLE_NIMBLE_PORT_REASSEMBLY_IDLE_MS * 1000U;
    const ble_link_timer_deadline_command_t command =
    {
        .armed = armed,
        .kind = kind,
        .revision = revision,
        .identity = current,
        .deadline_us = armed ? esp_timer_get_time() + timeout_us : 0U,
    };
    const bool applied = ble_link_timer_deadline_apply(
                             &s_timer_deadlines, &command);

    if (applied)
    {
        s_timer_epochs[kind] = revision;
    }
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
    if (!applied)
    {
        /* A stale generation/token is already retired and must not affect
         * the current operation. An invalid arm cannot admit I/O. */
        return !armed;
    }
    xTaskNotifyGive(owner);
    return true;
}

static bool _ble_nimble_port_timer_enqueue_control(
    const ble_nimble_port_timer_command_t *command, TickType_t wait_ticks)
{
    const TaskHandle_t owner = (TaskHandle_t)(uintptr_t)
                               atomic_load_explicit(
                                   &s_timer_wake_task, memory_order_acquire);

    if (command == NULL || s_timer_command_queue == NULL || owner == NULL ||
            xQueueSend(s_timer_command_queue, command, wait_ticks) != pdTRUE)
    {
        return false;
    }
    xTaskNotifyGive(owner);
    return true;
}

static void _ble_nimble_port_arm_reassembly_idle(
    bool armed, uint32_t generation,
    ble_link_service_rx_channel_t channel, uint32_t epoch)
{
    /* Each RX characteristic has its own 5000 ms idle window. */
    const unsigned int kind =
        (channel == BLE_LINK_SERVICE_RX_SESSION) ?
        BLE_NIMBLE_PORT_TIMER_KIND_SESSION :
        BLE_NIMBLE_PORT_TIMER_KIND_CONTROL;
    const ble_link_operation_identity_t identity =
    {
        .generation = generation,
        .security_epoch = ble_link_session_security2_epoch(),
        .token = epoch,
        .kind = channel == BLE_LINK_SERVICE_RX_SESSION ?
        BLE_LINK_OPERATION_REASSEMBLY_SESSION :
        BLE_LINK_OPERATION_REASSEMBLY_CONTROL,
        .conn_handle = BLE_LINK_TIMER_DEADLINE_CONN_ANY,
    };

    _ble_nimble_port_timer_send(armed, kind, &identity);
}

static bool _ble_nimble_port_arm_indication_timeout(
    bool armed, const ble_link_operation_identity_t *identity)
{
    return _ble_nimble_port_timer_send(
               armed, BLE_NIMBLE_PORT_TIMER_KIND_INDICATION, identity);
}

static void _ble_nimble_port_link_gatt_consumer(
    const ble_port_event_t *event, void *arg)
{
    (void)arg;
    ble_gap_manager_snapshot_t snapshot;
    ble_link_operation_identity_t teardown_identity;
    uint32_t generation = 0U;
    bool teardown = false;
    bool apply_rejected_gate = false;

    memset(&teardown_identity, 0, sizeof(teardown_identity));

    if (_ble_nimble_port_gap_snapshot(&snapshot) == ESP_OK)
    {
        generation = snapshot.generation;
    }
    switch (event->type)
    {
    case BLE_PORT_EVENT_CONNECT:
    case BLE_PORT_EVENT_DISCONNECT:
    case BLE_PORT_EVENT_ENC_CHANGE:
    case BLE_PORT_EVENT_MTU:
    case BLE_PORT_EVENT_SUBSCRIBE:
    case BLE_PORT_EVENT_RESET:
        break;
    default:
        /* Events that never touch the link session do not take the lock. */
        return;
    }
    if (s_link_state_lock != NULL &&
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    switch (event->type)
    {
    case BLE_PORT_EVENT_CONNECT:
        if (snapshot.connected && event->accepted &&
                _ble_nimble_port_connection_identity_matches(
                    event, BLE_LINK_OPERATION_CONNECT,
                    snapshot.generation, snapshot.conn_handle, true))
        {
            struct ble_gap_conn_desc desc;
            uint8_t peer_type = 0U;
            uint8_t peer_addr[6] = {0};

            if (ble_gap_conn_find(event->conn_handle, &desc) == 0)
            {
                peer_type = desc.peer_id_addr.type;
                memcpy(peer_addr, desc.peer_id_addr.val, 6U);
            }
            ble_link_gatt_set_connection(generation, event->conn_handle,
                                         peer_type, peer_addr);
            (void)ble_link_session_handle_event(
                generation, BLE_LINK_SESSION_EVENT_ACL_CONNECTED);
            (void)ble_link_session_set_connection_pairing_window(
                generation, _ble_nimble_port_pairing_window_open());
            /* Remember the connection identity and generation; the idle
             * timer arms only while a partial frame exists. */
            s_link_conn_handle = event->conn_handle;
            s_timer_generation = generation;
        }
        break;
    case BLE_PORT_EVENT_DISCONNECT:
        /* The gap consumer runs first and already cleared the snapshot;
         * compare against the remembered connection identity. */
        if (!_ble_nimble_port_connection_identity_matches(
                    event, BLE_LINK_OPERATION_DISCONNECT,
                    s_timer_generation, s_link_conn_handle, true))
        {
            break;
        }
        teardown_identity = event->identity;
        teardown = true;
        (void)ble_link_timer_terminate_retire(
            &s_terminate_obligation, &event->identity);
        (void)ble_link_cleanup_terminal_fence_release(
            &s_cleanup_obligations, event->identity.generation,
            event->conn_handle);
        (void)ble_link_session_handle_event(
            event->identity.generation,
            BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED);
        _ble_nimble_port_arm_reassembly_idle(
            false, s_timer_generation, BLE_LINK_SERVICE_RX_SESSION, 0U);
        _ble_nimble_port_arm_reassembly_idle(
            false, s_timer_generation, BLE_LINK_SERVICE_RX_CONTROL, 0U);
        {
            const ble_link_operation_identity_t indication =
            {
                .generation = s_timer_generation,
                .kind = BLE_LINK_OPERATION_TX_INDICATE,
                .conn_handle = event->conn_handle,
            };

            (void)_ble_nimble_port_arm_indication_timeout(
                false, &indication);
        }
        /* A disconnect with an unconfirmed response must not leave the
         * transaction gate busy for the next connection, and the Security
         * 2 session must not survive into the next connection (cross-
         * connection state reuse). */
        s_link_conn_handle = 0U;
        s_timer_generation = 0U;
        break;
    case BLE_PORT_EVENT_RESET:
        if (s_timer_generation != 0U)
        {
            teardown_identity = (ble_link_operation_identity_t)
            {
                .generation = s_timer_generation,
                .security_epoch = ble_link_session_security2_epoch(),
                .kind = BLE_LINK_OPERATION_RESET,
                .conn_handle = s_link_conn_handle,
            };
            teardown = true;
            (void)ble_link_cleanup_terminal_fence_release(
                &s_cleanup_obligations, s_timer_generation,
                s_link_conn_handle);
            (void)ble_link_session_handle_event(
                s_timer_generation, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED);
        }
        _ble_nimble_port_arm_reassembly_idle(
            false, s_timer_generation, BLE_LINK_SERVICE_RX_SESSION, 0U);
        _ble_nimble_port_arm_reassembly_idle(
            false, s_timer_generation, BLE_LINK_SERVICE_RX_CONTROL, 0U);
        {
            const ble_link_operation_identity_t indication =
            {
                .generation = s_timer_generation,
                .kind = BLE_LINK_OPERATION_TX_INDICATE,
                .conn_handle = s_link_conn_handle,
            };

            (void)_ble_nimble_port_arm_indication_timeout(
                false, &indication);
        }
        s_link_conn_handle = 0U;
        s_timer_generation = 0U;
        ble_link_timer_terminate_reset(&s_terminate_obligation);
        if (s_rejected_terminate_obligation.pending)
        {
            ble_link_rejected_terminate_reset(
                &s_rejected_terminate_obligation);
            if (!s_rejected_terminate_exhausted)
            {
                apply_rejected_gate = ble_nimble_pairing_gate_set_hold(
                                          &s_pairing_gate_state,
                                          BLE_NIMBLE_PAIRING_GATE_HOLD_REJECTED_ACL,
                                          false);
                (void)ble_adv_manager_set_pause_reason(
                    BLE_ADV_MANAGER_PAUSE_REASON_REJECTED_ACL, false);
            }
        }
        break;
    case BLE_PORT_EVENT_ENC_CHANGE:
        if (!_ble_nimble_port_connection_identity_matches(
                    event, BLE_LINK_OPERATION_ENCRYPT_CHANGE,
                    s_timer_generation, s_link_conn_handle, true))
        {
            break;
        }
        if (event->encrypted)
        {
            /* The bond/identity admission facts are accumulated and
             * reported by the GAP listener through the link security
             * reducer; nothing admission-related runs here. */
            break;
        }
        /* Encryption dropped: tear the session-level link security down.
         * The admission reducer holds its own facts until the next
         * encryption establishes. */
        (void)ble_link_session_clear_link_security(
            event->identity.generation);
        teardown_identity = event->identity;
        teardown = true;
        break;
    case BLE_PORT_EVENT_MTU:
        if (!_ble_nimble_port_connection_identity_matches(
                    event, BLE_LINK_OPERATION_MTU,
                    s_timer_generation, s_link_conn_handle, true))
        {
            break;
        }
        ble_link_gatt_set_att_mtu(event->mtu);
        break;
    case BLE_PORT_EVENT_SUBSCRIBE:
        if (_ble_nimble_port_connection_identity_matches(
                    event, BLE_LINK_OPERATION_SUBSCRIBE,
                    s_timer_generation, s_link_conn_handle, true) &&
                event->attr_handle == ble_link_gatt_link_state_handle())
        {
            ble_link_gatt_cccd_epoch_advance();
        }
        break;
    default:
        break;
    }
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
    if (apply_rejected_gate)
    {
        (void)_ble_nimble_port_apply_pairing_gate_context();
    }
    /* The service owner is allowed to call retained storage callbacks while
     * holding its mutex, and those callbacks take s_link_state_lock. Never
     * form the reverse edge here. The service revalidates the immutable
     * identity under its own lock before closing the adapter session. */
    if (teardown)
    {
        _ble_nimble_port_link_abort(&teardown_identity);
    }
    /* Host callbacks only retain the refresh fact and wake the Device Link
     * owner. Encoding, submission, and delivery-stamp mutation stay in the
     * owner task and cannot race SUBSCRIBE or TX completion callbacks. */
    ble_link_gatt_request_link_state_refresh();
}

static void _ble_nimble_port_adv_consumer(
    const ble_port_event_t *event, void *arg)
{
    (void)arg;
    /* A CONNECT dispatched with accepted=false was rejected by the
     * connection admission: it must never register as the advertising
     * connection (its disconnect would otherwise retire the real ACL's
     * state and restart advertising while the accepted ACL is live). */
    if (event->type == BLE_PORT_EVENT_CONNECT && event->status == 0 &&
            event->accepted && event->identity.generation != 0U &&
            event->identity.kind == BLE_LINK_OPERATION_CONNECT &&
            event->identity.conn_handle == event->conn_handle)
    {
        s_adv_conn_handle = event->conn_handle;
        s_adv_generation = event->identity.generation;
    }
    if (event->type == BLE_PORT_EVENT_DISCONNECT)
    {
        /* GAP runs first and clears its snapshot. Keep an independent
         * pre-clear identity so a stale disconnect cannot retire a newer
         * connection and the accepted disconnect still reaches advertising. */
        if (!_ble_nimble_port_connection_identity_matches(
                    event, BLE_LINK_OPERATION_DISCONNECT,
                    s_adv_generation, s_adv_conn_handle, false))
        {
            return;
        }
        s_adv_conn_handle = 0U;
        s_adv_generation = 0U;
    }
    else if (event->type == BLE_PORT_EVENT_RESET)
    {
        s_adv_conn_handle = 0U;
        s_adv_generation = 0U;
    }
    const esp_err_t result = ble_adv_manager_handle_event(event);

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    {
        LOG_E("adv manager event failed result=%d", result);
    }
}

static void _ble_nimble_port_tx_consumer(
    const ble_port_event_t *event, void *arg)
{
    (void)arg;
    switch (event->type)
    {
    case BLE_PORT_EVENT_NOTIFY_TX:
    {
        /* The scheduler serializes the TX path with its own lock and is
         * safe against synchronous re-entrant events; the link state lock
         * is deliberately NOT taken here because NimBLE may deliver
         * NOTIFY_TX synchronously inside an ops call that already holds
         * it, which would self-deadlock. Any rearm triggered by the
         * scheduler (production_indicate) takes the lock itself. */
        const esp_err_t result = ble_tx_scheduler_handle_notify_tx(event);

        if (result == ESP_OK &&
                (event->tx_result == BLE_PORT_TX_TIMEOUT ||
                 event->tx_result == BLE_PORT_TX_ERROR))
        {
            if (event->indication)
            {
                /* A response indication is transactional: ambiguity
                 * retires only its flow and closes that Security 2 epoch. */
                _ble_nimble_port_link_abort(&event->identity);
            }
            else if (event->attr_handle ==
                     ble_link_gatt_link_state_handle())
            {
                /* link_state notifications are best effort. */
                ble_link_gatt_mark_link_state_dirty();
            }
        }
        else if (result != ESP_OK && result != ESP_ERR_NOT_FOUND &&
                 result != ESP_ERR_INVALID_STATE)
        {
            if (event->indication)
            {
                _ble_nimble_port_link_abort(&event->identity);
            }
            else if (event->attr_handle ==
                     ble_link_gatt_link_state_handle())
            {
                ble_link_gatt_mark_link_state_dirty();
            }
            LOG_E("tx scheduler event failed result=%d", result);
        }
        break;
    }
    case BLE_PORT_EVENT_DISCONNECT:
        /* Only the current connection's disconnect resets the scheduler. */
        if (!_ble_nimble_port_connection_identity_matches(
                    event, BLE_LINK_OPERATION_DISCONNECT,
                    s_timer_generation, s_link_conn_handle, false))
        {
            break;
        }
        _ble_nimble_port_tx_tracker_clear();
        ble_tx_scheduler_reset();
        break;
    case BLE_PORT_EVENT_RESET:
        _ble_nimble_port_tx_tracker_clear();
        ble_tx_scheduler_reset();
        break;
    default:
        break;
    }
}

static void _ble_nimble_port_tx_lock_cb(void *arg)
{
    (void)arg;
    (void)xSemaphoreTake(s_port.adv_lock, portMAX_DELAY);
}

static void _ble_nimble_port_tx_unlock_cb(void *arg)
{
    (void)arg;
    (void)xSemaphoreGive(s_port.adv_lock);
}

static void _ble_nimble_port_adv_lock_cb(void *arg)
{
    (void)arg;
    (void)xSemaphoreTake(s_port.adv_lock, portMAX_DELAY);
}

static void _ble_nimble_port_adv_unlock_cb(void *arg)
{
    (void)arg;
    (void)xSemaphoreGive(s_port.adv_lock);
}

static uint32_t _ble_nimble_port_adv_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000U);
}

static void _ble_nimble_port_adv_arm_timer(uint32_t delay_ms, void *arg)
{
    (void)delay_ms;
    (void)arg;
    /* Arm and cancel are coalescible deadline-change hints. A task
     * notification cannot be lost when the bounded command queue is full. */
    if (s_port.adv_task != NULL && !s_port.quiescing)
    {
        xTaskNotifyGive(s_port.adv_task);
    }
}

static esp_err_t _ble_nimble_port_adv_manager_init(void)
{
    static const uint8_t device_link_uuid[16] =
    {
        0xa3, 0x4e, 0x85, 0x57, 0x11, 0x3d, 0x8a, 0xa2,
        0x59, 0x4e, 0xbb, 0xb4, 0x92, 0x31, 0x20, 0x3e,
    };
    static const uint8_t short_name[] = "MT";
    static ble_adv_manager_config_t config =
    {
        .fast_interval_ms = CONFIG_BLE_RUNTIME_ADV_FAST_INTERVAL_MS,
        .slow_interval_ms = CONFIG_BLE_RUNTIME_ADV_SLOW_INTERVAL_MS,
        .fast_window_ms = CONFIG_BLE_RUNTIME_ADV_FAST_WINDOW_MS,
        .short_name = short_name,
        .short_name_len = sizeof(short_name) - 1U,
        .service_uuid = device_link_uuid,
        .adv_version = 1U,
        .now_ms = _ble_nimble_port_adv_now_ms,
        .arm_timer = _ble_nimble_port_adv_arm_timer,
        .timer_arg = NULL,
        .ops = NULL,
        .lock = _ble_nimble_port_adv_lock_cb,
        .unlock = _ble_nimble_port_adv_unlock_cb,
        .lock_arg = NULL,
    };

    if (s_port.adv_lock == NULL)
    {
        s_port.adv_lock = xSemaphoreCreateMutex();
        if (s_port.adv_lock == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    config.ops = s_port.ops != NULL ? s_port.ops : &s_production_ops;
    ble_adv_manager_init(&config);
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_tx_manager_init(void)
{
    static ble_tx_scheduler_config_t scheduler_config =
    {
        .queue_depth = CONFIG_BLE_RUNTIME_TX_QUEUE_DEPTH,
        .max_frame_bytes = CONFIG_BLE_RUNTIME_TX_FRAME_BYTES,
        .ops = NULL,
        .completed = _ble_nimble_port_tx_completed,
        .completed_arg = NULL,
        .lock = _ble_nimble_port_tx_lock_cb,
        .unlock = _ble_nimble_port_tx_unlock_cb,
        .lock_arg = NULL,
    };
    static const ble_response_cache_config_t cache_config =
    {
        .max_entries = CONFIG_BLE_RUNTIME_RESPONSE_CACHE_ENTRIES,
        .max_entry_bytes = CONFIG_BLE_RUNTIME_RESPONSE_CACHE_ENTRY_BYTES,
        .max_key_bytes = CONFIG_BLE_RUNTIME_RESPONSE_CACHE_KEY_BYTES,
        .ttl_ms = CONFIG_BLE_RUNTIME_RESPONSE_CACHE_TTL_MS,
        .now_ms = _ble_nimble_port_adv_now_ms,
        .lock = _ble_nimble_port_tx_lock_cb,
        .unlock = _ble_nimble_port_tx_unlock_cb,
        .lock_arg = NULL,
    };

    if (s_port.adv_lock == NULL)
    {
        s_port.adv_lock = xSemaphoreCreateMutex();
        if (s_port.adv_lock == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    scheduler_config.ops = s_port.ops != NULL ? s_port.ops : &s_production_ops;
    {
        const esp_err_t scheduler_result = ble_tx_scheduler_init(&scheduler_config);

        if (scheduler_result != ESP_OK)
        {
            return scheduler_result;
        }
    }
    ble_response_cache_init(&cache_config);
    ble_used_id_set_init(&cache_config);
    return ESP_OK;
}

static bool _ble_nimble_port_bond_store_verified(
    const struct ble_gap_conn_desc *desc)
{
    if (desc == NULL || !desc->sec_state.encrypted ||
            !desc->sec_state.bonded)
    {
        return false;
    }
    if (desc->sec_state.key_size != 16)
    {
        return false;
    }
    struct ble_store_key_sec key;

    memset(&key, 0, sizeof(key));
    key.peer_addr = desc->peer_id_addr;
    struct ble_store_value_sec peer_sec;
    struct ble_store_value_sec our_sec;

    memset(&peer_sec, 0, sizeof(peer_sec));
    memset(&our_sec, 0, sizeof(our_sec));
    if (ble_store_read_peer_sec(&key, &peer_sec) != 0 ||
            ble_store_read_our_sec(&key, &our_sec) != 0)
    {
        return false;
    }
    /* The contract requires a Secure Connections bond with a 16-byte LTK
     * and both identity keys, indexed by the normalized identity address. */
    if (!peer_sec.sc || !our_sec.sc || !peer_sec.ltk_present ||
            !our_sec.ltk_present || !peer_sec.irk_present ||
            !our_sec.irk_present)
    {
        return false;
    }
    return peer_sec.key_size == 16 && our_sec.key_size == 16;
}

static bool _ble_nimble_port_pairing_window_open(void)
{
    return (ble_link_session_get_state_flags() &
            BLE_LINK_STATE_FLAG_BINDABLE) != 0U;
}

static bool _ble_nimble_port_peer_address_valid(const ble_addr_t *address)
{
    /* NimBLE address types: public, random, public identity, and random
     * identity (BLE_ADDR_RANDOM_ID == 3) are all legal identities. */
    if (address == NULL || address->type > 3U)
    {
        return false;
    }
    bool nonzero = false;

    for (size_t i = 0U; i < sizeof(address->val); ++i)
    {
        nonzero = nonzero || address->val[i] != 0U;
    }
    return nonzero;
}

static void _ble_nimble_port_track_provisional_connection(
    uint32_t generation, uint16_t conn_handle, bool had_bond,
    bool identity_ready, const ble_addr_t *peer_id_addr)
{
    if (s_link_state_lock != NULL)
    {
        (void)xSemaphoreTakeRecursive(s_link_state_lock, portMAX_DELAY);
    }
    s_provisional_bond = !had_bond;
    s_provisional_bond_promoted = false;
    s_provisional_cleanup_queued = false;
    s_provisional_generation = generation;
    s_provisional_conn_handle = conn_handle;
    s_provisional_peer_valid = identity_ready &&
                               _ble_nimble_port_peer_address_valid(peer_id_addr);
    memset(&s_provisional_peer, 0, sizeof(s_provisional_peer));
    if (s_provisional_peer_valid)
    {
        s_provisional_peer = *peer_id_addr;
    }
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
}

static void _ble_nimble_port_update_provisional_identity(
    uint32_t generation, uint16_t conn_handle, const ble_addr_t *peer_id_addr)
{
    if (s_link_state_lock != NULL)
    {
        (void)xSemaphoreTakeRecursive(s_link_state_lock, portMAX_DELAY);
    }
    if (s_provisional_generation == generation &&
            s_provisional_conn_handle == conn_handle &&
            _ble_nimble_port_peer_address_valid(peer_id_addr))
    {
        s_provisional_peer = *peer_id_addr;
        s_provisional_peer_valid = true;
    }
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
}

static void _ble_nimble_port_clear_provisional_tracking(void)
{
    if (s_link_state_lock != NULL)
    {
        (void)xSemaphoreTakeRecursive(s_link_state_lock, portMAX_DELAY);
    }
    memset(&s_provisional_peer, 0, sizeof(s_provisional_peer));
    s_provisional_bond = false;
    s_provisional_bond_promoted = false;
    s_provisional_cleanup_queued = false;
    s_provisional_peer_valid = false;
    s_provisional_generation = 0U;
    s_provisional_conn_handle = 0U;
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
}

/**
 * @brief Apply the actions returned by the link security reducer.
 *
 * Runs in the GAP listener context (host task) where the admission facts
 * were accumulated. The generation used for the session events is read
 * from the GAP manager snapshot, which the gap consumer updates first.
 */
static void _ble_nimble_port_apply_sec_actions(
    uint32_t actions, uint16_t conn_handle)
{
    ble_gap_manager_snapshot_t snapshot;
    uint32_t generation = 0U;

    if (_ble_nimble_port_gap_snapshot(&snapshot) == ESP_OK)
    {
        generation = snapshot.generation;
    }
    if ((actions & BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED) != 0U)
    {
        (void)ble_link_session_handle_event(
            generation, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED);
    }
    if ((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U)
    {
        (void)ble_link_session_handle_event(
            generation, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED);
    }
    if ((actions & BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN) != 0U)
    {
        (void)ble_link_session_set_identity_known(generation, true);
    }
    if ((actions & BLE_LINK_SEC_ACTION_DELETE_BOND) != 0U)
    {
        const bool terminate =
            (actions & BLE_LINK_SEC_ACTION_TERMINATE) != 0U;
        const esp_err_t delete_result =
            _ble_nimble_port_queue_peer_unpair(conn_handle, terminate);

        if (delete_result != ESP_OK)
        {
            ESP_LOGW(TAG, "orphan bond cleanup failed (%d)", delete_result);
        }
    }
    /* DELETE_BOND retains the corresponding host-event termination. The
     * security reducer never emits TERMINATE without DELETE_BOND, so there is
     * no second raw handle-only side effect in this callback. */
}

/**
 * @brief Query whether the store holds a bond for the connection identity.
 *
 * Connection-scoped prior-bond snapshot: called at connect and identity
 * resolution time, before any pairing of the current connection could
 * have persisted keys, so it distinguishes a pre-existing bond from one
 * created by the current pairing.
 */
static bool _ble_nimble_port_peer_has_bond(
    const struct ble_gap_conn_desc *desc)
{
    if (desc == NULL)
    {
        return false;
    }
    struct ble_store_key_sec key;

    memset(&key, 0, sizeof(key));
    key.peer_addr = desc->peer_id_addr;
    struct ble_store_value_sec value;

    memset(&value, 0, sizeof(value));
    return ble_store_read_peer_sec(&key, &value) == 0 &&
           value.ltk_present;
}

static int _ble_nimble_port_unpair_peer(const ble_addr_t *peer_id_addr)
{
    if (peer_id_addr == NULL)
    {
        return BLE_HS_EINVAL;
    }
    if (_ble_nimble_port_storage_error_load() != ESP_OK)
    {
        return BLE_HS_ESTORE_FAIL;
    }
    /* IDF v6.0.2 ble_gap_unpair() logs but discards a nonzero result from
     * ble_store_util_delete_peer() on its normal PEER_SEC path. Perform the
     * single explicit durable delete first and honor its result. The later
     * gap_unpair call sees an empty store and is used only to remove the
     * controller/privacy entry. */
    int result = ble_store_util_delete_peer(peer_id_addr);

    if (result != 0)
    {
        /* The IDF store removes the RAM entry before persisting NVS and does
         * not roll RAM back on persist failure. A later same-run absence is
         * therefore not durable confirmation; latch the host run fail closed
         * so only a reload from NVS may retry this obligation. */
        _ble_nimble_port_latch_storage_error(ESP_FAIL);
        LOG_W("peer unpair failed result=%d", result);
        return result;
    }
    static const int s_peer_store_types[] =
    {
        BLE_STORE_OBJ_TYPE_OUR_SEC,
        BLE_STORE_OBJ_TYPE_PEER_SEC,
        BLE_STORE_OBJ_TYPE_CCCD,
        BLE_STORE_OBJ_TYPE_PEER_ADDR,
    };

    for (size_t type_index = 0U;
            type_index < sizeof(s_peer_store_types) /
            sizeof(s_peer_store_types[0]); ++type_index)
    {
        ble_addr_t residual[CONFIG_BT_NIMBLE_MAX_BONDS];
        size_t residual_count = 0U;
        const esp_err_t collect_result = _ble_nimble_port_collect_residuals(
                                             s_peer_store_types[type_index],
                                             residual, &residual_count,
                                             CONFIG_BT_NIMBLE_MAX_BONDS);

        if (collect_result != ESP_OK)
        {
            _ble_nimble_port_latch_storage_error(collect_result);
            return BLE_HS_ESTORE_CAP;
        }
        for (size_t peer_index = 0U; peer_index < residual_count;
                ++peer_index)
        {
            if (ble_addr_cmp(&residual[peer_index], peer_id_addr) == 0)
            {
                _ble_nimble_port_latch_storage_error(ESP_FAIL);
                LOG_W("peer store residual type=%d",
                      s_peer_store_types[type_index]);
                return BLE_HS_ESTORE_FAIL;
            }
        }
    }
    const int gap_result = ble_gap_unpair(peer_id_addr);

    if (gap_result != 0 && gap_result != BLE_HS_ENOENT)
    {
        LOG_W("peer privacy cleanup failed result=%d", gap_result);
        return gap_result;
    }
    return 0;
}

static esp_err_t _ble_nimble_port_queue_peer_unpair_address(
    const ble_addr_t *peer_id_addr, uint16_t conn_handle,
    bool terminate_conn)
{
    if (peer_id_addr == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ble_gap_manager_snapshot_t snapshot;
    uint32_t generation = 0U;

    if (_ble_nimble_port_gap_snapshot(&snapshot) == ESP_OK &&
            snapshot.connected && snapshot.conn_handle == conn_handle)
    {
        generation = snapshot.generation;
    }
    if (generation == 0U || s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t token = _ble_nimble_port_next_operation_token_locked();

    xSemaphoreGiveRecursive(s_link_state_lock);
    if (token == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const ble_link_cleanup_request_t request =
    {
        .identity =
        {
            .generation = generation,
            .security_epoch = ble_link_session_security2_epoch(),
            .token = token,
            .kind = BLE_LINK_OPERATION_PEER_CLEANUP,
            .conn_handle = conn_handle,
        },
        .peer_addr_type = peer_id_addr->type,
        .peer_addr =
        {
            peer_id_addr->val[0], peer_id_addr->val[1],
            peer_id_addr->val[2], peer_id_addr->val[3],
            peer_id_addr->val[4], peer_id_addr->val[5],
        },
        .peer_addr_valid = true,
        .terminate_conn = terminate_conn,
    };

    return _ble_nimble_port_retain_cleanup(&request);
}

static esp_err_t _ble_nimble_port_queue_peer_unpair(
    uint16_t conn_handle, bool terminate_conn)
{
    struct ble_gap_conn_desc desc;

    if (ble_gap_conn_find(conn_handle, &desc) != 0)
    {
        return ESP_ERR_NOT_FOUND;
    }
    return _ble_nimble_port_queue_peer_unpair_address(
               &desc.peer_id_addr, conn_handle, terminate_conn);
}

static esp_err_t _ble_nimble_port_queue_provisional_unpair(
    const ble_link_operation_identity_t *identity, bool terminate_conn)
{
    if (identity == NULL ||
            identity->kind != BLE_LINK_OPERATION_PROVISIONAL_DISCARD ||
            s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_provisional_bond || s_provisional_bond_promoted)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
        return ESP_OK;
    }
    if (identity->generation != s_provisional_generation ||
            identity->conn_handle != s_provisional_conn_handle)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const ble_link_cleanup_request_t request =
    {
        .identity = *identity,
        .peer_addr_type = s_provisional_peer.type,
        .peer_addr =
        {
            s_provisional_peer.val[0], s_provisional_peer.val[1],
            s_provisional_peer.val[2], s_provisional_peer.val[3],
            s_provisional_peer.val[4], s_provisional_peer.val[5],
        },
        .peer_addr_valid = s_provisional_peer_valid,
        .delete_all_if_unresolved = !s_provisional_peer_valid,
        .provisional = true,
        .terminate_conn = terminate_conn,
    };

    const esp_err_t result = _ble_nimble_port_retain_cleanup(&request);

    if (result == ESP_OK &&
            identity->generation == s_provisional_generation &&
            identity->conn_handle == s_provisional_conn_handle)
    {
        /* Keep this flag linearized with the helper retain. The owner was
         * notified, but cannot claim the request until this recursive lock's
         * outer level is released. */
        s_provisional_cleanup_queued = true;
    }
    xSemaphoreGiveRecursive(s_link_state_lock);
    return result;
}

static esp_err_t _ble_nimble_port_promote_provisional_bond(
    const ble_link_operation_identity_t *identity)
{
    if (identity == NULL ||
            identity->kind != BLE_LINK_OPERATION_PROVISIONAL_PROMOTE ||
            s_link_state_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTakeRecursive(s_link_state_lock,
                                portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_provisional_bond &&
            (identity->generation != s_provisional_generation ||
             identity->conn_handle != s_provisional_conn_handle))
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
        return ESP_ERR_NOT_FOUND;
    }
    bool release_advertising = false;

    if (s_provisional_bond && s_provisional_cleanup_queued)
    {
        const ble_link_cleanup_promote_result_t promote_result =
            ble_link_cleanup_promote(&s_cleanup_obligations, identity);

        if (promote_result != BLE_LINK_CLEANUP_PROMOTE_COMPLETE)
        {
            xSemaphoreGiveRecursive(s_link_state_lock);
            return promote_result == BLE_LINK_CLEANUP_PROMOTE_IN_PROGRESS ?
                   ESP_ERR_NOT_FINISHED : ESP_ERR_NOT_FOUND;
        }
        s_provisional_cleanup_queued = false;
        release_advertising = !ble_link_cleanup_pending(
                                  &s_cleanup_obligations);
    }
    if (s_provisional_bond)
    {
        s_provisional_bond_promoted = true;
    }
    if (release_advertising)
    {
        (void)ble_nimble_pairing_gate_set_hold(
            &s_pairing_gate_state,
            BLE_NIMBLE_PAIRING_GATE_HOLD_PEER_CLEANUP, false);
        (void)ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP, false);
    }
    xSemaphoreGiveRecursive(s_link_state_lock);
    if (release_advertising)
    {
        (void)_ble_nimble_port_apply_pairing_gate_context();
    }
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_retain_remote_replacement(
    const ble_link_operation_identity_t *identity)
{
    if (identity == NULL ||
            identity->kind != BLE_LINK_OPERATION_REMOTE_REPLACEMENT)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const ble_link_cleanup_request_t request =
    {
        .identity = *identity,
        .delete_all_if_unresolved = true,
        .terminate_conn = true,
        .invalidate_authorization = true,
    };

    return _ble_nimble_port_retain_cleanup(&request);
}

static bool _ble_nimble_port_execute_unpair(
    const ble_nimble_port_timer_command_t *command)
{
    if (command == NULL)
    {
        return false;
    }
    /* Let the GAP callback or SMP procedure that queued this command return
     * before touching the store and connection state. The whole deletion
     * runs under the storage lock: the host task may concurrently reconcile
     * the same store, and interleaved record reads/deletes would corrupt
     * both. */
    vTaskDelay(pdMS_TO_TICKS(BLE_NIMBLE_PORT_SECURITY_COMMAND_DELAY_MS));
    _ble_nimble_port_storage_lock();
    if (_ble_nimble_port_storage_error_load() != ESP_OK)
    {
        _ble_nimble_port_storage_unlock();
        return false;
    }

    /* ble_gap_unpair() refuses with BLE_HS_EBUSY while advertising is
     * active, and a disconnect cleanup races the advertising resume of the
     * previous ACL. Pause advertising and wait for the STOPPED state before
     * touching the store; a failed pause fails closed without deleting. */
    const esp_err_t pause_result = _ble_nimble_port_wait_for_adv_stopped(
                                       BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP);
    const bool paused = pause_result == ESP_OK;
    bool complete = false;

    if (!paused)
    {
        /* Every mandatory unpair (provisional cleanup, malformed-bond
         * eviction, replacement) fails closed: the retained obligation
         * retries and advertising is never explicitly resumed here. */
        LOG_W("unpair advertising pause failed result=%d", pause_result);
        goto exit;
    }

    ble_addr_t peer_id_addr = command->peer_id_addr;
    bool peer_addr_valid = command->peer_addr_valid &&
                           _ble_nimble_port_peer_address_valid(&peer_id_addr);
    struct ble_gap_conn_desc desc;
    ble_gap_manager_snapshot_t snapshot;
    const bool same_acl =
        command->generation != 0U &&
        _ble_nimble_port_gap_snapshot(&snapshot) == ESP_OK &&
        snapshot.connected &&
        snapshot.generation == command->generation &&
        snapshot.conn_handle == command->conn_handle;

    if (command->terminate_conn && same_acl)
    {
        /* The delete helper can implicitly terminate a matching peer. First
         * retain and await the exact accepted ACL's terminal callback; only a
         * later retry with no live generation may touch the peer store. */
        ble_link_operation_identity_t terminate = command->identity;

        terminate.kind = BLE_LINK_OPERATION_TERMINATE;
        (void)_ble_nimble_port_retain_terminate(&terminate);
        goto exit;
    }

    if (command->invalidate_authorization)
    {
        device_link_security_auth_record_t record;

        memset(&record, 0, sizeof(record));
        const esp_err_t load_result =
            device_link_security_load_auth_record(&record);

        if (load_result != ESP_OK && load_result != ESP_ERR_NOT_FOUND)
        {
            _ble_nimble_port_zeroize(&record, sizeof(record));
            goto exit;
        }
        if (load_result == ESP_OK &&
                device_link_security_auth_record_valid(&record))
        {
            peer_id_addr.type = record.peer_addr_type;
            memcpy(peer_id_addr.val, record.peer_addr,
                   sizeof(peer_id_addr.val));
            peer_addr_valid =
                _ble_nimble_port_peer_address_valid(&peer_id_addr);
        }
        _ble_nimble_port_zeroize(&record, sizeof(record));
        const esp_err_t invalidate_result =
            _ble_nimble_port_invalidate_authorization();

        if (invalidate_result != ESP_OK)
        {
            goto exit;
        }
    }
    if (!peer_addr_valid && same_acl &&
            ble_gap_conn_find(command->conn_handle, &desc) == 0 &&
            _ble_nimble_port_peer_address_valid(&desc.peer_id_addr))
    {
        peer_id_addr = desc.peer_id_addr;
        peer_addr_valid = true;
    }
    int unpair_result = 0;

    if (peer_addr_valid)
    {
        unpair_result = _ble_nimble_port_unpair_peer(&peer_id_addr);
        if (unpair_result == BLE_HS_EBUSY)
        {
            /* Advertising was paused and STOPPED was confirmed, yet the
             * host still refused: retry with a bounded backoff before
             * failing closed. */
            for (unsigned int attempt = 0U; attempt < 5U; ++attempt)
            {
                vTaskDelay(pdMS_TO_TICKS(
                               BLE_NIMBLE_PORT_SECURITY_COMMAND_DELAY_MS));
                unpair_result = _ble_nimble_port_unpair_peer(&peer_id_addr);
                if (unpair_result != BLE_HS_EBUSY)
                {
                    break;
                }
            }
        }
    }
    else if (command->delete_all_if_unresolved)
    {
        /* The ACL never resolved an identity, so the provisional bond
         * cannot be targeted. Deleting every bond is safe only while no
         * other binding was promoted: a promoted bond would have evicted
         * this orphan through the single-bond store overflow, so the
         * remaining single bond can only be this ACL's. A committed
         * authorization record therefore means the orphan is already gone
         * and the newer bond must be preserved. */
        device_link_security_auth_record_t record;

        memset(&record, 0, sizeof(record));
        const esp_err_t load_result =
            device_link_security_load_auth_record(&record);

        if (load_result != ESP_OK && load_result != ESP_ERR_NOT_FOUND)
        {
            _ble_nimble_port_zeroize(&record, sizeof(record));
            goto exit;
        }
        if (command->provisional && load_result == ESP_OK &&
                device_link_security_auth_record_valid(&record))
        {
            _ble_nimble_port_zeroize(&record, sizeof(record));
            goto exit;
        }
        _ble_nimble_port_zeroize(&record, sizeof(record));
        const esp_err_t delete_result = _ble_nimble_port_delete_all_bonds();

        unpair_result = delete_result == ESP_OK ? 0 : -1;
    }
    else
    {
        goto exit;
    }
    if (unpair_result != 0)
    {
        /* Every mandatory deletion (provisional cleanup, malformed bond
         * eviction, replacement unpair) fails closed: an unfulfilled
         * intent must not silently vanish and reopen advertising with the
         * bond still present. */
        LOG_W("bond cleanup failed result=%d", unpair_result);
        goto exit;
    }
    complete = true;

exit:
    /* The retained owner retires the exact executed snapshot first and only
     * then releases advertising if the whole regular+overflow state is empty. */
    _ble_nimble_port_storage_unlock();
    return complete;
}

static esp_err_t _ble_nimble_port_invalidate_authorization(void)
{
    esp_err_t result = device_link_security_erase_auth_record();

    if (result == ESP_ERR_NOT_FOUND)
    {
        result = ESP_OK;
    }
    if (result == ESP_OK)
    {
        const esp_err_t verifier_result =
            device_link_security_load_long_term_verifier();

        if (verifier_result != ESP_OK && verifier_result != ESP_ERR_NOT_FOUND)
        {
            result = verifier_result;
        }
    }
    /* Clear live authorization even when persistence cleanup failed. The
     * durable record or journal remains available for a later retry, while
     * the current connection is never allowed to stay authorized. */
    ble_link_service_clear_session_state();
    const esp_err_t session_result =
        ble_link_session_set_authorization(false, 0U);

    if (result == ESP_OK && session_result != ESP_OK)
    {
        result = session_result;
    }
    return result;
}

/**
 * @brief Verify a stored bond for a normalized identity without a live
 * connection (startup reconciliation).
 *
 * Mirrors _ble_nimble_port_bond_store_verified() but keys the store by the
 * identity address instead of a connection descriptor. Returns
 * ESP_OK with *out_verified = false when no valid bond exists (the caller
 * may delete it); any other return is a storage I/O failure and the caller
 * must fail closed.
 */
static esp_err_t _ble_nimble_port_bond_store_verified_identity(
    const ble_addr_t *peer_id_addr, bool *out_verified)
{
    if (peer_id_addr == NULL || out_verified == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_verified = false;
    if (!_ble_nimble_port_peer_address_valid(peer_id_addr))
    {
        return ESP_OK;
    }
    struct ble_store_key_sec key;

    memset(&key, 0, sizeof(key));
    key.peer_addr = *peer_id_addr;
    struct ble_store_value_sec peer_sec;
    struct ble_store_value_sec our_sec;

    memset(&peer_sec, 0, sizeof(peer_sec));
    memset(&our_sec, 0, sizeof(our_sec));
    const int peer_result = ble_store_read_peer_sec(&key, &peer_sec);
    const int our_result = ble_store_read_our_sec(&key, &our_sec);

    if (peer_result == BLE_HS_ENOENT || our_result == BLE_HS_ENOENT)
    {
        /* No (complete) bond for this identity: the caller may clean up. */
        return ESP_OK;
    }
    if (peer_result != 0 || our_result != 0)
    {
        ESP_LOGW(TAG, "bond store read failed (%d, %d)",
                 peer_result, our_result);
        return ESP_FAIL;
    }
    /* The contract requires a Secure Connections bond with a 16-byte LTK
     * and both identity keys, indexed by the normalized identity address. */
    *out_verified = peer_sec.sc && our_sec.sc &&
                    peer_sec.ltk_present && our_sec.ltk_present &&
                    peer_sec.irk_present && our_sec.irk_present &&
                    peer_sec.key_size == 16 && our_sec.key_size == 16;
    return ESP_OK;
}

/**
 * @brief Startup reconciliation of the single bond and the authorization
 * record.
 *
 * Runs after the NimBLE host synchronizes, before advertising starts:
 *
 * - a bond whose identity matches a valid authorization record AND whose
 *   store material is complete (SC, 16-byte LTK, both identity keys)
 *   restores the long-term binding (BLE_LINK_STATE_FLAG_BOUND);
 * - a bond without a matching record, or a matching address whose material
 *   is malformed, is deleted (orphan/malformed bond) and the record is
 *   invalidated;
 * - an authorization record without its bond is invalidated.
 *
 * The single-bond model admits at most one bonded peer; any mismatch
 * converges to unbound. A storage or reconciliation failure returns an
 * error so the runtime start fails closed instead of advertising.
 */
static esp_err_t _ble_nimble_port_reconcile_storage(void)
{
    esp_err_t result;

    _ble_nimble_port_storage_lock();
    result = _ble_nimble_port_reconcile_storage_locked();
    _ble_nimble_port_storage_unlock();
    return result;
}

static esp_err_t _ble_nimble_port_reconcile_storage_locked(void)
{
    device_link_security_auth_record_t record;
    esp_err_t result = ESP_OK;

    memset(&record, 0, sizeof(record));
    const esp_err_t load_result =
        device_link_security_load_auth_record(&record);
    const bool have_record = load_result == ESP_OK;
    const bool record_valid =
        have_record && device_link_security_auth_record_valid(&record);

    if (load_result != ESP_OK && load_result != ESP_ERR_NOT_FOUND)
    {
        result = load_result;
        goto exit;
    }
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;

    const int enumerate_result = ble_store_util_bonded_peers(
                                     peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS);

    if (enumerate_result != 0 || count < 0 ||
            count > CONFIG_BT_NIMBLE_MAX_BONDS)
    {
        ESP_LOGW(TAG, "bonded peers enumeration failed (%d, %d)",
                 enumerate_result, count);
        result = ESP_FAIL;
        goto exit;
    }
    bool matching_bond = false;
    ble_addr_t matching_peer = {0};

    if (!record_valid)
    {
        ESP_LOGW(TAG, "unexpected %d stored bonds; reconciling", count);
        for (int i = 0; i < count; ++i)
        {
            if (_ble_nimble_port_unpair_peer(&peers[i]) != 0)
            {
                ESP_LOGW(TAG, "bond eviction failed");
                result = ESP_FAIL;
                goto exit;
            }
        }
    }
    else
    {
        for (int i = 0; i < count; ++i)
        {
            const bool matches = peers[i].type == record.peer_addr_type &&
                                 memcmp(peers[i].val, record.peer_addr,
                                        DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES) == 0;

            if (matches)
            {
                bool verified = false;

                result = _ble_nimble_port_bond_store_verified_identity(
                             &peers[i], &verified);

                if (result != ESP_OK)
                {
                    goto exit;
                }
                if (verified)
                {
                    matching_bond = true;
                    matching_peer = peers[i];
                    continue;
                }
                /* The address matches but the material is missing or
                 * malformed: durable-invalidate the authorization first,
                 * then delete the broken bond (never keep an authorized
                 * identity without its security material). */
                ESP_LOGW(TAG, "malformed bond for matching record");
                const esp_err_t invalidate_result =
                    _ble_nimble_port_invalidate_authorization();

                if (invalidate_result != ESP_OK)
                {
                    result = invalidate_result;
                    goto exit;
                }
            }
            if (_ble_nimble_port_unpair_peer(&peers[i]) != 0)
            {
                ESP_LOGW(TAG, "mismatched bond eviction failed");
                result = ESP_FAIL;
                goto exit;
            }
        }
    }
    if (have_record && !matching_bond)
    {
        /* Authorization record without its bond: invalidate it and reload
         * the verifier (none). */
        const esp_err_t invalidate_result =
            _ble_nimble_port_invalidate_authorization();

        if (invalidate_result != ESP_OK)
        {
            result = invalidate_result;
            goto exit;
        }
    }
    {
        /* Sweep residual bond records of every object type - even when a
         * verified matching bond exists, because other peers (or a power
         * cut between store records) can leave PEER_SEC/CCCD/PEER_ADDR
         * without OUR_SEC, and bonded_peers() only enumerates OUR_SEC.
         * Residuals of the verified matching peer are part of its bond
         * and are preserved. */
        ble_addr_t residual[CONFIG_BT_NIMBLE_MAX_BONDS];
        size_t residual_count = 0U;

        result = _ble_nimble_port_collect_residuals(
                     BLE_STORE_OBJ_TYPE_PEER_SEC, residual, &residual_count,
                     sizeof(residual) / sizeof(residual[0]));

        if (result != ESP_OK)
        {
            goto exit;
        }
        result = _ble_nimble_port_collect_residuals(
                     BLE_STORE_OBJ_TYPE_CCCD, residual, &residual_count,
                     sizeof(residual) / sizeof(residual[0]));

        if (result != ESP_OK)
        {
            goto exit;
        }
        result = _ble_nimble_port_collect_residuals(
                     BLE_STORE_OBJ_TYPE_PEER_ADDR, residual,
                     &residual_count,
                     sizeof(residual) / sizeof(residual[0]));

        if (result != ESP_OK)
        {
            goto exit;
        }
        for (size_t i = 0U; i < residual_count; ++i)
        {
            if (matching_bond &&
                    ble_addr_cmp(&residual[i], &matching_peer) == 0)
            {
                continue;
            }
            if (_ble_nimble_port_unpair_peer(&residual[i]) != 0)
            {
                ESP_LOGW(TAG, "reconcile residual deletion failed");
                result = ESP_FAIL;
                goto exit;
            }
        }
    }
    {
        /* Converge the persistent bound fact exactly: only a verified
         * matching bond reports BOUND; every other successful outcome
         * (no record, malformed, or no match) reports UNBOUND, including
         * after a host reset within the same boot. */
        const esp_err_t bound_result =
            ble_link_session_set_authorization(matching_bond, 0U);

        if (bound_result != ESP_OK)
        {
            result = bound_result;
            goto exit;
        }
    }

exit:
    _ble_nimble_port_zeroize(&record, sizeof(record));
    return result;
}

/**
 * @brief Delete every stored bond (and its CCCDs) on the host core.
 *
 * ble_store_util_bonded_peers() only enumerates OUR_SEC records, so an
 * interrupted deletion (power cut between store records) can leave
 * PEER_SEC/CCCD/PEER_ADDR residuals behind. The sweep therefore also
 * collects every peer address visible in the residual record types and
 * deletes them, then verifies all four record types are empty before
 * reporting success. A non-empty store after the sweep is an error so the
 * caller keeps the revoke journal and fails closed.
 */
static esp_err_t _ble_nimble_port_delete_all_bonds(void)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;

    const int enumerate_result = ble_store_util_bonded_peers(
                                     peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS);

    if (enumerate_result != 0 || count < 0 ||
            count > CONFIG_BT_NIMBLE_MAX_BONDS)
    {
        _ble_nimble_port_latch_storage_error(ESP_FAIL);
        ESP_LOGW(TAG, "revoke bond enumeration failed (%d, %d)",
                 enumerate_result, count);
        return ESP_FAIL;
    }
    for (int i = 0; i < count; ++i)
    {
        if (_ble_nimble_port_unpair_peer(&peers[i]) != 0)
        {
            ESP_LOGW(TAG, "revoke bond deletion failed");
            return ESP_FAIL;
        }
    }
    /* Sweep residual records of every bond object type: a peer whose
     * OUR_SEC was already deleted is invisible to bonded_peers(). */
    {
        static const int s_bond_types[] =
        {
            BLE_STORE_OBJ_TYPE_PEER_SEC,
            BLE_STORE_OBJ_TYPE_CCCD,
            BLE_STORE_OBJ_TYPE_PEER_ADDR,
        };
        ble_addr_t residual[CONFIG_BT_NIMBLE_MAX_BONDS];
        size_t residual_count = 0U;

        for (size_t t = 0U; t < sizeof(s_bond_types) / sizeof(s_bond_types[0]);
                ++t)
        {
            const int type = s_bond_types[t];
            const esp_err_t collect = _ble_nimble_port_collect_residuals(
                                          type, residual, &residual_count,
                                          sizeof(residual) / sizeof(residual[0]));

            if (collect != ESP_OK)
            {
                return collect;
            }
        }
        for (size_t i = 0U; i < residual_count; ++i)
        {
            if (_ble_nimble_port_unpair_peer(&residual[i]) != 0)
            {
                ESP_LOGW(TAG, "revoke residual deletion failed");
                return ESP_FAIL;
            }
        }
    }
    /* Verify the store is fully empty before the journal is cleared: a
     * residual record would otherwise resurrect after the revoke. */
    {
        static const int s_check_types[] =
        {
            BLE_STORE_OBJ_TYPE_OUR_SEC,
            BLE_STORE_OBJ_TYPE_PEER_SEC,
            BLE_STORE_OBJ_TYPE_CCCD,
            BLE_STORE_OBJ_TYPE_PEER_ADDR,
        };

        for (size_t t = 0U; t < sizeof(s_check_types) / sizeof(s_check_types[0]);
                ++t)
        {
            int remaining = 0;

            if (ble_store_util_count(s_check_types[t], &remaining) != 0)
            {
                _ble_nimble_port_latch_storage_error(ESP_FAIL);
                ESP_LOGW(TAG, "revoke store count failed");
                return ESP_FAIL;
            }
            if (remaining != 0)
            {
                ESP_LOGW(TAG, "revoke store not empty after deletion");
                return ESP_FAIL;
            }
        }
    }
    return ESP_OK;
}

/**
 * @brief Collect unique peer identities from one residual store type.
 *
 * Used by the revoke sweep: PEER_SEC/CCCD/PEER_ADDR records carry the peer
 * identity even when the OUR_SEC record (the bonded_peers() index) was
 * already deleted by an interrupted unpair.
 */
typedef struct ble_nimble_port_residual_sweep
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    size_t count;
    size_t capacity;
    bool failed;
} ble_nimble_port_residual_sweep_t;

static int _ble_nimble_port_residual_collect(
    int obj_type, union ble_store_value *value, void *cookie)
{
    ble_nimble_port_residual_sweep_t *sweep = cookie;

    if (sweep == NULL)
    {
        return 1;
    }
    if (value == NULL)
    {
        sweep->failed = true;
        return 1;
    }
    ble_addr_t addr = {0};

    switch (obj_type)
    {
    case BLE_STORE_OBJ_TYPE_OUR_SEC:
    case BLE_STORE_OBJ_TYPE_PEER_SEC:
        addr = value->sec.peer_addr;
        break;
    case BLE_STORE_OBJ_TYPE_CCCD:
        addr = value->cccd.peer_addr;
        break;
    case BLE_STORE_OBJ_TYPE_PEER_ADDR:
        addr = value->rpa_rec.peer_addr;
        break;
    default:
        sweep->failed = true;
        return 1;
    }
    if (!_ble_nimble_port_peer_address_valid(&addr))
    {
        sweep->failed = true;
        return 1;
    }
    for (size_t i = 0U; i < sweep->count; ++i)
    {
        if (ble_addr_cmp(&sweep->peers[i], &addr) == 0)
        {
            return 0;
        }
    }
    if (sweep->count >= sweep->capacity)
    {
        sweep->failed = true;
        return 1;
    }
    sweep->peers[sweep->count] = addr;
    sweep->count++;
    return 0;
}

static esp_err_t _ble_nimble_port_collect_residuals(
    int obj_type, ble_addr_t *peers, size_t *count, size_t capacity)
{
    ble_nimble_port_residual_sweep_t sweep =
    {
        .count = *count,
        .capacity = capacity,
    };

    if (peers != NULL && *count > 0U)
    {
        memcpy(sweep.peers, peers, *count * sizeof(sweep.peers[0]));
    }
    const int rc = ble_store_iterate(obj_type,
                                     _ble_nimble_port_residual_collect,
                                     &sweep);

    if (rc != 0 || sweep.failed)
    {
        _ble_nimble_port_latch_storage_error(ESP_FAIL);
        ESP_LOGW(TAG, "residual store iteration failed (%d)", rc);
        return ESP_FAIL;
    }
    *count = sweep.count;
    if (peers != NULL)
    {
        memcpy(peers, sweep.peers, sweep.count * sizeof(sweep.peers[0]));
    }
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_wait_for_adv_stopped(
    ble_adv_manager_pause_reason_t reason)
{
    esp_err_t result = ble_adv_manager_set_pause_reason(reason, true);

    for (unsigned int attempt = 0U;
            attempt < BLE_NIMBLE_PORT_REVOKE_WAIT_RETRIES; ++attempt)
    {
        const ble_adv_manager_state_t state = ble_adv_manager_get_state();

        if (state == BLE_ADV_MANAGER_STATE_STOPPED)
        {
            return ESP_OK;
        }
        /* A failed stop is retryable in the manager. Re-enter convergence
         * while paused so a transient queue or GAP failure cannot leave the
         * durable revoke half-applied. */
        const esp_err_t retry_result = ble_adv_manager_set_pause_reason(
                                           reason, true);

        if (retry_result != ESP_OK)
        {
            result = retry_result;
        }
        if (attempt + 1U < BLE_NIMBLE_PORT_REVOKE_WAIT_RETRIES)
        {
            vTaskDelay(pdMS_TO_TICKS(BLE_NIMBLE_PORT_REVOKE_WAIT_MS));
        }
    }
    return result == ESP_OK ? ESP_ERR_TIMEOUT : result;
}

static esp_err_t _ble_nimble_port_wait_for_adv_resumed(
    ble_adv_manager_pause_reason_t reason)
{
    esp_err_t result = ble_adv_manager_set_pause_reason(reason, false);

    /* Convergence is complete once the unpause was accepted: STOPPED is a
     * legal converged state when no lease is held or a connection exists,
     * so waiting for a visible FAST/SLOW would fail closed on every
     * startup without a lease. */
    for (unsigned int attempt = 0U;
            result != ESP_OK && attempt < BLE_NIMBLE_PORT_REVOKE_WAIT_RETRIES;
            ++attempt)
    {
        if (attempt + 1U < BLE_NIMBLE_PORT_REVOKE_WAIT_RETRIES)
        {
            vTaskDelay(pdMS_TO_TICKS(BLE_NIMBLE_PORT_REVOKE_WAIT_MS));
        }
        result = ble_adv_manager_set_pause_reason(reason, false);
    }
    return result;
}

/**
 * @brief Complete a journaled local revoke on the host core.
 *
 * Contract order: pause advertising, close the live session and terminate
 * the ACL, delete every bond/CCCD record (verified empty), clear the
 * journal marker, and only then restore advertising. The journal is the
 * durable revoke intent: it is cleared only after the store deletion
 * succeeded, so a crash before that point resumes the revoke at startup.
 * After the journal is cleared, a resume failure is an advertising
 * availability error, not an incomplete revoke, and must never recreate
 * the marker.
 */
static esp_err_t _ble_nimble_port_execute_revoke(void)
{
    esp_err_t result;

    _ble_nimble_port_storage_lock();
    result = _ble_nimble_port_execute_revoke_locked();
    _ble_nimble_port_storage_unlock();
    return result;
}

static esp_err_t _ble_nimble_port_execute_revoke_locked(void)
{
    const esp_err_t storage_error = _ble_nimble_port_storage_error_load();

    if (storage_error != ESP_OK)
    {
        return storage_error;
    }
    /* The storage lock is held: re-confirm the durable intent. If the
     * journal is already gone the revoke completed (idempotent resume) and
     * nothing must be deleted. */
    bool pending = false;
    const esp_err_t pending_result =
        device_link_security_revoke_pending(&pending);

    if (pending_result != ESP_OK)
    {
        return pending_result;
    }
    if (!pending)
    {
        (void)_ble_nimble_port_set_pairing_gate_hold(
            BLE_NIMBLE_PAIRING_GATE_HOLD_REVOKE, false);
        return ESP_OK;
    }
    const esp_err_t gate_hold_result =
        _ble_nimble_port_set_pairing_gate_hold(
            BLE_NIMBLE_PAIRING_GATE_HOLD_REVOKE, true);

    if (gate_hold_result != ESP_OK)
    {
        return gate_hold_result;
    }
    const esp_err_t pause_result = _ble_nimble_port_wait_for_adv_stopped(
                                       BLE_ADV_MANAGER_PAUSE_REASON_REVOKE_PORT);

    if (pause_result != ESP_OK)
    {
        return pause_result;
    }
    ble_gap_manager_snapshot_t snapshot;

    if (_ble_nimble_port_gap_snapshot(&snapshot) == ESP_OK &&
            snapshot.connected)
    {
        /* Close the Security 2 session and service state before the ACL is
         * terminated: a revoked peer must not keep a live session. */
        const ble_link_operation_identity_t identity =
        {
            .generation = snapshot.generation,
            .security_epoch = ble_link_session_security2_epoch(),
            .kind = BLE_LINK_OPERATION_TERMINATE,
            .conn_handle = snapshot.conn_handle,
        };

        _ble_nimble_port_link_abort(&identity);
        if (!_ble_nimble_port_retain_terminate(&identity))
        {
            return ESP_ERR_INVALID_STATE;
        }
        /* Do not erase the bond while its ACL is still alive. The retained
         * host-event terminate survives transient HCI failures and remains
         * pending until exact DISCONNECT/RESET; the journal keeps this revoke
         * durable while the service retries the command. */
        return ESP_ERR_NOT_FINISHED;
    }
    const esp_err_t delete_result = _ble_nimble_port_delete_all_bonds();

    if (delete_result != ESP_OK)
    {
        /* The journal stays: the startup continuation retries the delete
         * while advertising remains paused (fail closed). */
        return delete_result;
    }
    /* Durable boundary: only now, with the store verified empty, is the
     * revoke intent cleared. */
    const esp_err_t journal_result = device_link_security_end_revoke();

    if (journal_result != ESP_OK)
    {
        /* Clearing the marker failed: the revoke is NOT complete and
         * advertising must stay paused so no peer can connect while the
         * intent is still journaled. */
        return journal_result;
    }
    const esp_err_t gate_release_result =
        _ble_nimble_port_set_pairing_gate_hold(
            BLE_NIMBLE_PAIRING_GATE_HOLD_REVOKE, false);

    if (gate_release_result != ESP_OK)
    {
        LOG_W("revoke pairing gate restore deferred result=%d",
              gate_release_result);
    }
    /* Advertising resumes only after the revoke fully completed; a failure
     * here is an availability error, not an incomplete revoke. */
    const esp_err_t resume_result = _ble_nimble_port_wait_for_adv_resumed(
                                        BLE_ADV_MANAGER_PAUSE_REASON_REVOKE_PORT);

    if (resume_result != ESP_OK)
    {
        LOG_W("revoke advertising resume failed result=%d", resume_result);
    }
    return ESP_OK;
}

/**
 * @brief Request a local binding revoke.
 *
 * The caller (device-link worker) must journal the intent and erase the
 * authorization record first; this function only enqueues the bond/CCCD
 * deletion on the host core. Returns after the command is queued, not
 * after the deletion completed.
 *
 * @return ESP_OK when queued, otherwise a state error.
 */
esp_err_t ble_nimble_port_revoke_binding(void)
{
    if (s_timer_command_queue == NULL || s_timer_owner_task == NULL ||
            !s_port.started || s_port.quiescing || !ble_hs_synced())
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* The journal check, the in-flight dedup, and the enqueue form one
     * transaction under the storage lock: if the owner already completed
     * the revoke (journal gone) or is still executing it, nothing is
     * enqueued. A duplicate command could otherwise terminate a NEW
     * connection and delete ITS bond. */
    esp_err_t result = ESP_OK;

    _ble_nimble_port_storage_lock();
    bool pending = false;
    const esp_err_t pending_result =
        device_link_security_revoke_pending(&pending);

    if (pending_result != ESP_OK)
    {
        result = pending_result;
    }
    else if (!pending)
    {
        /* The revoke already completed (journal cleared): nothing to do. */
        result = ESP_OK;
    }
    else if (atomic_exchange_explicit(&s_revoke_command_pending, true,
                                      memory_order_acq_rel))
    {
        /* A revoke is already queued or executing: the journal will be
         * cleared by it; a duplicate would re-run the full deletion. */
        result = ESP_OK;
    }
    else
    {
        const ble_nimble_port_timer_command_t command =
        {
            .armed = false,
            .kind = BLE_NIMBLE_PORT_TIMER_KIND_REVOKE,
        };

        if (!_ble_nimble_port_timer_enqueue_control(&command, 0U))
        {
            atomic_store_explicit(&s_revoke_command_pending, false,
                                  memory_order_release);
            result = ESP_ERR_NO_MEM;
        }
    }
    _ble_nimble_port_storage_unlock();
    return result;
}

esp_err_t ble_nimble_port_request_disconnect(void)
{
    if (s_timer_command_queue == NULL || s_timer_owner_task == NULL ||
            !s_port.started || s_port.quiescing || !ble_hs_synced())
    {
        return ESP_ERR_INVALID_STATE;
    }
    ble_gap_manager_snapshot_t snapshot;

    if (_ble_nimble_port_gap_snapshot(&snapshot) != ESP_OK ||
            !snapshot.connected || snapshot.generation == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const ble_link_operation_identity_t identity =
    {
        .generation = snapshot.generation,
        .security_epoch = ble_link_session_security2_epoch(),
        .kind = BLE_LINK_OPERATION_TERMINATE,
        .conn_handle = snapshot.conn_handle,
    };

    return _ble_nimble_port_retain_terminate(&identity) ?
           ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t ble_nimble_port_begin_cleanup_drain(void)
{
    if (s_cleanup_drain_ack == NULL || s_cleanup_drain_lock == NULL ||
            !s_port.started || s_port.quiescing || !ble_hs_synced())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t stop_result = _ble_nimble_port_wait_for_adv_stopped(
                                      BLE_ADV_MANAGER_PAUSE_REASON_SERVICE_SHUTDOWN);

    if (stop_result != ESP_OK)
    {
        return stop_result;
    }
    (void)xSemaphoreTake(s_cleanup_drain_lock, portMAX_DELAY);
    atomic_store_explicit(&s_cleanup_draining, true, memory_order_release);
    while (xSemaphoreTake(s_cleanup_drain_ack, 0U) == pdTRUE)
    {
        /* Drain a stale acknowledgement from a timed-out caller. */
    }
    uint32_t requested = atomic_load_explicit(
                             &s_cleanup_drain_requested_seq,
                             memory_order_acquire);
    const uint32_t applied = atomic_load_explicit(
                                 &s_cleanup_drain_applied_seq,
                                 memory_order_acquire);
    esp_err_t result = ESP_OK;

    if (requested != applied)
    {
        while (xSemaphoreTake(
                    s_cleanup_drain_ack,
                    pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) == pdTRUE)
        {
            if (atomic_load_explicit(&s_cleanup_drain_applied_seq,
                                     memory_order_acquire) == requested)
            {
                break;
            }
        }
        if (atomic_load_explicit(&s_cleanup_drain_applied_seq,
                                 memory_order_acquire) != requested)
        {
            result = ESP_ERR_TIMEOUT;
        }
    }
    if (result == ESP_OK)
    {
        if (requested == UINT32_MAX)
        {
            result = ESP_ERR_INVALID_STATE;
        }
        else
        {
            requested++;
            atomic_store_explicit(&s_cleanup_drain_requested_seq, requested,
                                  memory_order_release);
            if (atomic_exchange_explicit(&s_cleanup_drain_event_queued, true,
                                         memory_order_acq_rel))
            {
                result = ESP_ERR_INVALID_STATE;
            }
            else
            {
                ble_npl_eventq_put(nimble_port_get_dflt_eventq(),
                                   &s_cleanup_drain_event);
                while (xSemaphoreTake(
                            s_cleanup_drain_ack,
                            pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) ==
                        pdTRUE)
                {
                    if (atomic_load_explicit(&s_cleanup_drain_applied_seq,
                                             memory_order_acquire) == requested)
                    {
                        break;
                    }
                }
                if (atomic_load_explicit(&s_cleanup_drain_applied_seq,
                                         memory_order_acquire) != requested)
                {
                    result = ESP_ERR_TIMEOUT;
                }
            }
        }
    }
    (void)xSemaphoreGive(s_cleanup_drain_lock);
    if (result != ESP_OK)
    {
        return result;
    }

    /* The barrier closed new admission. Retain the currently accepted ACL,
     * if any, so the fixed-point drain cannot declare completion before its
     * exact terminal callback. */
    ble_gap_manager_snapshot_t snapshot;

    if (_ble_nimble_port_gap_snapshot(&snapshot) == ESP_OK &&
            snapshot.connected)
    {
        const ble_link_operation_identity_t identity =
        {
            .generation = snapshot.generation,
            .security_epoch = ble_link_session_security2_epoch(),
            .kind = BLE_LINK_OPERATION_TERMINATE,
            .conn_handle = snapshot.conn_handle,
        };

        if (!_ble_nimble_port_retain_terminate(&identity))
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

bool ble_nimble_port_cleanup_pending(void)
{
    if (s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    const bool pending = ble_link_cleanup_pending(&s_cleanup_obligations) ||
                         s_cleanup_obligations.terminal_fence_active ||
                         s_terminate_obligation.pending ||
                         s_rejected_terminate_obligation.pending ||
                         s_rejected_terminate_exhausted;

    xSemaphoreGiveRecursive(s_link_state_lock);
    return pending;
}

esp_err_t ble_nimble_port_set_pairing_window(bool open)
{
    const esp_err_t stop_result = _ble_nimble_port_wait_for_adv_stopped(
                                      BLE_ADV_MANAGER_PAUSE_REASON_WINDOW_TRANSITION);

    if (stop_result != ESP_OK)
    {
        return stop_result;
    }
    return _ble_nimble_port_set_pairing_gate(open);
}

static esp_err_t _ble_nimble_port_reset_peer_store(void)
{
    if (!s_port.started || s_port.quiescing || !ble_hs_synced())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t storage_error = _ble_nimble_port_storage_error_load();

    if (storage_error != ESP_OK)
    {
        return storage_error;
    }
    /* Factory-reset-gated startup journals the revoke before host start.
     * on_sync performs and verifies the bond/CCCD sweep before signaling
     * start completion; the marker may disappear only after that sweep. */
    bool pending = true;
    const esp_err_t result = device_link_security_revoke_pending(&pending);

    if (result != ESP_OK)
    {
        return result;
    }
    return pending ? ESP_ERR_INVALID_STATE : ESP_OK;
}

/**
 * @brief Resume an interrupted local revoke before advertising.
 *
 * Runs at host sync when the revoke-intent journal is still present: the
 * authorization is erased if it survived, the bond is deleted, and the
 * journal is cleared. Idempotent; a failure keeps the journal so the
 * next boot retries (fail closed, no advertising).
 */
static esp_err_t _ble_nimble_port_resume_revoke(void)
{
    bool pending = false;
    const esp_err_t pending_result =
        device_link_security_revoke_pending(&pending);

    if (pending_result != ESP_OK)
    {
        return pending_result;
    }
    if (!pending)
    {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "resuming interrupted binding revoke");
    const esp_err_t erase_result =
        device_link_security_erase_auth_record();

    if (erase_result != ESP_OK && erase_result != ESP_ERR_NOT_FOUND)
    {
        return erase_result;
    }
    const esp_err_t verifier_result =
        device_link_security_load_long_term_verifier();

    if (verifier_result != ESP_OK && verifier_result != ESP_ERR_NOT_FOUND)
    {
        return verifier_result;
    }
    return _ble_nimble_port_execute_revoke();
}

static esp_err_t _ble_nimble_port_register_remote_replacement(
    uint16_t conn_handle)
{
    ble_gap_manager_snapshot_t snapshot;

    if (_ble_nimble_port_gap_snapshot(&snapshot) != ESP_OK ||
            !snapshot.connected || snapshot.generation == 0U ||
            snapshot.conn_handle != conn_handle ||
            s_link_state_lock == NULL ||
            xSemaphoreTakeRecursive(s_link_state_lock,
                                    portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t token = _ble_nimble_port_next_operation_token_locked();

    xSemaphoreGiveRecursive(s_link_state_lock);
    if (token == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const ble_link_operation_identity_t identity =
    {
        .generation = snapshot.generation,
        .security_epoch = ble_link_session_security2_epoch(),
        .token = token,
        .kind = BLE_LINK_OPERATION_REMOTE_REPLACEMENT,
        .conn_handle = snapshot.conn_handle,
    };

    return ble_link_service_register_remote_replacement(&identity);
}

static int _ble_nimble_port_store_status(
    struct ble_store_status_event *event, void *arg)
{
    (void)arg;
    if (event == NULL || (event->event_code != BLE_STORE_EVENT_FULL &&
                          event->event_code != BLE_STORE_EVENT_OVERFLOW))
    {
        return -1;
    }
    const int object_type = event->event_code == BLE_STORE_EVENT_OVERFLOW ?
                            event->overflow.obj_type : event->full.obj_type;

    if (!_ble_nimble_port_store_object_is_bond(object_type))
    {
        return -1;
    }
    if (event->event_code == BLE_STORE_EVENT_FULL)
    {
        /* FULL is only a capacity warning before the proposed key has been
         * validated. Never mutate the existing authorization or bond here. */
        return _ble_nimble_port_pairing_window_open() ? 0 : -1;
    }
    if (event->event_code != BLE_STORE_EVENT_OVERFLOW ||
            !_ble_nimble_port_pairing_window_open())
    {
        return -1;
    }
    /* The host callback only registers immutable work. This write fails and
     * pairing restarts on a fresh ACL after the owner invalidates and evicts. */
    ble_gap_manager_snapshot_t snapshot;
    const esp_err_t register_result =
        (_ble_nimble_port_gap_snapshot(&snapshot) == ESP_OK &&
         snapshot.connected) ?
        _ble_nimble_port_register_remote_replacement(
            snapshot.conn_handle) : ESP_ERR_INVALID_STATE;

    if (register_result != ESP_OK)
    {
        ESP_LOGW(TAG, "store overflow replacement rejected (%d)",
                 register_result);
    }
    return -1;
}

static bool _ble_nimble_port_store_object_is_bond(int object_type)
{
    switch (object_type)
    {
    case BLE_STORE_OBJ_TYPE_OUR_SEC:
    case BLE_STORE_OBJ_TYPE_PEER_SEC:
    case BLE_STORE_OBJ_TYPE_PEER_ADDR:
        return true;
    default:
        return false;
    }
}

static int _ble_nimble_port_gap_event(
    struct ble_gap_event *event, void *arg)
{
    (void)arg;
    ble_port_event_t port_event;

    memset(&port_event, 0, sizeof(port_event));
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        port_event.type = BLE_PORT_EVENT_CONNECT;
        port_event.conn_handle = event->connect.conn_handle;
        port_event.status = event->connect.status;
        if (event->connect.status == 0)
        {
            /* Admission runs before any per-connection state is touched: a
             * rejected ACL must never reset the security reducer or the
             * connection identity of the active connection. */
            ble_gap_manager_event_t manager_event;

            memset(&manager_event, 0, sizeof(manager_event));
            manager_event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
            manager_event.conn_handle = event->connect.conn_handle;
            manager_event.status = 0;
            const esp_err_t admit_result =
                _ble_nimble_port_gap_handle_event(&manager_event);

            port_event.accepted = admit_result == ESP_OK;
            if (!port_event.accepted)
            {
                _ble_nimble_port_dispatch(&port_event);
                ESP_LOGW(TAG, "connection admission rejected handle=%u "
                              "result=%d", event->connect.conn_handle,
                         admit_result);
                if (!_ble_nimble_port_retain_rejected_terminate(
                            event->connect.conn_handle))
                {
                    /* The callback is already on the host queue, so retain
                     * failure still gets one exact immediate attempt. Token
                     * exhaustion remains fail-closed through the pause and
                     * admission fence. */
                    const int terminate_result = ble_gap_terminate(
                                                     event->connect.conn_handle,
                                                     BLE_ERR_CONN_TERM_LOCAL);

                    if (terminate_result != 0 &&
                            terminate_result != BLE_HS_EALREADY &&
                            terminate_result != BLE_HS_ENOTCONN)
                    {
                        LOG_W("rejected connection terminate retain failed "
                              "result=%d", terminate_result);
                    }
                }
                return 0;
            }
            (void)_ble_nimble_port_capture_connection_identity(
                &port_event, BLE_LINK_OPERATION_CONNECT);
            struct ble_gap_conn_desc desc;

            ble_hs_cfg.sm_sc_only = 1;
            ble_link_sec_state_reset(&s_link_sec_state);
            s_link_sec_conn = event->connect.conn_handle;
            /* The controller supplies peer_id_addr when a stored IRK
             * resolves the OTA address. Only an unresolved RPA defers the
             * admission decision until Identity Information arrives. */
            bool identity_ready = false;
            bool conn_bonded = false;
            bool bond_verified = false;
            bool had_bond = false;
            bool found = false;
            uint32_t replay_actions = BLE_LINK_SEC_ACTION_NONE;

            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0)
            {
                found = true;
                /* Controller privacy reports the normalized identity in
                 * peer_id_addr during a restored bond; peer_ota_addr may
                 * remain the current RPA for the lifetime of the ACL. */
                identity_ready = !BLE_ADDR_IS_RPA(&desc.peer_id_addr);
                conn_bonded = desc.sec_state.bonded;
                bond_verified = _ble_nimble_port_bond_store_verified(&desc);
                had_bond = conn_bonded ||
                           (identity_ready &&
                            _ble_nimble_port_peer_has_bond(&desc));
            }
            LOG_I("connect security handle=%u found=%u ota_rpa=%u id_rpa=%u "
                  "identity=%u bonded=%u verified=%u had_bond=%u window=%u",
                  event->connect.conn_handle, found,
                  found && BLE_ADDR_IS_RPA(&desc.peer_ota_addr),
                  found && BLE_ADDR_IS_RPA(&desc.peer_id_addr),
                  identity_ready, conn_bonded, bond_verified, had_bond,
                  _ble_nimble_port_pairing_window_open());
            (void)ble_link_sec_state_on_connect(
                &s_link_sec_state,
                _ble_nimble_port_pairing_window_open(),
                identity_ready,
                had_bond,
                conn_bonded, bond_verified);
            if (found && desc.sec_state.encrypted)
            {
                /* ENC_CHANGE can precede this delayed CONNECT callback.
                 * Replay the descriptor's current encryption facts so the
                 * reducer cannot miss a restored-bond admission. */
                replay_actions = ble_link_sec_state_on_encrypted(
                                     &s_link_sec_state, true,
                                     desc.sec_state.bonded,
                                     bond_verified);
            }
            /* Establish the accepted ACL generation before replaying security
             * facts from the descriptor. Applying them first makes the session
             * reducer correctly reject them as pre-CONNECT stale events. */
            _ble_nimble_port_dispatch(&port_event);
            _ble_nimble_port_apply_sec_actions(
                replay_actions, event->connect.conn_handle);
            ble_link_gatt_request_link_state_refresh();
            ble_gap_manager_snapshot_t snapshot;

            if (_ble_nimble_port_gap_snapshot(&snapshot) == ESP_OK)
            {
                _ble_nimble_port_track_provisional_connection(
                    snapshot.generation, event->connect.conn_handle,
                    had_bond, identity_ready,
                    found ? &desc.peer_id_addr : NULL);
            }
            return 0;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
    {
        /* A rejected ACL never received a connection generation. Its raw
         * terminal callback is therefore retired by the independent
         * boot-token state before normal accepted-ACL capture runs. */
        _ble_nimble_port_retire_rejected_terminate(
            event->disconnect.conn.conn_handle);
        port_event.type = BLE_PORT_EVENT_DISCONNECT;
        port_event.conn_handle = event->disconnect.conn.conn_handle;
        port_event.reason = event->disconnect.reason;
        const bool disconnect_current =
            _ble_nimble_port_capture_connection_identity(
                &port_event, BLE_LINK_OPERATION_DISCONNECT);

        if (disconnect_current &&
                s_link_sec_conn == event->disconnect.conn.conn_handle)
        {
            /* The provisional-bond cleanup is NOT queued here: it must be
             * linearized with the durable Commit boundary, so the link
             * service performs it under its mutex (abort/clear paths in the
             * DISCONNECT consumer below). An early enqueue here could
             * delete the bond before a Commit that is still in flight
             * persisted the record. */
            ble_hs_cfg.sm_sc_only = 1;
            ble_link_sec_state_on_disconnect(&s_link_sec_state);
            s_link_sec_conn = 0U;
        }
        break;
    }
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.channel_id != BLE_L2CAP_CID_ATT)
        {
            return 0;
        }
        port_event.type = BLE_PORT_EVENT_MTU;
        port_event.conn_handle = event->mtu.conn_handle;
        port_event.mtu = event->mtu.value;
        (void)_ble_nimble_port_capture_connection_identity(
            &port_event, BLE_LINK_OPERATION_MTU);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        port_event.type = BLE_PORT_EVENT_ENC_CHANGE;
        port_event.conn_handle = event->enc_change.conn_handle;
        if (!_ble_nimble_port_capture_connection_identity(
                    &port_event, BLE_LINK_OPERATION_ENCRYPT_CHANGE))
        {
            return 0;
        }
        {
            struct ble_gap_conn_desc description;
            const bool found = event->enc_change.status == 0 &&
                               ble_gap_conn_find(
                                   event->enc_change.conn_handle,
                                   &description) == 0;
            const bool bond_verified = found &&
                                       _ble_nimble_port_bond_store_verified(
                                           &description);

            port_event.encrypted = found && description.sec_state.encrypted;
            /* SC-only enforces SC and a 16-byte key during SMP, but NimBLE
             * also upgrades every secured ATT attribute to authenticated
             * Level 4. With one connection, clear it only for an encrypted,
             * verified SC bond; reconnect and teardown restore it. */
            ble_hs_cfg.sm_sc_only =
                port_event.encrypted && bond_verified ? 0 : 1;
        }
        if (s_link_sec_conn != event->enc_change.conn_handle)
        {
            break;
        }
        /* Feed the admission reducer with the current bond facts and
         * apply its actions. The identity may not be resolved yet; the
         * reducer holds the decision until it is. */
        {
            struct ble_gap_conn_desc desc;

            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0)
            {
                const uint32_t actions = ble_link_sec_state_on_encrypted(
                                             &s_link_sec_state,
                                             desc.sec_state.encrypted,
                                             desc.sec_state.bonded,
                                             _ble_nimble_port_bond_store_verified(
                                                 &desc));

                _ble_nimble_port_apply_sec_actions(
                    actions, event->enc_change.conn_handle);
                LOG_I("encryption security handle=%u status=%d encrypted=%u "
                      "bonded=%u verified=%u actions=0x%lx",
                      event->enc_change.conn_handle, event->enc_change.status,
                      desc.sec_state.encrypted, desc.sec_state.bonded,
                      _ble_nimble_port_bond_store_verified(&desc),
                      (unsigned long)actions);
            }
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        port_event.type = BLE_PORT_EVENT_SUBSCRIBE;
        port_event.conn_handle = event->subscribe.conn_handle;
        port_event.attr_handle = event->subscribe.attr_handle;
        port_event.subscribed = event->subscribe.cur_notify ||
                                event->subscribe.cur_indicate;
        port_event.notify = event->subscribe.cur_notify;
        port_event.indicate = event->subscribe.cur_indicate;
        (void)_ble_nimble_port_capture_connection_identity(
            &port_event, BLE_LINK_OPERATION_SUBSCRIBE);
        break;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        /* The peer RPA resolved to a stored identity: the identity
         * address is now known and the admission decision can run. */
        if (s_link_sec_conn != event->identity_resolved.conn_handle)
        {
            return 0;
        }
        {
            struct ble_gap_conn_desc desc;

            if (ble_gap_conn_find(event->identity_resolved.conn_handle,
                                  &desc) != 0)
            {
                return 0;
            }
            ble_link_gatt_set_connection(
                s_timer_generation, event->identity_resolved.conn_handle,
                desc.peer_id_addr.type, desc.peer_id_addr.val);
            _ble_nimble_port_update_provisional_identity(
                s_timer_generation, event->identity_resolved.conn_handle,
                &desc.peer_id_addr);
            const bool bond_verified =
                _ble_nimble_port_bond_store_verified(&desc);

            if (desc.sec_state.encrypted && bond_verified)
            {
                ble_hs_cfg.sm_sc_only = 0;
            }
            /* The had_bond fact is frozen at CONNECT: identity resolution
             * may run after THIS connection's pairing persisted keys, so
             * the store must not be re-queried here (a fresh pairing would
             * otherwise look like a pre-existing bond and bypass a closed
             * pairing window). */
            const uint32_t actions = ble_link_sec_state_on_identity(
                                         &s_link_sec_state,
                                         desc.sec_state.bonded,
                                         bond_verified);

            _ble_nimble_port_apply_sec_actions(
                actions, event->identity_resolved.conn_handle);
            LOG_I("identity resolved security handle=%u encrypted=%u bonded=%u "
                  "verified=%u actions=0x%lx",
                  event->identity_resolved.conn_handle,
                  desc.sec_state.encrypted, desc.sec_state.bonded,
                  bond_verified, (unsigned long)actions);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* An existing bond attempts to pair again: allowed only while a
         * replacement window is open. The old authorization must be
         * invalidated before the old bond is deleted (replacement
         * ordering: invalidate first, delete second, so a crash leaves
         * the device unbound but never dual-authorized). */
        if (!event->repeat_pairing.new_sc ||
                !event->repeat_pairing.new_bonding ||
                event->repeat_pairing.new_key_size != BLE_SM_PAIR_KEY_SZ_MAX)
        {
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        if (_ble_nimble_port_pairing_window_open())
        {
            const esp_err_t register_result =
                _ble_nimble_port_register_remote_replacement(
                    event->repeat_pairing.conn_handle);

            if (register_result != ESP_OK)
            {
                ESP_LOGW(TAG, "repeat pairing replacement rejected (%d)",
                         register_result);
            }
            ble_hs_cfg.sm_sc_only = 1;
            /* The public unpair API removes the controller resolving-list
             * entry but also terminates the ACL. Pairing must therefore be
             * retried after the client reconnects. */
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        port_event.type = BLE_PORT_EVENT_ADV_COMPLETE;
        port_event.status = event->adv_complete.reason;
        port_event.host_reset_pending = !ble_hs_synced();
        if (port_event.host_reset_pending)
        {
            /* ble_hs_reset() emits ADV_COMPLETE before reset_cb. Fence the
             * independent ADV task now, before it can execute a command queued
             * by the retiring host generation. */
            atomic_store_explicit(&s_adv_host_ready, false,
                                  memory_order_release);
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        port_event.type = BLE_PORT_EVENT_NOTIFY_TX;
        port_event.conn_handle = event->notify_tx.conn_handle;
        port_event.attr_handle = event->notify_tx.attr_handle;
        port_event.status = event->notify_tx.status;
        port_event.indication = event->notify_tx.indication != 0;
        (void)_ble_nimble_port_tx_tracker_translate(
            event->notify_tx.conn_handle, event->notify_tx.attr_handle,
            port_event.indication, event->notify_tx.status != 0,
            &port_event.identity);
        if (event->notify_tx.status == 0)
        {
            port_event.tx_result = BLE_PORT_TX_SENT;
        }
        else if (event->notify_tx.status == BLE_HS_EDONE)
        {
            port_event.tx_result = BLE_PORT_TX_CONFIRMED;
        }
        else if (event->notify_tx.status == BLE_HS_ETIMEOUT)
        {
            port_event.tx_result = BLE_PORT_TX_TIMEOUT;
        }
        else
        {
            port_event.tx_result = BLE_PORT_TX_ERROR;
        }
        break;
    default:
        return 0;
    }
    _ble_nimble_port_dispatch(&port_event);
    return 0;
}

static void _ble_nimble_port_on_sync(void)
{
    if (ble_hs_synced())
    {
        ble_port_event_t event;

        ble_nimble_pairing_gate_request(&s_pairing_gate_state, false);
        ble_hs_cfg.sm_sec_lvl = 1;
        LOG_I("host synchronized");
        /* Reconcile the single bond against the authorization record
         * before advertising opens; a failure latches the port so the
         * runtime start fails closed. The latch keeps the FIRST error. */
        if (_ble_nimble_port_storage_error_load() == ESP_OK)
        {
            _ble_nimble_port_latch_storage_error(
                _ble_nimble_port_resume_revoke());
        }
        if (_ble_nimble_port_storage_error_load() == ESP_OK)
        {
            const esp_err_t reconcile =
                _ble_nimble_port_reconcile_storage();

            _ble_nimble_port_latch_storage_error(reconcile);
            if (reconcile != ESP_OK)
            {
                ESP_LOGE(TAG, "storage reconciliation failed (%d)",
                         reconcile);
            }
        }
        if (_ble_nimble_port_storage_error_load() != ESP_OK)
        {
            /* Do not publish SYNC after reconciliation failed. Advertising
             * remains stopped and the start caller receives the latched
             * storage error; a runtime reset stays fail-closed as well. */
            xSemaphoreGive(s_port.sync_semaphore);
            return;
        }
        /* Visibility opens only after reconciliation. A queued command also
         * validates its manager generation, so work retained across RESET
         * cannot become current merely because the host synchronized again. */
        atomic_store_explicit(&s_adv_host_ready, true, memory_order_release);
        xSemaphoreGive(s_port.sync_semaphore);
        memset(&event, 0, sizeof(event));
        event.type = BLE_PORT_EVENT_SYNC;
        _ble_nimble_port_dispatch(&event);
    }
}

static void _ble_nimble_port_on_reset(int reason)
{
    ble_port_event_t event;

    atomic_store_explicit(&s_adv_host_ready, false, memory_order_release);
    ble_hs_cfg.sm_sc_only = 1;
    ble_nimble_pairing_gate_request(&s_pairing_gate_state, false);
    ble_hs_cfg.sm_sec_lvl = 1;
    LOG_E("host reset reason=%d", reason);
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_RESET;
    event.status = reason;
    _ble_nimble_port_dispatch(&event);
    /* The matching RESET consumer first gives the service owner a chance to
     * retain a provisional cleanup from these facts. The helper owns the
     * copied payload after dispatch, so volatile connection tracking can now
     * be cleared. */
    _ble_nimble_port_clear_provisional_tracking();
}

static bool _ble_nimble_port_link_rx_channel(
    const ble_gatt_registry_characteristic_t *characteristic,
    ble_link_service_rx_channel_t *out_channel)
{
    static const uint8_t session_rx_uuid[16] =
    {
        0xa2, 0xf0, 0xcd, 0xfc, 0xe0, 0xe6, 0x5c, 0xb8,
        0xd8, 0x4d, 0x4c, 0xcb, 0x43, 0xe6, 0x01, 0x48,
    };
    static const uint8_t control_rx_uuid[16] =
    {
        0xc8, 0x13, 0x3d, 0x40, 0x3d, 0xfb, 0x0c, 0x8e,
        0x72, 0x47, 0x9d, 0x66, 0x62, 0x46, 0xa1, 0x81,
    };

    if (memcmp(characteristic->uuid, session_rx_uuid, 16U) == 0)
    {
        *out_channel = BLE_LINK_SERVICE_RX_SESSION;
        return true;
    }
    if (memcmp(characteristic->uuid, control_rx_uuid, 16U) == 0)
    {
        *out_channel = BLE_LINK_SERVICE_RX_CONTROL;
        return true;
    }
    return false;
}

static int _ble_nimble_port_access_bridge(
    uint16_t conn_handle, uint16_t attr_handle,
    struct ble_gatt_access_ctxt *context, void *arg)
{
    (void)arg;
    const ble_gatt_registry_characteristic_t *characteristic = NULL;
    uint8_t access_buffer[BLE_NIMBLE_PORT_ACCESS_BUFFER_BYTES];
    ble_gatt_registry_access_context_t port_context;
    uint16_t read_len = 0U;

    if (ble_gatt_registry_lookup_by_handle(attr_handle, &characteristic) !=
            ESP_OK)
    {
        return BLE_ATT_ERR_ATTR_NOT_FOUND;
    }
    ble_link_service_rx_channel_t channel = BLE_LINK_SERVICE_RX_SESSION;
    const bool link_rx = _ble_nimble_port_link_rx_channel(
                             characteristic, &channel);

    memset(&port_context, 0, sizeof(port_context));
    port_context.read_out = access_buffer;
    port_context.read_capacity = sizeof(access_buffer);
    port_context.read_len = &read_len;
    switch (context->op)
    {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        port_context.op = BLE_GATT_REGISTRY_OP_READ_CHR;
        port_context.offset = context->offset;
        break;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        port_context.op = BLE_GATT_REGISTRY_OP_WRITE_CHR;
        if (link_rx)
        {
            bool terminal_fenced = true;

            if (s_link_state_lock != NULL &&
                    xSemaphoreTakeRecursive(
                        s_link_state_lock, portMAX_DELAY) == pdTRUE)
            {
                terminal_fenced =
                    ble_link_cleanup_terminal_fence_matches(
                        &s_cleanup_obligations, s_timer_generation,
                        conn_handle);
                xSemaphoreGiveRecursive(s_link_state_lock);
            }
            if (terminal_fenced)
            {
                /* Preserve public link_state reads, but terminal cleanup
                 * owns every session/control write until this exact ACL is
                 * retired by DISCONNECT or RESET. */
                return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            }
        }
        if (context->om != NULL)
        {
            uint16_t write_len = 0U;

            if (ble_hs_mbuf_to_flat(context->om, access_buffer,
                                    sizeof(access_buffer),
                                    &write_len) != 0)
            {
                return BLE_ATT_ERR_UNLIKELY;
            }
            port_context.write_data = access_buffer;
            port_context.write_len = write_len;
        }
        break;
    case BLE_GATT_ACCESS_OP_READ_DSC:
        port_context.op = BLE_GATT_REGISTRY_OP_READ_DSC;
        break;
    case BLE_GATT_ACCESS_OP_WRITE_DSC:
        port_context.op = BLE_GATT_REGISTRY_OP_WRITE_DSC;
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
    int result;
    result = characteristic->access_cb(
                 conn_handle, attr_handle, &port_context,
                 characteristic->arg);
    if (result == 0 &&
            port_context.op == BLE_GATT_REGISTRY_OP_WRITE_CHR &&
            link_rx)
    {
        bool partial = false;
        uint32_t ingress_epoch = 0U;
        ble_link_reassembly_disposition_t disposition;

        if (ble_link_service_get_reassembly_state_ex(
                    channel, &partial, &ingress_epoch,
                    &disposition) == ESP_OK)
        {
            uint32_t generation = 0U;

            if (s_link_state_lock == NULL ||
                    xSemaphoreTakeRecursive(
                        s_link_state_lock, portMAX_DELAY) == pdTRUE)
            {
                generation = s_timer_generation;
                if (s_link_state_lock != NULL)
                {
                    xSemaphoreGiveRecursive(s_link_state_lock);
                }
            }
            if (generation != 0U &&
                    (!partial || disposition ==
                     BLE_LINK_REASSEMBLY_NEW_PARTIAL))
            {
                _ble_nimble_port_arm_reassembly_idle(
                    partial, generation, channel, ingress_epoch);
            }
        }
    }
    if (result == 0 &&
            port_context.op == BLE_GATT_REGISTRY_OP_READ_CHR &&
            read_len > 0U)
    {
        if (read_len > port_context.read_capacity)
        {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (os_mbuf_append(context->om, access_buffer, read_len) != 0)
        {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }
    return result;
}

static void _ble_nimble_port_gatts_register(
    struct ble_gatt_register_ctxt *context, void *arg)
{
    (void)arg;
    if (context->op == BLE_GATT_REGISTER_OP_CHR &&
            context->chr.chr_def->uuid->type == BLE_UUID_TYPE_128)
    {
        const uint8_t *uuid_flat =
            ((const ble_uuid128_t *)context->chr.chr_def->uuid)->value;
        const esp_err_t result = ble_gatt_registry_assign_handle(
                                     uuid_flat, context->chr.val_handle);

        if (result != ESP_OK)
        {
            LOG_W("handle assignment failed result=%d", result);
        }
    }
    ble_link_gatt_update_handles();
}

static esp_err_t _ble_nimble_port_register_database(void)
{
    static struct ble_gatt_svc_def services[
            BLE_GATT_REGISTRY_MAX_SERVICES + 1];
    static struct ble_gatt_chr_def characteristics[
            BLE_GATT_REGISTRY_MAX_CHARACTERISTICS +
            BLE_GATT_REGISTRY_MAX_SERVICES];
    static ble_uuid128_t service_uuids[BLE_GATT_REGISTRY_MAX_SERVICES];
    static ble_uuid128_t characteristic_uuids[
        BLE_GATT_REGISTRY_MAX_CHARACTERISTICS];
    size_t service_count = 0U;
    size_t characteristic_cursor = 0U;
    size_t definition_cursor = 0U;

    ble_gatt_registry_clear_handles();
    for (;;)
    {
        const ble_gatt_registry_service_t *service = NULL;

        if (ble_gatt_registry_get_service(service_count, &service) != ESP_OK)
        {
            break;
        }
        services[service_count].type = BLE_GATT_SVC_TYPE_PRIMARY;
        memcpy(service_uuids[service_count].value, service->uuid, 16U);
        service_uuids[service_count].u.type = BLE_UUID_TYPE_128;
        services[service_count].uuid = &service_uuids[service_count].u;

        for (size_t i = 0U; i < service->characteristic_count; ++i)
        {
            const ble_gatt_registry_characteristic_t *characteristic = NULL;

            if (ble_gatt_registry_get_characteristic(
                        characteristic_cursor + i, &characteristic) != ESP_OK)
            {
                return ESP_FAIL;
            }
            struct ble_gatt_chr_def *definition =
                    &characteristics[definition_cursor + i];

            memcpy(characteristic_uuids[characteristic_cursor + i].value,
                   characteristic->uuid, 16U);
            characteristic_uuids[characteristic_cursor + i].u.type =
                BLE_UUID_TYPE_128;
            definition->uuid =
                &characteristic_uuids[characteristic_cursor + i].u;
            definition->access_cb = _ble_nimble_port_access_bridge;
            definition->arg = NULL;
            definition->flags = 0;
            if (characteristic->properties & BLE_GATT_REGISTRY_PROP_READ)
            {
                definition->flags |= BLE_GATT_CHR_F_READ;
                if (ble_gatt_registry_admission_requires_encryption(
                            characteristic->read_admission))
                {
                    definition->flags |= BLE_GATT_CHR_F_READ_ENC;
                }
            }
            if (characteristic->properties & BLE_GATT_REGISTRY_PROP_WRITE)
            {
                definition->flags |= BLE_GATT_CHR_F_WRITE;
            }
            if (characteristic->properties &
                    BLE_GATT_REGISTRY_PROP_WRITE_NO_RESPONSE)
            {
                definition->flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
            }
            if ((characteristic->properties &
                    (BLE_GATT_REGISTRY_PROP_WRITE |
                     BLE_GATT_REGISTRY_PROP_WRITE_NO_RESPONSE)) != 0U &&
                    ble_gatt_registry_admission_requires_encryption(
                        characteristic->write_admission))
            {
                definition->flags |= BLE_GATT_CHR_F_WRITE_ENC;
            }
            if (characteristic->properties & BLE_GATT_REGISTRY_PROP_NOTIFY)
            {
                definition->flags |= BLE_GATT_CHR_F_NOTIFY;
            }
            if (characteristic->properties & BLE_GATT_REGISTRY_PROP_INDICATE)
            {
                definition->flags |= BLE_GATT_CHR_F_INDICATE;
            }
            if ((characteristic->properties &
                    (BLE_GATT_REGISTRY_PROP_NOTIFY |
                     BLE_GATT_REGISTRY_PROP_INDICATE)) != 0U &&
                    ble_gatt_registry_admission_requires_encryption(
                        characteristic->tx_admission))
            {
                definition->flags |= BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC;
            }
        }
        characteristics[definition_cursor +
                        service->characteristic_count].uuid = NULL;
        services[service_count].characteristics =
            &characteristics[definition_cursor];
        characteristic_cursor += service->characteristic_count;
        definition_cursor += service->characteristic_count + 1U;
        service_count++;
    }
    services[service_count].type = 0;
    services[service_count].uuid = NULL;
    services[service_count].characteristics = NULL;

    int result = ble_gatts_count_cfg(services);
    if (result != 0)
    {
        return ESP_FAIL;
    }
    result = ble_gatts_add_svcs(services);
    if (result != 0)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_production_adv_start(
    const ble_port_adv_config_t *config)
{
    ble_nimble_port_adv_cmd_t cmd;

    if (config == NULL || s_port.adv_queue == NULL ||
            s_port.adv_task == NULL || s_port.quiescing)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (_ble_nimble_port_storage_error_load() != ESP_OK)
    {
        /* A failed startup/reset reconciliation is terminal for this host
         * run. Do not reopen advertising with an unverified bond store. */
        return _ble_nimble_port_storage_error_load();
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = BLE_NIMBLE_PORT_ADV_CMD_START;
    cmd.config = *config;
    cmd.config.service_data = cmd.service_data;
    if (config->service_data != NULL && config->service_data_len > 0U)
    {
        const size_t copy = config->service_data_len <
                            sizeof(cmd.service_data) ?
                            config->service_data_len :
                            sizeof(cmd.service_data);

        memcpy(cmd.service_data, config->service_data, copy);
        cmd.config.service_data_len = copy;
    }
    if (xQueueSend(s_port.adv_queue, &cmd, 0U) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_port.adv_task);
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_production_adv_stop(uint32_t generation)
{
    ble_nimble_port_adv_cmd_t cmd;

    if (s_port.adv_queue == NULL || s_port.adv_task == NULL ||
            s_port.quiescing)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = BLE_NIMBLE_PORT_ADV_CMD_STOP;
    cmd.config.generation = generation;
    if (xQueueSend(s_port.adv_queue, &cmd, 0U) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_port.adv_task);
    return ESP_OK;
}

static int _ble_nimble_port_adv_start_execute(
    const ble_nimble_port_adv_cmd_t *cmd)
{
    const ble_port_adv_config_t *config = &cmd->config;
    uint8_t adv_data[31];
    size_t len = 0U;
    int result;

    adv_data[len++] = 2U;
    adv_data[len++] = BLE_HS_ADV_TYPE_FLAGS;
    adv_data[len++] = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    if (config->service_uuid != NULL)
    {
        adv_data[len++] = 1U + 16U + (uint8_t)config->service_data_len;
        adv_data[len++] = BLE_HS_ADV_TYPE_SVC_DATA_UUID128;
        memcpy(&adv_data[len], config->service_uuid, 16U);
        len += 16U;
        if (config->service_data_len > 0U)
        {
            const size_t copy = config->service_data_len <
                                sizeof(cmd->service_data) ?
                                config->service_data_len :
                                sizeof(cmd->service_data);

            memcpy(&adv_data[len], cmd->service_data, copy);
            len += copy;
        }
    }
    if (config->short_name != NULL && config->short_name_len > 0U)
    {
        adv_data[len++] = 1U + (uint8_t)config->short_name_len;
        adv_data[len++] = BLE_HS_ADV_TYPE_INCOMP_NAME;
        memcpy(&adv_data[len], config->short_name, config->short_name_len);
        len += config->short_name_len;
    }
    result = ble_gap_adv_set_data(adv_data, (int)len);
    if (result != 0)
    {
        LOG_E("adv data failed result=%d", result);
        return result;
    }
    struct ble_gap_adv_params params;

    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = (uint16_t)(config->interval_ms * 16U / 10U);
    params.itvl_max = params.itvl_min;
    result = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                               &params, NULL, NULL);
    if (result != 0)
    {
        LOG_E("adv start failed result=%d", result);
    }
    return result;
}

static bool _ble_nimble_port_adv_host_ready(void *arg)
{
    (void)arg;
    return atomic_load_explicit(&s_adv_host_ready, memory_order_acquire) &&
           ble_hs_synced();
}

static esp_err_t _ble_nimble_port_adv_set_pairing_gate(bool open, void *arg)
{
    (void)arg;
    return _ble_nimble_port_set_pairing_gate(open);
}

static int _ble_nimble_port_adv_start_guarded(void *arg)
{
    return _ble_nimble_port_adv_start_execute(
               (const ble_nimble_port_adv_cmd_t *)arg);
}

static int _ble_nimble_port_adv_stop_stale(void *arg)
{
    (void)arg;
    const int result = ble_gap_adv_stop();

    return result == BLE_HS_EALREADY ? 0 : result;
}

static void _ble_nimble_port_adv_result_event(
    ble_port_event_type_t type, int status,
    const ble_port_adv_config_t *config)
{
    ble_port_event_t port_event;

    memset(&port_event, 0, sizeof(port_event));
    port_event.type = type;
    port_event.status = status;
    if (config != NULL)
    {
        port_event.generation = config->generation;
    }
    const esp_err_t result = ble_adv_manager_handle_event(&port_event);

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    {
        LOG_E("adv manager result event failed result=%d", result);
    }
}

static void _ble_nimble_port_adv_task(void *param)
{
    (void)param;
    for (;;)
    {
        ble_nimble_port_adv_cmd_t cmd;
        while (xQueueReceive(s_port.adv_queue, &cmd, 0U) == pdTRUE)
        {
            if (cmd.type == BLE_NIMBLE_PORT_ADV_CMD_START)
            {
                cmd.config.service_data = cmd.service_data;
                const bool bindable = cmd.config.service_data_len >= 2U &&
                                      (cmd.service_data[1] & 0x01U) != 0U;
                const ble_nimble_adv_start_ops_t start_ops =
                {
                    .host_ready = _ble_nimble_port_adv_host_ready,
                    .set_pairing_gate =
                    _ble_nimble_port_adv_set_pairing_gate,
                    .start = _ble_nimble_port_adv_start_guarded,
                    .stop = _ble_nimble_port_adv_stop_stale,
                    .arg = &cmd,
                };
                const int result = ble_nimble_adv_start_execute(
                                       cmd.config.generation, bindable,
                                       &start_ops);

                _ble_nimble_port_adv_result_event(
                    BLE_PORT_EVENT_ADV_STARTED, result, &cmd.config);
            }
            else if (cmd.type == BLE_NIMBLE_PORT_ADV_CMD_STOP)
            {
                const bool command_current =
                    ble_adv_manager_stop_command_current(
                        cmd.config.generation);
                const bool host_ready = atomic_load_explicit(
                                            &s_adv_host_ready,
                                            memory_order_acquire) &&
                                        ble_hs_synced();

                if (!command_current || !host_ready)
                {
                    _ble_nimble_port_adv_result_event(
                        BLE_PORT_EVENT_ADV_STOPPED,
                        ESP_ERR_INVALID_STATE, &cmd.config);
                    continue;
                }
                const int result = ble_gap_adv_stop();
                int completion_result = result == BLE_HS_EALREADY ? 0 : result;

                if (result != 0 && result != BLE_HS_EALREADY)
                {
                    LOG_E("adv stop failed result=%d", result);
                }
                if (!ble_adv_manager_bindable_requested())
                {
                    const esp_err_t gate_result =
                        _ble_nimble_port_set_pairing_gate(false);

                    if (gate_result != ESP_OK)
                    {
                        LOG_E("pairing gate close failed result=%d",
                              gate_result);
                        /* Physical STOP and the closed SMP gate form one
                         * convergence boundary. Report the combined action
                         * as failed so the manager retains its STOP
                         * obligation and retries with backoff; otherwise a
                         * last-lease close could leave pairing admitted
                         * indefinitely with no later START to repair it. */
                        if (completion_result == 0)
                        {
                            completion_result = (int)gate_result;
                        }
                    }
                }
                _ble_nimble_port_adv_result_event(
                    BLE_PORT_EVENT_ADV_STOPPED,
                    completion_result, &cmd.config);
            }
            else if (cmd.type == BLE_NIMBLE_PORT_ADV_CMD_QUIT)
            {
                xSemaphoreGive(s_port.adv_exit_semaphore);
                vTaskDelete(NULL);
            }
        }
        uint32_t fast_remaining =
            ble_adv_manager_get_fast_window_remaining_ms();

        if (fast_remaining == 0U)
        {
            (void)ble_adv_manager_handle_fast_window_expired();
        }
        ble_adv_manager_poll();
        fast_remaining = ble_adv_manager_get_fast_window_remaining_ms();
        const uint32_t retry_remaining =
            ble_adv_manager_get_retry_remaining_ms();
        uint32_t wait_ms = fast_remaining;

        if (wait_ms == UINT32_MAX ||
                (retry_remaining != UINT32_MAX &&
                 retry_remaining < wait_ms))
        {
            wait_ms = retry_remaining;
        }
        TickType_t wait = portMAX_DELAY;

        if (wait_ms != UINT32_MAX)
        {
            wait = pdMS_TO_TICKS(wait_ms);
            if (wait == 0U && wait_ms > 0U)
            {
                wait = 1U;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, wait);
    }
}

static esp_err_t _ble_nimble_port_production_notify(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    int result;

    if (om == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    ble_link_operation_identity_t identity;

    if (ble_tx_scheduler_get_in_flight_identity(&identity) != ESP_OK ||
            identity.conn_handle != conn_handle ||
            _ble_nimble_port_tx_tracker_retain(
                &identity, value_handle, false) != ESP_OK)
    {
        os_mbuf_free_chain(om);
        return ESP_FAIL;
    }
    result = ble_gatts_notify_custom(conn_handle, value_handle, om);
    /* Notification completion is the synchronous host submit result. A
     * callback delivered inside the call already consumed the tracker entry;
     * a later callback must stay unmatched instead of targeting a newer send. */
    _ble_nimble_port_tx_tracker_remove_identity(&identity);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t _ble_nimble_port_production_indicate(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    int result;

    if (om == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    /* The 2000 ms confirmation window starts at submission, bound to the
     * scheduler's complete immutable in-flight identity. */
    ble_link_operation_identity_t identity;

    if (ble_tx_scheduler_get_in_flight_identity(&identity) != ESP_OK)
    {
        os_mbuf_free_chain(om);
        return ESP_FAIL;
    }
    if (identity.generation == 0U || identity.token == 0U ||
            identity.kind != BLE_LINK_OPERATION_TX_INDICATE ||
            identity.conn_handle != conn_handle ||
            _ble_nimble_port_tx_tracker_retain(
                &identity, value_handle, true) != ESP_OK ||
            !_ble_nimble_port_arm_indication_timeout(true, &identity))
    {
        /* The confirmation timer could not be armed: do not send an
         * indication that can never time out, and release the mbuf. */
        _ble_nimble_port_tx_tracker_remove_identity(&identity);
        os_mbuf_free_chain(om);
        return ESP_FAIL;
    }
    result = ble_gatts_indicate_custom(conn_handle, value_handle, om);
    if (result != 0)
    {
        (void)_ble_nimble_port_arm_indication_timeout(false, &identity);
        _ble_nimble_port_tx_tracker_remove_identity(&identity);
    }
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t _ble_nimble_port_quiesce_adv(void)
{
    if (s_port.adv_lock != NULL)
    {
        (void)xSemaphoreTake(s_port.adv_lock, portMAX_DELAY);
    }
    s_port.quiescing = true;
    if (s_port.adv_lock != NULL)
    {
        (void)xSemaphoreGive(s_port.adv_lock);
    }
    if (s_port.adv_task == NULL)
    {
        return ESP_OK;
    }
    ble_nimble_port_adv_cmd_t quit;

    memset(&quit, 0, sizeof(quit));
    quit.type = BLE_NIMBLE_PORT_ADV_CMD_QUIT;
    if (xQueueSend(s_port.adv_queue, &quit,
                   pdMS_TO_TICKS(BLE_NIMBLE_PORT_ADV_QUIT_TIMEOUT_MS)) !=
            pdTRUE)
    {
        s_port.deinit_failed = true;
        s_port.deinit_error = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }
    xTaskNotifyGive(s_port.adv_task);
    if (xSemaphoreTake(s_port.adv_exit_semaphore,
                       pdMS_TO_TICKS(BLE_NIMBLE_PORT_ADV_QUIT_TIMEOUT_MS)) !=
            pdTRUE)
    {
        s_port.deinit_failed = true;
        s_port.deinit_error = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }
    s_port.adv_task = NULL;
    return ESP_OK;
}

/**
 * @brief Quit the timer owner task and wait for its confirmed exit.
 *
 * The owner may be inside an NVS, GAP, or manager call when the quit is
 * queued; it must never be force-deleted (vTaskDelete from another task
 * while it holds an internal lock corrupts the lock or frees memory
 * underneath it). On timeout every resource is kept so the caller can
 * retry: the owner eventually processes the quit and self-deletes, after
 * which the exit semaphore holds the token.
 */
static esp_err_t _ble_nimble_port_owner_quit(uint32_t wait_ms)
{
    if (s_timer_owner_task == NULL)
    {
        return ESP_OK;
    }
    if (s_timer_command_queue == NULL)
    {
        s_timer_owner_task = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    const ble_nimble_port_timer_command_t quit =
    {
        .armed = false,
        .kind = BLE_NIMBLE_PORT_TIMER_KIND_QUIT,
    };

    if (!_ble_nimble_port_timer_enqueue_control(
                &quit, pdMS_TO_TICKS(BLE_NIMBLE_PORT_OWNER_QUIT_SEND_MS)))
    {
        return ESP_ERR_TIMEOUT;
    }
    if (s_timer_exit == NULL ||
            xSemaphoreTake(s_timer_exit, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    s_timer_owner_task = NULL;
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_rollback_init(
    esp_err_t original_error, bool listener_registered, bool with_queue,
    bool with_task)
{
    if (listener_registered)
    {
        (void)ble_gap_event_listener_unregister(&s_port.listener);
        s_port.listener_registered = false;
    }
    if (with_task && s_port.adv_task != NULL)
    {
        const esp_err_t quiesce = _ble_nimble_port_quiesce_adv();

        if (quiesce != ESP_OK)
        {
            return quiesce;
        }
    }
    if (with_queue && s_port.adv_queue != NULL)
    {
        vQueueDelete(s_port.adv_queue);
        s_port.adv_queue = NULL;
    }
    if (s_port.adv_exit_semaphore != NULL)
    {
        vSemaphoreDelete(s_port.adv_exit_semaphore);
        s_port.adv_exit_semaphore = NULL;
    }
    if (s_timer_owner_task != NULL)
    {
        const esp_err_t quit_result =
            _ble_nimble_port_owner_quit(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS);

        if (quit_result != ESP_OK)
        {
            /* The owner is still alive and may hold the queue, lock, or
             * exit semaphore: keep them all and let a later init retry the
             * quit instead of tearing resources out from under the task. */
            return quit_result;
        }
    }
    _ble_nimble_port_timer_teardown();
    /* The command queue is statically allocated and stays for the module
     * lifetime; the lock and exit semaphore are recycled. */
    if (s_link_state_lock != NULL)
    {
        vSemaphoreDelete(s_link_state_lock);
        s_link_state_lock = NULL;
    }
    if (s_storage_lock != NULL)
    {
        vSemaphoreDelete(s_storage_lock);
        s_storage_lock = NULL;
    }
    if (s_timer_exit != NULL)
    {
        vSemaphoreDelete(s_timer_exit);
        s_timer_exit = NULL;
    }
    const esp_err_t cleanup = s_port.nimble_init_attempted
                              ? nimble_port_deinit()
                              : ESP_OK;

    s_port.nimble_init_attempted = false;
    s_port.deinitialized = true;
    if (cleanup != ESP_OK)
    {
        s_port.deinit_failed = true;
        s_port.deinit_error = cleanup;
        return cleanup;
    }
    return original_error;
}

static esp_err_t _ble_nimble_port_link_security_init(void)
{
    if (s_port.link_security_initialized)
    {
        return ESP_OK;
    }
    const device_link_security_config_t security_config =
    {
        .username = DEVICE_LINK_SECURITY_USERNAME,
        .session_id = 1U,
        .request_cb = _ble_nimble_port_security_request,
        .request_arg = NULL,
        .authenticated_cb = _ble_nimble_port_sec_authenticated,
        .authenticated_arg = NULL,
    };
    esp_err_t result = device_link_security_init(&security_config);

    if (result != ESP_OK)
    {
        return result;
    }
    s_port.link_security_initialized = true;
    result = device_link_security_load_long_term_verifier();
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND)
    {
        device_link_security_deinit();
        s_port.link_security_initialized = false;
        return result;
    }
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_init(void)
{
    esp_err_t result;

    if (s_port.deinit_failed)
    {
        return s_port.deinit_error;
    }
    s_port.deinitialized = false;
    s_port.nimble_init_attempted = false;
    s_port.quiescing = false;
    atomic_store_explicit(&s_adv_host_ready, false, memory_order_release);
    ble_nimble_store_guard_reset(&s_port.storage_guard);
    s_adv_conn_handle = 0U;
    s_adv_generation = 0U;
    _ble_nimble_port_clear_provisional_tracking();
    result = ble_event_router_register(_ble_nimble_port_gap_consumer, NULL);
    if (result != ESP_OK)
    {
        return _ble_nimble_port_rollback_init(result, false, false, false);
    }
    result = ble_event_router_register(_ble_nimble_port_adv_consumer, NULL);
    if (result != ESP_OK)
    {
        return _ble_nimble_port_rollback_init(result, false, false, false);
    }
    result = ble_event_router_register(_ble_nimble_port_tx_consumer, NULL);
    if (result != ESP_OK)
    {
        return _ble_nimble_port_rollback_init(result, false, false, false);
    }
    if (s_timer_command_queue == NULL)
    {
        /* Statically allocated for retained security-control events. Timer
         * callbacks use the independently fenced task notification. */
        s_timer_command_queue = xQueueCreateStatic(
                                    16U, sizeof(ble_nimble_port_timer_command_t),
                                    (uint8_t *)s_timer_queue_items, &s_timer_queue_storage);
        if (s_timer_command_queue == NULL)
        {
            return _ble_nimble_port_rollback_init(
                       ESP_ERR_NO_MEM, false, false, false);
        }
    }
    if (s_timer_exit == NULL)
    {
        s_timer_exit = xSemaphoreCreateBinary();
        if (s_timer_exit == NULL)
        {
            return _ble_nimble_port_rollback_init(
                       ESP_ERR_NO_MEM, false, false, false);
        }
    }
    if (s_link_state_lock == NULL)
    {
        /* Recursive: the bridge holds the lock while feeding, and NimBLE
         * may deliver NOTIFY_TX synchronously inside the same ops call, so
         * the TX consumer and the rearm path re-enter the same task. */
        s_link_state_lock = xSemaphoreCreateRecursiveMutex();
        if (s_link_state_lock == NULL)
        {
            return _ble_nimble_port_rollback_init(
                       ESP_ERR_NO_MEM, false, false, false);
        }
    }
    ble_nimble_tx_tracker_init(
        &s_tx_tracker, s_tx_tracker_entries,
        BLE_NIMBLE_PORT_TX_TRACKER_CAPACITY);
    if (s_storage_lock == NULL)
    {
        s_storage_lock = xSemaphoreCreateMutex();
        if (s_storage_lock == NULL)
        {
            return _ble_nimble_port_rollback_init(
                       ESP_ERR_NO_MEM, false, false, false);
        }
    }
    if (s_timer_owner_task == NULL)
    {
        if (xTaskCreatePinnedToCore(
                    _ble_nimble_port_timer_owner,
                    "ble_link_timer", BLE_NIMBLE_PORT_LINK_TIMER_STACK,
                    NULL, BLE_NIMBLE_PORT_LINK_TIMER_PRIORITY,
                    &s_timer_owner_task,
                    BLE_NIMBLE_PORT_HOST_CORE) != pdPASS)
        {
            return _ble_nimble_port_rollback_init(
                       ESP_ERR_NO_MEM, false, false, false);
        }
    }
    atomic_store_explicit(&s_timer_wake_task,
                          (uintptr_t)s_timer_owner_task,
                          memory_order_release);
    result = ble_event_router_register(
                 _ble_nimble_port_link_gatt_consumer, NULL);
    if (result != ESP_OK)
    {
        return _ble_nimble_port_rollback_init(result, false, false, false);
    }
    if (s_port.ops == NULL)
    {
        s_port.ops = &s_production_ops;
    }
    ble_gap_manager_init();
    ble_gap_manager_set_admission_cb(
        _ble_nimble_port_cleanup_admission, NULL);
    result = _ble_nimble_port_adv_manager_init();
    if (result != ESP_OK)
    {
        return _ble_nimble_port_rollback_init(result, false, false, false);
    }
    result = _ble_nimble_port_tx_manager_init();
    if (result != ESP_OK)
    {
        return _ble_nimble_port_rollback_init(result, false, false, false);
    }
    result = _ble_nimble_port_link_security_init();
    if (result != ESP_OK)
    {
        return _ble_nimble_port_rollback_init(result, false, false, false);
    }
    if (!s_port.link_gatt_initialized)
    {
        static ble_link_gatt_config_t gatt_config;

        memset(&gatt_config, 0, sizeof(gatt_config));
        gatt_config.boot_id = esp_random();
        if (gatt_config.boot_id == 0U)
        {
            gatt_config.boot_id = 1U;
        }
        gatt_config.att_mtu = 23U;
        gatt_config.tx_queue_depth = CONFIG_BLE_RUNTIME_TX_QUEUE_DEPTH;
        gatt_config.publish_link_state = _ble_nimble_port_publish_link_state;
        gatt_config.security_ops = &s_security_ops;
        result = ble_link_gatt_init(&gatt_config);
        if (result != ESP_OK)
        {
            return _ble_nimble_port_rollback_init(
                       result, false, false, false);
        }
        if (!ble_gatt_registry_is_sealed())
        {
            const esp_err_t seal_result = ble_gatt_registry_seal();

            if (seal_result != ESP_OK)
            {
                return _ble_nimble_port_rollback_init(
                           seal_result, false, false, false);
            }
        }
        s_port.link_gatt_initialized = true;
    }
    else
    {
        /* A runtime restart re-initializes the transport service (the
         * deinit cleared its per-connection state and dispatcher) and
         * clears the connection facts so the fresh GAP generation is
         * accepted. The boot id, epoch allocator, and registry services
         * are preserved. */
        result = ble_link_gatt_restart();
        if (result != ESP_OK)
        {
            return _ble_nimble_port_rollback_init(
                       result, false, false, false);
        }
    }
    result = nimble_port_init();
    if (result != ESP_OK)
    {
        /* ESP-IDF's nimble_port_init() already rolled back the controller
         * and host on failure, so nimble_port_deinit() must not run again:
         * the flag stays false and only the project-owned resources are
         * cleaned by the rollback. */
        return _ble_nimble_port_rollback_init(result, false, false, false);
    }
    s_port.nimble_init_attempted = true;
    if (s_pairing_gate_ack == NULL)
    {
        s_pairing_gate_ack = xSemaphoreCreateBinaryStatic(
                                 &s_pairing_gate_ack_control);
    }
    if (s_pairing_gate_lock == NULL)
    {
        s_pairing_gate_lock = xSemaphoreCreateMutexStatic(
                                  &s_pairing_gate_lock_control);
    }
    if (s_cleanup_drain_ack == NULL)
    {
        s_cleanup_drain_ack = xSemaphoreCreateBinaryStatic(
                                  &s_cleanup_drain_ack_control);
    }
    if (s_cleanup_drain_lock == NULL)
    {
        s_cleanup_drain_lock = xSemaphoreCreateMutexStatic(
                                   &s_cleanup_drain_lock_control);
    }
    if (s_pairing_gate_ack == NULL || s_pairing_gate_lock == NULL ||
            s_cleanup_drain_ack == NULL || s_cleanup_drain_lock == NULL)
    {
        return _ble_nimble_port_rollback_init(
                   ESP_ERR_NO_MEM, true, false, false);
    }
    atomic_store_explicit(&s_pairing_gate_requested_seq, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_pairing_gate_applied_seq, 0U,
                          memory_order_release);
    ble_nimble_pairing_gate_reset(&s_pairing_gate_state);
    atomic_store_explicit(&s_pairing_gate_event_queued, false,
                          memory_order_release);
    atomic_store_explicit(&s_terminate_event_queued, false,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_draining, false,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_drain_event_queued, false,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_drain_requested_seq, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_drain_applied_seq, 0U,
                          memory_order_release);
    ble_npl_event_init(&s_pairing_gate_event,
                       _ble_nimble_port_pairing_gate_event, NULL);
    ble_npl_event_init(&s_terminate_event,
                       _ble_nimble_port_terminate_event, NULL);
    ble_npl_event_init(&s_cleanup_drain_event,
                       _ble_nimble_port_cleanup_drain_event, NULL);
    ble_hs_cfg.reset_cb = _ble_nimble_port_on_reset;
    ble_hs_cfg.sync_cb = _ble_nimble_port_on_sync;
    ble_hs_cfg.gatts_register_cb = _ble_nimble_port_gatts_register;
    ble_hs_cfg.store_status_cb = _ble_nimble_port_store_status;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_sec_lvl = 1;
    /* Identity keys let the GAP reducer verify the peer's SC bond against
     * the NimBLE store before admitting the Device Link session. */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                 BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID;
    if (!s_port.listener_registered)
    {
        const int register_result = ble_gap_event_listener_register(
                                        &s_port.listener,
                                        _ble_nimble_port_gap_event, NULL);

        if (register_result != 0 && register_result != BLE_HS_EALREADY)
        {
            return _ble_nimble_port_rollback_init(
                       register_result == BLE_HS_ENOMEM ? ESP_ERR_NO_MEM
                       : ESP_FAIL,
                       false, false, false);
        }
        s_port.listener_registered = true;
    }
    if (s_port.adv_queue == NULL)
    {
        s_port.adv_queue = xQueueCreate(BLE_NIMBLE_PORT_ADV_QUEUE_DEPTH,
                                        sizeof(ble_nimble_port_adv_cmd_t));
        if (s_port.adv_queue == NULL)
        {
            return _ble_nimble_port_rollback_init(ESP_ERR_NO_MEM, true, false,
                                                  false);
        }
    }
    if (s_port.adv_exit_semaphore == NULL)
    {
        s_port.adv_exit_semaphore = xSemaphoreCreateBinary();
        if (s_port.adv_exit_semaphore == NULL)
        {
            return _ble_nimble_port_rollback_init(ESP_ERR_NO_MEM, true, true,
                                                  false);
        }
    }
    if (s_port.adv_task == NULL)
    {
        if (xTaskCreatePinnedToCore(
                    _ble_nimble_port_adv_task, "ble_adv_ctrl",
                    BLE_NIMBLE_PORT_ADV_TASK_STACK, NULL,
                    BLE_NIMBLE_PORT_ADV_TASK_PRIORITY,
                    &s_port.adv_task,
                    BLE_NIMBLE_PORT_HOST_CORE) != pdPASS)
        {
            return _ble_nimble_port_rollback_init(ESP_ERR_NO_MEM, true, true,
                                                  false);
        }
    }

    result = _ble_nimble_port_register_database();
    if (result != ESP_OK)
    {
        const esp_err_t cleanup = _ble_nimble_port_rollback_init(
                                      result, true, true, true);

        if (cleanup != ESP_OK && cleanup != result)
        {
            LOG_E("database registration cleanup failed result=%d", cleanup);
        }
        return cleanup;
    }
    return ESP_OK;
}

static void _ble_nimble_port_host_task(void *param)
{
    (void)param;
    atomic_store_explicit(&s_nimble_host_task,
                          (uintptr_t)xTaskGetCurrentTaskHandle(),
                          memory_order_release);
    nimble_port_run();
    atomic_store_explicit(&s_nimble_host_task, 0U, memory_order_release);
    xSemaphoreGive(s_port.exit_semaphore);
    vTaskDelete(NULL);
}

static void _ble_nimble_port_drain_sync(void)
{
    while (xSemaphoreTake(s_port.sync_semaphore, 0U) == pdTRUE)
    {
        /* Discard stale sync tokens from a previous run. */
    }
}

static esp_err_t _ble_nimble_port_start(void)
{
    if (s_port.sync_semaphore == NULL)
    {
        s_port.sync_semaphore = xSemaphoreCreateBinary();
    }
    if (s_port.exit_semaphore == NULL)
    {
        s_port.exit_semaphore = xSemaphoreCreateBinary();
    }
    if (s_port.sync_semaphore == NULL || s_port.exit_semaphore == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    _ble_nimble_port_drain_sync();
    if (xTaskCreatePinnedToCore(_ble_nimble_port_host_task, "nimble_host",
                                CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, NULL,
                                (configMAX_PRIORITIES - 4),
                                &s_port.host_task,
                                BLE_NIMBLE_PORT_HOST_CORE) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    s_port.started = true;
    if (xSemaphoreTake(s_port.sync_semaphore,
                       pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) !=
            pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    if (_ble_nimble_port_storage_error_load() != ESP_OK)
    {
        return _ble_nimble_port_storage_error_load();
    }
    return ESP_OK;
}

static void _ble_nimble_port_stop_worker(void *param)
{
    (void)param;
    s_port.stop_result = nimble_port_stop();
    xSemaphoreGive(s_port.stop_done_semaphore);
    vTaskDelete(NULL);
}

static esp_err_t _ble_nimble_port_stop(void)
{
    if (s_port.deinit_failed)
    {
        return s_port.deinit_error;
    }
    const esp_err_t quiesce = _ble_nimble_port_quiesce_adv();

    if (quiesce != ESP_OK)
    {
        return quiesce;
    }
    if (s_port.started)
    {
        TaskHandle_t stop_task;

        if (s_port.stop_done_semaphore == NULL)
        {
            s_port.stop_done_semaphore = xSemaphoreCreateBinary();
            if (s_port.stop_done_semaphore == NULL)
            {
                return ESP_ERR_NO_MEM;
            }
        }
        s_port.stop_result = 0;
        if (xTaskCreatePinnedToCore(
                    _ble_nimble_port_stop_worker, "ble_stop",
                    BLE_NIMBLE_PORT_ADV_TASK_STACK, NULL,
                    BLE_NIMBLE_PORT_ADV_TASK_PRIORITY,
                    &stop_task,
                    BLE_NIMBLE_PORT_HOST_CORE) != pdPASS)
        {
            return ESP_ERR_NO_MEM;
        }
        if (xSemaphoreTake(s_port.stop_done_semaphore,
                           pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) !=
                pdTRUE)
        {
            s_port.deinit_failed = true;
            s_port.deinit_error = ESP_ERR_TIMEOUT;
            return ESP_ERR_TIMEOUT;
        }
        if (s_port.stop_result != 0 && s_port.stop_result != BLE_HS_EALREADY)
        {
            /* nimble_port_stop() rejected the stop: report the real host
             * error and stay retryable (do not latch the teardown as a
             * generic timeout). */
            ESP_LOGE(TAG, "nimble_port_stop failed result=%d",
                     s_port.stop_result);
            return ESP_FAIL;
        }
        if (xSemaphoreTake(s_port.exit_semaphore,
                           pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) !=
                pdTRUE)
        {
            s_port.deinit_failed = true;
            s_port.deinit_error = ESP_ERR_TIMEOUT;
            return ESP_ERR_TIMEOUT;
        }
        s_port.started = false;
    }
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_deinit(void)
{
    if (s_port.deinit_failed)
    {
        return s_port.deinit_error;
    }
    const esp_err_t quiesce = _ble_nimble_port_quiesce_adv();

    if (quiesce != ESP_OK)
    {
        return quiesce;
    }
    if (s_port.adv_queue != NULL)
    {
        vQueueDelete(s_port.adv_queue);
        s_port.adv_queue = NULL;
    }
    if (s_port.adv_exit_semaphore != NULL)
    {
        vSemaphoreDelete(s_port.adv_exit_semaphore);
        s_port.adv_exit_semaphore = NULL;
    }
    if (s_port.listener_registered)
    {
        (void)ble_gap_event_listener_unregister(&s_port.listener);
        s_port.listener_registered = false;
    }
    /* Stop the command owner while the NimBLE host and store are still
     * valid. Its teardown drains queued security commands before a restart. */
    if (s_timer_owner_task != NULL)
    {
        const esp_err_t quit_result =
            _ble_nimble_port_owner_quit(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS);

        if (quit_result != ESP_OK)
        {
            /* The owner is still alive: keep the queue, lock, exit
             * semaphore, and NimBLE so a retry can wait for the confirmed
             * exit again. Forcing a delete could free memory underneath a
             * task inside an NVS or manager call. */
            LOG_E("timer owner quit timeout");
            return quit_result;
        }
    }
    _ble_nimble_port_timer_teardown();
    if (!s_port.deinitialized)
    {
        const esp_err_t result = s_port.nimble_init_attempted
                                 ? nimble_port_deinit()
                                 : ESP_OK;

        s_port.nimble_init_attempted = false;
        s_port.deinitialized = true;
        if (result != ESP_OK)
        {
            s_port.deinit_failed = true;
            s_port.deinit_error = result;
            return result;
        }
    }
    if (s_port.sync_semaphore != NULL)
    {
        vSemaphoreDelete(s_port.sync_semaphore);
        s_port.sync_semaphore = NULL;
    }
    if (s_port.exit_semaphore != NULL)
    {
        vSemaphoreDelete(s_port.exit_semaphore);
        s_port.exit_semaphore = NULL;
    }
    if (s_port.stop_done_semaphore != NULL)
    {
        vSemaphoreDelete(s_port.stop_done_semaphore);
        s_port.stop_done_semaphore = NULL;
    }
    /* Runtime restart: clear per-connection transport state (partial
     * frames, subscriptions, transactions, feed generation) while keeping
     * the boot-scoped session facts and epoch allocator. */
    ble_link_gatt_reset();
    ble_link_session_reset();
    if (s_port.link_security_initialized)
    {
        device_link_security_deinit();
        s_port.link_security_initialized = false;
    }
    s_adv_conn_handle = 0U;
    s_adv_generation = 0U;
    _ble_nimble_port_clear_provisional_tracking();
    if (s_link_state_lock != NULL)
    {
        vSemaphoreDelete(s_link_state_lock);
        s_link_state_lock = NULL;
    }
    if (s_storage_lock != NULL)
    {
        vSemaphoreDelete(s_storage_lock);
        s_storage_lock = NULL;
    }
    /* The static queue and exit semaphore live for the module lifetime. */
    if (s_timer_exit != NULL)
    {
        vSemaphoreDelete(s_timer_exit);
        s_timer_exit = NULL;
    }
    ble_used_id_set_deinit();
    ble_response_cache_deinit();
    ble_tx_scheduler_deinit();
    ble_adv_manager_deinit();
    atomic_store_explicit(&s_adv_host_ready, false, memory_order_release);
    if (s_port.adv_lock != NULL)
    {
        vSemaphoreDelete(s_port.adv_lock);
        s_port.adv_lock = NULL;
    }
    return ESP_OK;
}

static const ble_runtime_host_port_t s_nimble_port =
{
    .init = _ble_nimble_port_init,
    .start = _ble_nimble_port_start,
    .set_pairing_gate = ble_nimble_port_set_pairing_window,
    .reset_peer_store = _ble_nimble_port_reset_peer_store,
    .stop = _ble_nimble_port_stop,
    .deinit = _ble_nimble_port_deinit,
};

esp_err_t ble_nimble_port_set_ops(const ble_port_ops_t *ops)
{
    if (s_port.started)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_port.ops = ops;
    return ESP_OK;
}

esp_err_t ble_nimble_port_register_event_cb(
    ble_port_event_cb_t callback, void *arg)
{
    return ble_event_router_register(callback, arg);
}

esp_err_t ble_nimble_port_unregister_event_cb(
    ble_port_event_cb_t callback, void *arg)
{
    return ble_event_router_unregister(callback, arg);
}

const ble_port_ops_t *ble_nimble_port_get_ops(void)
{
    return s_port.ops != NULL ? s_port.ops : &s_production_ops;
}

const ble_runtime_host_port_t *ble_nimble_port_get(void)
{
    return &s_nimble_port;
}
