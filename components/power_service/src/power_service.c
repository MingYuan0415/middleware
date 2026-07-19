#define DBG_TAG "power_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "power_service.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

EVENT_BUS_DEFINE_ID(POWER_SERVICE_MSG);

#ifndef CONFIG_POWER_SERVICE_TASK_STACK
    #define CONFIG_POWER_SERVICE_TASK_STACK 3072
#endif
#ifndef CONFIG_POWER_SERVICE_TASK_PRIORITY
    #define CONFIG_POWER_SERVICE_TASK_PRIORITY 4
#endif
#ifndef CONFIG_POWER_SERVICE_POLL_INTERVAL_MS
    #define CONFIG_POWER_SERVICE_POLL_INTERVAL_MS 5000
#endif

#define POWER_SERVICE_CMD_START  BIT0
#define POWER_SERVICE_CMD_PAUSE  BIT1
#define POWER_SERVICE_CMD_RESUME BIT2
#define POWER_SERVICE_CMD_STOP   BIT3
#define POWER_SERVICE_EVENT_RUNNING BIT0
#define POWER_SERVICE_EVENT_PAUSED  BIT1
#define POWER_SERVICE_EVENT_STOPPED BIT2
#define POWER_SERVICE_FAILURE_LOG_INTERVAL 12U

typedef enum
{
    POWER_SERVICE_STATE_STOPPED = 0,
    POWER_SERVICE_STATE_STARTING,
    POWER_SERVICE_STATE_RUNNING,
    POWER_SERVICE_STATE_PAUSE_PENDING,
    POWER_SERVICE_STATE_PAUSED,
    POWER_SERVICE_STATE_RESUME_PENDING,
    POWER_SERVICE_STATE_STOPPING,
} power_service_state_t;

static power_service_power_ops_t s_power_ops;
static bool s_power_ops_registered;
static bool s_initialized;
static TaskHandle_t s_worker;
static EventGroupHandle_t s_worker_events;
static SemaphoreHandle_t s_control_mutex;
static power_service_state_t s_state = POWER_SERVICE_STATE_STOPPED;
static power_service_snapshot_t s_snapshot;
static atomic_bool s_worker_event_tail_complete = ATOMIC_VAR_INIT(true);
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

static TickType_t _timeout_to_ticks(uint32_t timeout_ms)
{
    TickType_t result;
    if (timeout_ms == UINT32_MAX)
    {
        result = portMAX_DELAY;
    }
    else
    {
        uint64_t ticks = ((uint64_t)timeout_ms *
                          (uint64_t)configTICK_RATE_HZ + 999ULL) / 1000ULL;
        if (timeout_ms > 0 && ticks == 0)
        {
            ticks = 1;
        }
        if (ticks >= (uint64_t)portMAX_DELAY)
        {
            ticks = (uint64_t)portMAX_DELAY - 1ULL;
        }
        result = (TickType_t)ticks;
    }
    return result;
}

static void _wait_for_worker_event_tail(void)
{
    while (!atomic_load_explicit(&s_worker_event_tail_complete,
                                 memory_order_acquire))
    {
        vTaskDelay(1);
    }
}

static void _publish_snapshot(uint32_t subtype,
                              const power_service_snapshot_t *snapshot,
                              uint32_t flags)
{
    const esp_err_t result = event_bus_publish(
                                 POWER_SERVICE_MSG, subtype, snapshot, sizeof(*snapshot), flags);
    if (result != ESP_OK)
    {
        LOG_W("snapshot publish failed: 0x%x", result);
    }
}

static bool _hardware_available(void)
{
    bool available = s_power_ops_registered && s_power_ops.get_info != NULL;
    if (available && s_power_ops.is_available != NULL)
    {
        available = s_power_ops.is_available();
    }
    return available;
}

static bool _sample_hardware(power_info_t *info)
{
    bool sample_valid = false;
    if (_hardware_available())
    {
        memset(info, 0, sizeof(*info));
        sample_valid = s_power_ops.get_info(info) == ESP_OK;
    }
    return sample_valid;
}

static void _update_snapshot(bool sample_valid, const power_info_t *info)
{
    power_service_snapshot_t next;
    const int64_t sample_time = sample_valid ? esp_timer_get_time() / 1000LL : 0;

    taskENTER_CRITICAL(&s_snapshot_lock);
    next = s_snapshot;
    if (sample_valid)
    {
        next.info = *info;
        next.sampled_at_ms = sample_time;
    }
    next.valid = sample_valid;
    const bool availability_changed = next.valid != s_snapshot.valid;
    s_snapshot = next;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    if (availability_changed)
    {
        _publish_snapshot(POWER_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED,
                          &next, 0);
    }
    if (sample_valid || availability_changed)
    {
        _publish_snapshot(POWER_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE, &next,
                          EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
    }
}

static void _set_worker_state(power_service_state_t state)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_state = state;
    taskEXIT_CRITICAL(&s_state_lock);
}

static bool _worker_pause(uint32_t commands)
{
    bool stop = (commands & POWER_SERVICE_CMD_STOP) != 0;
    if ((commands & POWER_SERVICE_CMD_RESUME) != 0)
    {
        _set_worker_state(POWER_SERVICE_STATE_RUNNING);
        xEventGroupClearBits(s_worker_events, POWER_SERVICE_EVENT_PAUSED);
        xEventGroupSetBits(s_worker_events, POWER_SERVICE_EVENT_RUNNING);
    }
    else if (!stop && (commands & POWER_SERVICE_CMD_PAUSE) != 0)
    {
        _set_worker_state(POWER_SERVICE_STATE_PAUSED);
        xEventGroupClearBits(s_worker_events, POWER_SERVICE_EVENT_RUNNING);
        xEventGroupSetBits(s_worker_events, POWER_SERVICE_EVENT_PAUSED);

        while (!stop)
        {
            commands = 0;
            xTaskNotifyWait(0, UINT32_MAX, &commands, portMAX_DELAY);
            stop = (commands & POWER_SERVICE_CMD_STOP) != 0;
            if (!stop && (commands & POWER_SERVICE_CMD_RESUME) != 0)
            {
                _set_worker_state(POWER_SERVICE_STATE_RUNNING);
                xEventGroupClearBits(s_worker_events,
                                     POWER_SERVICE_EVENT_PAUSED);
                xEventGroupSetBits(s_worker_events,
                                   POWER_SERVICE_EVENT_RUNNING);
                break;
            }
        }
    }
    return stop;
}

static void _power_worker(void *context)
{
    (void)context;
    uint32_t commands = 0;
    xTaskNotifyWait(0, UINT32_MAX, &commands, portMAX_DELAY);
    bool stop = (commands & POWER_SERVICE_CMD_STOP) != 0;
    if (!stop)
    {
        stop = _worker_pause(commands);
    }
    if (!stop)
    {
        uint32_t consecutive_failures = 0;
        for (;;)
        {
            power_info_t info;
            const bool sample_valid = _sample_hardware(&info);
            _update_snapshot(sample_valid, &info);

            if (sample_valid)
            {
                consecutive_failures = 0;
            }
            else
            {
                ++consecutive_failures;
                if (consecutive_failures == 1U ||
                        consecutive_failures % POWER_SERVICE_FAILURE_LOG_INTERVAL == 0U)
                {
                    LOG_W("PMU sample unavailable (failures=%u)",
                          (unsigned)consecutive_failures);
                }
            }

            commands = 0;
            xTaskNotifyWait(0, UINT32_MAX, &commands,
                            pdMS_TO_TICKS(CONFIG_POWER_SERVICE_POLL_INTERVAL_MS));
            if ((commands & POWER_SERVICE_CMD_STOP) != 0 ||
                    _worker_pause(commands))
            {
                break;
            }
        }
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_state = POWER_SERVICE_STATE_STOPPED;
    s_worker = NULL;
    taskEXIT_CRITICAL(&s_state_lock);
    xEventGroupSetBits(s_worker_events, POWER_SERVICE_EVENT_STOPPED);
    atomic_store_explicit(&s_worker_event_tail_complete, true,
                          memory_order_release);
    vTaskDelete(NULL);
}

esp_err_t power_service_register_power_ops(const power_service_power_ops_t *ops)
{
    if (ops == NULL || ops->get_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != POWER_SERVICE_STATE_STOPPED || s_initialized)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        s_power_ops = *ops;
        s_power_ops_registered = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return result;
}

esp_err_t power_service_init(void)
{
    esp_err_t result = ESP_OK;
    bool initialize = false;
    taskENTER_CRITICAL(&s_state_lock);
    if (s_initialized)
    {
        initialize = false;
    }
    else if (s_state != POWER_SERVICE_STATE_STOPPED)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        s_state = POWER_SERVICE_STATE_STARTING;
        initialize = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (!initialize)
    {
        return result;
    }

    s_worker_events = xEventGroupCreate();
    if (s_worker_events == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    s_control_mutex = xSemaphoreCreateMutex();
    if (s_control_mutex == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    taskENTER_CRITICAL(&s_snapshot_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    taskEXIT_CRITICAL(&s_snapshot_lock);

    atomic_store_explicit(&s_worker_event_tail_complete, false,
                          memory_order_release);
    if (xTaskCreate(_power_worker, "power_worker", CONFIG_POWER_SERVICE_TASK_STACK,
                    NULL, CONFIG_POWER_SERVICE_TASK_PRIORITY, &s_worker) != pdPASS)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_initialized = true;
    s_state = POWER_SERVICE_STATE_RUNNING;
    taskEXIT_CRITICAL(&s_state_lock);
    xEventGroupSetBits(s_worker_events, POWER_SERVICE_EVENT_RUNNING);
    xTaskNotify(s_worker, POWER_SERVICE_CMD_START, eSetBits);
    LOG_I("worker started (interval=%dms)",
          CONFIG_POWER_SERVICE_POLL_INTERVAL_MS);
    return ESP_OK;

cleanup:
    atomic_store_explicit(&s_worker_event_tail_complete, true,
                          memory_order_release);
    if (s_control_mutex != NULL)
    {
        vSemaphoreDelete(s_control_mutex);
        s_control_mutex = NULL;
    }
    if (s_worker_events != NULL)
    {
        vEventGroupDelete(s_worker_events);
        s_worker_events = NULL;
    }
    _set_worker_state(POWER_SERVICE_STATE_STOPPED);
    return result;
}

esp_err_t power_service_deinit(void)
{
    esp_err_t result = ESP_OK;
    const SemaphoreHandle_t control = s_control_mutex;
    TaskHandle_t worker = NULL;
    EventGroupHandle_t events = NULL;
    bool control_owned = false;
    bool stop_worker = false;

    if (control != NULL)
    {
        xSemaphoreTake(control, portMAX_DELAY);
        control_owned = true;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized && s_state == POWER_SERVICE_STATE_STOPPED)
    {
        memset(&s_power_ops, 0, sizeof(s_power_ops));
        s_power_ops_registered = false;
    }
    else if (xTaskGetCurrentTaskHandle() == s_worker || s_worker == NULL ||
             s_worker_events == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        s_initialized = false;
        s_state = POWER_SERVICE_STATE_STOPPING;
        worker = s_worker;
        events = s_worker_events;
        stop_worker = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (stop_worker)
    {
        xTaskNotify(worker, POWER_SERVICE_CMD_STOP, eSetBits);
        xEventGroupWaitBits(events, POWER_SERVICE_EVENT_STOPPED,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        _wait_for_worker_event_tail();
        vEventGroupDelete(events);
        s_worker_events = NULL;

        taskENTER_CRITICAL(&s_state_lock);
        memset(&s_power_ops, 0, sizeof(s_power_ops));
        s_power_ops_registered = false;
        taskEXIT_CRITICAL(&s_state_lock);
    }

    if (control_owned)
    {
        xSemaphoreGive(control);
    }
    if (stop_worker)
    {
        vSemaphoreDelete(control);
        s_control_mutex = NULL;
    }
    return result;
}

static esp_err_t _reserve_pause(TaskHandle_t current_task,
                                TaskHandle_t *worker,
                                EventGroupHandle_t *events,
                                bool *send_pause)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool wait_for_pause = false;

    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || s_worker == NULL || s_worker_events == NULL ||
            current_task == s_worker)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else if (s_state == POWER_SERVICE_STATE_PAUSED)
    {
        result = ESP_OK;
    }
    else if (s_state == POWER_SERVICE_STATE_RUNNING)
    {
        s_state = POWER_SERVICE_STATE_PAUSE_PENDING;
        *send_pause = true;
        wait_for_pause = true;
    }
    else if (s_state == POWER_SERVICE_STATE_PAUSE_PENDING)
    {
        wait_for_pause = true;
    }
    if (wait_for_pause)
    {
        *worker = s_worker;
        *events = s_worker_events;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return result;
}

static void _cancel_timed_out_pause(TaskHandle_t worker,
                                    EventGroupHandle_t events,
                                    uint32_t timeout_ms)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state == POWER_SERVICE_STATE_PAUSE_PENDING ||
            s_state == POWER_SERVICE_STATE_PAUSED)
    {
        s_state = POWER_SERVICE_STATE_RESUME_PENDING;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    xEventGroupClearBits(events, POWER_SERVICE_EVENT_PAUSED |
                         POWER_SERVICE_EVENT_RUNNING);
    xTaskNotify(worker, POWER_SERVICE_CMD_RESUME, eSetBits);
    const EventBits_t bits = xEventGroupWaitBits(
                                 events, POWER_SERVICE_EVENT_RUNNING, pdFALSE, pdTRUE,
                                 _timeout_to_ticks(timeout_ms));
    if ((bits & POWER_SERVICE_EVENT_RUNNING) == 0)
    {
        LOG_E("pause timeout cancellation was not acknowledged");
    }
}

esp_err_t power_service_suspend(uint32_t timeout_ms)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    const SemaphoreHandle_t control = s_control_mutex;
    bool send_pause = false;
    bool control_owned = false;
    TaskHandle_t worker = NULL;
    EventGroupHandle_t events = NULL;

    if (control == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(control, portMAX_DELAY);
    control_owned = true;

    result = _reserve_pause(current_task, &worker, &events, &send_pause);
    if (worker == NULL)
    {
        goto exit;
    }

    if (send_pause)
    {
        xEventGroupClearBits(events, POWER_SERVICE_EVENT_PAUSED);
        xTaskNotify(worker, POWER_SERVICE_CMD_PAUSE, eSetBits);
    }

    const EventBits_t bits = xEventGroupWaitBits(
                                 events, POWER_SERVICE_EVENT_PAUSED, pdFALSE, pdTRUE,
                                 _timeout_to_ticks(timeout_ms));
    result = (bits & POWER_SERVICE_EVENT_PAUSED) != 0 ?
             ESP_OK : ESP_ERR_TIMEOUT;
    if (result == ESP_ERR_TIMEOUT)
    {
        _cancel_timed_out_pause(worker, events, timeout_ms);
    }

exit:
    if (control_owned)
    {
        xSemaphoreGive(control);
    }
    return result;
}

esp_err_t power_service_resume(uint32_t timeout_ms)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    const SemaphoreHandle_t control = s_control_mutex;
    bool wait_for_running = false;
    bool control_owned = false;
    TaskHandle_t worker = NULL;
    EventGroupHandle_t events = NULL;

    if (control == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(control, portMAX_DELAY);
    control_owned = true;

    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || s_worker == NULL || s_worker_events == NULL ||
            current_task == s_worker)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else if (s_state == POWER_SERVICE_STATE_RUNNING)
    {
        result = ESP_OK;
    }
    else if (s_state == POWER_SERVICE_STATE_PAUSED ||
             s_state == POWER_SERVICE_STATE_PAUSE_PENDING ||
             s_state == POWER_SERVICE_STATE_RESUME_PENDING)
    {
        s_state = POWER_SERVICE_STATE_RESUME_PENDING;
        worker = s_worker;
        events = s_worker_events;
        wait_for_running = true;
    }
    else
    {
        result = ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (!wait_for_running)
    {
        goto exit;
    }

    xEventGroupClearBits(events, POWER_SERVICE_EVENT_RUNNING);
    xTaskNotify(worker, POWER_SERVICE_CMD_RESUME, eSetBits);

    const EventBits_t bits = xEventGroupWaitBits(
                                 events, POWER_SERVICE_EVENT_RUNNING, pdFALSE, pdTRUE,
                                 _timeout_to_ticks(timeout_ms));
    result = (bits & POWER_SERVICE_EVENT_RUNNING) != 0 ?
             ESP_OK : ESP_ERR_TIMEOUT;

exit:
    if (control_owned)
    {
        xSemaphoreGive(control);
    }
    return result;
}

esp_err_t power_service_get_snapshot(power_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    const bool ready = s_initialized;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    return ESP_OK;
}

esp_err_t power_service_get_info(power_info_t *info)
{
    power_service_snapshot_t snapshot;
    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = power_service_get_snapshot(&snapshot);
    if (result != ESP_OK)
    {
        return result;
    }
    if (!snapshot.valid)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *info = snapshot.info;
    return ESP_OK;
}
