#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_random.h"

#include "ble_adv_manager.h"
#include "ble_link_session.h"
#include "ble_port_ops.h"
#include "ble_link_service.h"

#include "device_link_security.h"
#include "ble_runtime.h"
#include "device_link_service.h"
#include "event_bus.h"

#define DBG_TAG "device_link_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define DEVICE_LINK_SERVICE_DISCRIMINATOR_BYTES 3U
#define DEVICE_LINK_SERVICE_POP_BYTES 16U
#define DEVICE_LINK_SERVICE_QR_VERSION "link-v1"
#define DEVICE_LINK_SERVICE_QR_SHORT_NAME "MT"
#define DEVICE_LINK_SERVICE_QR_SERVICE_UUID \
    "3e203192-b4bb-4e59-a28a-3d1157854ea3"
#define DEVICE_LINK_SERVICE_WORKER_POLL_MS 100U
#define DEVICE_LINK_SERVICE_REMAINING_PUBLISH_MS 1000U

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
    DEVICE_LINK_SERVICE_COMMAND_DEINIT,
} device_link_service_command_type_t;

typedef struct device_link_service_command
{
    device_link_service_command_type_t type;
    uint32_t sequence; /**< Suspend acknowledgement sequence. */
    bool accept_binding; /**< Confirm (true) or deny (false). */
} device_link_service_command_t;

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
    bool router_registered;
    bool slow_lease_held;
    uint8_t slow_lease_id;
    bool window_open;
    bool bindable_lease_held;
    uint8_t bindable_lease_id;
    bool client_connected;
    uint16_t client_conn_handle; /**< Accepted ACL handle, 0 when idle. */
    bool suspended;
    bool qr_ready;
    uint8_t discriminator[DEVICE_LINK_SERVICE_DISCRIMINATOR_BYTES];
    uint8_t pop[DEVICE_LINK_SERVICE_POP_BYTES];
    char qr[DEVICE_LINK_SERVICE_QR_MAX_BYTES];
    TickType_t window_deadline;
    TickType_t last_remaining_publish;
} device_link_service_t;

static device_link_service_t s_service;
static atomic_int s_lifecycle = ATOMIC_VAR_INIT(
                                    DEVICE_LINK_SERVICE_LIFECYCLE_UNINITIALIZED);
static atomic_uint s_api_users = ATOMIC_VAR_INIT(0U);
static atomic_uint s_worker_result = ATOMIC_VAR_INIT(0U);
static atomic_bool s_worker_exited = ATOMIC_VAR_INIT(false);
static uint32_t s_suspend_next; /**< Caller-assigned, under the mutex. */
static atomic_uint s_suspend_applied = ATOMIC_VAR_INIT(0U);

EVENT_BUS_DEFINE_ID(DEVICE_LINK_SERVICE_MSG);

static void _device_link_service_zeroize(void *data, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    for (size_t i = 0U; i < size; ++i)
    {
        bytes[i] = 0U;
    }
}

static void _device_link_service_base64url(
    const uint8_t *input, size_t input_length, char *output)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t input_index = 0U;
    size_t output_index = 0U;

    while (input_index + 3U <= input_length)
    {
        const uint32_t value = ((uint32_t)input[input_index] << 16) |
                               ((uint32_t)input[input_index + 1U] << 8) |
                               input[input_index + 2U];

        output[output_index++] = alphabet[(value >> 18) & 0x3fU];
        output[output_index++] = alphabet[(value >> 12) & 0x3fU];
        output[output_index++] = alphabet[(value >> 6) & 0x3fU];
        output[output_index++] = alphabet[value & 0x3fU];
        input_index += 3U;
    }
    if (input_index + 1U == input_length)
    {
        const uint32_t value = (uint32_t)input[input_index] << 16;

        output[output_index++] = alphabet[(value >> 18) & 0x3fU];
        output[output_index++] = alphabet[(value >> 12) & 0x3fU];
    }
    else if (input_index + 2U == input_length)
    {
        const uint32_t value = ((uint32_t)input[input_index] << 16) |
                               ((uint32_t)input[input_index + 1U] << 8);

        output[output_index++] = alphabet[(value >> 18) & 0x3fU];
        output[output_index++] = alphabet[(value >> 12) & 0x3fU];
        output[output_index++] = alphabet[(value >> 6) & 0x3fU];
    }
    output[output_index] = '\0';
}

static void _device_link_service_zero_secrets(void)
{
    _device_link_service_zeroize(s_service.discriminator,
                                 sizeof(s_service.discriminator));
    _device_link_service_zeroize(s_service.pop, sizeof(s_service.pop));
    _device_link_service_zeroize(s_service.qr, sizeof(s_service.qr));
}

static bool _device_link_service_api_acquire(void)
{
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) !=
            DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING)
    {
        return false;
    }
    atomic_fetch_add_explicit(&s_api_users, 1U,
                              memory_order_acq_rel);
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) ==
            DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING)
    {
        return true;
    }
    atomic_fetch_sub_explicit(&s_api_users, 1U,
                              memory_order_release);
    return false;
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
    const device_link_service_lifecycle_t lifecycle =
        (device_link_service_lifecycle_t)atomic_load_explicit(
            &s_lifecycle, memory_order_acquire);

    if (lifecycle != DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING &&
            lifecycle != DEVICE_LINK_SERVICE_LIFECYCLE_STARTING)
    {
        return false;
    }
    atomic_fetch_add_explicit(&s_api_users, 1U,
                              memory_order_acq_rel);
    /* Revalidate after the increment: teardown may have transitioned to
     * STOPPING while this admission was in flight, in which case the
     * mutex may already be doomed and the count must be released. */
    const device_link_service_lifecycle_t after =
        (device_link_service_lifecycle_t)atomic_load_explicit(
            &s_lifecycle, memory_order_acquire);

    if (after == DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING ||
            after == DEVICE_LINK_SERVICE_LIFECYCLE_STARTING)
    {
        return true;
    }
    atomic_fetch_sub_explicit(&s_api_users, 1U,
                              memory_order_release);
    return false;
}

static void _device_link_service_api_release(void)
{
    atomic_fetch_sub_explicit(&s_api_users, 1U,
                              memory_order_release);
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

static device_link_service_state_t _device_link_service_derive_state(void)
{
    if (s_service.window_open)
    {
        return DEVICE_LINK_SERVICE_STATE_WINDOW;
    }
    if (s_service.client_connected)
    {
        return DEVICE_LINK_SERVICE_STATE_CONNECTED;
    }
    if (s_service.suspended)
    {
        return DEVICE_LINK_SERVICE_STATE_SUSPENDED;
    }
    return DEVICE_LINK_SERVICE_STATE_ADVERTISING;
}

static void _device_link_service_refresh_snapshot_locked(void)
{
    s_service.snapshot.state = _device_link_service_derive_state();
    s_service.snapshot.active = s_service.window_open;
    s_service.snapshot.pending_confirmation =
        ble_link_service_pending_confirmation();
    s_service.snapshot.client_connected = s_service.client_connected;
    s_service.snapshot.qr_ready = s_service.qr_ready;
    s_service.snapshot.window_remaining_ms =
        s_service.window_open ?
        _device_link_service_remaining_ms(s_service.window_deadline) : 0U;
}

static esp_err_t _device_link_service_open_window_locked(void)
{
    if (s_service.window_open)
    {
        return ESP_OK;
    }
    uint8_t discriminator[DEVICE_LINK_SERVICE_DISCRIMINATOR_BYTES];
    uint8_t pop[DEVICE_LINK_SERVICE_POP_BYTES];
    char discriminator_b64[5];
    char pop_b64[23];
    char qr[DEVICE_LINK_SERVICE_QR_MAX_BYTES];
    esp_err_t result = ESP_OK;

    esp_fill_random(discriminator, sizeof(discriminator));
    esp_fill_random(pop, sizeof(pop));
    if (discriminator[0] == 0U && discriminator[1] == 0U &&
            discriminator[2] == 0U)
    {
        discriminator[2] = 0x5aU;
    }
    bool pop_nonzero = false;

    for (size_t i = 0U; i < sizeof(pop); ++i)
    {
        pop_nonzero = pop_nonzero || pop[i] != 0U;
    }
    if (!pop_nonzero)
    {
        pop[sizeof(pop) - 1U] = 0x5aU;
    }
    _device_link_service_base64url(
        discriminator, sizeof(discriminator), discriminator_b64);
    _device_link_service_base64url(pop, sizeof(pop), pop_b64);
    const int qr_length = snprintf(
                              qr, sizeof(qr),
                              "{\"ver\":\"%s\",\"name\":\"%s\","
                              "\"service\":\"%s\",\"discriminator\":\"%s\","
                              "\"pop\":\"%s\",\"expires_in_ms\":%lu}",
                              DEVICE_LINK_SERVICE_QR_VERSION,
                              DEVICE_LINK_SERVICE_QR_SHORT_NAME,
                              DEVICE_LINK_SERVICE_QR_SERVICE_UUID,
                              discriminator_b64, pop_b64,
                              (unsigned long)s_service.config.window_ms);

    if (qr_length <= 0 || (size_t)qr_length >= sizeof(qr))
    {
        result = ESP_ERR_INVALID_SIZE;
        goto exit;
    }
    /* Arm the Security 2 bootstrap verifier before the window becomes
     * visible, so a handshake can never race an armed window and no
     * bindable advertisement outlives a failed verifier. */
    if (device_link_security_open_bootstrap(pop, sizeof(pop)) != ESP_OK)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    {
        ble_adv_lease_t lease;

        memset(&lease, 0, sizeof(lease));
        result = ble_adv_manager_acquire_lease(
                     &lease, BLE_ADV_MANAGER_MODE_FAST, true, discriminator);
        if (result != ESP_OK)
        {
            /* The manager may have installed the lease before reporting a
             * convergence error; release it so no bindable advertisement
             * outlives the failed window, and tear the verifier down so
             * no handshake can be attempted without a window. */
            if (lease.lease_id != 0U)
            {
                (void)ble_adv_manager_release_lease(lease.lease_id);
            }
            _device_link_service_zeroize(&lease, sizeof(lease));
            device_link_security_close_bootstrap();
            goto exit;
        }
        ble_link_session_set_pairing_window(true);
        s_service.bindable_lease_held = true;
        s_service.bindable_lease_id = lease.lease_id;
        /* The lease copy carries the discriminator; erase it on every
         * path, including a convergence failure that released it above. */
        _device_link_service_zeroize(&lease, sizeof(lease));
    }
    memcpy(s_service.discriminator, discriminator,
           sizeof(s_service.discriminator));
    memcpy(s_service.pop, pop, sizeof(s_service.pop));
    memcpy(s_service.qr, qr, (size_t)qr_length + 1U);
    s_service.window_open = true;
    s_service.qr_ready = true;
    s_service.suspended = false;
    s_service.window_deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(s_service.config.window_ms);
    s_service.last_remaining_publish = xTaskGetTickCount();
    s_service.snapshot.last_error = ESP_OK;

exit:
    /* The worker stack is static and long-lived: every secret-bearing
     * temporary must be erased on every path, success or failure. */
    _device_link_service_zeroize(discriminator, sizeof(discriminator));
    _device_link_service_zeroize(pop, sizeof(pop));
    _device_link_service_zeroize(discriminator_b64, sizeof(discriminator_b64));
    _device_link_service_zeroize(pop_b64, sizeof(pop_b64));
    _device_link_service_zeroize(qr, sizeof(qr));
    return result;
}

static void _device_link_service_close_window_locked(void)
{
    if (!s_service.window_open)
    {
        return;
    }
    if (s_service.bindable_lease_held)
    {
        (void)ble_adv_manager_release_lease(s_service.bindable_lease_id);
        s_service.bindable_lease_held = false;
    }
    ble_link_session_set_pairing_window(false);
    s_service.window_open = false;
    s_service.qr_ready = false;
    s_service.window_deadline = 0U;
    device_link_security_close_bootstrap();
    _device_link_service_zero_secrets();
}

static void _device_link_service_ble_event(
    const ble_port_event_t *event, void *arg)
{
    (void)arg;
    device_link_service_snapshot_t snapshot;
    bool publish = false;

    if (event->type != BLE_PORT_EVENT_CONNECT &&
            event->type != BLE_PORT_EVENT_DISCONNECT &&
            event->type != BLE_PORT_EVENT_RESET)
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
    if (event->type == BLE_PORT_EVENT_CONNECT && event->status == 0)
    {
        s_service.client_connected = true;
        s_service.client_conn_handle = event->conn_handle;
        publish = true;
    }
    else if (event->type == BLE_PORT_EVENT_DISCONNECT &&
             event->conn_handle == s_service.client_conn_handle)
    {
        /* A late disconnect for a retired connection must not clear a
         * newer one. */
        s_service.client_connected = false;
        s_service.client_conn_handle = 0U;
        publish = true;
    }
    else if (event->type == BLE_PORT_EVENT_RESET)
    {
        s_service.client_connected = false;
        s_service.client_conn_handle = 0U;
        publish = true;
    }
    if (publish &&
            atomic_load_explicit(&s_lifecycle, memory_order_acquire) ==
            DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING)
    {
        _device_link_service_refresh_snapshot_locked();
        s_service.snapshot.generation++;
        snapshot = s_service.snapshot;
    }
    else
    {
        publish = false;
    }
    xSemaphoreGive(s_service.mutex);
    if (publish)
    {
        _device_link_service_publish_now(&snapshot);
    }
    _device_link_service_api_release();
}

static void _device_link_service_handle_command(
    const device_link_service_command_t *command)
{
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
        _device_link_service_close_window_locked();
        s_service.snapshot.last_error = ESP_OK;
        break;
    case DEVICE_LINK_SERVICE_COMMAND_CONFIRM_BINDING:
        ble_link_service_confirm_binding(command->accept_binding);
        _device_link_service_refresh_snapshot_locked();
        s_service.snapshot.last_error = ESP_OK;
        break;
    case DEVICE_LINK_SERVICE_COMMAND_SUSPEND:
        /* Suspend always ends with no window and the suspended flag set,
         * so an OPEN that raced into the FIFO first cannot leave a window
         * open across standby. The sequence advances only here, so a
         * suspend caller can acknowledge its own command. */
        _device_link_service_close_window_locked();
        s_service.suspended = true;
        s_service.snapshot.last_error = ESP_OK;
        atomic_store_explicit(&s_suspend_applied, command->sequence,
                              memory_order_release);
        break;
    case DEVICE_LINK_SERVICE_COMMAND_RESUME:
        /* Resume only restores the pre-suspend idle state; it never opens a
         * pairing window. A window is opened only by an explicit user
         * action through device_link_service_open_window(). */
        s_service.suspended = false;
        s_service.snapshot.last_error = ESP_OK;
        break;
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
    if (s_service.window_open)
    {
        const TickType_t now = xTaskGetTickCount();

        if (_device_link_service_tick_reached(now, s_service.window_deadline))
        {
            _device_link_service_close_window_locked();
            publish = true;
        }
        else if (now - s_service.last_remaining_publish >=
                 pdMS_TO_TICKS(DEVICE_LINK_SERVICE_REMAINING_PUBLISH_MS))
        {
            s_service.last_remaining_publish = now;
            publish = true;
        }
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
}

static void _device_link_service_worker(void *arg)
{
    (void)arg;
    for (;;)
    {
        device_link_service_command_t command;

        if (xQueueReceive(s_service.queue, &command,
                          pdMS_TO_TICKS(
                              DEVICE_LINK_SERVICE_WORKER_POLL_MS)) == pdTRUE)
        {
            if (command.type == DEVICE_LINK_SERVICE_COMMAND_DEINIT)
            {
                break;
            }
            _device_link_service_handle_command(&command);
        }
        /* The deadline and periodic publication tick runs after every
         * command as well, so sustained command traffic cannot starve the
         * window expiry. */
        _device_link_service_worker_tick();
    }
    /* DEINIT: release every lease and clear secrets before the runtime
     * teardown that follows worker exit. */
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    _device_link_service_close_window_locked();
    if (s_service.slow_lease_held)
    {
        (void)ble_adv_manager_release_lease(s_service.slow_lease_id);
        s_service.slow_lease_held = false;
    }
    s_service.snapshot.available = false;
    _device_link_service_refresh_snapshot_locked();
    s_service.snapshot.generation++;
    {
        device_link_service_snapshot_t snapshot = s_service.snapshot;

        xSemaphoreGive(s_service.mutex);
        _device_link_service_publish_now(&snapshot);
    }
    atomic_store_explicit(&s_worker_result, ESP_OK,
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
    esp_err_t result = s_service.runtime_started ? ble_runtime_stop() : ESP_OK;

    if (result != ESP_OK)
    {
        return result;
    }
    s_service.runtime_started = false;
    if (!s_service.runtime_initialized)
    {
        /* Nothing was ever initialized; deinit would be invalid. */
        return ESP_OK;
    }
    result = ble_runtime_deinit();
    if (result == ESP_OK)
    {
        s_service.runtime_initialized = false;
    }
    return result;
}

static void _device_link_service_release_resources(void)
{
    if (s_service.task != NULL)
    {
        /* The worker parked (device) or returned (host) after its exit
         * signal, so deleting it here is the single, safe delete on both
         * platforms: on the host it joins the thread, on the device it
         * removes the parked task. The handle is cleared so a retry never
         * deletes twice. */
        vTaskDelete(s_service.task);
    }
    if (s_service.queue != NULL)
    {
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
    atomic_store_explicit(&s_worker_exited, false, memory_order_release);
}

static esp_err_t _device_link_service_rollback_init(esp_err_t primary_error)
{
    esp_err_t first_error = ESP_OK;
    esp_err_t step_result = ESP_OK;

    /* STOPPING rejects any new early admission; the drain then waits for
     * in-flight callbacks that were already admitted, so the mutex can
     * never be released underneath one. Every cleanup step runs
     * unconditionally afterwards so a failed step cannot abandon a live
     * runtime, callback registration, or resources. */
    atomic_store_explicit(&s_lifecycle,
                          DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING,
                          memory_order_release);
    while (atomic_load_explicit(&s_api_users, memory_order_acquire) != 0U)
    {
        vTaskDelay(1U);
    }
    if (s_service.slow_lease_held)
    {
        step_result = ble_adv_manager_release_lease(
                          s_service.slow_lease_id);
        s_service.slow_lease_held = false;
        if (first_error == ESP_OK && step_result != ESP_OK)
        {
            first_error = step_result;
        }
    }
    step_result = _device_link_service_runtime_teardown();
    if (first_error == ESP_OK && step_result != ESP_OK)
    {
        first_error = step_result;
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
    _device_link_service_release_resources();
    _device_link_service_zero_secrets();
    if (first_error == ESP_OK)
    {
        atomic_store_explicit(&s_lifecycle,
                              DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED,
                              memory_order_release);
    }
    /* On incomplete cleanup the lifecycle stays STOPPING so deinit can
     * retry the outstanding teardown; the caller must not re-init. */
    return first_error != ESP_OK ? first_error : primary_error;
}

esp_err_t device_link_service_init(const device_link_service_config_t *config)
{
    if (config == NULL || config->runtime_port == NULL ||
            config->runtime_port->init == NULL ||
            config->runtime_port->start == NULL ||
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
    if (config->window_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    int expected = DEVICE_LINK_SERVICE_LIFECYCLE_UNINITIALIZED;

    if (!atomic_compare_exchange_strong_explicit(
                &s_lifecycle, &expected,
                DEVICE_LINK_SERVICE_LIFECYCLE_STARTING,
                memory_order_acq_rel, memory_order_acquire))
    {
        if (expected != DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED)
        {
            return ESP_ERR_INVALID_STATE;
        }
        /* Re-init from STOPPED: a second exclusive CAS admits exactly one
         * caller. */
        if (!atomic_compare_exchange_strong_explicit(
                    &s_lifecycle, &expected,
                    DEVICE_LINK_SERVICE_LIFECYCLE_STARTING,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    memset(&s_service, 0, sizeof(s_service));
    s_service.config = *config;
    atomic_store_explicit(&s_api_users, 0U, memory_order_release);
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
    if (result == ESP_OK)
    {
        memset(&s_service.runtime_config, 0, sizeof(s_service.runtime_config));
        s_service.runtime_config.port = config->runtime_port;
        result = ble_runtime_init(&s_service.runtime_config);
        s_service.runtime_initialized = result == ESP_OK;
    }
    if (result == ESP_OK)
    {
        /* The router callback registers before the host task starts (the
         * router table is unsynchronized and the production port follows
         * the same register-before-run pattern), so registration never
         * races a dispatch. */
        result = ble_event_router_register(
                     _device_link_service_ble_event, NULL);
        s_service.router_registered = result == ESP_OK;
    }
    if (result == ESP_OK)
    {
        result = ble_runtime_start();
        s_service.runtime_started = result == ESP_OK;
    }
    if (result == ESP_OK)
    {
        ble_adv_lease_t lease;

        memset(&lease, 0, sizeof(lease));
        result = ble_adv_manager_acquire_lease(
                     &lease, BLE_ADV_MANAGER_MODE_SLOW, false, NULL);
        if (result == ESP_OK)
        {
            s_service.slow_lease_held = true;
            s_service.slow_lease_id = lease.lease_id;
        }
        else if (lease.lease_id != 0U)
        {
            /* The manager may have installed the lease before reporting a
             * convergence error; release it so the rollback does not leak
             * an installed lease. */
            (void)ble_adv_manager_release_lease(lease.lease_id);
        }
    }
    if (result == ESP_OK)
    {
        s_service.task = xTaskCreateStaticPinnedToCore(
                             _device_link_service_worker,
                             "device_link", CONFIG_DEVICE_LINK_SERVICE_TASK_STACK,
                             NULL, config->task_priority,
                             s_service.task_stack, &s_service.task_control,
                             CONFIG_MAIN_PROJECT_TASK_CORE_ID);
        result = s_service.task != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (result != ESP_OK)
    {
        return _device_link_service_rollback_init(result);
    }
    /* The initial snapshot is prepared under the lock and copied before
     * RUNNING is exposed, so the publish afterwards never touches service
     * state: get_status sees a complete snapshot, a publisher-context
     * subscriber that re-enters the API sees a running service, and no
     * teardown can race the publication. Consumers apply generation
     * filtering (the setup pages do), so a concurrent worker publication
     * arriving first is harmless. */
    {
        device_link_service_snapshot_t snapshot;

        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        s_service.snapshot.generation = 1U;
        s_service.snapshot.available = true;
        /* Reconcile any connection tracked during STARTING, then expose
         * RUNNING while still holding the mutex so no STARTING callback
         * can update state between the refresh and the store. */
        _device_link_service_refresh_snapshot_locked();
        atomic_store_explicit(&s_lifecycle,
                              DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING,
                              memory_order_release);
        snapshot = s_service.snapshot;
        xSemaphoreGive(s_service.mutex);
        _device_link_service_publish_now(&snapshot);
    }
    LOG_I("ready: window=%lu ms", (unsigned long)config->window_ms);
    return ESP_OK;
}

esp_err_t device_link_service_deinit(uint32_t timeout_ms)
{
    const device_link_service_lifecycle_t lifecycle =
        (device_link_service_lifecycle_t)atomic_load_explicit(
            &s_lifecycle, memory_order_acquire);
    const bool command_admitted = lifecycle ==
                                  DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING;

    if (lifecycle == DEVICE_LINK_SERVICE_LIFECYCLE_UNINITIALIZED ||
            lifecycle == DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED)
    {
        return ESP_OK;
    }
    if (!command_admitted)
    {
        int expected = DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING;

        if (!atomic_compare_exchange_strong_explicit(
                    &s_lifecycle, &expected,
                    DEVICE_LINK_SERVICE_LIFECYCLE_STOPPING,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    const TickType_t started = xTaskGetTickCount();
    TickType_t timeout = timeout_ms == DEVICE_LINK_SERVICE_WAIT_FOREVER ?
                         portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    if (timeout_ms > 0U && timeout == 0U)
    {
        timeout = 1U;
    }
    while (!command_admitted &&
            atomic_load_explicit(&s_api_users,
                                 memory_order_acquire) != 0U)
    {
        if (timeout != portMAX_DELAY &&
                xTaskGetTickCount() - started >= timeout)
        {
            atomic_store_explicit(&s_lifecycle,
                                  DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING,
                                  memory_order_release);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1U);
    }
    if (!command_admitted)
    {
        const device_link_service_command_t command =
        {
            .type = DEVICE_LINK_SERVICE_COMMAND_DEINIT,
        };

        if (xQueueSend(s_service.queue, &command, timeout) != pdTRUE)
        {
            atomic_store_explicit(&s_lifecycle,
                                  DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING,
                                  memory_order_release);
            return ESP_ERR_TIMEOUT;
        }
    }
    esp_err_t result = ESP_OK;

    if (s_service.task != NULL && (!command_admitted ||
                                   !atomic_load_explicit(&s_worker_exited, memory_order_acquire)))
    {
        /* The first deinit always waits for the worker's exit signal: the
         * give happens-before the take returns, so the worker can no
         * longer touch the semaphore afterwards. A retry skips the wait
         * because the signal was already consumed; a rollback that never
         * created the worker has no signal to wait for. */
        if (xSemaphoreTake(s_service.stopped, timeout) != pdTRUE)
        {
            /* The worker never exited: keep STOPPING so a retry can wait
             * again instead of resuming a half-torn-down service. */
            return ESP_ERR_TIMEOUT;
        }
    }
    /* The worker has parked (or returned) and cannot touch service state
     * anymore; delete it now so a later teardown failure cannot retry
     * against a live task. The handle is cleared by release_resources on
     * success and this call is skipped on retry. */
    if (s_service.task != NULL)
    {
        vTaskDelete(s_service.task);
        s_service.task = NULL;
    }
    result = atomic_load_explicit(&s_worker_result,
                                  memory_order_acquire);
    if (result == ESP_OK)
    {
        result = _device_link_service_runtime_teardown();
    }
    if (result == ESP_OK)
    {
        /* Unregister only after the host task stopped, so the router table
         * is never mutated while a dispatch runs. */
        result = ble_event_router_unregister(
                     _device_link_service_ble_event, NULL);
        if (result == ESP_ERR_NOT_FOUND)
        {
            result = ESP_OK;
        }
        s_service.router_registered = false;
    }
    if (result == ESP_OK)
    {
        _device_link_service_release_resources();
        _device_link_service_zero_secrets();
        atomic_store_explicit(&s_lifecycle,
                              DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED,
                              memory_order_release);
        return ESP_OK;
    }
    /* The worker is gone; keep STOPPING so APIs reject access and deinit
     * can be retried without touching a dead worker. */
    return result;
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

esp_err_t device_link_service_confirm_binding(bool accept)
{
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const device_link_service_command_t command =
    {
        .type = DEVICE_LINK_SERVICE_COMMAND_CONFIRM_BINDING,
        .accept_binding = accept,
    };
    const esp_err_t result = xQueueSend(s_service.queue, &command, 0U) ==
                             pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;

    _device_link_service_api_release();
    return result;
}

bool device_link_service_pending_confirmation(void)
{
    bool pending = false;

    if (xSemaphoreTake(s_service.mutex, portMAX_DELAY) == pdTRUE)
    {
        pending = s_service.snapshot.pending_confirmation;
        xSemaphoreGive(s_service.mutex);
    }
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
        expected_sequence = ++s_suspend_next;
        command.sequence = expected_sequence;
        result = xQueueSend(s_service.queue, &command, 0U) == pdTRUE ?
                 ESP_OK : ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_service.mutex);
    if (expected_sequence == 0U)
    {
        /* The suspend sequence exhausted: fail closed before enqueueing,
         * so the error is definite and no command is left pending. */
        _device_link_service_api_release();
        return ESP_ERR_INVALID_STATE;
    }
    if (result != ESP_OK)
    {
        _device_link_service_api_release();
        return result;
    }
    for (;;)
    {
        if (atomic_load_explicit(&s_suspend_applied,
                                 memory_order_acquire) >= expected_sequence)
        {
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
    _device_link_service_api_release();
    return result;
}

esp_err_t device_link_service_resume(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return _device_link_service_enqueue(DEVICE_LINK_SERVICE_COMMAND_RESUME);
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

esp_err_t device_link_service_copy_qr(
    char *output, size_t capacity, size_t *out_length)
{
    if (output == NULL || out_length == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_ERR_NOT_FOUND;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (s_service.qr_ready)
    {
        const size_t length = strlen(s_service.qr);

        if (length + 1U <= capacity)
        {
            memcpy(output, s_service.qr, length + 1U);
            *out_length = length;
            result = ESP_OK;
        }
        else
        {
            result = ESP_ERR_INVALID_SIZE;
        }
    }
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
    return result;
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
    /* Interim standby admission: any window or ACL blocks light sleep.
     * Once P3.3/P3.4 provide session-aware state and a disconnect path, an
     * idle authenticated-free ACL should quiesce instead of blocking. */
    busy = s_service.window_open || s_service.client_connected;
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
    return busy;
}
