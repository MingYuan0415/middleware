#define DBG_TAG "time_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "time_service.h"
#include "time_service_core.h"
#include "time_service_port.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TIME_SERVICE_SYNC_COMPLETE_BIT BIT0
#define TIME_SERVICE_WORKER_STOPPED_BIT BIT1
#define TIME_SERVICE_WORKER_PAUSED_BIT  BIT2
#define TIME_SERVICE_WORKER_RUNNING_BIT BIT3
#define TIME_SERVICE_NOTIFY_SYNC         BIT0
#define TIME_SERVICE_NOTIFY_STOP         BIT1
#define TIME_SERVICE_NOTIFY_PAUSE        BIT2
#define TIME_SERVICE_NOTIFY_RESUME       BIT3
#define TIME_SERVICE_LEGACY_TIMEOUT_MS  30000U
#define TIME_SERVICE_ALARM_POLL_MS       100U

EVENT_BUS_DEFINE_ID(TIME_SERVICE_MSG);

typedef enum time_service_sleep_state
{
    TIME_SERVICE_SLEEP_STOPPED = 0,
    TIME_SERVICE_SLEEP_RUNNING,
    TIME_SERVICE_SLEEP_SUSPEND_PENDING,
    TIME_SERVICE_SLEEP_SUSPENDED,
    TIME_SERVICE_SLEEP_RESUME_PENDING,
} time_service_sleep_state_t;

typedef struct time_service_deadline
{
    TickType_t started_at;
    TickType_t duration;
    bool wait_forever;
} time_service_deadline_t;

static time_service_rtc_ops_t s_rtc_ops;
static time_service_config_t s_config;
static char s_timezone[TIME_SERVICE_TIMEZONE_MAX_BYTES + 1U];
static char s_sntp_server[TIME_SERVICE_SNTP_SERVER_MAX_BYTES + 1U];
static bool s_rtc_ops_registered;
static bool s_ntp_initialized;
static bool s_initialized;
static bool s_stopping;
static bool s_sync_pending;
static uint32_t s_completed_generation;
static esp_err_t s_completed_result = ESP_ERR_INVALID_STATE;
static time_service_quality_t s_quality = TIME_SERVICE_QUALITY_INVALID;
static esp_err_t s_last_rtc_error = ESP_ERR_NOT_SUPPORTED;
static EventGroupHandle_t s_sync_events;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_control_mutex;
static SemaphoreHandle_t s_update_mutex;
static TaskHandle_t s_sync_worker;
static atomic_uint_fast32_t s_active_generation;
static atomic_uint_fast32_t s_notified_generation;
static atomic_uint s_callback_active;
static atomic_bool s_callback_enabled;
static atomic_bool s_worker_event_tail_complete = ATOMIC_VAR_INIT(true);
static atomic_bool s_rtc_io_admitted = ATOMIC_VAR_INIT(false);
static time_service_sleep_state_t s_sleep_state = TIME_SERVICE_SLEEP_STOPPED;
static bool s_alarm_monitor_enabled;
static bool s_alarm_event_pending;
static time_service_alarm_event_t s_pending_alarm_event;
static uint32_t s_alarm_sequence;
static esp_err_t s_alarm_worker_error = ESP_OK;

static bool _rtc_available(void)
{
    bool available = s_rtc_ops_registered;
    if (available && s_rtc_ops.is_available != NULL)
    {
        available = s_rtc_ops.is_available();
    }
    return available;
}

static bool _rtc_alarm_supported(void)
{
    return s_rtc_ops_registered && s_rtc_ops.alarm_configure != NULL &&
           s_rtc_ops.alarm_disable != NULL &&
           s_rtc_ops.alarm_get_status != NULL &&
           s_rtc_ops.alarm_clear != NULL &&
           s_rtc_ops.alarm_poll_interrupt != NULL;
}

static bool _alarm_config_valid(const time_service_alarm_config_t *config)
{
    if (config == NULL ||
            (!config->match_second && !config->match_minute &&
             !config->match_hour && !config->match_day &&
             !config->match_weekday))
    {
        return false;
    }
    return (!config->match_second || config->second <= 59U) &&
           (!config->match_minute || config->minute <= 59U) &&
           (!config->match_hour || config->hour <= 23U) &&
           (!config->match_day ||
            (config->day >= 1U && config->day <= 31U)) &&
           (!config->match_weekday || config->weekday <= 6U);
}

static bool _rtc_io_allowed(void)
{
    return atomic_load_explicit(&s_rtc_io_admitted, memory_order_acquire);
}

static void _set_alarm_monitor_enabled(bool enabled)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_alarm_monitor_enabled = enabled;
    xSemaphoreGive(s_state_mutex);
}

static void _wait_for_worker_event_tail(void)
{
    while (!atomic_load_explicit(&s_worker_event_tail_complete,
                                 memory_order_acquire))
    {
        vTaskDelay(1);
    }
}

static esp_err_t _set_system_epoch(int64_t epoch)
{
    return time_service_port_clock_set(epoch);
}

static esp_err_t _get_system_epoch(int64_t *epoch)
{
    return time_service_port_clock_get(epoch);
}

static esp_err_t _write_rtc_epoch(int64_t epoch)
{
    if (!_rtc_available() || s_rtc_ops.write == NULL)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    struct tm utc_time;
    if (!time_service_core_epoch_to_utc(epoch, &utc_time))
    {
        return ESP_ERR_INVALID_ARG;
    }
    return s_rtc_ops.write(&utc_time);
}

static uint32_t _next_generation_locked(void)
{
    uint32_t generation = (uint32_t)atomic_load(&s_active_generation) + 1U;
    if (generation == 0)
    {
        generation = 1U;
    }
    atomic_store(&s_active_generation, generation);
    return generation;
}

static void _ntp_sync_callback(struct timeval *value)
{
    (void)value;
    atomic_fetch_add(&s_callback_active, 1U);
    if (atomic_load(&s_callback_enabled))
    {
        TaskHandle_t worker = s_sync_worker;
        const uint32_t generation =
            (uint32_t)atomic_load(&s_active_generation);
        if (worker != NULL)
        {
            atomic_store(&s_notified_generation, generation);
            xTaskNotify(worker, TIME_SERVICE_NOTIFY_SYNC, eSetBits);
        }
    }
    atomic_fetch_sub(&s_callback_active, 1U);
}

static void _wait_for_sntp_callbacks(void)
{
    while (atomic_load(&s_callback_active) != 0U)
    {
        vTaskDelay(1);
    }
}

static esp_err_t _stop_sntp_locked(void)
{
    esp_err_t result = ESP_OK;
    atomic_store(&s_callback_enabled, false);
    if (s_ntp_initialized)
    {
        result = time_service_port_sntp_stop();
        if (result == ESP_OK)
        {
            s_ntp_initialized = false;
        }
    }
    _wait_for_sntp_callbacks();
    return result;
}

static void _complete_generation_locked(uint32_t generation, esp_err_t result)
{
    if (s_sync_pending &&
            generation == (uint32_t)atomic_load(&s_active_generation))
    {
        s_completed_generation = generation;
        s_completed_result = result;
        s_sync_pending = false;
        xEventGroupSetBits(s_sync_events, TIME_SERVICE_SYNC_COMPLETE_BIT);
    }
}

static bool _process_sync_notification(void)
{
    if (!_rtc_io_allowed())
    {
        return false;
    }
    const uint32_t generation =
        (uint32_t)atomic_load(&s_notified_generation);

    xSemaphoreTake(s_update_mutex, portMAX_DELAY);
    if (!_rtc_io_allowed())
    {
        xSemaphoreGive(s_update_mutex);
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool current_session = !s_stopping && s_ntp_initialized &&
                                 generation == (uint32_t)atomic_load(
                                     &s_active_generation);
    xSemaphoreGive(s_state_mutex);
    if (!current_session)
    {
        xSemaphoreGive(s_update_mutex);
        return true;
    }

    int64_t epoch;
    esp_err_t sync_result = _get_system_epoch(&epoch);
    esp_err_t rtc_result = ESP_ERR_NOT_SUPPORTED;
    if (sync_result == ESP_OK)
    {
        rtc_result = _write_rtc_epoch(epoch);
        if (rtc_result != ESP_OK && rtc_result != ESP_ERR_NOT_SUPPORTED)
        {
            LOG_W("RTC write after NTP sync failed: 0x%x", rtc_result);
        }
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_stopping && s_ntp_initialized &&
            generation == (uint32_t)atomic_load(&s_active_generation))
    {
        if (sync_result == ESP_OK)
        {
            s_quality = TIME_SERVICE_QUALITY_NTP;
            s_last_rtc_error = rtc_result;
        }
        _complete_generation_locked(generation, sync_result);
    }
    xSemaphoreGive(s_state_mutex);
    xSemaphoreGive(s_update_mutex);
    return true;
}

static esp_err_t _publish_pending_alarm_event(void)
{
    time_service_alarm_event_t event = {0};
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool pending = s_alarm_event_pending && !s_stopping;
    if (pending)
    {
        event = s_pending_alarm_event;
    }
    xSemaphoreGive(s_state_mutex);
    if (!pending)
    {
        return ESP_OK;
    }

    esp_err_t result = event_bus_publish(
                           TIME_SERVICE_MSG,
                           TIME_SERVICE_MSG_SUB_TYPE_RTC_ALARM,
                           &event, sizeof(event), 0U);
    if (result == ESP_OK)
    {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (s_alarm_event_pending &&
                s_pending_alarm_event.sequence == event.sequence)
        {
            s_alarm_event_pending = false;
        }
        xSemaphoreGive(s_state_mutex);
    }
    return result;
}

static esp_err_t _poll_rtc_alarm(void)
{
    if (!_rtc_io_allowed())
    {
        return ESP_OK;
    }
    esp_err_t result = _publish_pending_alarm_event();
    if (result != ESP_OK)
    {
        return result;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool monitor = s_alarm_monitor_enabled && !s_stopping;
    xSemaphoreGive(s_state_mutex);
    if (!monitor || !_rtc_alarm_supported() || !_rtc_available())
    {
        return ESP_OK;
    }

    xSemaphoreTake(s_update_mutex, portMAX_DELAY);
    if (!_rtc_io_allowed())
    {
        xSemaphoreGive(s_update_mutex);
        return ESP_OK;
    }
    bool interrupt_active = false;
    result = s_rtc_ops.alarm_poll_interrupt(&interrupt_active);
    if (result != ESP_OK || !interrupt_active)
    {
        goto exit;
    }

    time_service_alarm_status_t status = {0};
    result = s_rtc_ops.alarm_get_status(&status);
    if (result != ESP_OK)
    {
        goto exit;
    }
    if (!status.enabled)
    {
        _set_alarm_monitor_enabled(false);
        goto exit;
    }
    if (!status.pending)
    {
        goto exit;
    }

    result = s_rtc_ops.alarm_clear();
    if (result == ESP_OK)
    {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        ++s_alarm_sequence;
        if (s_alarm_sequence == 0U)
        {
            s_alarm_sequence = 1U;
        }
        s_pending_alarm_event.sequence = s_alarm_sequence;
        s_alarm_event_pending = true;
        xSemaphoreGive(s_state_mutex);
    }

exit:
    xSemaphoreGive(s_update_mutex);
    return result == ESP_OK ? _publish_pending_alarm_event() : result;
}

static void _worker_set_running(void)
{
    xEventGroupClearBits(s_sync_events, TIME_SERVICE_WORKER_PAUSED_BIT);
    xEventGroupSetBits(s_sync_events, TIME_SERVICE_WORKER_RUNNING_BIT);
}

static bool _worker_pause(uint32_t *notifications)
{
    if ((*notifications & TIME_SERVICE_NOTIFY_PAUSE) == 0U)
    {
        return false;
    }
    if ((*notifications & TIME_SERVICE_NOTIFY_RESUME) != 0U)
    {
        *notifications &= ~(TIME_SERVICE_NOTIFY_PAUSE |
                            TIME_SERVICE_NOTIFY_RESUME);
        _worker_set_running();
        return false;
    }

    xEventGroupClearBits(s_sync_events, TIME_SERVICE_WORKER_RUNNING_BIT);
    xEventGroupSetBits(s_sync_events, TIME_SERVICE_WORKER_PAUSED_BIT);
    *notifications &= ~TIME_SERVICE_NOTIFY_PAUSE;
    for (;;)
    {
        uint32_t pending = 0U;
        xTaskNotifyWait(0U, UINT32_MAX, &pending, portMAX_DELAY);
        *notifications |= pending;
        if ((*notifications & TIME_SERVICE_NOTIFY_STOP) != 0U)
        {
            return true;
        }
        if ((*notifications & TIME_SERVICE_NOTIFY_RESUME) != 0U)
        {
            *notifications &= ~TIME_SERVICE_NOTIFY_RESUME;
            _worker_set_running();
            return false;
        }
    }
}

static void _sync_worker(void *context)
{
    (void)context;
    uint32_t notifications = 0U;
    for (;;)
    {
        uint32_t pending = 0U;
        xTaskNotifyWait(0U, UINT32_MAX, &pending,
                        pdMS_TO_TICKS(TIME_SERVICE_ALARM_POLL_MS));
        notifications |= pending;
        if ((notifications & TIME_SERVICE_NOTIFY_STOP) != 0U)
        {
            break;
        }
        if (_worker_pause(&notifications))
        {
            break;
        }
        if ((notifications & TIME_SERVICE_NOTIFY_RESUME) != 0U)
        {
            notifications &= ~TIME_SERVICE_NOTIFY_RESUME;
            _worker_set_running();
        }
        if ((notifications & TIME_SERVICE_NOTIFY_SYNC) != 0U &&
                _process_sync_notification())
        {
            notifications &= ~TIME_SERVICE_NOTIFY_SYNC;
        }

        const esp_err_t alarm_result = _poll_rtc_alarm();
        if (alarm_result != ESP_OK && alarm_result != s_alarm_worker_error)
        {
            LOG_W("RTC alarm worker failed: 0x%x", alarm_result);
        }
        s_alarm_worker_error = alarm_result;
    }

    xEventGroupClearBits(s_sync_events, TIME_SERVICE_WORKER_RUNNING_BIT |
                         TIME_SERVICE_WORKER_PAUSED_BIT);
    s_sync_worker = NULL;
    xEventGroupSetBits(s_sync_events, TIME_SERVICE_WORKER_STOPPED_BIT);
    atomic_store_explicit(&s_worker_event_tail_complete, true,
                          memory_order_release);
    vTaskDelete(NULL);
}

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

static time_service_deadline_t _deadline_start(uint32_t timeout_ms)
{
    const time_service_deadline_t deadline =
    {
        .started_at = xTaskGetTickCount(),
        .duration = _timeout_to_ticks(timeout_ms),
        .wait_forever = timeout_ms == TIME_SERVICE_WAIT_FOREVER,
    };
    return deadline;
}

static TickType_t _deadline_remaining(
    const time_service_deadline_t *deadline)
{
    if (deadline->wait_forever)
    {
        return portMAX_DELAY;
    }
    const TickType_t elapsed = xTaskGetTickCount() - deadline->started_at;
    return elapsed >= deadline->duration ? 0U : deadline->duration - elapsed;
}

static void _delete_service_mutexes(void)
{
    if (s_update_mutex != NULL)
    {
        vSemaphoreDelete(s_update_mutex);
        s_update_mutex = NULL;
    }
    if (s_control_mutex != NULL)
    {
        vSemaphoreDelete(s_control_mutex);
        s_control_mutex = NULL;
    }
    if (s_state_mutex != NULL)
    {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }
}

static void _clear_rtc_registration(void)
{
    memset(&s_rtc_ops, 0, sizeof(s_rtc_ops));
    s_rtc_ops_registered = false;
}

static void _reset_alarm_runtime(void)
{
    s_alarm_monitor_enabled = false;
    s_alarm_event_pending = false;
    memset(&s_pending_alarm_event, 0, sizeof(s_pending_alarm_event));
    s_alarm_sequence = 0U;
    s_alarm_worker_error = ESP_OK;
}

static void _reset_sleep_runtime(void)
{
    atomic_store_explicit(&s_rtc_io_admitted, false, memory_order_release);
    s_sleep_state = TIME_SERVICE_SLEEP_STOPPED;
}

static esp_err_t _create_service_mutexes(void)
{
    esp_err_t result = ESP_OK;
    s_state_mutex = xSemaphoreCreateMutex();
    s_control_mutex = xSemaphoreCreateMutex();
    s_update_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL || s_control_mutex == NULL ||
            s_update_mutex == NULL)
    {
        result = ESP_ERR_NO_MEM;
    }
    return result;
}

static esp_err_t _restore_initial_clock(void)
{
    int64_t initial_epoch = 0;
    if (setenv("TZ", s_config.timezone, 1) != 0)
    {
        return ESP_FAIL;
    }
    tzset();

    s_quality = TIME_SERVICE_QUALITY_INVALID;
    s_last_rtc_error = ESP_ERR_NOT_SUPPORTED;
    if (_rtc_available() && s_rtc_ops.read != NULL)
    {
        struct tm rtc_time;
        s_last_rtc_error = s_rtc_ops.read(&rtc_time);
        if (s_last_rtc_error == ESP_OK &&
                time_service_core_utc_to_epoch(&rtc_time, &initial_epoch))
        {
            s_quality = TIME_SERVICE_QUALITY_RTC;
        }
        else
        {
            if (s_last_rtc_error == ESP_OK)
            {
                s_last_rtc_error = ESP_ERR_INVALID_RESPONSE;
            }
            LOG_W("RTC is available but its UTC time is invalid");
        }
    }
    return _set_system_epoch(initial_epoch);
}

static void _restore_alarm_monitor(void)
{
    if (!_rtc_alarm_supported() || !_rtc_available())
    {
        return;
    }

    time_service_alarm_status_t status = {0};
    const esp_err_t result = s_rtc_ops.alarm_get_status(&status);
    if (result != ESP_OK)
    {
        LOG_W("RTC alarm status restore failed: 0x%x", result);
        return;
    }
    s_alarm_monitor_enabled = status.enabled;
}

static esp_err_t _create_sync_worker(void)
{
    s_sync_events = xEventGroupCreate();
    if (s_sync_events == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_worker_event_tail_complete, false,
                          memory_order_release);
    if (xTaskCreate(_sync_worker, "time_ntp",
                    CONFIG_TIME_SERVICE_SYNC_WORKER_STACK, NULL,
                    s_config.task_priority, &s_sync_worker) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void _activate_time_service(void)
{
    atomic_store(&s_active_generation, 0U);
    atomic_store(&s_notified_generation, 0U);
    atomic_store(&s_callback_active, 0U);
    atomic_store(&s_callback_enabled, false);
    s_completed_generation = 0;
    s_completed_result = ESP_ERR_INVALID_STATE;
    s_sync_pending = false;
    s_stopping = false;
    s_sleep_state = TIME_SERVICE_SLEEP_RUNNING;
    s_initialized = true;
    atomic_store_explicit(&s_rtc_io_admitted, true, memory_order_release);
    xEventGroupSetBits(s_sync_events, TIME_SERVICE_WORKER_RUNNING_BIT);
}

esp_err_t time_service_register_rtc_ops(const time_service_rtc_ops_t *ops)
{
    if (ops == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized || s_state_mutex != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool any_alarm_op = ops->alarm_configure != NULL ||
                              ops->alarm_disable != NULL ||
                              ops->alarm_get_status != NULL ||
                              ops->alarm_clear != NULL ||
                              ops->alarm_poll_interrupt != NULL;
    const bool complete_alarm_ops = ops->alarm_configure != NULL &&
                                    ops->alarm_disable != NULL &&
                                    ops->alarm_get_status != NULL &&
                                    ops->alarm_clear != NULL &&
                                    ops->alarm_poll_interrupt != NULL;
    if (any_alarm_op && !complete_alarm_ops)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_rtc_ops = *ops;
    s_rtc_ops_registered = true;
    return ESP_OK;
}

esp_err_t time_service_init(const time_service_config_t *config)
{
    if (config == NULL || config->timezone == NULL ||
            config->sntp_server == NULL || config->timezone[0] == '\0' ||
            config->sntp_server[0] == '\0' ||
            strlen(config->timezone) > TIME_SERVICE_TIMEZONE_MAX_BYTES ||
            strlen(config->sntp_server) > TIME_SERVICE_SNTP_SERVER_MAX_BYTES ||
            config->task_priority == 0U ||
            config->task_priority >= configMAX_PRIORITIES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_OK;
    if (s_initialized)
    {
        return strcmp(s_config.timezone, config->timezone) == 0 &&
               strcmp(s_config.sntp_server, config->sntp_server) == 0 &&
               s_config.task_priority == config->task_priority ?
               ESP_OK : ESP_ERR_INVALID_STATE;
    }

    memcpy(s_timezone, config->timezone, strlen(config->timezone) + 1U);
    memcpy(s_sntp_server, config->sntp_server,
           strlen(config->sntp_server) + 1U);
    s_config = *config;
    s_config.timezone = s_timezone;
    s_config.sntp_server = s_sntp_server;

    result = _create_service_mutexes();
    if (result != ESP_OK)
    {
        goto cleanup;
    }
    _reset_alarm_runtime();
    _reset_sleep_runtime();
    result = _restore_initial_clock();
    if (result != ESP_OK)
    {
        goto cleanup;
    }
    _restore_alarm_monitor();
    result = _create_sync_worker();
    if (result != ESP_OK)
    {
        goto cleanup;
    }

    _activate_time_service();
    LOG_I("initialized with quality=%d", (int)s_quality);
    return ESP_OK;

cleanup:
    atomic_store_explicit(&s_worker_event_tail_complete, true,
                          memory_order_release);
    if (s_sync_events != NULL)
    {
        vEventGroupDelete(s_sync_events);
        s_sync_events = NULL;
    }
    _reset_alarm_runtime();
    _reset_sleep_runtime();
    _delete_service_mutexes();
    return result;
}

esp_err_t time_service_deinit(void)
{
    esp_err_t result = ESP_OK;
    bool control_owned = false;
    if (!s_initialized && s_state_mutex == NULL)
    {
        _clear_rtc_registration();
        _reset_alarm_runtime();
        _reset_sleep_runtime();
        s_quality = TIME_SERVICE_QUALITY_INVALID;
        s_last_rtc_error = ESP_ERR_NOT_SUPPORTED;
        return ESP_OK;
    }
    if (s_state_mutex == NULL || s_control_mutex == NULL ||
            s_update_mutex == NULL || s_sync_events == NULL ||
            s_sync_worker == NULL ||
            xTaskGetCurrentTaskHandle() == s_sync_worker)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    control_owned = true;
    atomic_store_explicit(&s_rtc_io_admitted, false, memory_order_release);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_stopping = true;
    result = _stop_sntp_locked();
    if (s_sync_pending)
    {
        _complete_generation_locked((uint32_t)atomic_load(&s_active_generation),
                                    result == ESP_OK ? ESP_ERR_INVALID_STATE :
                                    result);
    }
    _next_generation_locked();
    if (result != ESP_OK)
    {
        xSemaphoreGive(s_state_mutex);
        goto exit;
    }
    s_initialized = false;
    xSemaphoreGive(s_state_mutex);

    xTaskNotify(s_sync_worker, TIME_SERVICE_NOTIFY_STOP, eSetBits);
    xEventGroupWaitBits(s_sync_events, TIME_SERVICE_WORKER_STOPPED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    _wait_for_worker_event_tail();

    vEventGroupDelete(s_sync_events);
    s_sync_events = NULL;
    _clear_rtc_registration();
    _reset_alarm_runtime();
    _reset_sleep_runtime();
    s_stopping = false;
    s_quality = TIME_SERVICE_QUALITY_INVALID;
    s_last_rtc_error = ESP_ERR_NOT_SUPPORTED;
    xSemaphoreGive(s_control_mutex);
    control_owned = false;
    _delete_service_mutexes();

exit:
    if (control_owned)
    {
        xSemaphoreGive(s_control_mutex);
    }
    return result;
}

esp_err_t time_service_suspend(uint32_t timeout_ms)
{
    SemaphoreHandle_t control = s_control_mutex;
    EventGroupHandle_t events = s_sync_events;
    TaskHandle_t worker = s_sync_worker;
    if (control == NULL || s_state_mutex == NULL || events == NULL ||
            worker == NULL || xTaskGetCurrentTaskHandle() == worker)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const time_service_deadline_t deadline = _deadline_start(timeout_ms);
    const bool admission_was_open = atomic_exchange_explicit(
                                        &s_rtc_io_admitted, false,
                                        memory_order_acq_rel);
    if (xSemaphoreTake(control, _deadline_remaining(&deadline)) != pdTRUE)
    {
        if (admission_was_open)
        {
            atomic_store_explicit(&s_rtc_io_admitted, true,
                                  memory_order_release);
        }
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool reopen_admission = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool active = s_initialized && !s_stopping &&
                        s_sync_worker == worker && s_sync_events == events;
    if (active && s_sleep_state == TIME_SERVICE_SLEEP_SUSPENDED)
    {
        result = ESP_OK;
    }
    else if (active && s_sleep_state == TIME_SERVICE_SLEEP_RUNNING &&
             admission_was_open)
    {
        s_sleep_state = TIME_SERVICE_SLEEP_SUSPEND_PENDING;
        result = ESP_OK;
    }
    else
    {
        reopen_admission = admission_was_open;
    }
    xSemaphoreGive(s_state_mutex);
    if (result != ESP_OK || !admission_was_open)
    {
        goto exit;
    }

    xEventGroupClearBits(events, TIME_SERVICE_WORKER_PAUSED_BIT);
    if (xTaskNotify(worker, TIME_SERVICE_NOTIFY_PAUSE, eSetBits) != pdPASS)
    {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_sleep_state = TIME_SERVICE_SLEEP_RUNNING;
        xSemaphoreGive(s_state_mutex);
        reopen_admission = true;
        result = ESP_FAIL;
        goto exit;
    }

    const EventBits_t bits = xEventGroupWaitBits(
                                 events, TIME_SERVICE_WORKER_PAUSED_BIT, pdFALSE,
                                 pdTRUE, _deadline_remaining(&deadline));
    if ((bits & TIME_SERVICE_WORKER_PAUSED_BIT) != 0U)
    {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_sleep_state = TIME_SERVICE_SLEEP_SUSPENDED;
        xSemaphoreGive(s_state_mutex);
        result = ESP_OK;
        goto exit;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_sleep_state = TIME_SERVICE_SLEEP_RESUME_PENDING;
    xSemaphoreGive(s_state_mutex);
    xEventGroupClearBits(events, TIME_SERVICE_WORKER_RUNNING_BIT);
    (void)xTaskNotify(worker, TIME_SERVICE_NOTIFY_RESUME, eSetBits);
    result = ESP_ERR_TIMEOUT;

exit:
    xSemaphoreGive(control);
    if (reopen_admission)
    {
        atomic_store_explicit(&s_rtc_io_admitted, true,
                              memory_order_release);
    }
    return result;
}

esp_err_t time_service_resume(uint32_t timeout_ms)
{
    SemaphoreHandle_t control = s_control_mutex;
    EventGroupHandle_t events = s_sync_events;
    TaskHandle_t worker = s_sync_worker;
    if (control == NULL || s_state_mutex == NULL || events == NULL ||
            worker == NULL || xTaskGetCurrentTaskHandle() == worker)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const time_service_deadline_t deadline = _deadline_start(timeout_ms);
    if (xSemaphoreTake(control, _deadline_remaining(&deadline)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool notify_worker = false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const bool active = s_initialized && !s_stopping &&
                        s_sync_worker == worker && s_sync_events == events;
    if (active && s_sleep_state == TIME_SERVICE_SLEEP_RUNNING)
    {
        atomic_store_explicit(&s_rtc_io_admitted, true,
                              memory_order_release);
        result = ESP_OK;
    }
    else if (active &&
             (s_sleep_state == TIME_SERVICE_SLEEP_SUSPEND_PENDING ||
              s_sleep_state == TIME_SERVICE_SLEEP_SUSPENDED ||
              s_sleep_state == TIME_SERVICE_SLEEP_RESUME_PENDING))
    {
        s_sleep_state = TIME_SERVICE_SLEEP_RESUME_PENDING;
        notify_worker = true;
    }
    xSemaphoreGive(s_state_mutex);
    if (!notify_worker)
    {
        goto exit;
    }

    xEventGroupClearBits(events, TIME_SERVICE_WORKER_RUNNING_BIT);
    if (xTaskNotify(worker, TIME_SERVICE_NOTIFY_RESUME, eSetBits) != pdPASS)
    {
        result = ESP_FAIL;
        goto exit;
    }
    const EventBits_t bits = xEventGroupWaitBits(
                                 events, TIME_SERVICE_WORKER_RUNNING_BIT, pdFALSE,
                                 pdTRUE, _deadline_remaining(&deadline));
    if ((bits & TIME_SERVICE_WORKER_RUNNING_BIT) == 0U)
    {
        result = ESP_ERR_TIMEOUT;
        goto exit;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_initialized && !s_stopping && s_sync_worker == worker &&
            s_sync_events == events)
    {
        s_sleep_state = TIME_SERVICE_SLEEP_RUNNING;
        atomic_store_explicit(&s_rtc_io_admitted, true,
                              memory_order_release);
        result = ESP_OK;
    }
    xSemaphoreGive(s_state_mutex);

exit:
    xSemaphoreGive(control);
    return result;
}

esp_err_t time_service_get_utc(struct tm *utc_time)
{
    if (utc_time == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t epoch;
    esp_err_t result = _get_system_epoch(&epoch);
    if (result != ESP_OK)
    {
        return result;
    }
    return time_service_core_epoch_to_utc(epoch, utc_time) ?
           ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t time_service_get_local(struct tm *local_time)
{
    if (local_time == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t epoch;
    esp_err_t result = _get_system_epoch(&epoch);
    if (result != ESP_OK)
    {
        return result;
    }
    const time_t native_epoch = (time_t)epoch;
    if ((int64_t)native_epoch != epoch)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    return localtime_r(&native_epoch, local_time) != NULL ? ESP_OK : ESP_FAIL;
}

static esp_err_t _cancel_active_sync(void)
{
    esp_err_t result;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    result = _stop_sntp_locked();
    if (s_sync_pending)
    {
        _complete_generation_locked((uint32_t)atomic_load(&s_active_generation),
                                    result == ESP_OK ? ESP_ERR_INVALID_STATE :
                                    result);
    }
    _next_generation_locked();
    xSemaphoreGive(s_state_mutex);
    return result;
}

static esp_err_t _apply_local_time(const struct tm *local_time,
                                   esp_err_t *rtc_result)
{
    esp_err_t result = ESP_OK;
    xSemaphoreTake(s_update_mutex, portMAX_DELAY);
    struct tm converted = *local_time;
    converted.tm_isdst = -1;
    errno = 0;
    const time_t native_epoch = mktime(&converted);
    if (native_epoch == (time_t) -1 && errno != 0)
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    result = _set_system_epoch((int64_t)native_epoch);
    if (result != ESP_OK)
    {
        goto exit;
    }

    *rtc_result = _write_rtc_epoch((int64_t)native_epoch);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_quality = TIME_SERVICE_QUALITY_MANUAL;
    s_last_rtc_error = *rtc_result;
    xSemaphoreGive(s_state_mutex);

exit:
    xSemaphoreGive(s_update_mutex);
    return result;
}

esp_err_t time_service_set_local(const struct tm *local_time)
{
    esp_err_t result = ESP_OK;
    esp_err_t rtc_result = ESP_ERR_NOT_SUPPORTED;
    bool control_owned = false;
    if (local_time == NULL || !time_service_core_tm_valid(local_time))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_control_mutex == NULL || s_state_mutex == NULL ||
            s_update_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    control_owned = true;
    if (!s_initialized || s_stopping || !_rtc_io_allowed())
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = _cancel_active_sync();
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = _apply_local_time(local_time, &rtc_result);

exit:
    if (control_owned)
    {
        xSemaphoreGive(s_control_mutex);
    }
    if (result == ESP_OK && rtc_result != ESP_OK &&
            rtc_result != ESP_ERR_NOT_SUPPORTED)
    {
        LOG_W("RTC write after manual update failed: 0x%x", rtc_result);
    }
    return result;
}

time_service_quality_t time_service_get_quality(void)
{
    if (s_state_mutex == NULL)
    {
        return TIME_SERVICE_QUALITY_INVALID;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    time_service_quality_t quality = s_quality;
    xSemaphoreGive(s_state_mutex);
    return quality;
}

esp_err_t time_service_get_last_rtc_error(void)
{
    if (s_state_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    esp_err_t result = s_last_rtc_error;
    xSemaphoreGive(s_state_mutex);
    return result;
}

esp_err_t time_service_alarm_configure(
    const time_service_alarm_config_t *config)
{
    if (!_alarm_config_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_control_mutex == NULL ||
            s_state_mutex == NULL || s_update_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    if (!s_initialized || s_stopping || !_rtc_io_allowed())
    {
        goto exit;
    }
    if (!_rtc_alarm_supported() || !_rtc_available())
    {
        result = ESP_ERR_NOT_SUPPORTED;
        goto exit;
    }

    xSemaphoreTake(s_update_mutex, portMAX_DELAY);
    result = s_rtc_ops.alarm_configure(config);
    if (result == ESP_OK)
    {
        _set_alarm_monitor_enabled(true);
    }
    xSemaphoreGive(s_update_mutex);

exit:
    xSemaphoreGive(s_control_mutex);
    return result;
}

esp_err_t time_service_alarm_disable(void)
{
    if (!s_initialized || s_control_mutex == NULL ||
            s_state_mutex == NULL || s_update_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    if (!s_initialized || s_stopping || !_rtc_io_allowed())
    {
        goto exit;
    }
    if (!_rtc_alarm_supported() || !_rtc_available())
    {
        result = ESP_ERR_NOT_SUPPORTED;
        goto exit;
    }

    xSemaphoreTake(s_update_mutex, portMAX_DELAY);
    result = s_rtc_ops.alarm_disable();
    if (result == ESP_OK)
    {
        _set_alarm_monitor_enabled(false);
    }
    xSemaphoreGive(s_update_mutex);

exit:
    xSemaphoreGive(s_control_mutex);
    return result;
}

esp_err_t time_service_alarm_get_status(time_service_alarm_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_control_mutex == NULL ||
            s_state_mutex == NULL || s_update_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    if (!s_initialized || s_stopping || !_rtc_io_allowed())
    {
        goto exit;
    }
    if (!_rtc_alarm_supported() || !_rtc_available())
    {
        result = ESP_ERR_NOT_SUPPORTED;
        goto exit;
    }

    time_service_alarm_status_t snapshot = {0};
    xSemaphoreTake(s_update_mutex, portMAX_DELAY);
    result = s_rtc_ops.alarm_get_status(&snapshot);
    if (result == ESP_OK)
    {
        result = s_rtc_ops.alarm_poll_interrupt(
                     &snapshot.interrupt_active);
    }
    if (result == ESP_OK)
    {
        _set_alarm_monitor_enabled(snapshot.enabled);
        *status = snapshot;
    }
    xSemaphoreGive(s_update_mutex);

exit:
    xSemaphoreGive(s_control_mutex);
    return result;
}

esp_err_t time_service_alarm_clear(void)
{
    if (!s_initialized || s_control_mutex == NULL ||
            s_update_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    if (!s_initialized || s_stopping || !_rtc_io_allowed())
    {
        goto exit;
    }
    if (!_rtc_alarm_supported() || !_rtc_available())
    {
        result = ESP_ERR_NOT_SUPPORTED;
        goto exit;
    }

    xSemaphoreTake(s_update_mutex, portMAX_DELAY);
    result = s_rtc_ops.alarm_clear();
    xSemaphoreGive(s_update_mutex);

exit:
    xSemaphoreGive(s_control_mutex);
    return result;
}

esp_err_t time_service_request_sync(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool control_owned = false;
    bool state_owned = false;
    if (!s_initialized || s_state_mutex == NULL || s_control_mutex == NULL ||
            s_sync_events == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    control_owned = true;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    state_owned = true;
    if (s_stopping || !_rtc_io_allowed() || s_sync_pending)
    {
        goto exit;
    }
    if (s_ntp_initialized && !atomic_load(&s_callback_enabled))
    {
        result = _stop_sntp_locked();
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
    const uint32_t generation = _next_generation_locked();
    s_sync_pending = true;
    xEventGroupClearBits(s_sync_events, TIME_SERVICE_SYNC_COMPLETE_BIT);

    if (!s_ntp_initialized)
    {
        atomic_store(&s_callback_enabled, true);
        result = time_service_port_sntp_start(s_config.sntp_server,
                                              _ntp_sync_callback);
        if (result != ESP_OK)
        {
            atomic_store(&s_callback_enabled, false);
            const esp_err_t stop_result = time_service_port_sntp_stop();
            if (stop_result != ESP_OK)
            {
                s_ntp_initialized = true;
            }
            _wait_for_sntp_callbacks();
            _complete_generation_locked(generation, result);
            goto exit;
        }
        s_ntp_initialized = true;
    }
    else
    {
        result = time_service_port_sntp_restart();
        if (result != ESP_OK)
        {
            (void)_stop_sntp_locked();
            _complete_generation_locked(generation, result);
            goto exit;
        }
    }
    result = ESP_OK;

exit:
    if (state_owned)
    {
        xSemaphoreGive(s_state_mutex);
    }
    if (control_owned)
    {
        xSemaphoreGive(s_control_mutex);
    }
    return result;
}

esp_err_t time_service_cancel_sync(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool control_owned = false;
    bool state_owned = false;
    if (!s_initialized || s_state_mutex == NULL || s_control_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_control_mutex, portMAX_DELAY);
    control_owned = true;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    state_owned = true;
    if (s_stopping || !_rtc_io_allowed())
    {
        goto exit;
    }
    result = _stop_sntp_locked();
    if (s_sync_pending)
    {
        _complete_generation_locked((uint32_t)atomic_load(&s_active_generation),
                                    result == ESP_OK ? ESP_ERR_INVALID_STATE :
                                    result);
    }
    _next_generation_locked();

exit:
    if (state_owned)
    {
        xSemaphoreGive(s_state_mutex);
    }
    if (control_owned)
    {
        xSemaphoreGive(s_control_mutex);
    }
    return result;
}

esp_err_t time_service_wait_sync(uint32_t timeout_ms)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool state_owned = false;
    bool control_owned = false;
    if (!s_initialized || s_state_mutex == NULL || s_control_mutex == NULL ||
            s_sync_events == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    state_owned = true;
    if (s_stopping || !_rtc_io_allowed())
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    const bool pending = s_sync_pending;
    const uint32_t generation = pending ?
                                (uint32_t)atomic_load(&s_active_generation) :
                                s_completed_generation;
    result = s_completed_result;
    xSemaphoreGive(s_state_mutex);
    state_owned = false;
    if (!pending)
    {
        return result;
    }

    const EventBits_t bits = xEventGroupWaitBits(
                                 s_sync_events, TIME_SERVICE_SYNC_COMPLETE_BIT, pdFALSE, pdTRUE,
                                 _timeout_to_ticks(timeout_ms));

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    state_owned = true;
    if (s_completed_generation == generation)
    {
        result = s_completed_result;
        goto exit;
    }
    if ((bits & TIME_SERVICE_SYNC_COMPLETE_BIT) == 0 && s_sync_pending &&
            generation == (uint32_t)atomic_load(&s_active_generation))
    {
        xSemaphoreGive(s_state_mutex);
        state_owned = false;
        xSemaphoreTake(s_control_mutex, portMAX_DELAY);
        control_owned = true;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        state_owned = true;
        if (s_stopping || !_rtc_io_allowed())
        {
            result = ESP_ERR_INVALID_STATE;
            goto exit;
        }
        if (s_completed_generation == generation)
        {
            result = s_completed_result;
            goto exit;
        }
        if (s_sync_pending &&
                generation == (uint32_t)atomic_load(&s_active_generation))
        {
            const esp_err_t stop_result = _stop_sntp_locked();
            result = stop_result == ESP_OK ? ESP_ERR_TIMEOUT : stop_result;
            _complete_generation_locked(generation, result);
            _next_generation_locked();
        }
        else
        {
            result = ESP_ERR_TIMEOUT;
        }
        goto exit;
    }
    result = ESP_ERR_TIMEOUT;

exit:
    if (state_owned)
    {
        xSemaphoreGive(s_state_mutex);
    }
    if (control_owned)
    {
        xSemaphoreGive(s_control_mutex);
    }
    return result;
}

esp_err_t time_service_get_time(struct tm *timeinfo)
{
    return time_service_get_local(timeinfo);
}

esp_err_t time_service_set_time(const struct tm *timeinfo)
{
    return time_service_set_local(timeinfo);
}

esp_err_t time_service_sync_ntp(void)
{
    esp_err_t result = time_service_request_sync();
    return result == ESP_OK ?
           time_service_wait_sync(TIME_SERVICE_LEGACY_TIMEOUT_MS) : result;
}
