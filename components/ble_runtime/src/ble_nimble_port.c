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
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_adv_manager.h"
#include "ble_gap_manager.h"
#include "ble_gatt_registry.h"
#include "ble_nimble_port.h"
#include "ble_port_ops.h"

#include "device_link_security.h"

#include "ble_link_security_ops.h"
#include "ble_link_service.h"

static const char *const TAG = "ble_nimble_port";

/**
 * @brief Security 2 ops bound to the device_link_security adapter.
 *
 * The adapter owns the session; the port wires it to the GATT transport
 * (type 0x00 handshake, 0x01 protected). The bootstrap/long-term verifier
 * lifecycle is driven by the link service window ops.
 */
static esp_err_t _ble_nimble_port_sec_handshake(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len)
{
    return device_link_security_handshake(input, input_len,
                                          output, output_len);
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

static void _ble_nimble_port_sec_close_session(void)
{
    device_link_security_close_session();
}

static esp_err_t _ble_nimble_port_security_request(
    const uint8_t *request, size_t request_len,
    uint8_t **response, size_t *response_len, void *arg)
{
    (void)arg;
    return ble_link_service_process_plaintext(request, request_len,
            response, response_len);
}

static const ble_link_security_ops_t s_security_ops =
{
    .handshake = _ble_nimble_port_sec_handshake,
    .unprotect = _ble_nimble_port_sec_unprotect,
    .protect = _ble_nimble_port_sec_protect,
    .is_authenticated = _ble_nimble_port_sec_is_authenticated,
    .close_session = _ble_nimble_port_sec_close_session,
};
#include "ble_response_cache.h"
#include "ble_link_gatt.h"
#include "ble_link_session.h"
#include "ble_runtime.h"
#include "ble_tx_scheduler.h"

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
    BLE_NIMBLE_PORT_ADV_CMD_WAKE,
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
    bool conn_had_bond; /**< Peer had a stored bond before this pairing. */
    bool nimble_init_attempted;
    bool quiescing;
    bool deinit_failed;
    esp_err_t deinit_error;
    int stop_result;
} ble_nimble_port_t;

static ble_nimble_port_t s_port;

static bool _ble_nimble_port_bond_store_verified(
    const struct ble_gap_conn_desc *desc);
static bool _ble_nimble_port_pairing_window_open(void);
static esp_err_t _ble_nimble_port_delete_peer_bond(uint16_t conn_handle);
static int _ble_nimble_port_store_status(
    struct ble_store_status_event *event, void *arg);
static esp_err_t _ble_nimble_port_production_adv_start(
    const ble_port_adv_config_t *config);
static esp_err_t _ble_nimble_port_production_adv_stop(void);
static esp_err_t _ble_nimble_port_production_notify(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len);
static esp_err_t _ble_nimble_port_production_indicate(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len);

static void _ble_nimble_port_tx_completed(
    const ble_tx_scheduler_result_t *result, void *arg)
{
    (void)arg;
    if (result == NULL)
    {
        return;
    }
    /* The final fragment's confirmed completion releases the service
     * transaction gate. A failure closes the session separately through
     * the TX consumer. */
    if (result->is_last && result->status == ESP_OK)
    {
        (void)ble_link_service_response_completed();
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
        manager_event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
        manager_event.conn_handle = event->conn_handle;
        manager_event.status = event->status;
        break;
    case BLE_PORT_EVENT_DISCONNECT:
        manager_event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
        manager_event.conn_handle = event->conn_handle;
        manager_event.reason = event->reason;
        break;
    case BLE_PORT_EVENT_MTU:
        manager_event.type = BLE_GAP_MANAGER_EVENT_MTU;
        manager_event.conn_handle = event->conn_handle;
        manager_event.mtu = event->mtu;
        break;
    case BLE_PORT_EVENT_ENC_CHANGE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE;
        manager_event.conn_handle = event->conn_handle;
        manager_event.encrypted = event->encrypted;
        break;
    case BLE_PORT_EVENT_SUBSCRIBE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE;
        manager_event.conn_handle = event->conn_handle;
        manager_event.attr_handle = event->attr_handle;
        manager_event.subscribed = event->subscribed;
        break;
    case BLE_PORT_EVENT_ADV_COMPLETE:
        manager_event.type = BLE_GAP_MANAGER_EVENT_ADV_COMPLETE;
        break;
    default:
        return;
    }
    (void)ble_gap_manager_handle_event(&manager_event);
}

static void _ble_nimble_port_publish_link_state(
    const uint8_t *value, size_t len, void *arg)
{
    (void)arg;
    const uint16_t handle = ble_link_gatt_link_state_handle();

    if (handle == 0U)
    {
        return;
    }
    ble_gap_manager_snapshot_t snapshot;

    if (ble_gap_manager_get_snapshot(&snapshot) != ESP_OK ||
            !snapshot.connected)
    {
        return;
    }
    /* tx_admission: authorized; the link_state CCCD must be enabled. */
    uint32_t admission_error = 0U;

    if (ble_link_session_query_admission(
                snapshot.generation, BLE_LINK_SESSION_CHANNEL_EVENT,
                &admission_error) != ESP_OK ||
            admission_error != BLE_LINK_ERROR_OK ||
            !ble_gap_manager_is_subscribed(snapshot.conn_handle, handle))
    {
        return;
    }
    /* The raw link_state notification is not a service transaction: its
     * completion must not release the service response gate. */
    (void)ble_tx_scheduler_submit(
        BLE_TX_SCHEDULER_KIND_NOTIFY, snapshot.conn_handle, handle, value,
        len, false);
}

/* One-shot timers for reassembly idle and indication confirmation. The
 * timer state is owned by a dedicated task; callbacks only enqueue an
 * immutable timer id (epoch and kind encoded in the esp_timer argument), so
 * a stale callback can never act on a newer arm. */
#define BLE_NIMBLE_PORT_TIMER_KIND_SESSION 0U
#define BLE_NIMBLE_PORT_TIMER_KIND_CONTROL 1U
#define BLE_NIMBLE_PORT_TIMER_KIND_INDICATION 2U
#define BLE_NIMBLE_PORT_TIMER_KINDS 3U

/* The timer identity is encoded in the esp_timer callback argument:
 * kind in the low bits, epoch shifted above. The argument is immutable
 * for the timer lifetime, so a callback always carries the identity of the
 * exact arm that created it; no shared record is read at fire time. */
#define BLE_NIMBLE_PORT_TIMER_ID_KIND_SHIFT 0U
#define BLE_NIMBLE_PORT_TIMER_ID_KIND_MASK 0x3U
#define BLE_NIMBLE_PORT_TIMER_ID_EPOCH_SHIFT 2U
#define BLE_NIMBLE_PORT_TIMER_ID_EPOCH_MASK 0x3fffffffU

typedef struct ble_nimble_port_timer_command
{
    bool command;   /**< True: arm/disarm/quit; False: timer tick. */
    bool armed;
    unsigned int kind;
    uint32_t generation;
    uint32_t token;
    uint32_t timer_id; /**< Encoded (epoch << 2) | kind. */
} ble_nimble_port_timer_command_t;

/* The command queue is statically allocated and never freed, so a timer
 * callback that fires during teardown can always enqueue safely. */
static StaticQueue_t s_timer_queue_storage;
static ble_nimble_port_timer_command_t s_timer_queue_items[16];
static QueueHandle_t s_timer_command_queue;
static TaskHandle_t s_timer_owner_task;
static SemaphoreHandle_t s_timer_exit;
static uint32_t s_timer_generation;
static uint16_t s_link_conn_handle;

/* Per-kind state, updated under s_link_state_lock by the host task and
 * read by the owner task. The epoch advances synchronously with the arm,
 * so a stale tick is rejected as soon as it is dequeued. */
static uint32_t s_timer_epochs[BLE_NIMBLE_PORT_TIMER_KINDS];
static bool s_timer_exhausted[BLE_NIMBLE_PORT_TIMER_KINDS];
static esp_timer_handle_t s_timer_handles[BLE_NIMBLE_PORT_TIMER_KINDS];
static uint32_t s_timer_generations[BLE_NIMBLE_PORT_TIMER_KINDS];
static uint32_t s_timer_tokens[BLE_NIMBLE_PORT_TIMER_KINDS];
static uint64_t s_timer_deadlines[BLE_NIMBLE_PORT_TIMER_KINDS];
static bool s_timer_armed[BLE_NIMBLE_PORT_TIMER_KINDS];

/* Serializes every link session/service access between the NimBLE host
 * task (GATT feed) and the timer owner task (idle/indication expiry). */
static SemaphoreHandle_t s_link_state_lock;

static void _ble_nimble_port_timer_cb(void *arg)
{
    /* Only the statically allocated queue is touched, so this is safe
     * even while the owner task is being torn down. The immutable timer id
     * identifies the exact arm that created this timer. */
    const ble_nimble_port_timer_command_t tick =
    {
        .command = false,
        .timer_id = (uint32_t)(uintptr_t)arg,
    };

    if (s_timer_command_queue != NULL)
    {
        if (xQueueSend(s_timer_command_queue, &tick, 0U) != pdTRUE)
        {
            /* A dropped tick loses this expiry; the next arm or the
             * connection teardown re-establishes the window. */
        }
    }
}

static uint32_t _ble_nimble_port_timer_epoch_advance(unsigned int kind)
{
    if (s_timer_exhausted[kind])
    {
        return 0U;
    }
    if (s_timer_epochs[kind] >= BLE_NIMBLE_PORT_TIMER_ID_EPOCH_MASK)
    {
        /* The epoch no longer fits the immutable timer id: fail closed
         * before the encoded value would wrap. */
        s_timer_exhausted[kind] = true;
        return 0U;
    }
    s_timer_epochs[kind]++;
    return s_timer_epochs[kind];
}

static void _ble_nimble_port_timer_free_handle(unsigned int kind)
{
    if (s_timer_handles[kind] != NULL)
    {
        esp_timer_stop(s_timer_handles[kind]);
        esp_timer_delete(s_timer_handles[kind]);
        s_timer_handles[kind] = NULL;
    }
}

static void _ble_nimble_port_timer_fail_closed(
    unsigned int kind, uint32_t generation, uint32_t token)
{
    s_timer_armed[kind] = false;
    if (kind == BLE_NIMBLE_PORT_TIMER_KIND_INDICATION)
    {
        /* End the in-flight indication and drop the queued transaction;
         * only a token match closes the session and the transaction gate. */
        if (ble_tx_scheduler_handle_indication_timeout(token) == ESP_OK)
        {
            (void)ble_link_session_security2_close_current(generation);
            ble_link_service_abort_transactions();
        }
    }
    else
    {
        /* Abort the reassembly slot for this channel. */
        ble_link_gatt_on_reassembly_idle_generation(generation);
    }
}

static void _ble_nimble_port_timer_rebuild(
    const ble_nimble_port_timer_command_t *command)
{
    /* The epoch and deadline were fixed by the host task under the lock;
     * the owner rebuilds the esp_timer with the remaining time. A stale
     * command (superseded by a newer arm) is dropped. */
    const unsigned int kind = command->kind;
    const uint32_t command_epoch =
        (command->timer_id >> BLE_NIMBLE_PORT_TIMER_ID_EPOCH_SHIFT) &
        BLE_NIMBLE_PORT_TIMER_ID_EPOCH_MASK;

    if (command_epoch != s_timer_epochs[kind])
    {
        return;
    }
    const uint64_t now = esp_timer_get_time();
    uint64_t remaining_us = 0U;

    if (s_timer_deadlines[kind] > now)
    {
        remaining_us = s_timer_deadlines[kind] - now;
    }
    _ble_nimble_port_timer_free_handle(kind);
    esp_timer_create_args_t args;

    memset(&args, 0, sizeof(args));
    args.callback = _ble_nimble_port_timer_cb;
    args.arg = (void *)(uintptr_t)command->timer_id;
    args.name = (kind == BLE_NIMBLE_PORT_TIMER_KIND_INDICATION) ?
                "ble_ind_tmo" : "ble_reasm_idle";
    if (remaining_us == 0U ||
            esp_timer_create(&args, &s_timer_handles[kind]) != ESP_OK ||
            esp_timer_start_once(s_timer_handles[kind], remaining_us) != ESP_OK)
    {
        /* Fail closed: without the timer the timeout contract cannot be
         * met, so the transaction and session are terminated; a
         * reassembly slot is aborted through the idle path. */
        _ble_nimble_port_timer_free_handle(kind);
        s_timer_armed[kind] = false;
        _ble_nimble_port_timer_fail_closed(
            kind, command->generation, command->token);
    }
}

static void _ble_nimble_port_timer_check_deadlines(void)
{
    /* Any wakeup checks every armed kind; a lost tick cannot strand an
     * indication. Runs with s_link_state_lock held. */
    const uint64_t now = esp_timer_get_time();

    for (unsigned int kind = 0U; kind < BLE_NIMBLE_PORT_TIMER_KINDS; ++kind)
    {
        if (!s_timer_armed[kind] || s_timer_deadlines[kind] > now)
        {
            continue;
        }
        const uint32_t generation = s_timer_generations[kind];
        const uint32_t token = s_timer_tokens[kind];

        s_timer_armed[kind] = false;
        if (kind == BLE_NIMBLE_PORT_TIMER_KIND_INDICATION)
        {
            if (ble_tx_scheduler_handle_indication_timeout(token) == ESP_OK)
            {
                (void)ble_link_session_security2_close_current(generation);
                ble_link_service_abort_transactions();
            }
        }
        else
        {
            ble_link_gatt_on_reassembly_idle_generation(generation);
        }
    }
}

static void _ble_nimble_port_timer_teardown(void)
{
    /* Free live timer handles and clear per-kind runtime state. Safe from
     * the owner task or from teardown when the owner had to be deleted. */
    for (unsigned int kind = 0U; kind < BLE_NIMBLE_PORT_TIMER_KINDS; ++kind)
    {
        _ble_nimble_port_timer_free_handle(kind);
        s_timer_armed[kind] = false;
        s_timer_deadlines[kind] = 0U;
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

static void _ble_nimble_port_timer_owner(void *arg)
{
    (void)arg;
    ble_nimble_port_timer_command_t command;

    for (;;)
    {
        if (xQueueReceive(s_timer_command_queue, &command, portMAX_DELAY) !=
                pdTRUE)
        {
            continue;
        }
        if (s_link_state_lock == NULL ||
                xSemaphoreTakeRecursive(s_link_state_lock, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if (command.command)
        {
            if (!command.armed)
            {
                if (command.kind >= BLE_NIMBLE_PORT_TIMER_KINDS)
                {
                    /* Quit. */
                    xSemaphoreGiveRecursive(s_link_state_lock);
                    break;
                }
                _ble_nimble_port_timer_free_handle(command.kind);
            }
            else
            {
                _ble_nimble_port_timer_rebuild(&command);
            }
        }
        /* A tick only wakes us up; the deadline sweep below is the actual
         * expiry check, so a lost or stale tick is harmless. */
        _ble_nimble_port_timer_check_deadlines();
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
    _ble_nimble_port_timer_teardown();
    if (s_timer_exit != NULL)
    {
        xSemaphoreGive(s_timer_exit);
    }
    vTaskDelete(NULL);
}

static bool _ble_nimble_port_timer_send(
    bool armed, unsigned int kind, uint32_t generation, uint32_t token)
{
    if (s_timer_command_queue == NULL ||
            (s_link_state_lock != NULL &&
             xSemaphoreTakeRecursive(s_link_state_lock,
                                     portMAX_DELAY) != pdTRUE))
    {
        return false;
    }
    /* Advance the epoch synchronously so a stale tick cannot pass. */
    if (armed)
    {
        const uint32_t epoch = _ble_nimble_port_timer_epoch_advance(kind);

        if (epoch == 0U)
        {
            /* Exhausted: fail closed so the timeout contract is not
             * silently dropped. */
            s_timer_armed[kind] = false;
            if (s_link_state_lock != NULL)
            {
                xSemaphoreGiveRecursive(s_link_state_lock);
            }
            _ble_nimble_port_timer_fail_closed(kind, generation, token);
            return false;
        }
        s_timer_generations[kind] = generation;
        s_timer_tokens[kind] = token;
        s_timer_deadlines[kind] =
            esp_timer_get_time() +
            ((kind == BLE_NIMBLE_PORT_TIMER_KIND_INDICATION) ?
             (uint64_t)BLE_NIMBLE_PORT_INDICATION_TIMEOUT_MS * 1000U :
             (uint64_t)BLE_NIMBLE_PORT_REASSEMBLY_IDLE_MS * 1000U);
        s_timer_armed[kind] = true;
        const ble_nimble_port_timer_command_t command =
        {
            .command = true,
            .armed = true,
            .kind = kind,
            .generation = generation,
            .token = token,
            .timer_id = (epoch << BLE_NIMBLE_PORT_TIMER_ID_EPOCH_SHIFT) |
            (kind & BLE_NIMBLE_PORT_TIMER_ID_KIND_MASK),
        };

        if (xQueueSend(s_timer_command_queue, &command, 0U) != pdTRUE)
        {
            LOG_E("link timer arm dropped kind=%u", (unsigned int)kind);
            _ble_nimble_port_timer_fail_closed(kind, generation, token);
            if (s_link_state_lock != NULL)
            {
                xSemaphoreGiveRecursive(s_link_state_lock);
            }
            return false;
        }
    }
    else
    {
        const uint32_t epoch = _ble_nimble_port_timer_epoch_advance(kind);

        (void)epoch;
        s_timer_armed[kind] = false;
        const ble_nimble_port_timer_command_t command =
        {
            .command = true,
            .armed = false,
            .kind = kind,
        };

        if (xQueueSend(s_timer_command_queue, &command, 0U) != pdTRUE)
        {
            LOG_E("link timer disarm dropped kind=%u", (unsigned int)kind);
        }
    }
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
    return true;
}

static void _ble_nimble_port_arm_reassembly_idle(
    bool armed, uint32_t generation,
    ble_link_service_rx_channel_t channel)
{
    /* Each RX characteristic has its own 5000 ms idle window. */
    const unsigned int kind =
        (channel == BLE_LINK_SERVICE_RX_SESSION) ?
        BLE_NIMBLE_PORT_TIMER_KIND_SESSION :
        BLE_NIMBLE_PORT_TIMER_KIND_CONTROL;

    _ble_nimble_port_timer_send(armed, kind, generation, 0U);
}

static bool _ble_nimble_port_arm_indication_timeout(
    bool armed, uint32_t generation, uint32_t token)
{
    return _ble_nimble_port_timer_send(
               armed, BLE_NIMBLE_PORT_TIMER_KIND_INDICATION, generation,
               token);
}

static void _ble_nimble_port_link_gatt_consumer(
    const ble_port_event_t *event, void *arg)
{
    (void)arg;
    ble_gap_manager_snapshot_t snapshot;
    uint32_t generation = 0U;

    if (ble_gap_manager_get_snapshot(&snapshot) == ESP_OK)
    {
        generation = snapshot.generation;
    }
    switch (event->type)
    {
    case BLE_PORT_EVENT_CONNECT:
    case BLE_PORT_EVENT_DISCONNECT:
    case BLE_PORT_EVENT_ENC_CHANGE:
    case BLE_PORT_EVENT_MTU:
        break;
    default:
        /* Events that never touch the link session do not take the lock. */
        return;
    }
    if (s_link_state_lock != NULL &&
            xSemaphoreTakeRecursive(s_link_state_lock, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    switch (event->type)
    {
    case BLE_PORT_EVENT_CONNECT:
        if (snapshot.connected &&
                event->conn_handle == snapshot.conn_handle)
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
            /* Remember the connection identity and generation; the idle
             * timer arms only while a partial frame exists. */
            s_link_conn_handle = event->conn_handle;
            s_timer_generation = generation;
        }
        break;
    case BLE_PORT_EVENT_DISCONNECT:
        /* The gap consumer runs first and already cleared the snapshot;
         * compare against the remembered connection identity. */
        if (s_link_conn_handle != event->conn_handle)
        {
            break;
        }
        (void)ble_link_session_handle_event(
            generation, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED);
        _ble_nimble_port_arm_reassembly_idle(
            false, generation, BLE_LINK_SERVICE_RX_SESSION);
        _ble_nimble_port_arm_reassembly_idle(
            false, generation, BLE_LINK_SERVICE_RX_CONTROL);
        _ble_nimble_port_arm_indication_timeout(false, 0U, 0U);
        /* A disconnect with an unconfirmed response must not leave the
         * transaction gate busy for the next connection, and the Security
         * 2 session must not survive into the next connection (cross-
         * connection state reuse). */
        ble_link_service_abort_transactions();
        _ble_nimble_port_sec_close_session();
        ble_link_service_clear_session_state();
        s_link_conn_handle = 0U;
        break;
    case BLE_PORT_EVENT_ENC_CHANGE:
        if (s_link_conn_handle != event->conn_handle)
        {
            break;
        }
        if (event->encrypted)
        {
            (void)ble_link_session_handle_event(
                generation, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED);
            struct ble_gap_conn_desc desc;

            if (ble_gap_conn_find(event->conn_handle, &desc) != 0)
            {
                break;
            }
            if (!_ble_nimble_port_bond_store_verified(&desc))
            {
                /* Incomplete or non-SC bond material: fail closed and
                 * remove the malformed record so it cannot occupy the
                 * single bond slot. */
                const esp_err_t delete_result =
                    _ble_nimble_port_delete_peer_bond(event->conn_handle);

                if (delete_result != ESP_OK)
                {
                    ESP_LOGW(TAG, "malformed bond cleanup failed (%d)",
                             delete_result);
                }
                (void)ble_gap_terminate(event->conn_handle,
                                        BLE_ERR_CONN_TERM_LOCAL);
                break;
            }
            if (_ble_nimble_port_pairing_window_open() ||
                    s_port.conn_had_bond)
            {
                /* The bond matches the contract and the peer is either
                 * pairing inside a window or reconnecting with a stored
                 * bond: report the verified identity. */
                (void)ble_link_session_handle_event(
                    generation, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED);
                (void)ble_link_session_set_identity_known(
                    generation, true);
            }
            else
            {
                /* An unknown peer paired outside a window: terminate the
                 * connection and delete the orphan bond. */
                const esp_err_t delete_result =
                    _ble_nimble_port_delete_peer_bond(event->conn_handle);

                if (delete_result != ESP_OK)
                {
                    ESP_LOGW(TAG, "orphan bond cleanup failed (%d)",
                             delete_result);
                }
                (void)ble_gap_terminate(event->conn_handle,
                                        BLE_ERR_CONN_TERM_LOCAL);
            }
        }
        else
        {
            (void)ble_link_session_clear_link_security(generation);
            (void)ble_link_session_security2_close_current(generation);
        }
        break;
    case BLE_PORT_EVENT_MTU:
        if (s_link_conn_handle != event->conn_handle)
        {
            break;
        }
        ble_link_gatt_set_att_mtu(event->mtu);
        break;
    default:
        break;
    }
    (void)ble_link_gatt_refresh_link_state();
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
    }
}

static void _ble_nimble_port_adv_consumer(
    const ble_port_event_t *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_PORT_EVENT_DISCONNECT)
    {
        /* Only the current connection's disconnect stops advertising. */
        ble_gap_manager_snapshot_t snapshot;

        if (ble_gap_manager_get_snapshot(&snapshot) == ESP_OK &&
                snapshot.conn_handle != event->conn_handle)
        {
            return;
        }
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
            /* The scheduler confirmed this event completed the in-flight
             * frame with a failure: close the Security 2 session per the
             * framing contract and release the transaction gate (a failed
             * notification also drops queued service frames). */
            ble_gap_manager_snapshot_t snapshot;

            if (ble_gap_manager_get_snapshot(&snapshot) == ESP_OK &&
                    snapshot.conn_handle == event->conn_handle)
            {
                (void)ble_link_session_security2_close_current(
                    snapshot.generation);
                ble_link_service_abort_transactions();
            }
        }
        else if (result != ESP_OK && result != ESP_ERR_NOT_FOUND &&
                 result != ESP_ERR_INVALID_STATE)
        {
            /* The scheduler surfaced a synchronous failure of a queued
             * fragment: close the session for the current connection and
             * release the transaction gate. */
            ble_gap_manager_snapshot_t snapshot;

            if (ble_gap_manager_get_snapshot(&snapshot) == ESP_OK &&
                    snapshot.conn_handle == event->conn_handle)
            {
                (void)ble_link_session_security2_close_current(
                    snapshot.generation);
                ble_link_service_abort_transactions();
            }
            LOG_E("tx scheduler event failed result=%d", result);
        }
        break;
    }
    case BLE_PORT_EVENT_DISCONNECT:
        /* Only the current connection's disconnect resets the scheduler. */
        if (s_link_conn_handle != event->conn_handle)
        {
            break;
        }
        ble_tx_scheduler_reset();
        break;
    case BLE_PORT_EVENT_RESET:
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
    (void)arg;
    if (delay_ms > 0U && s_port.adv_queue != NULL && !s_port.quiescing)
    {
        ble_nimble_port_adv_cmd_t wake;

        memset(&wake, 0, sizeof(wake));
        wake.type = BLE_NIMBLE_PORT_ADV_CMD_WAKE;
        (void)xQueueSend(s_port.adv_queue, &wake, 0U);
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

static esp_err_t _ble_nimble_port_delete_peer_bond(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;

    if (ble_gap_conn_find(conn_handle, &desc) != 0)
    {
        return ESP_ERR_NOT_FOUND;
    }
    /* Orphan cleanup: a bond without admission must not outlive the
     * connection. */
    return ble_store_util_delete_peer(&desc.peer_id_addr) == 0 ?
           ESP_OK : ESP_FAIL;
}

static int _ble_nimble_port_store_status(
    struct ble_store_status_event *event, void *arg)
{
    (void)arg;
    if (event == NULL || event->event_code != BLE_STORE_EVENT_FULL)
    {
        return 0;
    }
    if (!_ble_nimble_port_pairing_window_open())
    {
        /* Outside a pairing window no new bond may displace the existing
         * one; abort the store so the pairing cannot complete. */
        return -1;
    }
    /* Replacement window: make room for the new bond. The single-bond
     * model means there is at most one existing peer to evict. */
    ble_addr_t peers[1];
    int count = 0;

    if (ble_store_util_bonded_peers(peers, &count, 1) == 0 && count > 0)
    {
        (void)ble_store_util_delete_peer(&peers[0]);
    }
    return 0;
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
            /* Snapshot whether a bond already existed before any pairing
             * of this connection could persist keys; an RPA peer is
             * re-evaluated on IDENTITY_RESOLVED. */
            struct ble_gap_conn_desc desc;

            s_port.conn_had_bond = false;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0)
            {
                s_port.conn_had_bond = _ble_nimble_port_peer_has_bond(&desc);
            }
            _ble_nimble_port_dispatch(&port_event);
            ble_gap_manager_snapshot_t snapshot;

            if (ble_gap_manager_get_snapshot(&snapshot) == ESP_OK &&
                    (!snapshot.connected ||
                     snapshot.conn_handle != event->connect.conn_handle))
            {
                const int terminate_result = ble_gap_terminate(
                                                 event->connect.conn_handle,
                                                 BLE_ERR_CONN_TERM_LOCAL);

                if (terminate_result != 0)
                {
                    LOG_W("rejected connection terminate failed result=%d",
                          terminate_result);
                }
            }
            return 0;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        port_event.type = BLE_PORT_EVENT_DISCONNECT;
        port_event.conn_handle = event->disconnect.conn.conn_handle;
        port_event.reason = event->disconnect.reason;
        s_port.conn_had_bond = false;
        break;
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.channel_id != BLE_L2CAP_CID_ATT)
        {
            return 0;
        }
        port_event.type = BLE_PORT_EVENT_MTU;
        port_event.conn_handle = event->mtu.conn_handle;
        port_event.mtu = event->mtu.value;
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        port_event.type = BLE_PORT_EVENT_ENC_CHANGE;
        port_event.conn_handle = event->enc_change.conn_handle;
        {
            struct ble_gap_conn_desc description;

            port_event.encrypted =
                ble_gap_conn_find(event->enc_change.conn_handle,
                                  &description) == 0 &&
                description.sec_state.encrypted;
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        port_event.type = BLE_PORT_EVENT_SUBSCRIBE;
        port_event.conn_handle = event->subscribe.conn_handle;
        port_event.attr_handle = event->subscribe.attr_handle;
        port_event.subscribed = event->subscribe.cur_notify ||
                                event->subscribe.cur_indicate;
        break;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        /* The peer RPA resolved to a stored identity: this is a known
         * peer reconnect, so the pairing window is not required. */
    {
        struct ble_gap_conn_desc desc;

        s_port.conn_had_bond = false;
        if (ble_gap_conn_find(event->identity_resolved.conn_handle,
                              &desc) == 0)
        {
            s_port.conn_had_bond = _ble_nimble_port_peer_has_bond(&desc);
            ble_link_gatt_set_connection(
                s_timer_generation, event->identity_resolved.conn_handle,
                desc.peer_id_addr.type, desc.peer_id_addr.val);
        }
    }
    return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* An existing bond attempts to pair again: allowed only while a
         * replacement window is open, after evicting the old bond. */
        if (_ble_nimble_port_pairing_window_open())
        {
            const esp_err_t delete_result =
                _ble_nimble_port_delete_peer_bond(
                    event->repeat_pairing.conn_handle);

            if (delete_result != ESP_OK)
            {
                ESP_LOGW(TAG, "repeat pairing eviction failed (%d)",
                         delete_result);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        port_event.type = BLE_PORT_EVENT_ADV_COMPLETE;
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        port_event.type = BLE_PORT_EVENT_NOTIFY_TX;
        port_event.conn_handle = event->notify_tx.conn_handle;
        port_event.attr_handle = event->notify_tx.attr_handle;
        port_event.status = event->notify_tx.status;
        port_event.indication = event->notify_tx.indication != 0;
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

        LOG_I("host synchronized");
        xSemaphoreGive(s_port.sync_semaphore);
        memset(&event, 0, sizeof(event));
        event.type = BLE_PORT_EVENT_SYNC;
        _ble_nimble_port_dispatch(&event);
    }
}

static void _ble_nimble_port_on_reset(int reason)
{
    ble_port_event_t event;

    LOG_E("host reset reason=%d", reason);
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_RESET;
    event.status = reason;
    _ble_nimble_port_dispatch(&event);
}

static bool _ble_nimble_port_is_link_rx(
    const ble_gatt_registry_characteristic_t *characteristic)
{
    static const uint8_t session_rx_uuid[16] =
    {
        0xa2, 0xf0, 0xcd, 0xfc, 0xe0, 0xe6, 0x5c, 0xb8,
        0xd8, 0x4d, 0xcb, 0x4c, 0x43, 0xe6, 0x01, 0x48,
    };
    static const uint8_t control_rx_uuid[16] =
    {
        0xc8, 0x13, 0x3d, 0x40, 0xfb, 0x3d, 0x0c, 0x8e,
        0x72, 0x47, 0x9d, 0x66, 0x62, 0x46, 0xa1, 0x81,
    };

    return memcmp(characteristic->uuid, session_rx_uuid, 16U) == 0 ||
           memcmp(characteristic->uuid, control_rx_uuid, 16U) == 0;
}

static ble_link_service_rx_channel_t _ble_nimble_port_link_rx_channel(
    const ble_gatt_registry_characteristic_t *characteristic)
{
    static const uint8_t session_rx_uuid[16] =
    {
        0xa2, 0xf0, 0xcd, 0xfc, 0xe0, 0xe6, 0x5c, 0xb8,
        0xd8, 0x4d, 0xcb, 0x4c, 0x43, 0xe6, 0x01, 0x48,
    };
    static const uint8_t control_rx_uuid[16] =
    {
        0xc8, 0x13, 0x3d, 0x40, 0xfb, 0x3d, 0x0c, 0x8e,
        0x72, 0x47, 0x9d, 0x66, 0x62, 0x46, 0xa1, 0x81,
    };

    if (memcmp(characteristic->uuid, session_rx_uuid, 16U) == 0)
    {
        return BLE_LINK_SERVICE_RX_SESSION;
    }
    if (memcmp(characteristic->uuid, control_rx_uuid, 16U) == 0)
    {
        return BLE_LINK_SERVICE_RX_CONTROL;
    }
    return BLE_LINK_SERVICE_RX_CONTROL;
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

    if (s_link_state_lock != NULL &&
            xSemaphoreTakeRecursive(s_link_state_lock, portMAX_DELAY) != pdTRUE)
    {
        return BLE_ATT_ERR_UNLIKELY;
    }
    result = characteristic->access_cb(
                 conn_handle, attr_handle, &port_context,
                 characteristic->arg);
    if (result == 0 &&
            port_context.op == BLE_GATT_REGISTRY_OP_WRITE_CHR &&
            s_timer_generation != 0U &&
            _ble_nimble_port_is_link_rx(characteristic))
    {
        /* After an accepted feed, keep the idle timer only while a partial
         * frame remains on this RX channel. Rejected writes never restart
         * the window, and other characteristics never touch it. This runs
         * while the link state lock is held, so the partial state cannot
         * change concurrently. */
        const ble_link_service_rx_channel_t channel =
            _ble_nimble_port_link_rx_channel(characteristic);

        _ble_nimble_port_arm_reassembly_idle(
            ble_link_service_has_partial_frame(channel),
            s_timer_generation, channel);
    }
    if (s_link_state_lock != NULL)
    {
        xSemaphoreGiveRecursive(s_link_state_lock);
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
    static struct ble_gatt_chr_def characteristic_sets[
            BLE_GATT_REGISTRY_MAX_SERVICES]
        [BLE_GATT_REGISTRY_MAX_CHARACTERISTICS + 1];
    static ble_uuid128_t service_uuids[BLE_GATT_REGISTRY_MAX_SERVICES];
    static ble_uuid128_t characteristic_uuids[
        BLE_GATT_REGISTRY_MAX_SERVICES]
    [BLE_GATT_REGISTRY_MAX_CHARACTERISTICS];
    size_t service_count = 0U;
    size_t characteristic_cursor = 0U;

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
                    &characteristic_sets[service_count][i];

            memcpy(characteristic_uuids[service_count][i].value,
                   characteristic->uuid, 16U);
            characteristic_uuids[service_count][i].u.type = BLE_UUID_TYPE_128;
            definition->uuid = &characteristic_uuids[service_count][i].u;
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
        characteristic_sets[service_count]
        [service->characteristic_count].uuid = NULL;
        services[service_count].characteristics =
            characteristic_sets[service_count];
        characteristic_cursor += service->characteristic_count;
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

    if (config == NULL || s_port.adv_queue == NULL || s_port.quiescing)
    {
        return ESP_ERR_INVALID_STATE;
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
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_production_adv_stop(void)
{
    ble_nimble_port_adv_cmd_t cmd;

    if (s_port.adv_queue == NULL || s_port.quiescing)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = BLE_NIMBLE_PORT_ADV_CMD_STOP;
    if (xQueueSend(s_port.adv_queue, &cmd, 0U) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
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
        const uint32_t remaining =
            ble_adv_manager_get_fast_window_remaining_ms();

        if (remaining == 0U)
        {
            (void)ble_adv_manager_handle_fast_window_expired();
            continue;
        }
        const TickType_t wait = remaining == UINT32_MAX
                                ? portMAX_DELAY
                                : pdMS_TO_TICKS(remaining);

        if (xQueueReceive(s_port.adv_queue, &cmd, wait) != pdTRUE)
        {
            continue;
        }
        if (cmd.type == BLE_NIMBLE_PORT_ADV_CMD_START)
        {
            cmd.config.service_data = cmd.service_data;
            const int result = _ble_nimble_port_adv_start_execute(&cmd);

            _ble_nimble_port_adv_result_event(BLE_PORT_EVENT_ADV_STARTED,
                                              result, &cmd.config);
        }
        else if (cmd.type == BLE_NIMBLE_PORT_ADV_CMD_STOP)
        {
            const int result = ble_gap_adv_stop();

            if (result != 0 && result != BLE_HS_EALREADY)
            {
                LOG_E("adv stop failed result=%d", result);
            }
            _ble_nimble_port_adv_result_event(
                BLE_PORT_EVENT_ADV_STOPPED,
                result == BLE_HS_EALREADY ? 0 : result, NULL);
        }
        else if (cmd.type == BLE_NIMBLE_PORT_ADV_CMD_WAKE)
        {
            /* Loop to recompute the fast-window wait. */
        }
        else
        {
            xSemaphoreGive(s_port.adv_exit_semaphore);
            vTaskDelete(NULL);
        }
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
    result = ble_gatts_notify_custom(conn_handle, value_handle, om);
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
     * scheduler's in-flight token. */
    const uint32_t token = ble_tx_scheduler_get_in_flight_token();
    ble_gap_manager_snapshot_t snapshot;

    if (ble_gap_manager_get_snapshot(&snapshot) != ESP_OK)
    {
        snapshot.generation = 0U;
    }
    if (token != 0U &&
            !_ble_nimble_port_arm_indication_timeout(true,
                    snapshot.generation,
                    token))
    {
        /* The confirmation timer could not be armed: do not send an
         * indication that can never time out, and release the mbuf. */
        os_mbuf_free_chain(om);
        return ESP_FAIL;
    }
    result = ble_gatts_indicate_custom(conn_handle, value_handle, om);
    if (result != 0)
    {
        _ble_nimble_port_arm_indication_timeout(false, 0U, 0U);
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
            pdTRUE ||
            xSemaphoreTake(s_port.adv_exit_semaphore,
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
        const ble_nimble_port_timer_command_t quit =
        {
            .command = true,
            .armed = false,
            .kind = BLE_NIMBLE_PORT_TIMER_KINDS,
        };

        if (s_timer_command_queue != NULL)
        {
            (void)xQueueSend(s_timer_command_queue, &quit, 0U);
        }
        if (s_timer_exit != NULL &&
                xSemaphoreTake(s_timer_exit,
                               pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) !=
                pdTRUE)
        {
            vTaskDelete(s_timer_owner_task);
        }
        s_timer_owner_task = NULL;
    }
    _ble_nimble_port_timer_teardown();
    /* The command queue is statically allocated and stays for the module
     * lifetime; the lock and exit semaphore are recycled. */
    if (s_link_state_lock != NULL)
    {
        vSemaphoreDelete(s_link_state_lock);
        s_link_state_lock = NULL;
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
    result = ble_event_router_register(_ble_nimble_port_gap_consumer, NULL);
    if (result != ESP_OK)
    {
        return result;
    }
    result = ble_event_router_register(_ble_nimble_port_adv_consumer, NULL);
    if (result != ESP_OK)
    {
        return result;
    }
    result = ble_event_router_register(_ble_nimble_port_tx_consumer, NULL);
    if (result != ESP_OK)
    {
        return result;
    }
    if (s_timer_command_queue == NULL)
    {
        /* Statically allocated and never freed: a timer callback that
         * fires during teardown can always enqueue safely. */
        s_timer_command_queue = xQueueCreateStatic(
                                    16U, sizeof(ble_nimble_port_timer_command_t),
                                    (uint8_t *)s_timer_queue_items, &s_timer_queue_storage);
        if (s_timer_command_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_timer_exit == NULL)
    {
        s_timer_exit = xSemaphoreCreateBinary();
        if (s_timer_exit == NULL)
        {
            return ESP_ERR_NO_MEM;
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
            return ESP_ERR_NO_MEM;
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
            return ESP_ERR_NO_MEM;
        }
    }
    result = ble_event_router_register(
                 _ble_nimble_port_link_gatt_consumer, NULL);
    if (result != ESP_OK)
    {
        return result;
    }
    if (s_port.ops == NULL)
    {
        s_port.ops = &s_production_ops;
    }
    ble_gap_manager_init();
    result = _ble_nimble_port_adv_manager_init();
    if (result != ESP_OK)
    {
        return result;
    }
    result = _ble_nimble_port_tx_manager_init();
    if (result != ESP_OK)
    {
        return result;
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
        if (!s_port.link_security_initialized)
        {
            const device_link_security_config_t security_config =
            {
                .username = DEVICE_LINK_SECURITY_USERNAME,
                .session_id = 1U,
                .request_cb = _ble_nimble_port_security_request,
                .request_arg = NULL,
            };

            result = device_link_security_init(&security_config);
            if (result != ESP_OK)
            {
                return result;
            }
            s_port.link_security_initialized = true;
        }
        result = ble_link_gatt_init(&gatt_config);
        if (result == ESP_OK)
        {
            /* A committed authorization record grants reconnects: load
             * the long-term verifier so an outside-window handshake can
             * succeed after a reboot. NOT_FOUND is the unbound state. */
            const esp_err_t long_term_result =
                device_link_security_load_long_term_verifier();

            if (long_term_result != ESP_OK &&
                    long_term_result != ESP_ERR_NOT_FOUND)
            {
                ESP_LOGW(TAG, "long-term verifier load failed (%d)",
                         long_term_result);
            }
        }
        if (result != ESP_OK)
        {
            return result;
        }
        if (!ble_gatt_registry_is_sealed())
        {
            const esp_err_t seal_result = ble_gatt_registry_seal();

            if (seal_result != ESP_OK)
            {
                return seal_result;
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
            return result;
        }
    }
    s_port.nimble_init_attempted = true;
    result = nimble_port_init();
    if (result != ESP_OK)
    {
        s_port.deinit_failed = true;
        s_port.deinit_error = result;
        return result;
    }
    ble_hs_cfg.reset_cb = _ble_nimble_port_on_reset;
    ble_hs_cfg.sync_cb = _ble_nimble_port_on_sync;
    ble_hs_cfg.gatts_register_cb = _ble_nimble_port_gatts_register;
    ble_hs_cfg.store_status_cb = _ble_nimble_port_store_status;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    /* Identity keys are distributed so the peer identity can be verified
     * on device. The bond-store identity/SC/LTK verification is NOT yet
     * implemented: SC_BOND_VERIFIED and identity_known stay false and the
     * session admission remains fail-closed until that verification lands
     * in the ENC_CHANGE path. */
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
    nimble_port_run();
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
                pdTRUE ||
                (s_port.stop_result != 0 && s_port.stop_result != BLE_HS_EALREADY))
        {
            s_port.deinit_failed = true;
            s_port.deinit_error = ESP_ERR_TIMEOUT;
            return ESP_ERR_TIMEOUT;
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
    /* Timers are per-arm handles created by the owner task; there is no
     * persistent timer to stop here. The callback only wakes the owner task
     * through xQueueSend to a statically allocated queue that is never
     * freed, so teardown cannot race it: the owner task is stopped first,
     * and any callback that fires afterwards enqueues harmlessly into the
     * still-valid static queue. */
    if (s_timer_owner_task != NULL)
    {
        /* Quit: kind >= KINDS signals the owner to stop, free its timers,
         * signal exit, and delete itself. */
        const ble_nimble_port_timer_command_t quit =
        {
            .command = true,
            .armed = false,
            .kind = BLE_NIMBLE_PORT_TIMER_KINDS,
        };

        (void)xQueueSend(s_timer_command_queue, &quit, 0U);
        if (s_timer_exit != NULL &&
                xSemaphoreTake(s_timer_exit,
                               pdMS_TO_TICKS(BLE_NIMBLE_PORT_SYNC_TIMEOUT_MS)) !=
                pdTRUE)
        {
            /* Fallback: the owner may have died already. */
            vTaskDelete(s_timer_owner_task);
        }
        s_timer_owner_task = NULL;
    }
    _ble_nimble_port_timer_teardown();
    /* Runtime restart: clear per-connection transport state (partial
     * frames, subscriptions, transactions, feed generation) while keeping
     * the boot-scoped session facts and epoch allocator. */
    ble_link_gatt_reset();
    ble_link_session_reset();
    if (s_link_state_lock != NULL)
    {
        vSemaphoreDelete(s_link_state_lock);
        s_link_state_lock = NULL;
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
