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
    DEVICE_LINK_SERVICE_COMMAND_SUSPEND,
    DEVICE_LINK_SERVICE_COMMAND_RESUME,
    DEVICE_LINK_SERVICE_COMMAND_PUBLISH,
    DEVICE_LINK_SERVICE_COMMAND_DEINIT,
} device_link_service_command_type_t;

typedef struct device_link_service_command
{
    device_link_service_command_type_t type;
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
    bool router_registered;
    bool slow_lease_held;
    uint8_t slow_lease_id;
    bool window_open;
    bool bindable_lease_held;
    uint8_t bindable_lease_id;
    bool client_connected;
    bool suspended;
    bool qr_ready;
    uint8_t discriminator[DEVICE_LINK_SERVICE_DISCRIMINATOR_BYTES];
    uint8_t pop[DEVICE_LINK_SERVICE_POP_BYTES];
    char qr[DEVICE_LINK_SERVICE_QR_MAX_BYTES];
    TickType_t window_deadline;
} device_link_service_t;

static device_link_service_t s_service;
static atomic_int s_lifecycle = ATOMIC_VAR_INIT(
                                    DEVICE_LINK_SERVICE_LIFECYCLE_UNINITIALIZED);
static atomic_uint s_api_users = ATOMIC_VAR_INIT(0U);
static atomic_uint s_worker_result = ATOMIC_VAR_INIT(0U);

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
    memset(s_service.discriminator, 0, sizeof(s_service.discriminator));
    memset(s_service.pop, 0, sizeof(s_service.pop));
    memset(s_service.qr, 0, sizeof(s_service.qr));
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

static void _device_link_service_publish_locked(void)
{
    s_service.snapshot.generation++;
    _device_link_service_publish_now(&s_service.snapshot);
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
    char discriminator_b64[5];
    char pop_b64[23];

    _device_link_service_base64url(
        discriminator, sizeof(discriminator), discriminator_b64);
    _device_link_service_base64url(pop, sizeof(pop), pop_b64);
    char qr[DEVICE_LINK_SERVICE_QR_MAX_BYTES];
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
        return ESP_ERR_INVALID_SIZE;
    }
    ble_adv_lease_t lease;

    esp_err_t result = ble_adv_manager_acquire_lease(
                           &lease, BLE_ADV_MANAGER_MODE_FAST, true,
                           discriminator);

    if (result != ESP_OK)
    {
        return result;
    }
    ble_link_session_set_pairing_window(true);
    s_service.bindable_lease_held = true;
    s_service.bindable_lease_id = lease.lease_id;
    memcpy(s_service.discriminator, discriminator,
           sizeof(s_service.discriminator));
    memcpy(s_service.pop, pop, sizeof(s_service.pop));
    memcpy(s_service.qr, qr, (size_t)qr_length + 1U);
    s_service.window_open = true;
    s_service.qr_ready = true;
    s_service.suspended = false;
    s_service.window_deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(s_service.config.window_ms);
    s_service.snapshot.last_error = ESP_OK;
    return ESP_OK;
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
    _device_link_service_zero_secrets();
}

static void _device_link_service_ble_event(
    const ble_port_event_t *event, void *arg)
{
    (void)arg;
    bool publish = false;

    if (event->type != BLE_PORT_EVENT_CONNECT &&
            event->type != BLE_PORT_EVENT_DISCONNECT)
    {
        return;
    }
    if (!_device_link_service_api_acquire())
    {
        return;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) ==
            DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING)
    {
        if (event->type == BLE_PORT_EVENT_CONNECT && event->status == 0)
        {
            s_service.client_connected = true;
            publish = true;
        }
        else if (event->type == BLE_PORT_EVENT_DISCONNECT)
        {
            s_service.client_connected = false;
            publish = true;
        }
        if (publish)
        {
            _device_link_service_refresh_snapshot_locked();
            _device_link_service_publish_locked();
        }
    }
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
}

static esp_err_t _device_link_service_handle_command(
    const device_link_service_command_t *command)
{
    esp_err_t result = ESP_OK;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    switch (command->type)
    {
    case DEVICE_LINK_SERVICE_COMMAND_OPEN:
        result = _device_link_service_open_window_locked();
        if (result != ESP_OK)
        {
            s_service.snapshot.last_error = result;
        }
        break;
    case DEVICE_LINK_SERVICE_COMMAND_CLOSE:
        _device_link_service_close_window_locked();
        break;
    case DEVICE_LINK_SERVICE_COMMAND_SUSPEND:
        s_service.suspended = true;
        break;
    case DEVICE_LINK_SERVICE_COMMAND_RESUME:
        s_service.suspended = false;
        result = _device_link_service_open_window_locked();
        if (result != ESP_OK)
        {
            s_service.snapshot.last_error = result;
        }
        break;
    case DEVICE_LINK_SERVICE_COMMAND_PUBLISH:
        break;
    case DEVICE_LINK_SERVICE_COMMAND_DEINIT:
        break;
    default:
        result = ESP_ERR_INVALID_ARG;
        break;
    }
    if (result == ESP_OK)
    {
        _device_link_service_refresh_snapshot_locked();
        _device_link_service_publish_locked();
    }
    xSemaphoreGive(s_service.mutex);
    return result;
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
            (void)_device_link_service_handle_command(&command);
        }
        /* The poll tick also serves the binding window deadline. */
        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        if (s_service.window_open &&
                _device_link_service_tick_reached(
                    xTaskGetTickCount(), s_service.window_deadline))
        {
            _device_link_service_close_window_locked();
            _device_link_service_refresh_snapshot_locked();
            _device_link_service_publish_locked();
        }
        xSemaphoreGive(s_service.mutex);
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
    _device_link_service_publish_locked();
    xSemaphoreGive(s_service.mutex);
    atomic_store_explicit(&s_worker_result, ESP_OK,
                          memory_order_release);
    xSemaphoreGive(s_service.stopped);
    vTaskDelete(NULL);
}

static esp_err_t _device_link_service_runtime_teardown(void)
{
    esp_err_t result = s_service.runtime_started ? ble_runtime_stop() : ESP_OK;

    if (result != ESP_OK)
    {
        return result;
    }
    s_service.runtime_started = false;
    return ble_runtime_deinit();
}

static void _device_link_service_release_resources(void)
{
    if (s_service.task != NULL)
    {
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
}

static esp_err_t _device_link_service_rollback_init(esp_err_t primary_error)
{
    esp_err_t cleanup_result = ESP_OK;

    if (s_service.router_registered)
    {
        cleanup_result = ble_event_router_unregister(
                             _device_link_service_ble_event, NULL);
        s_service.router_registered =
            cleanup_result == ESP_OK || cleanup_result == ESP_ERR_NOT_FOUND;
    }
    if (cleanup_result == ESP_OK && s_service.slow_lease_held)
    {
        cleanup_result = ble_adv_manager_release_lease(
                             s_service.slow_lease_id);
        s_service.slow_lease_held = cleanup_result != ESP_OK;
    }
    if (cleanup_result == ESP_OK)
    {
        cleanup_result = _device_link_service_runtime_teardown();
    }
    if (cleanup_result == ESP_OK)
    {
        _device_link_service_release_resources();
    }
    _device_link_service_zero_secrets();
    atomic_store_explicit(&s_lifecycle,
                          DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED,
                          memory_order_release);
    return cleanup_result != ESP_OK ? cleanup_result : primary_error;
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
                memory_order_acq_rel, memory_order_acquire) &&
            expected != DEVICE_LINK_SERVICE_LIFECYCLE_STOPPED)
    {
        return ESP_ERR_INVALID_STATE;
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

    if (result == ESP_OK)
    {
        memset(&s_service.runtime_config, 0, sizeof(s_service.runtime_config));
        s_service.runtime_config.port = config->runtime_port;
        result = ble_runtime_init(&s_service.runtime_config);
    }
    if (result == ESP_OK)
    {
        result = ble_runtime_start();
        s_service.runtime_started = result == ESP_OK;
    }
    if (result == ESP_OK)
    {
        result = ble_event_router_register(
                     _device_link_service_ble_event, NULL);
        s_service.router_registered = result == ESP_OK;
    }
    if (result == ESP_OK)
    {
        ble_adv_lease_t lease;

        result = ble_adv_manager_acquire_lease(
                     &lease, BLE_ADV_MANAGER_MODE_SLOW, false, NULL);
        if (result == ESP_OK)
        {
            s_service.slow_lease_held = true;
            s_service.slow_lease_id = lease.lease_id;
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
    atomic_store_explicit(&s_worker_result, ESP_OK,
                          memory_order_release);
    atomic_store_explicit(&s_lifecycle,
                          DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING,
                          memory_order_release);
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    s_service.snapshot.generation = 1U;
    s_service.snapshot.available = true;
    s_service.snapshot.state = DEVICE_LINK_SERVICE_STATE_ADVERTISING;
    s_service.snapshot.window_remaining_ms = 0U;
    _device_link_service_publish_locked();
    xSemaphoreGive(s_service.mutex);
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

    if (xSemaphoreTake(s_service.stopped, timeout) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    result = atomic_load_explicit(&s_worker_result,
                                  memory_order_acquire);
    if (result == ESP_OK)
    {
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
        result = _device_link_service_runtime_teardown();
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
    atomic_store_explicit(&s_lifecycle,
                          DEVICE_LINK_SERVICE_LIFECYCLE_RUNNING,
                          memory_order_release);
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
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    bool suspended = false;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    suspended = s_service.suspended;
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
    if (suspended)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return _device_link_service_enqueue(DEVICE_LINK_SERVICE_COMMAND_OPEN);
}

esp_err_t device_link_service_close_window(void)
{
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    bool open = false;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    open = s_service.window_open;
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
    if (!open)
    {
        return ESP_OK;
    }
    return _device_link_service_enqueue(DEVICE_LINK_SERVICE_COMMAND_CLOSE);
}

esp_err_t device_link_service_suspend(uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (s_service.window_open)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        s_service.suspended = true;
        _device_link_service_refresh_snapshot_locked();
        _device_link_service_publish_locked();
    }
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
    return result;
}

esp_err_t device_link_service_resume(uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!_device_link_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    bool suspended = false;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    suspended = s_service.suspended;
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
    if (!suspended)
    {
        return ESP_OK;
    }
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
    busy = s_service.window_open || s_service.client_connected;
    xSemaphoreGive(s_service.mutex);
    _device_link_service_api_release();
    return busy;
}
