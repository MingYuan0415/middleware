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

#include "esp_err.h"
#ifndef UNIT_TEST_HOST
    #include "nvs.h"
#endif
#include "nv_storage.h"

#include "ble_adv_manager.h"
#include "ble_link_gatt.h"
#include "ble_link_session.h"
#include "ble_port_ops.h"
#include "ble_link_service.h"

#include "ble_nimble_port.h"
#include "ble_nimble_port_revoke_journal.h"
#include "ble_runtime.h"

#include "device_link_service.h"

#include "device_link_service_test_hooks.h"
#include "device_link_confirmation.h"
#include "device_link_revoke_progress.h"
#include "device_link_wifi_adapter.h"
#include "event_bus.h"

#define DBG_TAG "device_link_service"
#include "mt_log.h"

#define DEVICE_LINK_SERVICE_RETRY_MS 100U
#define DEVICE_LINK_SERVICE_REMAINING_PUBLISH_MS 1000U
#define DEVICE_LINK_SERVICE_WINDOW_MS 120000U
#define DEVICE_LINK_SERVICE_BLUETOOTH_POLICY_KEY "dl_bt_policy"
#define DEVICE_LINK_SERVICE_BLUETOOTH_POLICY_MAGIC UINT32_C(0x444c4254)
#define DEVICE_LINK_SERVICE_BLUETOOTH_POLICY_VERSION 1U
#define DEVICE_LINK_SERVICE_BLUETOOTH_RETRY_MAX_MS 1000U
#define DEVICE_LINK_SERVICE_BLUETOOTH_RETRY_MAX_ATTEMPTS 2U
#define DEVICE_LINK_SERVICE_BLUETOOTH_TRANSITION_TIMEOUT_MS 5000U
#define DEVICE_LINK_SERVICE_BLE_HOST_SYNC_TIMEOUT_MS 2000U

typedef struct device_link_service_bluetooth_policy
{
    uint32_t magic;
    uint8_t version;
    uint8_t enabled;
    uint16_t reserved;
} device_link_service_bluetooth_policy_t;

typedef enum
{
    DEVICE_LINK_SERVICE_LIFECYCLE_UNINITIALIZED = 0,
    DEVICE_LINK_SERVICE_LIFECYCLE_STARTING,
    DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING,
    DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING,
    DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED,
} device_link_service_lifecycle_t;

typedef enum
{
    DEVICE_LINK_SERVICE_COMMAND_OPEN = 0,
    DEVICE_LINK_SERVICE_COMMAND_CLOSE,
    DEVICE_LINK_SERVICE_COMMAND_CONFIRM_BINDING,
    DEVICE_LINK_SERVICE_COMMAND_SUSPEND,
    DEVICE_LINK_SERVICE_COMMAND_RESUME,
    DEVICE_LINK_SERVICE_COMMAND_PROCESS_LINK,
    DEVICE_LINK_SERVICE_COMMAND_REVOKE,
    DEVICE_LINK_SERVICE_COMMAND_DEINIT,
} device_link_service_command_type_t;

typedef struct device_link_service_command
{
    device_link_service_command_type_t type;
    uint32_t sequence; /**< Suspend acknowledgement sequence. */
    device_link_confirmation_token_t confirmation_token; /**< Exact local
                                                           * decision. */
    bool accept_binding; /**< Confirm (true) or deny (false). */
    ble_link_work_t *link_work; /**< Owned completed Link message. */
} device_link_service_command_t;

typedef enum
{
    DEVICE_LINK_SERVICE_STOP_NONE = 0,
    DEVICE_LINK_SERVICE_STOP_QUIESCING,
    DEVICE_LINK_SERVICE_STOP_DRAINING,
    DEVICE_LINK_SERVICE_STOP_READY,
} device_link_service_stop_phase_t;

typedef struct device_link_service
{
    device_link_service_config_t config;
    ble_runtime_config_t runtime_config; /**< Kept for the runtime lifetime. */
    device_link_service_snapshot_t snapshot;
    uint8_t queue_storage[CONFIG_DEVICE_LINK_SERVICE_QUEUE_DEPTH *
                          sizeof(device_link_service_command_t)];
    StaticQueue_t queue_control;
    QueueHandle_t queue;
    StaticSemaphore_t mutex_control;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t stopped_control;
    SemaphoreHandle_t stopped;
    TaskHandle_t task;
    StackType_t task_stack[CONFIG_DEVICE_LINK_SERVICE_TASK_STACK /
                           sizeof(StackType_t)];
    StaticTask_t task_control;
    bool runtime_started;
    bool runtime_initialized;
    device_link_service_stop_phase_t stop_phase;
    bool bluetooth_enabled;
    bool bluetooth_desired_enabled;
    bool bluetooth_transitioning;
    bool bluetooth_operation_in_flight;
    bool bluetooth_cleanup_pending;
    bool bluetooth_policy_dirty;
    uint32_t bluetooth_request_revision;
    uint8_t bluetooth_retry_attempt;
    TickType_t bluetooth_retry_not_before;
    TickType_t bluetooth_transition_deadline;
    bool bluetooth_policy_default_pending;
    esp_err_t bluetooth_policy_error;
    bool startup_gate_released;
    bool router_registered;
    bool wifi_domain_registered; /**< Startup-frozen Wi-Fi domain installed. */
    bool slow_lease_held;
    uint8_t slow_lease_id;
    bool window_open;
    bool close_pending; /**< A close failed mid-way; the tick retries it. */
    bool bindable_lease_held;
    uint8_t bindable_lease_id;
    bool client_connected;
    uint16_t client_conn_handle; /**< Accepted ACL handle, 0 when idle. */
    uint32_t client_generation; /**< Accepted ACL generation, 0 when idle. */
    uint32_t link_revision; /**< Connection/reset boundary during window waits. */
    bool status_dirty; /**< External state awaits owner publication. */
    bool status_publish_ready; /**< Initial status is already published. */
    bool window_open_pending; /**< Window deferred until the ACL is gone. */
    bool revoke_in_progress; /**< Journaled revoke awaiting completion. */
    bool suspended;
    TickType_t window_deadline;
    TickType_t last_remaining_publish;
} device_link_service_t;

static device_link_service_t s_service;
#define DEVICE_LINK_SERVICE_API_LIFECYCLE_MASK UINT64_C(0x00000000000000ff)
#define DEVICE_LINK_SERVICE_API_USER_INCREMENT UINT64_C(0x0000000000000100)
#define DEVICE_LINK_SERVICE_API_USER_MASK UINT64_C(0x00000000ffffff00)
#define DEVICE_LINK_SERVICE_API_GENERATION_MASK UINT64_C(0xffffffff00000000)
#define DEVICE_LINK_SERVICE_API_GENERATION_SHIFT 32U

/* One CAS-visible word prevents an admission that sampled an old RUNNING
 * instance from crossing a full deinit/reinit ABA cycle. */
static atomic_uint_fast64_t s_api_state = DEVICE_LINK_SERVICE_LIFECYCLE_UNINITIALIZED;
static atomic_uint s_worker_result = 0U;
static atomic_bool s_worker_exited = false;
static atomic_bool s_deinit_command_admitted = false;
/* BLE callbacks become live before the worker is created. Publish its handle
 * atomically and issue one catch-up notification after publication so work
 * retained during STARTING cannot lose its wake. */
static atomic_uintptr_t s_worker_task = (uintptr_t)NULL;
static atomic_flag s_deinit_guard = ATOMIC_FLAG_INIT;
#ifdef UNIT_TEST_HOST
    static device_link_service_test_api_acquire_hook_t s_api_acquire_hook;
    static void *s_api_acquire_hook_arg;
#endif
#define DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS 8U
#define DEVICE_LINK_SERVICE_SUSPEND_APPLIED 1U
#define DEVICE_LINK_SERVICE_SUSPEND_CANCELLED 2U

/* Per-sequence suspend outcomes: a waiter polls its own slot and can
 * distinguish "applied" from "cancelled by RESUME" even when later
 * sequences complete first (two monotonic watermarks cannot express
 * alternating per-sequence results). The slot index is seq % SLOTS and
 * the sequence/outcome pair is published as ONE atomic word so a waiter
 * can never observe a cross-generation combination. The number of
 * outstanding suspends is capped at SLOTS: allocation refuses a new
 * sequence when the outstanding count is full, so no two undecided
 * sequences ever share a slot. */
typedef struct device_link_service_suspend_result
{
    atomic_uint_fast64_t packed; /**< (outcome << 32) | sequence. */
} device_link_service_suspend_result_t;

static uint32_t s_suspend_next; /**< Caller-assigned, under the mutex. */
/* Suspend sequences whose window close failed: they are confirmed (or
 * cancelled by a RESUME) only when the close retried by the worker tick
 * succeeds, so standby preparation can rely on the window being gone.
 * One close obligation serves every pending suspend. */
static uint32_t s_suspend_pending[DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];
static size_t s_suspend_pending_count;
/* The result slots are owned by their waiter: a slot stays occupied from
 * allocation until the waiter consumes the terminal outcome (or times
 * out), so a WAIT_FOREVER waiter can never be starved by slot reuse. A
 * new allocation refuses a slot whose packed value is nonzero. */
static device_link_service_suspend_result_t
s_suspend_results[DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];

EVENT_BUS_DEFINE_ID(DEVICE_LINK_SERVICE_MSG);

static void _device_link_service_wake_worker(void *arg);
static esp_err_t _device_link_service_runtime_start(void);
static esp_err_t _device_link_service_runtime_stop(void);
static esp_err_t _device_link_service_prepare_stop(void);

static void _device_link_service_zeroize(void *data, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    for (size_t i = 0U; i < size; ++i)
    {
        bytes[i] = 0U;
    }
}

static bool _device_link_service_policy_valid(
    const device_link_service_bluetooth_policy_t *policy)
{
    return policy != NULL &&
           policy->magic == DEVICE_LINK_SERVICE_BLUETOOTH_POLICY_MAGIC &&
           policy->version == DEVICE_LINK_SERVICE_BLUETOOTH_POLICY_VERSION &&
           policy->reserved == 0U && policy->enabled <= 1U;
}

static esp_err_t _device_link_service_load_bluetooth_policy(
    bool *enabled)
{
    device_link_service_bluetooth_policy_t policy;
    size_t size = sizeof(policy);
    esp_err_t result;

    if (enabled == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&policy, 0, sizeof(policy));
    result = nv_storage_get_blob(DEVICE_LINK_SERVICE_BLUETOOTH_POLICY_KEY,
                                 &policy, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        *enabled = true;
        return ESP_OK;
    }
    if (result != ESP_OK)
    {
        *enabled = false;
        return result;
    }
    if (size != sizeof(policy) || !_device_link_service_policy_valid(&policy))
    {
        _device_link_service_zeroize(&policy, sizeof(policy));
        *enabled = false;
        return ESP_ERR_INVALID_STATE;
    }
    *enabled = policy.enabled != 0U;
    _device_link_service_zeroize(&policy, sizeof(policy));
    return ESP_OK;
}

static esp_err_t _device_link_service_store_bluetooth_policy(bool enabled)
{
    const device_link_service_bluetooth_policy_t policy =
    {
        .magic = DEVICE_LINK_SERVICE_BLUETOOTH_POLICY_MAGIC,
        .version = DEVICE_LINK_SERVICE_BLUETOOTH_POLICY_VERSION,
        .enabled = enabled ? 1U : 0U,
        .reserved = 0U,
    };

    return nv_storage_set_blob(DEVICE_LINK_SERVICE_BLUETOOTH_POLICY_KEY,
                               &policy, sizeof(policy));
}

static device_link_service_lifecycle_t _device_link_service_lifecycle(void)
{
    return (device_link_service_lifecycle_t)(atomic_load_explicit(
                &s_api_state, memory_order_acquire) &
            DEVICE_LINK_SERVICE_API_LIFECYCLE_MASK);
}

static bool _device_link_service_transition(
    device_link_service_lifecycle_t from,
    device_link_service_lifecycle_t to)
{
    uint_fast64_t state = atomic_load_explicit(
                              &s_api_state, memory_order_acquire);

    while ((state & DEVICE_LINK_SERVICE_API_LIFECYCLE_MASK) ==
            (uint_fast64_t)from)
    {
        const uint_fast64_t desired =
            (state & ~DEVICE_LINK_SERVICE_API_LIFECYCLE_MASK) |
            (uint_fast64_t)to;

        if (atomic_compare_exchange_weak_explicit(
                    &s_api_state, &state, desired,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

static bool _device_link_service_begin_instance(void)
{
    uint_fast64_t state = atomic_load_explicit(
                              &s_api_state, memory_order_acquire);

    for (;;)
    {
        const device_link_service_lifecycle_t lifecycle =
            (device_link_service_lifecycle_t)(
                state & DEVICE_LINK_SERVICE_API_LIFECYCLE_MASK);

        if ((lifecycle != DEVICE_LINK_SERVICE_LIFECYCLE_UNINITIALIZED &&
                lifecycle != DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED) ||
                (state & DEVICE_LINK_SERVICE_API_USER_MASK) != 0U)
        {
            return false;
        }
        const uint_fast64_t generation =
            (state & DEVICE_LINK_SERVICE_API_GENERATION_MASK) >>
            DEVICE_LINK_SERVICE_API_GENERATION_SHIFT;

        if (generation == UINT32_MAX)
        {
            return false;
        }
        const uint_fast64_t desired =
            ((generation + 1U) <<
             DEVICE_LINK_SERVICE_API_GENERATION_SHIFT) |
            DEVICE_LINK_SERVICE_LIFECYCLE_STARTING;

        if (atomic_compare_exchange_weak_explicit(
                    &s_api_state, &state, desired,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return true;
        }
    }
}

static bool _device_link_service_api_acquire_common(bool allow_starting)
{
    uint_fast64_t state = atomic_load_explicit(
                              &s_api_state, memory_order_acquire);
#ifdef UNIT_TEST_HOST
    if (s_api_acquire_hook != NULL)
    {
        s_api_acquire_hook(s_api_acquire_hook_arg);
    }
#endif
    const uint_fast64_t generation =
        state & DEVICE_LINK_SERVICE_API_GENERATION_MASK;

    while ((state & DEVICE_LINK_SERVICE_API_GENERATION_MASK) == generation)
    {
        const device_link_service_lifecycle_t lifecycle =
            (device_link_service_lifecycle_t)(
                state & DEVICE_LINK_SERVICE_API_LIFECYCLE_MASK);

        if (lifecycle != DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING &&
                !(allow_starting &&
                  lifecycle == DEVICE_LINK_SERVICE_LIFECYCLE_STARTING))
        {
            return false;
        }
        if ((state & DEVICE_LINK_SERVICE_API_USER_MASK) ==
                DEVICE_LINK_SERVICE_API_USER_MASK)
        {
            return false;
        }
        const uint_fast64_t desired =
            state + DEVICE_LINK_SERVICE_API_USER_INCREMENT;

        if (atomic_compare_exchange_weak_explicit(
                    &s_api_state, &state, desired,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

static bool _device_link_service_api_acquire(void)
{
    return _device_link_service_api_acquire_common(false);
}

/**
 * @brief Early admission for the BLE event callback only.
 *
 * STARTING is admitted so a connection accepted between the router
 * registration and the RUNNING store is tracked; the count keeps teardown
 * from deleting the mutex underneath an in-flight callback. Public APIs
 * keep the strict RUNNING-only admission so they can never touch
 * partially initialized handles.
 */
static bool _device_link_service_api_acquire_early(void)
{
    return _device_link_service_api_acquire_common(true);
}

static void _device_link_service_api_release(void)
{
    atomic_fetch_sub_explicit(&s_api_state,
                              DEVICE_LINK_SERVICE_API_USER_INCREMENT,
                              memory_order_release);
}

#ifdef UNIT_TEST_HOST
void device_link_service_test_set_api_acquire_hook(
    device_link_service_test_api_acquire_hook_t hook, void *arg)
{
    s_api_acquire_hook = hook;
    s_api_acquire_hook_arg = arg;
}
#endif

static esp_err_t _device_link_service_submit_link_work(
    ble_link_work_t *work, void *arg)
{
    (void)arg;
    if (work == NULL || !_device_link_service_api_acquire_early())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const device_link_service_command_t command =
    {
        .type = DEVICE_LINK_SERVICE_COMMAND_PROCESS_LINK,
        .link_work = work,
    };
    const esp_err_t result = xQueueSend(s_service.queue, &command, 0U) ==
                             pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;

    if (result == ESP_OK && s_service.bluetooth_enabled)
    {
        _device_link_service_wake_worker(NULL);
    }
    _device_link_service_api_release();
    return result;
}

static void _device_link_service_publish_now(
    const device_link_service_snapshot_t *snapshot)
{
    (void)event_bus_publish(
        DEVICE_LINK_SERVICE_MSG,
        DEVICE_LINK_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
        snapshot, sizeof(*snapshot), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

static bool _device_link_service_tick_reached(TickType_t now,
        TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static uint32_t _device_link_service_remaining_ms(TickType_t deadline)
{
    const TickType_t now = xTaskGetTickCount();

    if (!_device_link_service_tick_reached(now, deadline))
    {
        const TickType_t remaining = deadline - now;

        return (uint32_t)(((uint64_t)remaining * 1000U +
                           configTICK_RATE_HZ - 1U) / configTICK_RATE_HZ);
    }
    return 0U;
}

static uint32_t _device_link_service_min_wait(
    uint32_t current, uint32_t candidate)
{
    return candidate < current ? candidate : current;
}

static TickType_t _device_link_service_wait_ticks(uint32_t wait_ms)
{
    if (wait_ms == UINT32_MAX)
    {
        return portMAX_DELAY;
    }
    TickType_t wait = pdMS_TO_TICKS(wait_ms);

    if (wait_ms > 0U && wait == 0U)
    {
        wait = 1U;
    }
    return wait;
}

static TickType_t _device_link_service_remaining_timeout(
    TickType_t started, TickType_t timeout)
{
    if (timeout == portMAX_DELAY)
    {
        return portMAX_DELAY;
    }
    const TickType_t elapsed = xTaskGetTickCount() - started;

    return elapsed < timeout ? timeout - elapsed : 0U;
}

static esp_err_t _device_link_service_deinit_return(esp_err_t result)
{
    atomic_flag_clear_explicit(&s_deinit_guard, memory_order_release);
    return result;
}

static uint32_t _device_link_service_next_wait_ms(void)
{
    uint32_t wait_ms = UINT32_MAX;
    const uint32_t operation_timeout_ms =
        ble_link_service_operation_timeout_remaining_ms();

    if (operation_timeout_ms != UINT32_MAX)
    {
        wait_ms = _device_link_service_min_wait(
                      wait_ms, operation_timeout_ms);
    }

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (s_service.bluetooth_transitioning)
    {
        const TickType_t retry_deadline = s_service.bluetooth_retry_not_before;

        wait_ms = _device_link_service_min_wait(
                      wait_ms,
                      _device_link_service_remaining_ms(retry_deadline));
    }
    if (s_service.close_pending || s_service.revoke_in_progress ||
            s_service.window_open_pending)
    {
        wait_ms = _device_link_service_min_wait(
                      wait_ms, DEVICE_LINK_SERVICE_RETRY_MS);
    }
    if (s_service.window_open)
    {
        wait_ms = _device_link_service_min_wait(
                      wait_ms,
                      _device_link_service_remaining_ms(
                          s_service.window_deadline));
        const TickType_t publish_deadline =
            s_service.last_remaining_publish +
            pdMS_TO_TICKS(DEVICE_LINK_SERVICE_REMAINING_PUBLISH_MS);

        wait_ms = _device_link_service_min_wait(
                      wait_ms,
                      _device_link_service_remaining_ms(publish_deadline));
    }
    xSemaphoreGive(s_service.mutex);
    return wait_ms;
}

static device_link_service_state_t _device_link_service_derive_state(
    ble_adv_manager_state_t adv)
{
    if (s_service.bluetooth_policy_error != ESP_OK)
    {
        return DEVICE_LINK_SERVICE_STATE_ERROR;
    }
    if (s_service.bluetooth_transitioning)
    {
        return DEVICE_LINK_SERVICE_STATE_STARTING;
    }
    if (s_service.bluetooth_cleanup_pending)
    {
        return DEVICE_LINK_SERVICE_STATE_ERROR;
    }
    if (!s_service.bluetooth_enabled)
    {
        return DEVICE_LINK_SERVICE_STATE_DISABLED;
    }
    if (s_service.client_connected)
    {
        return DEVICE_LINK_SERVICE_STATE_CONNECTED;
    }
    if (s_service.suspended || !s_service.startup_gate_released)
    {
        return DEVICE_LINK_SERVICE_STATE_SUSPENDED;
    }
    if (adv == BLE_ADV_MANAGER_STATE_FAULTED)
    {
        return DEVICE_LINK_SERVICE_STATE_ERROR;
    }
    if (s_service.window_open || s_service.window_open_pending)
    {
        return DEVICE_LINK_SERVICE_STATE_WINDOW;
    }
    return adv == BLE_ADV_MANAGER_STATE_FAST || adv == BLE_ADV_MANAGER_STATE_SLOW
           ? DEVICE_LINK_SERVICE_STATE_ADVERTISING
           : DEVICE_LINK_SERVICE_STATE_STARTING;
}

static bool _device_link_service_sync_confirmation_locked(void)
{
    const ble_link_confirmation_snapshot_t confirmation =
        ble_link_service_get_confirmation();
    const bool changed = device_link_confirmation_sync(
                             &s_service.snapshot, &confirmation);

    if (changed)
    {
        LOG_I("confirmation pending=%u token=%" PRIu64 " generation=%" PRIu64,
              confirmation.pending, confirmation.token,
              s_service.snapshot.generation + 1U);
    }
    return changed;
}

static void _device_link_service_refresh_snapshot_locked(void)
{
    const ble_adv_manager_state_t adv = ble_adv_manager_get_state();

    s_service.snapshot.state = _device_link_service_derive_state(adv);
    s_service.snapshot.enabled = s_service.bluetooth_enabled;
    s_service.snapshot.desired_enabled = s_service.bluetooth_desired_enabled;
    s_service.snapshot.policy_error = s_service.bluetooth_policy_error;
    s_service.snapshot.transitioning = s_service.bluetooth_transitioning;
    s_service.snapshot.public_discovery =
        (adv == BLE_ADV_MANAGER_STATE_FAST || adv == BLE_ADV_MANAGER_STATE_SLOW) &&
        s_service.bluetooth_enabled &&
        s_service.slow_lease_held &&
        !s_service.window_open &&
        !s_service.client_connected;
    s_service.snapshot.active = s_service.window_open ||
                                s_service.window_open_pending;
    (void)_device_link_service_sync_confirmation_locked();
    s_service.snapshot.bound =
        (ble_link_session_get_state_flags() &
         BLE_LINK_STATE_FLAG_BOUND) != 0U;
    s_service.snapshot.client_connected = s_service.client_connected;
    s_service.snapshot.window_remaining_ms =
        s_service.window_open ?
        _device_link_service_remaining_ms(s_service.window_deadline) : 0U;
    s_service.status_dirty = false;
}

static void _device_link_service_wake_worker(void *arg)
{
    (void)arg;
    TaskHandle_t task = (TaskHandle_t)(uintptr_t)atomic_load_explicit(
                            &s_worker_task, memory_order_acquire);

    if (task != NULL)
    {
        (void)xTaskNotifyGive(task);
    }
}

static esp_err_t _device_link_service_continue_revoke(void)
{
    return device_link_revoke_progress();
}

static esp_err_t _device_link_service_close_window_locked(void);

static esp_err_t _device_link_service_open_window_locked(void)
{
    if (!s_service.startup_gate_released || !s_service.bluetooth_desired_enabled ||
            s_service.bluetooth_transitioning || s_service.bluetooth_cleanup_pending)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.revoke_in_progress)
    {
        /* A binding window must never open while a revoke is pending: a
         * new bootstrap binding would race the durable deletion. This is
         * checked before the idempotent window_open return so the error
         * is reported even when a stale window is still being closed. */
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.close_pending)
    {
        /* The previous close did not complete its teardown obligation. A
         * new window must not open on top of it. Fail closed until the
         * worker retry finishes the close. */
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.window_open)
    {
        return ESP_OK;
    }
    if (s_service.bindable_lease_held)
    {
        /* The old bindable lease could not be released: opening a new
         * window would overwrite its lease id and leave the old
         * advertisement live forever. Fail closed until the close
         * retried by the worker tick succeeds. */
        return ESP_ERR_INVALID_STATE;
    }
    if ((ble_link_session_get_state_flags() &
            BLE_LINK_STATE_FLAG_BOUND) != 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.client_connected)
    {
        /* The single-ACL model keeps the accepted connection; the pairing
         * window may only open after it is gone, otherwise the existing
         * long-term session could survive into the window and the device
         * could miss the pairing window entirely. Request the disconnect
         * and defer the open to the worker tick, which retries the request
         * until the ACL is gone. */
        s_service.window_open_pending = true;
        xSemaphoreGive(s_service.mutex);
        const esp_err_t disconnect_result =
            ble_nimble_port_request_disconnect();

        xSemaphoreTake(s_service.mutex, portMAX_DELAY);

        if (disconnect_result != ESP_OK)
        {
            s_service.snapshot.last_error = disconnect_result;
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }
    const uint32_t revision = s_service.link_revision;
    ble_adv_lease_t lease = {0};

    /* The worker owns window/lease mutations. Host callbacks and target
     * requests remain free to acquire the service mutex during every wait. */
    xSemaphoreGive(s_service.mutex);
    esp_err_t result = ble_adv_manager_set_pause_reason(
                           BLE_ADV_MANAGER_PAUSE_REASON_WINDOW_TRANSITION, true);

    if (result == ESP_OK)
    {
        ble_link_session_set_pairing_window(true);
        ble_link_service_set_pairing_window(true);
        result = ble_adv_manager_acquire_lease(&lease, BLE_ADV_MANAGER_MODE_FAST, true);
    }
    if (result == ESP_OK)
    {
        result = s_service.config.runtime_port->set_pairing_gate(true);
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (lease.lease_id != 0U)
    {
        s_service.bindable_lease_held = true;
        s_service.bindable_lease_id = lease.lease_id;
    }
    if (result == ESP_OK &&
            (!s_service.bluetooth_desired_enabled || s_service.client_connected ||
             s_service.link_revision != revision ||
             _device_link_service_lifecycle() != DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING))
    {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result == ESP_OK)
    {
        xSemaphoreGive(s_service.mutex);
        result = ble_adv_manager_set_pause_reason(
                     BLE_ADV_MANAGER_PAUSE_REASON_WINDOW_TRANSITION, false);
        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        if (result == ESP_OK && (!s_service.bluetooth_desired_enabled ||
                                 s_service.link_revision != revision))
        {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    if (result == ESP_OK)
    {
        s_service.window_open = true;
        s_service.suspended = false;
        s_service.window_deadline = xTaskGetTickCount() +
                                    pdMS_TO_TICKS(s_service.config.window_ms);
        s_service.last_remaining_publish = xTaskGetTickCount();
        s_service.snapshot.last_error = ESP_OK;
    }
    else
    {
        s_service.close_pending = true;
        const esp_err_t cleanup = _device_link_service_close_window_locked();

        if (cleanup != ESP_OK)
        {
            LOG_W("window open cleanup pending result=%d", cleanup);
        }
    }
    _device_link_service_zeroize(&lease, sizeof(lease));
    return result;
}

static void _device_link_service_record_suspend_result_locked(
    uint32_t sequence, uint32_t outcome)
{
    if (sequence == 0U)
    {
        return;
    }
    device_link_service_suspend_result_t *slot =
        &s_suspend_results[sequence % DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];
    uint64_t expected = sequence; /* The pending claim (outcome 0). */
    const uint64_t packed = ((uint64_t)outcome << 32U) | sequence;

    /* Transition only this sequence's own claim to the terminal outcome.
     * If the waiter already timed out and released the slot, the CAS
     * fails and the outcome is not resurrected. */
    (void)atomic_compare_exchange_strong_explicit(
        &slot->packed, &expected, packed, memory_order_acq_rel,
        memory_order_acquire);
}

static void _device_link_service_confirm_pending_suspend_locked(void)
{
    if (s_suspend_pending_count == 0U)
    {
        return;
    }
    s_service.suspended = true;
    for (size_t i = 0U; i < s_suspend_pending_count; ++i)
    {
        _device_link_service_record_suspend_result_locked(
            s_suspend_pending[i], DEVICE_LINK_SERVICE_SUSPEND_APPLIED);
        s_suspend_pending[i] = 0U;
    }
    s_suspend_pending_count = 0U;
}

static void _device_link_service_cancel_pending_suspend_locked(void)
{
    for (size_t i = 0U; i < s_suspend_pending_count; ++i)
    {
        _device_link_service_record_suspend_result_locked(
            s_suspend_pending[i], DEVICE_LINK_SERVICE_SUSPEND_CANCELLED);
        s_suspend_pending[i] = 0U;
    }
    s_suspend_pending_count = 0U;
}

static void _device_link_service_clear_link_session(void)
{
    ble_nimble_port_numeric_comparison_cancel();
    ble_link_service_clear_session_state();
}

static esp_err_t _device_link_service_close_window_locked(void)
{
    esp_err_t result = ESP_OK;

    /* Every close boundary cancels a deferred OPEN first: a CLOSE, SUSPEND,
     * or REVOKE while the window waits for the ACL to disconnect must not
     * let the window open later on the disconnect event. */
    s_service.window_open_pending = false;
    if (!s_service.window_open && !s_service.bindable_lease_held &&
            !s_service.close_pending)
    {
        return ESP_OK;
    }
    const bool held = s_service.bindable_lease_held;
    const uint8_t lease_id = s_service.bindable_lease_id;
    bool released = !held;

    s_service.close_pending = true;
    xSemaphoreGive(s_service.mutex);
    result = ble_adv_manager_set_pause_reason(
                 BLE_ADV_MANAGER_PAUSE_REASON_WINDOW_TRANSITION, true);
    if (result == ESP_OK && held)
    {
        result = ble_adv_manager_release_lease(lease_id);
        if (result == ESP_OK || result == ESP_ERR_NOT_FOUND)
        {
            released = true;
            result = ESP_OK;
        }
    }
    if (result == ESP_OK)
    {
        result = s_service.config.runtime_port->set_pairing_gate(false);
    }
    if (result == ESP_OK)
    {
        ble_link_session_set_pairing_window(false);
        ble_link_service_set_pairing_window(false);
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (released)
    {
        s_service.bindable_lease_held = false;
        s_service.bindable_lease_id = 0U;
    }
    if (result != ESP_OK)
    {
        return result;
    }
    s_service.window_open = false;
    s_service.window_deadline = 0U;
    const uint32_t flags = ble_link_session_get_state_flags();
    const bool verified_bound = (flags & BLE_LINK_STATE_FLAG_BOUND) != 0U &&
                                (flags & BLE_LINK_STATE_FLAG_AUTHENTICATED) != 0U;
    const ble_link_confirmation_snapshot_t confirmation =
        ble_link_service_get_confirmation();
    const bool retire_session = !verified_bound || confirmation.pending;
    const bool drop_acl = s_service.client_connected && retire_session;
    const ble_link_operation_identity_t identity =
    {
        .generation = s_service.client_generation,
        .conn_handle = s_service.client_conn_handle,
    };

    xSemaphoreGive(s_service.mutex);
    if (drop_acl)
    {
        result = ble_nimble_port_request_disconnect();
    }
    if (retire_session)
    {
        if (identity.generation != 0U)
        {
            (void)ble_link_service_clear_session_state_if_current(&identity);
        }
        ble_nimble_port_numeric_comparison_cancel();
    }
    if (result == ESP_OK)
    {
        result = ble_adv_manager_set_pause_reason(
                     BLE_ADV_MANAGER_PAUSE_REASON_WINDOW_TRANSITION, false);
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    s_service.close_pending = result != ESP_OK;
    return result;
}

static void _device_link_service_ble_event(
    const ble_port_event_t *event, void *arg)
{
    (void)arg;
    bool state_changed = false;

    if (event->type != BLE_PORT_EVENT_CONNECT &&
            event->type != BLE_PORT_EVENT_DISCONNECT &&
            event->type != BLE_PORT_EVENT_RESET &&
            event->type != BLE_PORT_EVENT_ADV_STARTED &&
            event->type != BLE_PORT_EVENT_ADV_STOPPED)
    {
        return;
    }
    if (!_device_link_service_api_acquire_early())
    {
        return;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    /* Connection state is tracked from STARTING (the router is already
     * registered and advertising may be live), so a connection accepted
     * before the RUNNING store is not lost; only the publication is gated
     * on RUNNING. */
    if (event->type == BLE_PORT_EVENT_CONNECT && event->status == 0 &&
            event->accepted && event->identity.generation != 0U &&
            event->identity.flow_id == 0U && event->identity.token == 0U &&
            event->identity.kind == BLE_LINK_OPERATION_CONNECT &&
            event->identity.conn_handle == event->conn_handle)
    {
        s_service.client_connected = true;
        s_service.client_conn_handle = event->conn_handle;
        s_service.client_generation = event->identity.generation;
        state_changed = true;
    }
    else if (event->type == BLE_PORT_EVENT_DISCONNECT &&
             event->conn_handle == s_service.client_conn_handle &&
             event->identity.generation == s_service.client_generation &&
             event->identity.flow_id == 0U && event->identity.token == 0U &&
             event->identity.kind == BLE_LINK_OPERATION_DISCONNECT &&
             event->identity.conn_handle == event->conn_handle)
    {
        /* A late disconnect for a retired connection must not clear a
         * newer one. */
        s_service.client_connected = false;
        s_service.client_conn_handle = 0U;
        s_service.client_generation = 0U;
        state_changed = true;
    }
    else if (event->type == BLE_PORT_EVENT_RESET)
    {
        s_service.client_connected = false;
        s_service.client_conn_handle = 0U;
        s_service.client_generation = 0U;
        state_changed = true;
    }
    if (state_changed)
    {
        s_service.link_revision++;
    }
    if (event->type == BLE_PORT_EVENT_ADV_STARTED ||
            event->type == BLE_PORT_EVENT_ADV_STOPPED)
    {
        state_changed = true;
        s_service.snapshot.last_error = event->status;
    }
    if (state_changed)
    {
        s_service.status_dirty = true;
    }
    xSemaphoreGive(s_service.mutex);
    if (state_changed)
    {
        _device_link_service_wake_worker(NULL);
    }
    _device_link_service_api_release();
}

static uint32_t _device_link_service_bluetooth_retry_delay_ms(
    uint8_t attempt)
{
    uint32_t delay = DEVICE_LINK_SERVICE_RETRY_MS;

    for (uint8_t i = 0U; i < attempt && delay <
            DEVICE_LINK_SERVICE_BLUETOOTH_RETRY_MAX_MS; ++i)
    {
        delay *= 2U;
    }
    return delay > DEVICE_LINK_SERVICE_BLUETOOTH_RETRY_MAX_MS ?
           DEVICE_LINK_SERVICE_BLUETOOTH_RETRY_MAX_MS : delay;
}

static bool _device_link_service_bluetooth_settled_locked(void)
{
    return s_service.stop_phase == DEVICE_LINK_SERVICE_STOP_NONE &&
           !s_service.bluetooth_operation_in_flight &&
           !s_service.bluetooth_cleanup_pending &&
           !s_service.bluetooth_policy_dirty &&
           s_service.bluetooth_desired_enabled == s_service.bluetooth_enabled;
}

static void _device_link_service_bt_power_step(void)
{
    device_link_service_snapshot_t snapshot;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (!s_service.bluetooth_transitioning)
    {
        xSemaphoreGive(s_service.mutex);
        return;
    }
    if (_device_link_service_bluetooth_settled_locked())
    {
        s_service.bluetooth_transitioning = false;
        s_service.bluetooth_retry_attempt = 0U;
        s_service.bluetooth_retry_not_before = 0U;
        s_service.bluetooth_transition_deadline = 0U;
        _device_link_service_refresh_snapshot_locked();
        s_service.snapshot.generation++;
        snapshot = s_service.snapshot;
        xSemaphoreGive(s_service.mutex);
        _device_link_service_publish_now(&snapshot);
        return;
    }
    const TickType_t now = xTaskGetTickCount();

    if (s_service.bluetooth_retry_not_before != 0U &&
            (int32_t)(now - s_service.bluetooth_retry_not_before) < 0)
    {
        xSemaphoreGive(s_service.mutex);
        return;
    }
    if (s_service.bluetooth_transition_deadline == 0U)
    {
        s_service.bluetooth_transition_deadline = now +
            pdMS_TO_TICKS(DEVICE_LINK_SERVICE_BLUETOOTH_TRANSITION_TIMEOUT_MS);
    }
    const bool target = s_service.bluetooth_desired_enabled;
    const bool stopping = s_service.stop_phase != DEVICE_LINK_SERVICE_STOP_NONE;
    const bool persist_policy = s_service.bluetooth_policy_dirty;
    const uint32_t request_revision = s_service.bluetooth_request_revision;
    const TickType_t deadline = s_service.bluetooth_transition_deadline;

    s_service.bluetooth_operation_in_flight = true;
    xSemaphoreGive(s_service.mutex);

    const esp_err_t store = persist_policy ?
                            _device_link_service_store_bluetooth_policy(target) : ESP_OK;
    const esp_err_t result = store == ESP_OK
                             ? (target && !stopping ? _device_link_service_runtime_start()
                                : _device_link_service_runtime_stop())
                             : store;
    const bool running = s_service.stop_phase == DEVICE_LINK_SERVICE_STOP_NONE &&
                         s_service.runtime_started &&
                         ble_runtime_get_state() == BLE_RUNTIME_STATE_RUNNING;
    const bool cleanup_pending = s_service.stop_phase != DEVICE_LINK_SERVICE_STOP_NONE ||
                                 (s_service.runtime_initialized && !running);
    const bool expired = (int32_t)(xTaskGetTickCount() -
                                   deadline) >= 0;
    const bool waiting = result == ESP_ERR_NOT_FINISHED;
    const esp_err_t operation_error = waiting ? ESP_ERR_TIMEOUT : result;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    s_service.bluetooth_operation_in_flight = false;
    s_service.bluetooth_cleanup_pending = cleanup_pending;
    s_service.bluetooth_enabled = running;
    const bool superseded = s_service.bluetooth_request_revision != request_revision;
    bool restore_policy = false;

    s_service.bluetooth_policy_dirty = superseded || store != ESP_OK;
    if (superseded)
    {
        /* A completed older operation updates resource facts only. The latest
         * request owns its error and retry budget. */
        s_service.bluetooth_retry_attempt = 0U;
        s_service.bluetooth_retry_not_before = 0U;
        s_service.bluetooth_transition_deadline = 0U;
        s_service.bluetooth_transitioning =
            !_device_link_service_bluetooth_settled_locked();
        /* A new target still owns a pending NVS write, even when it equals
         * the state reached by this older operation. */
    }
    else if (waiting && !expired)
    {
        /* Retained cleanup is progress, not a failed start/stop attempt. */
        s_service.bluetooth_policy_error = ESP_OK;
        s_service.snapshot.last_error = ESP_OK;
        s_service.bluetooth_retry_not_before = xTaskGetTickCount() +
                                               pdMS_TO_TICKS(DEVICE_LINK_SERVICE_RETRY_MS);
        s_service.bluetooth_transitioning = true;
    }
    else if (result == ESP_OK)
    {
        s_service.bluetooth_policy_error = ESP_OK;
        s_service.snapshot.last_error = ESP_OK;
        s_service.bluetooth_retry_attempt = 0U;
        s_service.bluetooth_retry_not_before = 0U;
        s_service.bluetooth_transition_deadline = 0U;
        s_service.bluetooth_transitioning =
            !_device_link_service_bluetooth_settled_locked();
    }
    else
    {
        s_service.bluetooth_policy_error = operation_error;
        s_service.snapshot.last_error = operation_error;

        if (store != ESP_OK || expired ||
                s_service.bluetooth_retry_attempt >=
                DEVICE_LINK_SERVICE_BLUETOOTH_RETRY_MAX_ATTEMPTS)
        {
            /* Terminal error is not successful convergence. Retain cleanup
             * ownership and allow a later request/deinit to finish it. */
            s_service.bluetooth_desired_enabled = running;
            s_service.bluetooth_transitioning = false;
            s_service.bluetooth_retry_not_before = 0U;
            s_service.bluetooth_transition_deadline = 0U;
            restore_policy = store == ESP_OK;
        }
        else
        {
            s_service.bluetooth_retry_not_before = xTaskGetTickCount() +
                                                   pdMS_TO_TICKS(_device_link_service_bluetooth_retry_delay_ms(
                                                           s_service.bluetooth_retry_attempt++));
            s_service.bluetooth_transitioning = true;
        }
    }
    if (restore_policy)
    {
        const bool effective = s_service.bluetooth_enabled;
        const uint32_t revision = s_service.bluetooth_request_revision;

        /* Requests may arrive during NVS I/O. Keep the operation visible and
         * never overwrite the target installed by those requests. */
        s_service.bluetooth_operation_in_flight = true;
        s_service.bluetooth_transitioning = true;
        xSemaphoreGive(s_service.mutex);
        const esp_err_t persist = _device_link_service_store_bluetooth_policy(effective);

        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        s_service.bluetooth_operation_in_flight = false;
        s_service.bluetooth_policy_dirty = persist != ESP_OK ||
                                           s_service.bluetooth_desired_enabled != effective;
        s_service.bluetooth_transitioning =
            s_service.bluetooth_desired_enabled != effective ||
            (s_service.bluetooth_request_revision != revision &&
             !_device_link_service_bluetooth_settled_locked());
        if (persist != ESP_OK)
        {
            s_service.bluetooth_policy_error = persist;
            s_service.snapshot.last_error = persist;
        }
    }
    _device_link_service_refresh_snapshot_locked();
    s_service.snapshot.generation++;
    snapshot = s_service.snapshot;
    xSemaphoreGive(s_service.mutex);
    _device_link_service_publish_now(&snapshot);
}

static void _device_link_service_handle_command(
    const device_link_service_command_t *command)
{
    if (command->type == DEVICE_LINK_SERVICE_COMMAND_PROCESS_LINK)
    {
        const esp_err_t result = ble_link_service_execute(command->link_work);

        ble_link_service_release_work(command->link_work);
        if (result == ESP_OK)
        {
            return;
        }
        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
        {
            s_service.snapshot.last_error = result;
        }
        _device_link_service_refresh_snapshot_locked();
        s_service.snapshot.generation++;
        device_link_service_snapshot_t snapshot = s_service.snapshot;

        xSemaphoreGive(s_service.mutex);
        _device_link_service_publish_now(&snapshot);
        return;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    switch (command->type)
    {
    case DEVICE_LINK_SERVICE_COMMAND_OPEN:
    {
        const esp_err_t open_result = s_service.suspended ?
                                      ESP_ERR_INVALID_STATE :
                                      _device_link_service_open_window_locked();

        s_service.snapshot.last_error = open_result;
        break;
    }
    case DEVICE_LINK_SERVICE_COMMAND_CLOSE:
        s_service.snapshot.last_error =
            _device_link_service_close_window_locked();
        if (s_service.snapshot.last_error == ESP_OK)
        {
            /* A successful close satisfies any pending suspend. */
            _device_link_service_confirm_pending_suspend_locked();
        }
        break;
    case DEVICE_LINK_SERVICE_COMMAND_CONFIRM_BINDING:
    {
        const ble_link_confirmation_snapshot_t confirmation =
            ble_link_service_get_confirmation();

        if (!confirmation.pending ||
                command->confirmation_token == 0U ||
                command->confirmation_token != confirmation.token)
        {
            s_service.snapshot.last_error = ESP_ERR_INVALID_STATE;
            break;
        }
        xSemaphoreGive(s_service.mutex);
        const esp_err_t confirm_result =
            ble_nimble_port_numeric_comparison_reply(
                command->confirmation_token, command->accept_binding);
        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        s_service.snapshot.last_error = confirm_result;
        _device_link_service_refresh_snapshot_locked();
        break;
    }
    case DEVICE_LINK_SERVICE_COMMAND_SUSPEND:
        /* Suspend always ends with no window and the suspended flag set,
         * so an OPEN that raced into the FIFO first cannot leave a window
         * open across standby. The sequence advances only once the window
         * close succeeded: a stuck bindable lease keeps the close pending,
         * and the acknowledgement is deferred to the worker-tick retry so
         * standby preparation can rely on the window being gone. */
        s_service.snapshot.last_error =
            _device_link_service_close_window_locked();
        if (s_service.snapshot.last_error == ESP_OK)
        {
            s_service.suspended = true;
            _device_link_service_record_suspend_result_locked(
                command->sequence, DEVICE_LINK_SERVICE_SUSPEND_APPLIED);
            /* Previously failed suspends are also satisfied by this
             * successful close. */
            _device_link_service_confirm_pending_suspend_locked();
        }
        else if (s_suspend_pending_count <
                 DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS)
        {
            s_suspend_pending[s_suspend_pending_count] = command->sequence;
            s_suspend_pending_count++;
        }
        else
        {
            /* The pending set is full: the close cannot be retried for
             * this sequence within the bounded bookkeeping; fail it
             * closed so the caller times out deterministically. */
            s_service.snapshot.last_error = ESP_ERR_INVALID_STATE;
        }
        break;
    case DEVICE_LINK_SERVICE_COMMAND_RESUME:
        /* Resume only restores the pre-suspend idle state; it never opens a
         * pairing window. A window is opened only by an explicit user
         * action through device_link_service_open_window(). A suspend
         * whose close is still pending is cancelled here: its caller must
         * time out instead of being confirmed later (the cancelled
         * watermark keeps a later sequence from confirming it). */
        s_service.suspended = false;
        _device_link_service_cancel_pending_suspend_locked();
        s_service.snapshot.last_error = ESP_OK;
        break;
    case DEVICE_LINK_SERVICE_COMMAND_REVOKE:
    {
        /* Local revoke: journal the durable intent first, close the
         * pairing window so no new bootstrap binding races the revoke,
         * then erase the authorization and its verifier, drop the session
         * state, and let the port delete the bond/CCCD on the host core
         * and clear the journal. A crash at any point resumes at startup
         * before advertising; a failure here keeps the obligation and the
         * worker tick retries until the revoke completes. */
        bool journal_written = s_service.revoke_in_progress;
        esp_err_t revoke_result = ESP_OK;

        if (!journal_written)
        {
            xSemaphoreGive(s_service.mutex);
            revoke_result = ble_nimble_port_revoke_journal_begin();
            xSemaphoreTake(s_service.mutex, portMAX_DELAY);
            journal_written = revoke_result == ESP_OK;
        }
        if (revoke_result == ESP_OK && journal_written)
        {
            /* Transfer the durable revoke journal into an independent ADV
             * pause before WINDOW_TRANSITION can be released. This service
             * owner releases REVOKE only after it observes the journal gone;
             * the NimBLE owner independently holds REVOKE_PORT while deleting
             * the peer store. */
            revoke_result = ble_adv_manager_set_pause_reason(
                                BLE_ADV_MANAGER_PAUSE_REASON_REVOKE, true);
        }
        if (revoke_result == ESP_OK)
        {
            if (!journal_written || s_service.window_open ||
                    s_service.window_open_pending ||
                    s_service.bindable_lease_held ||
                    s_service.close_pending)
            {
                revoke_result = _device_link_service_close_window_locked();
                if (revoke_result == ESP_OK)
                {
                    _device_link_service_confirm_pending_suspend_locked();
                }
            }
        }
        if (journal_written)
        {
            /* The worker tick continues the durable storage/port operation
             * after this command releases the service mutex. */
            s_service.revoke_in_progress = true;
        }
        /* A revoke request is a local security boundary even when a storage
         * step failed. Retire the live application session immediately. */
        xSemaphoreGive(s_service.mutex);
        (void)ble_link_session_set_authorization(false, 0U);
        _device_link_service_clear_link_session();
        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        s_service.snapshot.last_error = revoke_result;
        break;
    }
    case DEVICE_LINK_SERVICE_COMMAND_DEINIT:
        break;
    default:
        s_service.snapshot.last_error = ESP_ERR_INVALID_ARG;
        break;
    }
    _device_link_service_refresh_snapshot_locked();
    s_service.snapshot.generation++;
    {
        device_link_service_snapshot_t snapshot = s_service.snapshot;

        xSemaphoreGive(s_service.mutex);
        _device_link_service_publish_now(&snapshot);
    }
}

static void _device_link_service_worker_tick(void)
{
    bool publish = false;
    device_link_service_snapshot_t snapshot;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    const bool owner_ready = s_service.status_publish_ready &&
                             _device_link_service_lifecycle() == DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING;
    const bool runtime_ready = s_service.runtime_started &&
                               s_service.stop_phase == DEVICE_LINK_SERVICE_STOP_NONE;
    xSemaphoreGive(s_service.mutex);
    if (!owner_ready)
    {
        return;
    }
    device_link_wifi_adapter_tick();
    ble_link_service_tick((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    if (!runtime_ready)
    {
        return;
    }

    /* Transport work requires a live radio; backend results and their original
     * logical deadlines remain owned while Bluetooth is disabled. */
    (void)ble_link_service_pump_tx();
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    publish = s_service.status_dirty ||
              s_service.snapshot.state != _device_link_service_derive_state(
                  ble_adv_manager_get_state());
    publish = _device_link_service_sync_confirmation_locked() || publish;
    if (s_service.window_open_pending)
    {
        /* A window deferred until the existing ACL is gone: retry the
         * disconnect request while connected, then open once clear. */
        if (s_service.client_connected)
        {
            xSemaphoreGive(s_service.mutex);
            (void)ble_nimble_port_request_disconnect();
            xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        }
        else if (!s_service.suspended &&
                 _device_link_service_open_window_locked() == ESP_OK)
        {
            s_service.window_open_pending = false;
            publish = true;
        }
    }
    {
        /* A window must be closed when: its deadline expired, a previous
         * close is pending a retry, a revoke is in progress (the
         * security ordering closes the window before the revoke touches
         * the store), or a durable binding is already committed. A live,
         * unexpired window without a revoke or binding stays open; only
         * the remaining-time publication runs. */
        const TickType_t now = xTaskGetTickCount();
        const bool bound = (ble_link_session_get_state_flags() &
                            BLE_LINK_STATE_FLAG_BOUND) != 0U;
        const bool deadline_reached = s_service.window_open &&
                                      _device_link_service_tick_reached(
                                          now, s_service.window_deadline);
        const bool close_required = s_service.close_pending ||
                                    deadline_reached ||
                                    s_service.revoke_in_progress ||
                                    (s_service.window_open && bound);

        if (bound && s_service.window_open_pending)
        {
            s_service.window_open_pending = false;
            publish = true;
        }

        if (close_required && (s_service.window_open ||
                               s_service.window_open_pending ||
                               s_service.bindable_lease_held ||
                               s_service.close_pending))
        {
            const esp_err_t close_result =
                _device_link_service_close_window_locked();

            if (close_result != ESP_OK)
            {
                s_service.snapshot.last_error = close_result;
            }
            else
            {
                s_service.snapshot.last_error = ESP_OK;
                _device_link_service_confirm_pending_suspend_locked();
            }
            publish = true;
        }
        else if (s_service.window_open &&
                 now - s_service.last_remaining_publish >=
                 pdMS_TO_TICKS(DEVICE_LINK_SERVICE_REMAINING_PUBLISH_MS))
        {
            s_service.last_remaining_publish = now;
            publish = true;
        }
    }
    bool revoke_pause_ready = true;
    bool continue_revoke = false;

    if (s_service.revoke_in_progress)
    {
        const esp_err_t pause_result = ble_adv_manager_set_pause_reason(
                                           BLE_ADV_MANAGER_PAUSE_REASON_REVOKE,
                                           true);

        revoke_pause_ready = pause_result == ESP_OK;
        if (!revoke_pause_ready && s_service.snapshot.last_error != pause_result)
        {
            s_service.snapshot.last_error = pause_result;
            publish = true;
        }
    }
    if (revoke_pause_ready && s_service.revoke_in_progress &&
            !s_service.window_open &&
            !s_service.window_open_pending && !s_service.bindable_lease_held &&
            !s_service.close_pending)
    {
        continue_revoke = true;
    }
    if (publish)
    {
        _device_link_service_refresh_snapshot_locked();
        s_service.snapshot.generation++;
        snapshot = s_service.snapshot;
    }
    xSemaphoreGive(s_service.mutex);
    if (publish)
    {
        _device_link_service_publish_now(&snapshot);
    }
    if (continue_revoke)
    {
        /* The NimBLE revoke owner holds its storage lock while it may close
         * the current service session. Never call it with the service mutex
         * held, or the two owners can form service->storage / storage->service. */
        const esp_err_t revoke_result =
            _device_link_service_continue_revoke();
        bool revoke_publish = false;
        bool release_revoke_pause = false;

        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        if (revoke_result == ESP_OK && s_service.revoke_in_progress)
        {
            s_service.revoke_in_progress = false;
            s_service.snapshot.last_error = ESP_OK;
            release_revoke_pause = true;
            revoke_publish = true;
        }
        else if (revoke_result != ESP_OK &&
                 revoke_result != ESP_ERR_NOT_FINISHED &&
                 s_service.snapshot.last_error != revoke_result)
        {
            s_service.snapshot.last_error = revoke_result;
            revoke_publish = true;
        }
        if (revoke_publish)
        {
            _device_link_service_refresh_snapshot_locked();
            s_service.snapshot.generation++;
            snapshot = s_service.snapshot;
        }
        xSemaphoreGive(s_service.mutex);
        if (release_revoke_pause)
        {
            const esp_err_t resume_result = ble_adv_manager_set_pause_reason(
                                                BLE_ADV_MANAGER_PAUSE_REASON_REVOKE,
                                                false);

            if (resume_result != ESP_OK)
            {
                LOG_W("revoke advertising deferred result=%d", resume_result);
            }
        }
        if (revoke_publish)
        {
            _device_link_service_publish_now(&snapshot);
        }
    }
}

static void _device_link_service_worker(void *arg)
{
    (void)arg;
    for (;;)
    {
        device_link_service_command_t command;

        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        const bool publish_ready = s_service.status_publish_ready;
        const device_link_service_lifecycle_t lifecycle =
            _device_link_service_lifecycle();

        xSemaphoreGive(s_service.mutex);
        if (!publish_ready)
        {
            if (lifecycle == DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING &&
                    xQueueReceive(s_service.queue, &command, 0U) == pdTRUE)
            {
                if (command.type == DEVICE_LINK_SERVICE_COMMAND_DEINIT)
                {
                    break;
                }
                if (command.type == DEVICE_LINK_SERVICE_COMMAND_PROCESS_LINK)
                {
                    ble_link_service_release_work(command.link_work);
                }
                continue;
            }
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (xQueueReceive(s_service.queue, &command, 0U) == pdTRUE)
        {
            if (command.type == DEVICE_LINK_SERVICE_COMMAND_DEINIT)
            {
                break;
            }
            _device_link_service_handle_command(&command);
        }
        else
        {
            /* Queue producers and TX completions notify this task. Owner
             * state and the link-service operation deadline supply every
             * retry, so no timer event or fixed periodic poll is required. */
            (void)ulTaskNotifyTake(pdTRUE,
                                   _device_link_service_wait_ticks(
                                       _device_link_service_next_wait_ms()));
        }
        /* Converge the requested Bluetooth policy after every command/wake
         * so a superseded target never turns into a queued full cycle. */
        _device_link_service_bt_power_step();
        /* The deadline and periodic publication tick runs after every
         * command as well, so sustained command traffic cannot starve the
         * window expiry. */
        _device_link_service_worker_tick();
    }
    /* DEINIT: retain and retry every close obligation before the runtime
     * teardown. A bounded caller may time out while this owner keeps
     * working; a WAIT_FOREVER caller observes the eventual convergence. */
    esp_err_t worker_result = ESP_OK;
    while ((worker_result = _device_link_service_prepare_stop()) != ESP_OK)
    {
        ble_adv_manager_poll();
        vTaskDelay(pdMS_TO_TICKS(DEVICE_LINK_SERVICE_RETRY_MS));
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    s_service.snapshot.available = false;
    _device_link_service_refresh_snapshot_locked();
    s_service.snapshot.generation++;
    {
        device_link_service_snapshot_t snapshot = s_service.snapshot;

        xSemaphoreGive(s_service.mutex);
        _device_link_service_publish_now(&snapshot);
    }
    atomic_store_explicit(&s_worker_result, (unsigned int)worker_result,
                          memory_order_release);
    atomic_store_explicit(&s_worker_exited, true, memory_order_release);
    xSemaphoreGive(s_service.stopped);
#ifdef UNIT_TEST_HOST
    /* The host FreeRTOS fake needs the caller-side vTaskDelete to join the
     * worker thread; the worker returns and the trampoline exits. */
    return;
#else
    /* The device task never self-deletes: it parks here until the caller
     * deletes it after taking the exit signal, so teardown owns the task
     * lifetime and cannot race a self-deletion. */
    for (;;)
    {
        device_link_service_command_t parked;

        (void)xQueueReceive(s_service.queue, &parked, portMAX_DELAY);
    }
#endif
}

static esp_err_t _device_link_service_runtime_teardown(void)
{
    esp_err_t stop_result = ESP_OK;

    if (s_service.runtime_started)
    {
        stop_result = ble_runtime_stop();
        if (stop_result == ESP_OK ||
                stop_result == ESP_ERR_INVALID_STATE)
        {
            s_service.runtime_started = false;
        }
    }
    if (!s_service.runtime_initialized)
    {
        if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE)
        {
            LOG_W("runtime stop failed without initialized port: %s",
                  esp_err_to_name(stop_result));
        }
        return stop_result == ESP_ERR_INVALID_STATE ? ESP_OK : stop_result;
    }
    /* A failed stop leaves the runtime FAULTED; deinit still owns the port
       teardown and must run so the next start does not inherit a half-up
       controller/host. */
    const esp_err_t deinit_result = ble_runtime_deinit();

    if (deinit_result != ESP_OK)
    {
        LOG_E("runtime teardown failed: stop=%s deinit=%s",
              esp_err_to_name(stop_result),
              esp_err_to_name(deinit_result));
        return deinit_result;
    }
    s_service.runtime_started = false;
    s_service.runtime_initialized = false;
    /* Successful SDK teardown retires every lease/window obligation from
     * that host generation, including a previously failed window close. */
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    s_service.stop_phase = DEVICE_LINK_SERVICE_STOP_NONE;
    s_service.slow_lease_held = false;
    s_service.slow_lease_id = 0U;
    s_service.bindable_lease_held = false;
    s_service.bindable_lease_id = 0U;
    s_service.window_open = false;
    s_service.window_open_pending = false;
    s_service.close_pending = false;
    s_service.window_deadline = 0U;
    s_service.client_connected = false;
    s_service.client_conn_handle = 0U;
    s_service.client_generation = 0U;
    xSemaphoreGive(s_service.mutex);
    if (stop_result != ESP_OK)
    {
        LOG_W("runtime stop recovered by deinit: %s",
              esp_err_to_name(stop_result));
    }
    return ESP_OK;
}

static esp_err_t _device_link_service_acquire_slow_lease(void);

/* Start/stop helpers are also used by the local Bluetooth policy command.
 * The service worker remains alive across these calls; only the NimBLE-owned
 * runtime and its event registrations are replaced. */
/**
 * @brief Register the v1 Wi-Fi owner with the link service.
 *
 * Registration is idempotent for the boot. A failed registration blocks
 * runtime startup so slot commands never sit ACCEPTED without an owner.
 */
static esp_err_t _device_link_service_register_wifi_domain(void)
{
    if (s_service.wifi_domain_registered)
    {
        return ESP_OK;
    }
    const void *descriptor = NULL;
    esp_err_t result = device_link_wifi_adapter_get_descriptor(&descriptor);

    if (result == ESP_OK)
    {
        result = ble_link_service_set_domain_descriptors(descriptor, 0U);
    }
    if (result != ESP_OK)
    {
        LOG_E("wifi domain registration failed result=%d", result);
        return result;
    }
    s_service.wifi_domain_registered = true;
    LOG_I("wifi domain registered");
    return ESP_OK;
}

static esp_err_t _device_link_service_runtime_start(void)
{
    if (s_service.stop_phase != DEVICE_LINK_SERVICE_STOP_NONE)
    {
        const esp_err_t cleanup = _device_link_service_runtime_stop();

        if (cleanup != ESP_OK)
        {
            return cleanup;
        }
    }
    if (s_service.runtime_started &&
            ble_runtime_get_state() == BLE_RUNTIME_STATE_RUNNING)
    {
        return ESP_OK;
    }
    if (s_service.runtime_initialized)
    {
        /* A previous stop may have left the port initialized but not
           running; a fresh init/start must begin from a released runtime. */
        const esp_err_t cleanup = _device_link_service_runtime_teardown();

        if (cleanup != ESP_OK)
        {
            return cleanup;
        }
    }
    /* The Wi-Fi domain must be frozen into the router before the GATT
     * service initializes the link service (boot_id is still zero). */
    esp_err_t result = _device_link_service_register_wifi_domain();

    if (result != ESP_OK)
    {
        return result;
    }
    memset(&s_service.runtime_config, 0, sizeof(s_service.runtime_config));
    s_service.runtime_config.port = s_service.config.runtime_port;
    result = ble_runtime_init(&s_service.runtime_config);

    s_service.runtime_initialized = result == ESP_OK;
    if (result != ESP_OK)
    {
        return result;
    }
    ble_link_gatt_set_work_submit(_device_link_service_submit_link_work, NULL);
    result = ble_event_router_register(_device_link_service_ble_event, NULL);
    s_service.router_registered = result == ESP_OK;
    if (result == ESP_OK)
    {
        result = ble_runtime_start();
        s_service.runtime_started = result == ESP_OK;
    }
    if (result == ESP_OK)
    {
        /* The port starts the host asynchronously. Acquiring the persistent
           lease before synchronization can make the first advertising start
           fail and leave the manager in a retry/faulted state. */
        result = ble_nimble_port_wait_for_host_sync(
                     DEVICE_LINK_SERVICE_BLE_HOST_SYNC_TIMEOUT_MS);
        if (result != ESP_OK)
        {
            LOG_E("host sync wait after start failed: %s",
                  esp_err_to_name(result));
        }
    }
    if (result == ESP_OK)
    {
        if (!s_service.startup_gate_released)
        {
            result = s_service.config.runtime_port->reset_peer_store();
        }
        if (result == ESP_OK && !s_service.startup_gate_released)
        {
            result = ble_adv_manager_set_pause_reason(
                         BLE_ADV_MANAGER_PAUSE_REASON_STARTUP_GATE, true);
        }
        if (result == ESP_OK)
        {
            if (s_service.bluetooth_policy_default_pending)
            {
                result = _device_link_service_store_bluetooth_policy(true);
            }
        }
        if (result == ESP_OK)
        {
            result = _device_link_service_acquire_slow_lease();
        }
    }
    if (result == ESP_OK)
    {
        return ESP_OK;
    }

    const esp_err_t primary = result;
    const esp_err_t cleanup = _device_link_service_runtime_teardown();

    if (cleanup != ESP_OK)
    {
        return cleanup;
    }
    if (s_service.router_registered)
    {
        (void)ble_event_router_unregister(_device_link_service_ble_event,
                                          NULL);
        s_service.router_registered = false;
    }
    ble_link_gatt_set_work_submit(NULL, NULL);
    return primary;
}

static esp_err_t _device_link_service_prepare_stop(void)
{
    /* A failed/partially initialized host has no active service generation.
     * Its SDK teardown must not wait for a barrier on a host that never ran. */
    if (!s_service.runtime_started ||
            ble_runtime_get_state() != BLE_RUNTIME_STATE_RUNNING)
    {
        return ESP_OK;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (s_service.stop_phase == DEVICE_LINK_SERVICE_STOP_NONE)
    {
        s_service.stop_phase = DEVICE_LINK_SERVICE_STOP_QUIESCING;
        s_service.bluetooth_cleanup_pending = true;
    }
    const device_link_service_stop_phase_t phase = s_service.stop_phase;

    xSemaphoreGive(s_service.mutex);
    if (phase == DEVICE_LINK_SERVICE_STOP_READY)
    {
        return ESP_OK;
    }
    if (phase == DEVICE_LINK_SERVICE_STOP_QUIESCING)
    {
        esp_err_t result = ble_adv_manager_set_pause_reason(
                               BLE_ADV_MANAGER_PAUSE_REASON_SERVICE_SHUTDOWN, true);

        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        if (result == ESP_OK)
        {
            result = _device_link_service_close_window_locked();
        }
        if (result == ESP_OK && s_service.slow_lease_held)
        {
            result = ble_adv_manager_release_lease(s_service.slow_lease_id);
            if (result == ESP_OK || result == ESP_ERR_NOT_FOUND)
            {
                s_service.slow_lease_held = false;
                s_service.slow_lease_id = 0U;
                result = ESP_OK;
            }
        }
        xSemaphoreGive(s_service.mutex);
        if (result != ESP_OK)
        {
            return result;
        }
        result = ble_nimble_port_begin_cleanup_drain();
        if (result != ESP_OK)
        {
            return result;
        }
        _device_link_service_clear_link_session();
        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        s_service.stop_phase = DEVICE_LINK_SERVICE_STOP_DRAINING;
        xSemaphoreGive(s_service.mutex);
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    const bool revoke_pending = s_service.revoke_in_progress;

    xSemaphoreGive(s_service.mutex);
    if (revoke_pending)
    {
        const esp_err_t result = _device_link_service_continue_revoke();

        if (result != ESP_OK)
        {
            return result;
        }
        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        s_service.revoke_in_progress = false;
        xSemaphoreGive(s_service.mutex);
        /* SERVICE_SHUTDOWN still suppresses visibility throughout teardown. */
        (void)ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_REVOKE, false);
    }
    (void)ble_link_service_pump_tx();
    if (ble_nimble_port_cleanup_pending())
    {
        return ESP_ERR_NOT_FINISHED;
    }
    /* New admission is closed. A second host barrier and empty observation
     * account for DISCONNECT callbacks that retained fresh cleanup. */
    const esp_err_t barrier = ble_nimble_port_begin_cleanup_drain();

    if (barrier != ESP_OK)
    {
        return barrier;
    }
    (void)ble_link_service_pump_tx();
    if (ble_nimble_port_cleanup_pending())
    {
        return ESP_ERR_NOT_FINISHED;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    s_service.stop_phase = DEVICE_LINK_SERVICE_STOP_READY;
    xSemaphoreGive(s_service.mutex);
    return ESP_OK;
}

static esp_err_t _device_link_service_runtime_stop(void)
{
    const esp_err_t prepare = _device_link_service_prepare_stop();

    if (prepare != ESP_OK)
    {
        return prepare;
    }
    const esp_err_t teardown = _device_link_service_runtime_teardown();

    if (teardown != ESP_OK)
    {
        return teardown;
    }
    if (s_service.router_registered)
    {
        const esp_err_t unregister = ble_event_router_unregister(
                                         _device_link_service_ble_event,
                                         NULL);

        if (unregister != ESP_OK && unregister != ESP_ERR_NOT_FOUND)
        {
            LOG_W("event router unregister during teardown failed: %s",
                  esp_err_to_name(unregister));
        }
        s_service.router_registered = false;
    }
    ble_link_gatt_set_work_submit(NULL, NULL);
    return ESP_OK;
}

static esp_err_t _device_link_service_acquire_slow_lease(void)
{
    ble_adv_lease_t lease;

    memset(&lease, 0, sizeof(lease));
    const esp_err_t result = ble_adv_manager_acquire_lease(
                                 &lease, BLE_ADV_MANAGER_MODE_SLOW,
                                 false);

    if (lease.lease_id != 0U)
    {
        s_service.slow_lease_held = true;
        s_service.slow_lease_id = lease.lease_id;
    }
    LOG_I("slow lease result=%d id=%u", (int)result, lease.lease_id);
    _device_link_service_zeroize(&lease, sizeof(lease));
    return result;
}

static void _device_link_service_release_resources(void)
{
    /* response_completed invokes the wake callback under the link-service
     * lock, so clearing it is a teardown barrier for that producer. The BLE
     * host and router are already stopped before this function is entered. */
    ble_link_service_set_worker_wake(NULL, NULL);
    atomic_store_explicit(&s_worker_task, (uintptr_t)NULL,
                          memory_order_release);
    if (s_service.task != NULL)
    {
        /* The worker parked (device) or returned (host) after its exit
         * signal and the host task has been stopped by the runtime
         * teardown, so no completion can notify it anymore. Deleting it
         * here is the single, safe delete on both platforms: on the host
         * it joins the thread, on the device it removes the parked task.
         * The handle is cleared so a retry never deletes twice. */
        vTaskDelete(s_service.task);
    }
    if (s_service.queue != NULL)
    {
        device_link_service_command_t command;

        while (xQueueReceive(s_service.queue, &command, 0U) == pdTRUE)
        {
            if (command.type == DEVICE_LINK_SERVICE_COMMAND_PROCESS_LINK)
            {
                ble_link_service_release_work(command.link_work);
            }
        }
        vQueueDelete(s_service.queue);
    }
    if (s_service.stopped != NULL)
    {
        vSemaphoreDelete(s_service.stopped);
    }
    if (s_service.mutex != NULL)
    {
        vSemaphoreDelete(s_service.mutex);
    }
    s_service.task = NULL;
    s_service.queue = NULL;
    s_service.stopped = NULL;
    s_service.mutex = NULL;
    ble_link_gatt_set_work_submit(NULL, NULL);
    /* The completion bridge subscription dies with the service: no table
     * survives to consume terminal snapshots after teardown. */
    device_link_wifi_adapter_bridge_stop();
    atomic_store_explicit(&s_worker_exited, false, memory_order_release);
    atomic_store_explicit(&s_deinit_command_admitted, false,
                          memory_order_release);
}

static esp_err_t _device_link_service_rollback_init(esp_err_t primary_error)
{
    esp_err_t first_error = ESP_OK;
    esp_err_t step_result = ESP_OK;

    /* STOPPING rejects any new early admission; the drain then waits for
     * in-flight callbacks that were already admitted, so the mutex can
     * never be released underneath one. Runtime teardown must complete
     * before callback dependencies can be released. */
    while (atomic_flag_test_and_set_explicit(
                &s_deinit_guard, memory_order_acquire))
    {
        vTaskDelay(1U);
    }
    (void)_device_link_service_transition(
        DEVICE_LINK_SERVICE_LIFECYCLE_STARTING,
        DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING);
    while ((atomic_load_explicit(&s_api_state, memory_order_acquire) &
            DEVICE_LINK_SERVICE_API_USER_MASK) != 0U)
    {
        vTaskDelay(1U);
    }
    if (s_service.slow_lease_held)
    {
        step_result = ble_adv_manager_release_lease(
                          s_service.slow_lease_id);
        if (step_result == ESP_OK || step_result == ESP_ERR_NOT_FOUND)
        {
            s_service.slow_lease_held = false;
            step_result = ESP_OK;
        }
        if (first_error == ESP_OK && step_result != ESP_OK)
        {
            first_error = step_result;
        }
    }
    if (s_service.task != NULL)
    {
        const device_link_service_command_t command =
        {
            .type = DEVICE_LINK_SERVICE_COMMAND_DEINIT,
        };

        if (xQueueSend(s_service.queue, &command, portMAX_DELAY) != pdTRUE)
        {
            return _device_link_service_deinit_return(
                       first_error != ESP_OK ? first_error : ESP_ERR_TIMEOUT);
        }
        atomic_store_explicit(&s_deinit_command_admitted, true,
                              memory_order_release);
        _device_link_service_wake_worker(NULL);
        if (xSemaphoreTake(s_service.stopped, portMAX_DELAY) != pdTRUE)
        {
            /* The worker never confirmed exit: it may still own the queue,
             * mutex, and semaphore. Tearing down the runtime or releasing
             * those resources now would delete them underneath a live task.
             * Keep STOPPING with every resource in place so a retry
             * device_link_service_deinit() can wait for the worker again. */
            return _device_link_service_deinit_return(
                       first_error != ESP_OK ? first_error : ESP_ERR_TIMEOUT);
        }
        /* The worker parked (or returned) and cannot touch service state
         * anymore, but it is NOT deleted yet: the host task may still
         * deliver a straggler TX completion that notifies the parked task.
         * The runtime teardown stops the host first; release_resources
         * deletes the task afterwards. */
    }
    step_result = _device_link_service_runtime_teardown();
    if (step_result != ESP_OK)
    {
        return _device_link_service_deinit_return(step_result);
    }
    if (s_service.router_registered)
    {
        /* Unregister only after the host task stopped. */
        step_result = ble_event_router_unregister(
                          _device_link_service_ble_event, NULL);
        if (step_result == ESP_ERR_NOT_FOUND)
        {
            step_result = ESP_OK;
        }
        s_service.router_registered = step_result != ESP_OK;
        if (first_error == ESP_OK && step_result != ESP_OK)
        {
            first_error = step_result;
        }
    }
    if (s_service.router_registered)
    {
        return _device_link_service_deinit_return(first_error);
    }
    _device_link_service_release_resources();
    (void)_device_link_service_transition(
        DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING,
        DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED);
    return _device_link_service_deinit_return(
               first_error != ESP_OK ? first_error : primary_error);
}

esp_err_t device_link_service_init(const device_link_service_config_t *config)
{
    if (config == NULL || config->runtime_port == NULL ||
            config->runtime_port->init == NULL ||
            config->runtime_port->start == NULL ||
            config->runtime_port->set_pairing_gate == NULL ||
            config->runtime_port->stop == NULL ||
            config->runtime_port->deinit == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->task_priority == 0U ||
            config->task_priority >= configMAX_PRIORITIES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->window_ms != DEVICE_LINK_SERVICE_WINDOW_MS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->startup_mode != DEVICE_LINK_SERVICE_STARTUP_NORMAL &&
            config->startup_mode !=
            DEVICE_LINK_SERVICE_STARTUP_FACTORY_RESET_GATED)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->startup_mode ==
            DEVICE_LINK_SERVICE_STARTUP_FACTORY_RESET_GATED &&
            config->runtime_port->reset_peer_store == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_device_link_service_begin_instance())
    {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_worker_task, (uintptr_t)NULL,
                          memory_order_release);
    atomic_store_explicit(&s_deinit_command_admitted, false,
                          memory_order_release);
    memset(&s_service, 0, sizeof(s_service));
    s_service.config = *config;
    device_link_wifi_adapter_set_firmware(config->firmware_major,
                                          config->firmware_minor,
                                          config->firmware_patch);
    s_service.startup_gate_released =
        config->startup_mode == DEVICE_LINK_SERVICE_STARTUP_NORMAL;
    bool policy_enabled = true;
    esp_err_t policy_result = _device_link_service_load_bluetooth_policy(
                                  &policy_enabled);

    if (config->startup_mode == DEVICE_LINK_SERVICE_STARTUP_FACTORY_RESET_GATED)
    {
        /* Factory reset restores the default-on policy only after the reset
         * journal commits. The gated startup itself must therefore run with
         * BLE available, while a failed recovery still remains closed. */
        policy_enabled = true;
        s_service.bluetooth_policy_default_pending = true;
        policy_result = ESP_OK;
    }
    s_service.bluetooth_enabled = policy_enabled && policy_result == ESP_OK;
    s_service.bluetooth_desired_enabled = s_service.bluetooth_enabled;
    s_service.bluetooth_transitioning = false;
    s_service.bluetooth_retry_attempt = 0U;
    s_service.bluetooth_retry_not_before = 0U;
    s_service.bluetooth_transition_deadline = 0U;
    s_service.bluetooth_policy_dirty = policy_result != ESP_OK;
    s_service.bluetooth_policy_error = policy_result == ESP_OK ?
                                       ESP_OK : policy_result;
    s_service.mutex = xSemaphoreCreateMutexStatic(&s_service.mutex_control);
    s_service.stopped = xSemaphoreCreateBinaryStatic(
                            &s_service.stopped_control);
    s_service.queue = xQueueCreateStatic(
                          CONFIG_DEVICE_LINK_SERVICE_QUEUE_DEPTH,
                          sizeof(device_link_service_command_t),
                          s_service.queue_storage, &s_service.queue_control);
    esp_err_t result = s_service.mutex != NULL && s_service.stopped != NULL &&
                       s_service.queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;

    atomic_store_explicit(&s_worker_result, ESP_OK,
                          memory_order_release);
    s_suspend_pending_count = 0U;
    memset(s_suspend_pending, 0, sizeof(s_suspend_pending));
    memset(s_suspend_results, 0, sizeof(s_suspend_results));
    if (result == ESP_OK && s_service.bluetooth_enabled)
    {
        if (config->startup_mode ==
                DEVICE_LINK_SERVICE_STARTUP_FACTORY_RESET_GATED)
        {
            result = ble_nimble_port_revoke_journal_begin();
            if (result == ESP_OK)
            {
                s_service.revoke_in_progress = true;
            }
        }
    }
    if (result == ESP_OK && s_service.bluetooth_enabled)
    {
        /* The Wi-Fi domain must be frozen into the router before the GATT
         * service initializes the link service (boot_id is still zero).
         * Startup and the runtime_start path share this registration. */
        result = _device_link_service_register_wifi_domain();
    }
    if (result == ESP_OK && s_service.bluetooth_enabled)
    {
        memset(&s_service.runtime_config, 0, sizeof(s_service.runtime_config));
        s_service.runtime_config.port = config->runtime_port;
        result = ble_runtime_init(&s_service.runtime_config);
        s_service.runtime_initialized = result == ESP_OK;
    }
    if (result == ESP_OK && s_service.bluetooth_enabled)
    {
        /* Register the queue sink before the host starts. A GATT callback
         * during startup may retain work in the already-created queue, but
         * no worker is allowed to execute it until the port has initialized
         * the link-service state and mutex. */
        ble_link_gatt_set_work_submit(_device_link_service_submit_link_work,
                                      NULL);
    }
    if (result == ESP_OK && s_service.bluetooth_enabled)
    {
        /* The router callback registers before the host task starts (the
         * router table is unsynchronized and the production port follows
         * the same register-before-run pattern), so registration never
         * races a dispatch. */
        result = ble_event_router_register(
                     _device_link_service_ble_event, NULL);
        s_service.router_registered = result == ESP_OK;
    }
    if (result == ESP_OK && s_service.bluetooth_enabled)
    {
        result = ble_runtime_start();
        s_service.runtime_started = result == ESP_OK;
    }
    if (result == ESP_OK && s_service.bluetooth_enabled)
    {
        /* Advertising prerequisites are installed below; wait for the host
           boundary first so the persistent lease cannot race it. */
        result = ble_nimble_port_wait_for_host_sync(
                     DEVICE_LINK_SERVICE_BLE_HOST_SYNC_TIMEOUT_MS);
        if (result != ESP_OK)
        {
            LOG_E("host sync wait during init failed: %s",
                  esp_err_to_name(result));
        }
    }
    if (result == ESP_OK)
    {
        /* Register before publishing the task. Completions retained during
         * this interval observe NULL; the catch-up notification below makes
         * their owner state visible without relying on a lossy edge. */
        ble_link_service_set_worker_wake(
            _device_link_service_wake_worker, NULL);
        /* The runtime port has now initialized GATT, link-session, and the
         * link-service mutex. Starting the owner earlier lets its first
         * deadline sweep race that initialization. */
        s_service.task = xTaskCreateStaticPinnedToCore(
                             _device_link_service_worker,
                             "device_link", CONFIG_DEVICE_LINK_SERVICE_TASK_STACK,
                             NULL, config->task_priority,
                             s_service.task_stack, &s_service.task_control,
                             CONFIG_MAIN_PROJECT_TASK_CORE_ID);
        result = s_service.task != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (result == ESP_OK)
    {
        atomic_store_explicit(&s_worker_task,
                              (uintptr_t)s_service.task,
                              memory_order_release);
        _device_link_service_wake_worker(NULL);
    }
    if (result == ESP_OK && s_service.bluetooth_enabled &&
            !s_service.startup_gate_released)
    {
        /* The durable reset marker was written before this service was
         * started. Host synchronization consumes the Device Link revoke
         * journal; this explicit port operation verifies that the bond and
         * CCCD store is empty before the global reset marker may clear. */
        result = config->runtime_port->reset_peer_store();
    }
    if (result == ESP_OK && s_service.bluetooth_enabled &&
            !s_service.startup_gate_released)
    {
        /* Install every fallible advertising prerequisite while the global
         * reset marker is still durable. The slow lease is acquired below
         * while paused, so no advertisement is exposed before gate release. */
        result = ble_adv_manager_set_pause_reason(
                     BLE_ADV_MANAGER_PAUSE_REASON_STARTUP_GATE, true);
    }
    if (result == ESP_OK && s_service.bluetooth_enabled &&
            !s_service.startup_gate_released)
    {
        result = _device_link_service_acquire_slow_lease();
    }
    if (result == ESP_OK && s_service.bluetooth_enabled &&
            s_service.startup_gate_released)
    {
        result = _device_link_service_acquire_slow_lease();
    }
    if (result == ESP_OK)
    {
        /* v1 Wi-Fi commands occupy the single operation slot. Install the
         * completion bridge independently of the BLE runtime so a later
         * Bluetooth-enable still completes SCAN/SET_CREDENTIALS. */
        result = device_link_wifi_adapter_bridge_start();
    }
    if (result != ESP_OK)
    {
        return _device_link_service_rollback_init(result);
    }
    /* The owner remains publication-gated until this first snapshot is on
     * the bus. A subscriber may re-enter the RUNNING API while it is being
     * published; any resulting state change is marked dirty and published
     * by the owner after the gate opens. */
    {
        device_link_service_snapshot_t snapshot;

        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        s_service.snapshot.available = true;
        s_service.snapshot.last_error = s_service.bluetooth_policy_error;
        /* Reconcile any connection tracked during STARTING, then expose
         * RUNNING while still holding the mutex so no STARTING callback
         * can update state between the refresh and the store. */
        _device_link_service_refresh_snapshot_locked();
        s_service.snapshot.generation = 1U;
        (void)_device_link_service_transition(
            DEVICE_LINK_SERVICE_LIFECYCLE_STARTING,
            DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING);
        snapshot = s_service.snapshot;
        xSemaphoreGive(s_service.mutex);
        _device_link_service_publish_now(&snapshot);
        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        s_service.status_publish_ready = true;
        xSemaphoreGive(s_service.mutex);
        _device_link_service_wake_worker(NULL);
    }
    LOG_I("ready: window=%lu ms", (unsigned long)config->window_ms);
    return ESP_OK;
}

esp_err_t device_link_service_release_startup_gate(void)
{
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_service.mutex, portMAX_DELAY) != pdTRUE)
    {
        _device_link_service_api_release();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;

    if (s_service.config.startup_mode !=
            DEVICE_LINK_SERVICE_STARTUP_FACTORY_RESET_GATED)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else if (!s_service.startup_gate_released)
    {
        if (!s_service.slow_lease_held)
        {
            result = ESP_ERR_INVALID_STATE;
        }
        if (result == ESP_OK)
        {
            /* Gated init already acquired the persistent lease while paused
             * and verified every durable reset obligation. This is the
             * visibility commit after the global marker clears. A physical
             * START failure is retained by the ADV owner and retried with
             * backoff, so it is an availability event rather than a failed
             * factory-reset transaction or reopen the reset journal. */
            s_service.startup_gate_released = true;
            s_service.bluetooth_policy_default_pending = false;
            const esp_err_t resume_result = ble_adv_manager_set_pause_reason(
                                                BLE_ADV_MANAGER_PAUSE_REASON_STARTUP_GATE,
                                                false);

            if (resume_result != ESP_OK)
            {
                LOG_W("startup advertising deferred result=%d", resume_result);
            }
        }
    }
    if (result == ESP_OK)
    {
        s_service.status_dirty = true;
    }

    xSemaphoreGive(s_service.mutex);
    if (result == ESP_OK)
    {
        _device_link_service_wake_worker(NULL);
    }
    _device_link_service_api_release();
    return result;
}

esp_err_t device_link_service_deinit(uint32_t timeout_ms)
{
    device_link_service_lifecycle_t lifecycle =
        _device_link_service_lifecycle();

    if (lifecycle == DEVICE_LINK_SERVICE_LIFECYCLE_UNINITIALIZED ||
            lifecycle == DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED)
    {
        return ESP_OK;
    }
    /* A STOPPING lifecycle alone does not prove that the first caller has
     * enqueued DEINIT yet: it may still be draining admitted API users. Keep
     * one teardown coordinator so a concurrent WAIT_FOREVER caller cannot
     * wait for an exit signal that nobody has arranged. A later retry is
     * admitted after the previous caller releases this guard. */
    if (atomic_flag_test_and_set_explicit(
                &s_deinit_guard, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }
    lifecycle = _device_link_service_lifecycle();
    if (lifecycle == DEVICE_LINK_SERVICE_LIFECYCLE_UNINITIALIZED ||
            lifecycle == DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED)
    {
        return _device_link_service_deinit_return(ESP_OK);
    }
    const bool command_admitted = atomic_load_explicit(
                                      &s_deinit_command_admitted,
                                      memory_order_acquire);
    const bool entered_stopping = lifecycle ==
                                  DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING;

    if (entered_stopping)
    {
        if (!_device_link_service_transition(
                    DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING,
                    DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING))
        {
            return _device_link_service_deinit_return(
                       ESP_ERR_INVALID_STATE);
        }
    }
    else if (lifecycle != DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING)
    {
        return _device_link_service_deinit_return(ESP_ERR_INVALID_STATE);
    }
    const TickType_t started = xTaskGetTickCount();
    TickType_t timeout = timeout_ms == DEVICE_LINK_SERVICE_WAIT_FOREVER ?
                         portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    if (timeout_ms > 0U && timeout == 0U)
    {
        timeout = 1U;
    }
    while (!command_admitted &&
            (atomic_load_explicit(&s_api_state, memory_order_acquire) &
             DEVICE_LINK_SERVICE_API_USER_MASK) != 0U)
    {
        if (timeout != portMAX_DELAY &&
                xTaskGetTickCount() - started >= timeout)
        {
            if (entered_stopping)
            {
                (void)_device_link_service_transition(
                    DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING,
                    DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING);
            }
            return _device_link_service_deinit_return(ESP_ERR_TIMEOUT);
        }
        vTaskDelay(1U);
    }
    if (!command_admitted)
    {
        const device_link_service_command_t command =
        {
            .type = DEVICE_LINK_SERVICE_COMMAND_DEINIT,
        };

        const TickType_t remaining =
            _device_link_service_remaining_timeout(started, timeout);

        if (xQueueSend(s_service.queue, &command, remaining) != pdTRUE)
        {
            if (entered_stopping)
            {
                (void)_device_link_service_transition(
                    DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING,
                    DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING);
            }
            return _device_link_service_deinit_return(ESP_ERR_TIMEOUT);
        }
        atomic_store_explicit(&s_deinit_command_admitted, true,
                              memory_order_release);
        _device_link_service_wake_worker(NULL);
    }
    if (s_service.task != NULL && (!command_admitted ||
                                   !atomic_load_explicit(&s_worker_exited, memory_order_acquire)))
    {
        /* The first deinit always waits for the worker's exit signal: the
         * give happens-before the take returns, so the worker can no
         * longer touch the semaphore afterwards. A retry skips the wait
         * because the signal was already consumed; a rollback that never
         * created the worker has no signal to wait for. */
        const TickType_t remaining =
            _device_link_service_remaining_timeout(started, timeout);

        if (xSemaphoreTake(s_service.stopped, remaining) != pdTRUE)
        {
            /* The worker never exited: keep STOPPING so a retry can wait
             * again instead of resuming a half-torn-down service. */
            return _device_link_service_deinit_return(ESP_ERR_TIMEOUT);
        }
    }
    /* The worker has parked (or returned) and cannot touch service state
     * anymore, but it is NOT deleted yet: the host task may still deliver a
     * straggler TX completion that notifies the parked task, and deleting
     * a task that is concurrently notified is unsafe. The runtime teardown
     * below stops the host first; release_resources deletes the task
     * afterwards. */
    /* The worker confirmed the shared cleanup fixed point. A runtime
     * teardown failure keeps STOPPING and every remaining dependency. */
    esp_err_t result = (esp_err_t)atomic_load_explicit(&s_worker_result,
        memory_order_acquire);
    esp_err_t teardown_result = _device_link_service_runtime_teardown();

    if (result == ESP_OK)
    {
        result = teardown_result;
    }
    if (teardown_result == ESP_OK && s_service.router_registered)
    {
        /* Unregister only after the host task stopped, so the router table
         * is never mutated while a dispatch runs. */
        const esp_err_t unregister_result = ble_event_router_unregister(
                                                _device_link_service_ble_event, NULL);

        if (unregister_result == ESP_ERR_NOT_FOUND ||
                unregister_result == ESP_OK)
        {
            s_service.router_registered = false;
        }
        else
        {
            if (result == ESP_OK)
            {
                result = unregister_result;
            }
            teardown_result = unregister_result;
        }
    }
    if (teardown_result == ESP_OK)
    {
        _device_link_service_release_resources();
        (void)_device_link_service_transition(
            DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING,
            DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED);
        return _device_link_service_deinit_return(result);
    }
    /* The runtime could not be stopped (or unregistered): keep STOPPING
     * with the parked worker and resources so deinit can be retried. */
    return _device_link_service_deinit_return(
               result != ESP_OK ? result : teardown_result);
}

static esp_err_t _device_link_service_enqueue(
    device_link_service_command_type_t type)
{
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const device_link_service_command_t command =
    {
        .type = type,
    };
    const esp_err_t result = xQueueSend(s_service.queue, &command, 0U) ==
                             pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;

    if (result == ESP_OK)
    {
        _device_link_service_wake_worker(NULL);
    }
    _device_link_service_api_release();
    return result;
}

esp_err_t device_link_service_open_window(void)
{
    /* The suspended check happens in the worker, in FIFO order with every
     * other transition, so an open that races with suspend cannot win out
     * of order. */
    return _device_link_service_enqueue(DEVICE_LINK_SERVICE_COMMAND_OPEN);
}

esp_err_t device_link_service_close_window(void)
{
    /* The command is always enqueued: the FIFO ordering guarantees a close
     * issued after an open always lands after it in the worker, so a
     * pending open can never outlive its owner's close. */
    return _device_link_service_enqueue(DEVICE_LINK_SERVICE_COMMAND_CLOSE);
}

esp_err_t device_link_service_confirm_binding(
    device_link_confirmation_token_t token, bool accept)
{
    if (token == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const device_link_service_command_t command =
    {
        .type = DEVICE_LINK_SERVICE_COMMAND_CONFIRM_BINDING,
        .confirmation_token = token,
        .accept_binding = accept,
    };
    const esp_err_t result = xQueueSend(s_service.queue, &command, 0U) ==
                             pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;

    if (result == ESP_OK)
    {
        _device_link_service_wake_worker(NULL);
    }
    _device_link_service_api_release();
    return result;
}

esp_err_t device_link_service_revoke_binding(void)
{
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const device_link_service_command_t command =
    {
        .type = DEVICE_LINK_SERVICE_COMMAND_REVOKE,
    };
    const esp_err_t result = xQueueSend(s_service.queue, &command, 0U) ==
                             pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;

    if (result == ESP_OK)
    {
        _device_link_service_wake_worker(NULL);
    }
    _device_link_service_api_release();
    return result;
}

bool device_link_service_pending_confirmation(void)
{
    bool pending = false;

    if (!_device_link_service_api_acquire())
    {
        return false;
    }
    if (xSemaphoreTake(s_service.mutex, portMAX_DELAY) == pdTRUE)
    {
        pending = s_service.snapshot.pending_confirmation;
        xSemaphoreGive(s_service.mutex);
    }
    _device_link_service_api_release();
    return pending;
}

esp_err_t device_link_service_suspend(uint32_t timeout_ms)
{
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    /* Standby preparation needs the suspended state applied before it
     * proceeds. The caller assigns a unique sequence under the mutex
     * before the enqueue, so the acknowledgement always refers to this
     * command: the worker stores the command's sequence after applying it,
     * and a queued resume ahead of it can never be acknowledged against.
     * The API user count is held across the wait, so deinit cannot tear
     * the service down underneath it. */
    uint32_t expected_sequence = 0U;
    device_link_service_command_t command;

    memset(&command, 0, sizeof(command));
    command.type = DEVICE_LINK_SERVICE_COMMAND_SUSPEND;
    /* The sequence assignment and the queue insertion happen under the
     * same mutex, so concurrent suspend callers cannot reorder: the FIFO
     * application order matches the sequence order and the acknowledgement
     * always refers to this exact command. The nonblocking send cannot
     * block while the worker briefly holds the mutex. One deadline covers
     * the take and the acknowledgement loop, so a finite timeout bounds
     * the whole call and WAIT_FOREVER never times out. */
    const TickType_t now = xTaskGetTickCount();
    const TickType_t deadline = timeout_ms == DEVICE_LINK_SERVICE_WAIT_FOREVER ?
                                0U : now + pdMS_TO_TICKS(timeout_ms);
    TickType_t take_wait = portMAX_DELAY;

    if (timeout_ms != DEVICE_LINK_SERVICE_WAIT_FOREVER)
    {
        take_wait = pdMS_TO_TICKS(50U);
        if (deadline - now < take_wait)
        {
            take_wait = deadline - now;
        }
    }
    if (xSemaphoreTake(s_service.mutex, take_wait) != pdTRUE)
    {
        _device_link_service_api_release();
        return ESP_ERR_TIMEOUT;
    }
    if (s_suspend_next != UINT32_MAX)
    {
        const uint32_t candidate = s_suspend_next + 1U;
        device_link_service_suspend_result_t *slot =
            &s_suspend_results[candidate % DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];
        bool slot_free = atomic_load_explicit(&slot->packed,
                                              memory_order_acquire) == 0U;

        /* A pending (not yet terminal) suspend also owns its slot: the
         * close obligation may still resolve it at any time, so its
         * result must never be overwritten by a later sequence. */
        for (size_t i = 0U; slot_free && i < s_suspend_pending_count; ++i)
        {
            if (s_suspend_pending[i] % DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS ==
                    candidate % DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS)
            {
                slot_free = false;
            }
        }
        if (slot_free)
        {
            expected_sequence = candidate;
            s_suspend_next = candidate;
            command.sequence = expected_sequence;
            /* Claim the slot immediately (pending marker): a later
             * same-slot sequence must be refused until this one reaches a
             * terminal outcome and its waiter releases the slot. */
            device_link_service_suspend_result_t *claim =
                &s_suspend_results[candidate %
                                   DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];

            atomic_store_explicit(&claim->packed, candidate,
                                  memory_order_release);
            result = xQueueSend(s_service.queue, &command, 0U) == pdTRUE ?
                     ESP_OK : ESP_ERR_NO_MEM;
            if (result != ESP_OK)
            {
                /* The queue rejected the command: nothing was admitted;
                 * release the claim and return the real queue error. */
                uint64_t expected_claim = candidate;

                (void)atomic_compare_exchange_strong_explicit(
                    &claim->packed, &expected_claim, 0U,
                    memory_order_acq_rel, memory_order_acquire);
                expected_sequence = 0U;
            }
        }
    }
    xSemaphoreGive(s_service.mutex);
    if (result == ESP_OK && expected_sequence != 0U)
    {
        _device_link_service_wake_worker(NULL);
    }
    if (expected_sequence == 0U)
    {
        /* The suspend sequence space is exhausted, the result slot for
         * the next sequence is still owned by an unconsumed outcome, or
         * the queue rejected the command (result carries that error):
         * fail closed before enqueueing. */
        _device_link_service_api_release();
        return result != ESP_OK ? result : ESP_ERR_INVALID_STATE;
    }
    if (result != ESP_OK)
    {
        _device_link_service_api_release();
        return result;
    }
    for (;;)
    {
        const device_link_service_suspend_result_t *slot =
            &s_suspend_results[expected_sequence %
                               DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];
        const uint64_t packed = atomic_load_explicit(
                                    &slot->packed, memory_order_acquire);
        const uint32_t sequence = (uint32_t)packed;
        const uint32_t outcome = (uint32_t)(packed >> 32U);

        if (sequence == expected_sequence &&
                outcome == DEVICE_LINK_SERVICE_SUSPEND_APPLIED)
        {
            break;
        }
        if (sequence == expected_sequence &&
                outcome == DEVICE_LINK_SERVICE_SUSPEND_CANCELLED)
        {
            /* This suspend was cancelled by a RESUME before it applied. */
            result = ESP_ERR_TIMEOUT;
            break;
        }
        if (timeout_ms != DEVICE_LINK_SERVICE_WAIT_FOREVER &&
                _device_link_service_tick_reached(xTaskGetTickCount(),
                        deadline))
        {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(1U);
    }
    {
        /* The waiter consumed its terminal outcome: release the slot so a
         * later sequence can reuse it. Only this sequence's own value is
         * cleared; the CAS is retried until it wins (or the slot already
         * moved to a different sequence, which cannot happen while this
         * slot is occupied). */
        device_link_service_suspend_result_t *slot =
            &s_suspend_results[expected_sequence %
                               DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];

        for (;;)
        {
            uint64_t expected = atomic_load_explicit(
                                    &slot->packed, memory_order_acquire);

            if ((uint32_t)expected != expected_sequence)
            {
                break;
            }
            if (atomic_compare_exchange_strong_explicit(
                        &slot->packed, &expected, 0U,
                        memory_order_acq_rel, memory_order_acquire))
            {
                break;
            }
        }
    }
    _device_link_service_api_release();
    return result;
}

esp_err_t device_link_service_resume(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return _device_link_service_enqueue(DEVICE_LINK_SERVICE_COMMAND_RESUME);
}

esp_err_t device_link_service_set_enabled(bool enabled, uint32_t timeout_ms)
{
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    device_link_service_snapshot_t snapshot;
    bool publish = false;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    const bool already = s_service.bluetooth_desired_enabled == enabled;

    if (!already || s_service.bluetooth_policy_error != ESP_OK ||
            (!s_service.bluetooth_transitioning &&
             (s_service.bluetooth_cleanup_pending || s_service.bluetooth_policy_dirty)))
    {
        /* Latest target wins: no command is queued per request. The worker
           converges to this value and skips intermediate targets. */
        s_service.bluetooth_desired_enabled = enabled;
        s_service.bluetooth_request_revision++;
        s_service.bluetooth_policy_dirty = true;
        s_service.bluetooth_policy_error = ESP_OK;
        s_service.bluetooth_transitioning =
            !_device_link_service_bluetooth_settled_locked();
        s_service.bluetooth_retry_attempt = 0U;
        s_service.bluetooth_retry_not_before = 0U;
        s_service.bluetooth_transition_deadline = 0U;
        _device_link_service_refresh_snapshot_locked();
        s_service.snapshot.generation++;
        snapshot = s_service.snapshot;
        publish = true;
    }
    xSemaphoreGive(s_service.mutex);

    if (publish)
    {
        _device_link_service_publish_now(&snapshot);
        _device_link_service_wake_worker(NULL);
    }
    if (timeout_ms == 0U)
    {
        _device_link_service_api_release();
        return ESP_OK;
    }

    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = timeout_ms == DEVICE_LINK_SERVICE_WAIT_FOREVER ?
                               portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    for (;;)
    {
        bool settled = false;
        bool effective = false;
        esp_err_t policy_error = ESP_OK;

        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        settled = !s_service.bluetooth_transitioning;
        const bool complete = _device_link_service_bluetooth_settled_locked();
        effective = s_service.bluetooth_enabled;
        policy_error = s_service.bluetooth_policy_error;
        xSemaphoreGive(s_service.mutex);
        if (settled)
        {
            _device_link_service_api_release();
            if (complete && effective == enabled && policy_error == ESP_OK)
            {
                return ESP_OK;
            }
            return policy_error != ESP_OK ? policy_error :
                   ESP_ERR_INVALID_STATE;
        }
        if (timeout != portMAX_DELAY &&
                xTaskGetTickCount() - started >= timeout)
        {
            _device_link_service_api_release();
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1U);
    }
}

esp_err_t device_link_service_get_status(
    device_link_service_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    *status = s_service.snapshot;
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
    return ESP_OK;
}

bool device_link_service_is_active(void)
{
    bool active = false;

    if (!_device_link_service_api_acquire())
    {
        return false;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    active = s_service.window_open;
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
    return active;
}

bool device_link_service_is_busy(void)
{
    bool busy = false;

    if (!_device_link_service_api_acquire())
    {
        return false;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    /* Interim standby admission: any window (open or deferred until the
     * ACL is gone) or ACL blocks light sleep. */
    busy = s_service.window_open || s_service.window_open_pending ||
           s_service.client_connected;
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
    return busy;
}
