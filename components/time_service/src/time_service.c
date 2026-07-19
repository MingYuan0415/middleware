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
#define TIME_SERVICE_NOTIFY_SYNC         BIT0
#define TIME_SERVICE_NOTIFY_STOP         BIT1
#define TIME_SERVICE_SYNC_WORKER_STACK  3072U
#define TIME_SERVICE_SYNC_WORKER_PRIO   4U
#define TIME_SERVICE_LEGACY_TIMEOUT_MS  30000U

static time_service_rtc_ops_t s_rtc_ops;
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

static bool _rtc_available(void)
{
    bool available = s_rtc_ops_registered;
    if (available && s_rtc_ops.is_available != NULL)
    {
        available = s_rtc_ops.is_available();
    }
    return available;
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

static void _sync_worker(void *context)
{
    (void)context;
    for (;;)
    {
        uint32_t notification = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notification, portMAX_DELAY);
        if ((notification & TIME_SERVICE_NOTIFY_STOP) != 0)
        {
            break;
        }
        if ((notification & TIME_SERVICE_NOTIFY_SYNC) == 0)
        {
            continue;
        }
        const uint32_t generation =
            (uint32_t)atomic_load(&s_notified_generation);

        xSemaphoreTake(s_update_mutex, portMAX_DELAY);
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        const bool current_session = !s_stopping && s_ntp_initialized &&
                                     generation == (uint32_t)atomic_load(&s_active_generation);
        xSemaphoreGive(s_state_mutex);
        if (!current_session)
        {
            xSemaphoreGive(s_update_mutex);
            continue;
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
    }

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
    if (setenv("TZ", "CST-8", 1) != 0)
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

static esp_err_t _create_sync_worker(void)
{
    s_sync_events = xEventGroupCreate();
    if (s_sync_events == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_worker_event_tail_complete, false,
                          memory_order_release);
    if (xTaskCreate(_sync_worker, "time_ntp", TIME_SERVICE_SYNC_WORKER_STACK,
                    NULL, TIME_SERVICE_SYNC_WORKER_PRIO, &s_sync_worker) != pdPASS)
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
    s_initialized = true;
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
    s_rtc_ops = *ops;
    s_rtc_ops_registered = true;
    return ESP_OK;
}

esp_err_t time_service_init(void)
{
    esp_err_t result = ESP_OK;
    if (s_initialized)
    {
        return ESP_OK;
    }

    result = _create_service_mutexes();
    if (result != ESP_OK)
    {
        goto cleanup;
    }
    result = _restore_initial_clock();
    if (result != ESP_OK)
    {
        goto cleanup;
    }
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
    if (!s_initialized)
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
    if (s_stopping || s_sync_pending)
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
        result = time_service_port_sntp_start(_ntp_sync_callback);
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
    result = _stop_sntp_locked();
    if (s_sync_pending)
    {
        _complete_generation_locked((uint32_t)atomic_load(&s_active_generation),
                                    result == ESP_OK ? ESP_ERR_INVALID_STATE :
                                    result);
    }
    _next_generation_locked();

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
