#define DBG_TAG "imu_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "imu_service.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

EVENT_BUS_DEFINE_ID(IMU_SERVICE_MSG);

#define IMU_SERVICE_CMD_PAUSE       BIT0
#define IMU_SERVICE_CMD_RESUME      BIT1
#define IMU_SERVICE_CMD_STOP        BIT2
#define IMU_SERVICE_EVENT_RUNNING   BIT0
#define IMU_SERVICE_EVENT_PAUSED    BIT1
#define IMU_SERVICE_EVENT_STOPPED   BIT2
#define IMU_SERVICE_STATUS_INT1_LEVEL (1U << 1)

static imu_service_imu_ops_t s_ops;
static imu_service_config_t s_config;
static bool s_ops_registered;
static bool s_initialized;
static TaskHandle_t s_worker;
static EventGroupHandle_t s_worker_events;
static SemaphoreHandle_t s_control_mutex;
static SemaphoreHandle_t s_io_mutex;
static imu_service_state_t s_state = IMU_SERVICE_STATE_STOPPED;
static uint32_t s_active_readers;
static bool s_sensor_disable_required;
static imu_service_snapshot_t s_snapshot;
static uint32_t s_next_sequence;
static imu_service_sample_t s_pending_interrupt_sample;
static bool s_interrupt_event_pending;
static atomic_bool s_worker_event_tail_complete = ATOMIC_VAR_INIT(true);
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;

static TickType_t _timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == IMU_SERVICE_WAIT_FOREVER)
    {
        return portMAX_DELAY;
    }

    uint64_t ticks = ((uint64_t)timeout_ms * (uint64_t)configTICK_RATE_HZ +
                      999ULL) / 1000ULL;
    if (timeout_ms != 0U && ticks == 0U)
    {
        ticks = 1U;
    }
    if (ticks >= (uint64_t)portMAX_DELAY)
    {
        ticks = (uint64_t)portMAX_DELAY - 1U;
    }
    return (TickType_t)ticks;
}

static TickType_t _sample_period_ticks(void)
{
    uint32_t rate = s_config.sample_rate_hz;
    if (rate == 0U)
    {
        rate = 1U;
    }
    uint64_t ticks = ((uint64_t)configTICK_RATE_HZ + rate - 1U) / rate;
    if (ticks == 0U)
    {
        ticks = 1U;
    }
    if (ticks >= (uint64_t)portMAX_DELAY)
    {
        ticks = (uint64_t)portMAX_DELAY - 1U;
    }
    return (TickType_t)ticks;
}

static void _wait_for_worker_event_tail(void)
{
    while (!atomic_load_explicit(&s_worker_event_tail_complete,
                                 memory_order_acquire))
    {
        vTaskDelay(1);
    }
}

static void _set_state(imu_service_state_t state)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_state = state;
    taskEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t _wait_for_readers(uint32_t timeout_ms)
{
    TickType_t remaining = _timeout_to_ticks(timeout_ms);

    for (;;)
    {
        taskENTER_CRITICAL(&s_state_lock);
        const bool quiesced = s_active_readers == 0U;
        taskEXIT_CRITICAL(&s_state_lock);
        if (quiesced)
        {
            return ESP_OK;
        }
        if (remaining == 0U)
        {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(1);
        if (remaining != portMAX_DELAY)
        {
            --remaining;
        }
    }
}

static esp_err_t _acquire_reader(SemaphoreHandle_t *io_mutex)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool readable = s_initialized &&
                          s_state == IMU_SERVICE_STATE_RUNNING &&
                          s_ops_registered && s_ops.read != NULL &&
                          s_io_mutex != NULL;
    if (readable)
    {
        ++s_active_readers;
        *io_mutex = s_io_mutex;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return readable ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void _release_reader(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    --s_active_readers;
    taskEXIT_CRITICAL(&s_state_lock);
}

static bool _hardware_available(void)
{
    bool available = s_ops_registered && s_ops.read != NULL;
    if (available && s_ops.is_available != NULL)
    {
        available = s_ops.is_available();
    }
    return available;
}

static esp_err_t _read_hardware(imu_service_sample_t *sample,
                                SemaphoreHandle_t io_mutex)
{
    if (!s_ops_registered || s_ops.read == NULL || io_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(io_mutex, portMAX_DELAY);
    esp_err_t result = s_ops.read(sample);
    if (result == ESP_OK)
    {
        if ((sample->status_int & IMU_SERVICE_STATUS_INT1_LEVEL) != 0U)
        {
            sample->interrupt_active = true;
            sample->interrupt_level_valid = true;
        }
        if (s_ops.poll_interrupt != NULL)
        {
            bool active = false;
            if (s_ops.poll_interrupt(&active) == ESP_OK)
            {
                sample->interrupt_active = sample->interrupt_active || active;
                sample->interrupt_level_valid = true;
            }
        }
    }
    xSemaphoreGive(io_mutex);
    return result;
}

static esp_err_t _set_sensor_enabled(bool enabled)
{
    if (!s_ops_registered || s_ops.set_enabled == NULL)
    {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_state_lock);
    SemaphoreHandle_t io_mutex = s_io_mutex;
    if (enabled)
    {
        s_sensor_disable_required = true;
    }
    const bool required = enabled || s_sensor_disable_required;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!required || io_mutex == NULL)
    {
        return ESP_OK;
    }

    xSemaphoreTake(io_mutex, portMAX_DELAY);
    const esp_err_t result = s_ops.set_enabled(enabled);
    xSemaphoreGive(io_mutex);
    if (!enabled && result == ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_sensor_disable_required = false;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    return result;
}

static esp_err_t _configure_sensor(void)
{
    if (!s_ops_registered || s_ops.configure == NULL)
    {
        return ESP_OK;
    }
    if (s_io_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_io_mutex, portMAX_DELAY);
    const esp_err_t result = s_ops.configure(s_config.sample_rate_hz);
    xSemaphoreGive(s_io_mutex);
    return result;
}

static void _publish_snapshot(uint32_t subtype,
                              const imu_service_snapshot_t *snapshot,
                              uint32_t flags)
{
    const esp_err_t result = event_bus_publish(IMU_SERVICE_MSG, subtype,
                             snapshot, sizeof(*snapshot), flags);
    if (result != ESP_OK)
    {
        LOG_W("snapshot publish failed: 0x%x", result);
    }
}

static esp_err_t _publish_pending_interrupt(void)
{
    if (!s_interrupt_event_pending)
    {
        return ESP_OK;
    }

    const imu_service_sample_t sample = s_pending_interrupt_sample;
    esp_err_t result = event_bus_publish(IMU_SERVICE_MSG,
                                         IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT,
                                         &sample, sizeof(sample), 0U);
    if (result == ESP_OK && s_interrupt_event_pending &&
            s_pending_interrupt_sample.sequence == sample.sequence)
    {
        memset(&s_pending_interrupt_sample, 0,
               sizeof(s_pending_interrupt_sample));
        s_interrupt_event_pending = false;
    }
    return result;
}

static void _update_snapshot(bool available, bool valid,
                             imu_service_sample_t *sample)
{
    imu_service_snapshot_t next;
    bool availability_changed;
    bool interrupt_event = false;

    taskENTER_CRITICAL(&s_snapshot_lock);
    next = s_snapshot;
    availability_changed = next.available != available;
    if (valid)
    {
        sample->sampled_at_us = esp_timer_get_time();
        ++s_next_sequence;
        if (s_next_sequence == 0U)
        {
            ++s_next_sequence;
        }
        sample->sequence = s_next_sequence;
        if (sample->interrupt_level_valid)
        {
            const bool previous_active = next.valid &&
                                         next.sample.interrupt_level_valid &&
                                         next.sample.interrupt_active;
            interrupt_event = sample->interrupt_active && !previous_active;
        }
        interrupt_event = interrupt_event ||
                          (sample->status_int &
                           IMU_SERVICE_STATUS_INT1_LEVEL) != 0U;
        next.sample = *sample;
        next.sampled_at_us = sample->sampled_at_us;
        next.sequence = sample->sequence;
    }
    next.valid = valid;
    next.available = available;
    s_snapshot = next;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    if (availability_changed)
    {
        _publish_snapshot(IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED,
                          &next, 0U);
    }
    if (valid)
    {
        _publish_snapshot(IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE, &next,
                          EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
        if (interrupt_event)
        {
            s_pending_interrupt_sample = next.sample;
            s_interrupt_event_pending = true;
            const esp_err_t result = _publish_pending_interrupt();
            if (result != ESP_OK)
            {
                LOG_W("interrupt publish pending: 0x%x", result);
            }
        }
    }
}

static void _worker_pause(uint32_t *commands)
{
    if ((*commands & IMU_SERVICE_CMD_PAUSE) == 0U)
    {
        return;
    }

    /* A resume can arrive before the worker observes a pending pause. Treat
     * the pair as a canceled pause instead of leaving callers waiting for a
     * PAUSED acknowledgement that will never be consumed. */
    if ((*commands & IMU_SERVICE_CMD_RESUME) != 0U)
    {
        _set_state(IMU_SERVICE_STATE_RUNNING);
        xEventGroupClearBits(s_worker_events, IMU_SERVICE_EVENT_PAUSED);
        xEventGroupSetBits(s_worker_events, IMU_SERVICE_EVENT_RUNNING);
        *commands &= ~(IMU_SERVICE_CMD_PAUSE | IMU_SERVICE_CMD_RESUME);
        return;
    }

    _set_state(IMU_SERVICE_STATE_PAUSED);
    xEventGroupClearBits(s_worker_events, IMU_SERVICE_EVENT_RUNNING);
    xEventGroupSetBits(s_worker_events, IMU_SERVICE_EVENT_PAUSED);
    *commands = 0U;

    for (;;)
    {
        uint32_t pending = 0U;
        xTaskNotifyWait(0U, UINT32_MAX, &pending, portMAX_DELAY);
        if ((pending & IMU_SERVICE_CMD_STOP) != 0U)
        {
            *commands = pending;
            return;
        }
        if ((pending & IMU_SERVICE_CMD_RESUME) != 0U)
        {
            _set_state(IMU_SERVICE_STATE_RUNNING);
            xEventGroupClearBits(s_worker_events, IMU_SERVICE_EVENT_PAUSED);
            xEventGroupSetBits(s_worker_events, IMU_SERVICE_EVENT_RUNNING);
            *commands = 0U;
            return;
        }
    }
}

static void _worker_apply_resume(uint32_t *commands)
{
    if ((*commands & IMU_SERVICE_CMD_RESUME) == 0U)
    {
        return;
    }
    _set_state(IMU_SERVICE_STATE_RUNNING);
    xEventGroupClearBits(s_worker_events, IMU_SERVICE_EVENT_PAUSED);
    xEventGroupSetBits(s_worker_events, IMU_SERVICE_EVENT_RUNNING);
    *commands &= ~(IMU_SERVICE_CMD_PAUSE | IMU_SERVICE_CMD_RESUME);
}

static void _imu_worker(void *argument)
{
    SemaphoreHandle_t io_mutex = argument;
    uint32_t commands = 0U;
    const TickType_t period = _sample_period_ticks();

    for (;;)
    {
        if ((commands & IMU_SERVICE_CMD_STOP) != 0U)
        {
            break;
        }
        _worker_apply_resume(&commands);
        _worker_pause(&commands);
        if ((commands & IMU_SERVICE_CMD_STOP) != 0U)
        {
            break;
        }

        if (_publish_pending_interrupt() != ESP_OK)
        {
            commands = 0U;
            xTaskNotifyWait(0U, UINT32_MAX, &commands, period);
            continue;
        }

        imu_service_sample_t sample;
        memset(&sample, 0, sizeof(sample));
        const bool available = _hardware_available();
        const esp_err_t result = available ? _read_hardware(&sample,
                                 io_mutex) :
                                 ESP_ERR_NOT_FOUND;
        _update_snapshot(available, result == ESP_OK, &sample);
        if (result != ESP_OK && available)
        {
            LOG_W("IMU sample failed: 0x%x", result);
        }

        commands = 0U;
        xTaskNotifyWait(0U, UINT32_MAX, &commands, period);
    }

    xEventGroupSetBits(s_worker_events, IMU_SERVICE_EVENT_STOPPED);
    taskENTER_CRITICAL(&s_state_lock);
    s_worker = NULL;
    taskEXIT_CRITICAL(&s_state_lock);
    atomic_store_explicit(&s_worker_event_tail_complete, true,
                          memory_order_release);
    vTaskDelete(NULL);
}

esp_err_t imu_service_register_ops(const imu_service_imu_ops_t *ops)
{
    if (ops == NULL || ops->read == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state_lock);
    const bool active = s_initialized || s_state != IMU_SERVICE_STATE_STOPPED;
    if (active)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ops = *ops;
    s_ops_registered = true;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t imu_service_register_imu_ops(const imu_service_imu_ops_t *ops)
{
    return imu_service_register_ops(ops);
}

esp_err_t imu_service_init(const imu_service_config_t *config)
{
    if (config == NULL || config->sample_rate_hz == 0U ||
            config->sample_rate_hz > 1000U || config->task_priority == 0U ||
            config->task_priority >= configMAX_PRIORITIES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (s_initialized)
    {
        const bool same_config = s_config.sample_rate_hz ==
                                 config->sample_rate_hz &&
                                 s_config.task_priority == config->task_priority;
        taskEXIT_CRITICAL(&s_state_lock);
        return same_config ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (s_state != IMU_SERVICE_STATE_STOPPED)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = IMU_SERVICE_STATE_STARTING;
    s_active_readers = 0U;
    s_sensor_disable_required = false;
    s_worker = NULL;
    s_config = *config;
    taskEXIT_CRITICAL(&s_state_lock);

    esp_err_t result = ESP_OK;
    s_io_mutex = xSemaphoreCreateMutex();
    if (s_io_mutex == NULL)
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
    s_worker_events = xEventGroupCreate();
    if (s_worker_events == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_next_sequence = 0U;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    memset(&s_pending_interrupt_sample, 0,
           sizeof(s_pending_interrupt_sample));
    s_interrupt_event_pending = false;

    result = _configure_sensor();
    if (result != ESP_OK)
    {
        goto cleanup;
    }
    result = _set_sensor_enabled(true);
    if (result != ESP_OK)
    {
        goto cleanup;
    }

    atomic_store_explicit(&s_worker_event_tail_complete, false,
                          memory_order_release);
    if (xTaskCreate(_imu_worker, "imu_service", CONFIG_IMU_SERVICE_TASK_STACK,
                    s_io_mutex, s_config.task_priority,
                    &s_worker) != pdPASS)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_initialized = true;
    s_state = IMU_SERVICE_STATE_RUNNING;
    taskEXIT_CRITICAL(&s_state_lock);
    xEventGroupSetBits(s_worker_events, IMU_SERVICE_EVENT_RUNNING);
    LOG_I("worker started (rate=%uHz)", (unsigned)s_config.sample_rate_hz);
    return ESP_OK;

cleanup:
    atomic_store_explicit(&s_worker_event_tail_complete, true,
                          memory_order_release);
    const esp_err_t cleanup_result = _set_sensor_enabled(false);
    if (cleanup_result != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_initialized = false;
        s_worker = NULL;
        s_state = IMU_SERVICE_STATE_ERROR;
        taskEXIT_CRITICAL(&s_state_lock);
        return result;
    }
    if (s_worker_events != NULL)
    {
        vEventGroupDelete(s_worker_events);
        s_worker_events = NULL;
    }
    if (s_control_mutex != NULL)
    {
        vSemaphoreDelete(s_control_mutex);
        s_control_mutex = NULL;
    }
    if (s_io_mutex != NULL)
    {
        vSemaphoreDelete(s_io_mutex);
        s_io_mutex = NULL;
    }
    taskENTER_CRITICAL(&s_state_lock);
    s_initialized = false;
    s_worker = NULL;
    s_active_readers = 0U;
    s_state = IMU_SERVICE_STATE_STOPPED;
    taskEXIT_CRITICAL(&s_state_lock);
    return result;
}

esp_err_t imu_service_start(const imu_service_config_t *config)
{
    return imu_service_init(config);
}

esp_err_t imu_service_stop(uint32_t timeout_ms)
{
    SemaphoreHandle_t control = s_control_mutex;
    if (control == NULL)
    {
        return ESP_OK;
    }
    if (xTaskGetCurrentTaskHandle() == s_worker)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(control, portMAX_DELAY);
    taskENTER_CRITICAL(&s_state_lock);
    TaskHandle_t worker = s_worker;
    EventGroupHandle_t events = s_worker_events;
    const bool active = s_initialized || worker != NULL || events != NULL ||
                        s_io_mutex != NULL || s_sensor_disable_required;
    const bool notify_worker = active && worker != NULL &&
                               s_state != IMU_SERVICE_STATE_STOPPING;
    if (active)
    {
        s_state = IMU_SERVICE_STATE_STOPPING;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (!active)
    {
        xSemaphoreGive(control);
        return ESP_OK;
    }

    if (worker != NULL)
    {
        if (notify_worker)
        {
            xTaskNotify(worker, IMU_SERVICE_CMD_STOP, eSetBits);
        }
        const EventBits_t bits = xEventGroupWaitBits(
                                     events, IMU_SERVICE_EVENT_STOPPED, pdFALSE,
                                     pdTRUE, _timeout_to_ticks(timeout_ms));
        if ((bits & IMU_SERVICE_EVENT_STOPPED) == 0U)
        {
            xSemaphoreGive(control);
            return ESP_ERR_TIMEOUT;
        }
    }
    _wait_for_worker_event_tail();

    esp_err_t result = _wait_for_readers(timeout_ms);
    if (result != ESP_OK)
    {
        xSemaphoreGive(control);
        return result;
    }

    result = _set_sensor_enabled(false);
    if (result != ESP_OK)
    {
        _set_state(IMU_SERVICE_STATE_ERROR);
        xSemaphoreGive(control);
        return result;
    }

    taskENTER_CRITICAL(&s_state_lock);
    events = s_worker_events;
    SemaphoreHandle_t io_mutex = s_io_mutex;
    s_worker_events = NULL;
    s_io_mutex = NULL;
    s_control_mutex = NULL;
    s_initialized = false;
    s_worker = NULL;
    s_active_readers = 0U;
    s_state = IMU_SERVICE_STATE_STOPPED;
    taskEXIT_CRITICAL(&s_state_lock);

    if (events != NULL)
    {
        vEventGroupDelete(events);
    }
    if (io_mutex != NULL)
    {
        vSemaphoreDelete(io_mutex);
    }
    xSemaphoreGive(control);
    vSemaphoreDelete(control);
    return result;
}

esp_err_t imu_service_deinit(void)
{
    esp_err_t result = imu_service_stop(IMU_SERVICE_WAIT_FOREVER);
    if (result != ESP_OK)
    {
        return result;
    }

    taskENTER_CRITICAL(&s_state_lock);
    if (s_initialized || s_state == IMU_SERVICE_STATE_STOPPING)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_ops, 0, sizeof(s_ops));
    s_ops_registered = false;
    s_state = IMU_SERVICE_STATE_STOPPED;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t imu_service_suspend(uint32_t timeout_ms)
{
    SemaphoreHandle_t control = s_control_mutex;
    if (control == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == s_worker)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(control, portMAX_DELAY);

    taskENTER_CRITICAL(&s_state_lock);
    const bool active = s_initialized && s_worker != NULL &&
                        s_worker_events != NULL;
    const imu_service_state_t state = s_state;
    TaskHandle_t worker = s_worker;
    EventGroupHandle_t events = s_worker_events;
    if (active && state == IMU_SERVICE_STATE_RUNNING)
    {
        s_state = IMU_SERVICE_STATE_PAUSE_PENDING;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (!active || (state != IMU_SERVICE_STATE_RUNNING &&
                    state != IMU_SERVICE_STATE_PAUSED))
    {
        xSemaphoreGive(control);
        return ESP_ERR_INVALID_STATE;
    }
    if (state == IMU_SERVICE_STATE_PAUSED)
    {
        const esp_err_t result = _set_sensor_enabled(false);
        xSemaphoreGive(control);
        return result;
    }

    xEventGroupClearBits(events, IMU_SERVICE_EVENT_PAUSED);
    xTaskNotify(worker, IMU_SERVICE_CMD_PAUSE, eSetBits);
    const EventBits_t bits = xEventGroupWaitBits(
                                 events, IMU_SERVICE_EVENT_PAUSED, pdFALSE,
                                 pdTRUE, _timeout_to_ticks(timeout_ms));
    esp_err_t result = (bits & IMU_SERVICE_EVENT_PAUSED) != 0U ?
                       ESP_OK : ESP_ERR_TIMEOUT;
    if (result != ESP_OK)
    {
        goto cancel_pause;
    }

    result = _wait_for_readers(timeout_ms);
    if (result != ESP_OK)
    {
        goto cancel_pause;
    }

    result = _set_sensor_enabled(false);
    xSemaphoreGive(control);
    return result;

cancel_pause:
    xEventGroupClearBits(events, IMU_SERVICE_EVENT_RUNNING);
    taskENTER_CRITICAL(&s_state_lock);
    if (s_initialized && s_worker == worker &&
            s_state != IMU_SERVICE_STATE_RUNNING)
    {
        s_state = IMU_SERVICE_STATE_RESUME_PENDING;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    xTaskNotify(worker, IMU_SERVICE_CMD_RESUME, eSetBits);
    (void)xEventGroupWaitBits(events, IMU_SERVICE_EVENT_RUNNING, pdFALSE,
                              pdTRUE, _timeout_to_ticks(timeout_ms));
    xSemaphoreGive(control);
    return result;
}

esp_err_t imu_service_resume(uint32_t timeout_ms)
{
    SemaphoreHandle_t control = s_control_mutex;
    if (control == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == s_worker)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(control, portMAX_DELAY);

    taskENTER_CRITICAL(&s_state_lock);
    EventGroupHandle_t events = s_worker_events;
    bool active = s_initialized && s_worker != NULL && events != NULL;
    const imu_service_state_t initial_state = s_state;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!active)
    {
        xSemaphoreGive(control);
        return ESP_ERR_INVALID_STATE;
    }
    if (initial_state == IMU_SERVICE_STATE_RUNNING)
    {
        xSemaphoreGive(control);
        return ESP_OK;
    }

    xEventGroupClearBits(events, IMU_SERVICE_EVENT_RUNNING);
    taskENTER_CRITICAL(&s_state_lock);
    active = s_initialized && s_worker != NULL &&
             s_worker_events == events;
    const imu_service_state_t state = s_state;
    TaskHandle_t worker = s_worker;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!active || (state != IMU_SERVICE_STATE_RUNNING &&
                    state != IMU_SERVICE_STATE_PAUSE_PENDING &&
                    state != IMU_SERVICE_STATE_PAUSED &&
                    state != IMU_SERVICE_STATE_RESUME_PENDING))
    {
        xSemaphoreGive(control);
        return ESP_ERR_INVALID_STATE;
    }
    if (state == IMU_SERVICE_STATE_RUNNING)
    {
        xEventGroupSetBits(events, IMU_SERVICE_EVENT_RUNNING);
        xSemaphoreGive(control);
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;
    if (state != IMU_SERVICE_STATE_RESUME_PENDING)
    {
        result = _set_sensor_enabled(true);
        if (result != ESP_OK)
        {
            xSemaphoreGive(control);
            return result;
        }
    }

    taskENTER_CRITICAL(&s_state_lock);
    const bool worker_running = s_state == IMU_SERVICE_STATE_RUNNING;
    if (!worker_running)
    {
        s_state = IMU_SERVICE_STATE_RESUME_PENDING;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (worker_running)
    {
        xSemaphoreGive(control);
        return ESP_OK;
    }

    xTaskNotify(worker, IMU_SERVICE_CMD_RESUME, eSetBits);
    const EventBits_t bits = xEventGroupWaitBits(
                                 events, IMU_SERVICE_EVENT_RUNNING, pdFALSE,
                                 pdTRUE, _timeout_to_ticks(timeout_ms));
    result = (bits & IMU_SERVICE_EVENT_RUNNING) != 0U ? ESP_OK :
             ESP_ERR_TIMEOUT;
    xSemaphoreGive(control);
    return result;
}

imu_service_state_t imu_service_get_state(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const imu_service_state_t state = s_state;
    taskEXIT_CRITICAL(&s_state_lock);
    return state;
}

esp_err_t imu_service_get_snapshot(imu_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    const bool initialized = s_initialized;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    *snapshot = s_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    return ESP_OK;
}

esp_err_t imu_service_read(imu_service_sample_t *sample)
{
    if (sample == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    SemaphoreHandle_t io_mutex = NULL;
    esp_err_t result = _acquire_reader(&io_mutex);
    if (result != ESP_OK)
    {
        return result;
    }

    memset(sample, 0, sizeof(*sample));
    result = _read_hardware(sample, io_mutex);
    if (result == ESP_OK)
    {
        taskENTER_CRITICAL(&s_snapshot_lock);
        ++s_next_sequence;
        if (s_next_sequence == 0U)
        {
            ++s_next_sequence;
        }
        sample->sampled_at_us = esp_timer_get_time();
        sample->sequence = s_next_sequence;
        taskEXIT_CRITICAL(&s_snapshot_lock);
    }
    _release_reader();
    return result;
}

esp_err_t imu_service_read_sample(imu_service_sample_t *sample)
{
    return imu_service_read(sample);
}
