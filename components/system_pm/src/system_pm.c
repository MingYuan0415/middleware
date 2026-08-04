#define DBG_TAG "system_pm"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "system_pm.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#if CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0 || \
    CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1
    #include "freertos/idf_additions.h"
#endif
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"

#define SYSTEM_PM_CMD_REQUEST BIT0
#define SYSTEM_PM_CMD_STOP    BIT1
#define SYSTEM_PM_CMD_RECOVER BIT2
#define SYSTEM_PM_EVENT_STOPPED BIT0
#define SYSTEM_PM_EVENT_REQUEST_COMPLETE BIT1

typedef enum
{
    SYSTEM_PM_STATE_STOPPED = 0,
    SYSTEM_PM_STATE_CLEANUP_PENDING,
    SYSTEM_PM_STATE_IDLE,
    SYSTEM_PM_STATE_QUEUED,
    SYSTEM_PM_STATE_PREPARING,
    SYSTEM_PM_STATE_CANCELING,
    SYSTEM_PM_STATE_RECOVERY_PENDING,
    SYSTEM_PM_STATE_SLEEPING,
} system_pm_state_t;

static system_pm_config_t s_config;
static esp_pm_lock_handle_t s_cpu_max_lock;
static SemaphoreHandle_t s_cpu_lock_mutex;
static SemaphoreHandle_t s_command_mutex;
static unsigned s_cpu_lock_count;
static TaskHandle_t s_sleep_task;
static EventGroupHandle_t s_worker_events;
static bool s_initialized;
static bool s_stopping;
static uint32_t s_request_generation;
static uint32_t s_pending_completion_generation;
static uint32_t s_completed_generation;
static esp_err_t s_request_complete_result;
static atomic_bool s_worker_event_tail_complete = ATOMIC_VAR_INIT(true);
static system_pm_state_t s_state = SYSTEM_PM_STATE_STOPPED;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static bool _cleanup_resources_present(void)
{
    return s_cpu_max_lock != NULL || s_cpu_lock_mutex != NULL ||
           s_command_mutex != NULL || s_worker_events != NULL ||
           s_sleep_task != NULL || s_cpu_lock_count != 0;
}

static void _wait_for_worker_event_tail(void)
{
    while (!atomic_load_explicit(&s_worker_event_tail_complete,
                                 memory_order_acquire))
    {
        vTaskDelay(1);
    }
}

static esp_err_t _cleanup_cpu_lock(void)
{
    esp_err_t result = ESP_OK;

    if (s_cpu_lock_mutex != NULL)
    {
        if (xSemaphoreTake(s_cpu_lock_mutex, portMAX_DELAY) != pdTRUE)
        {
            return ESP_FAIL;
        }
    }

    if (s_cpu_lock_count > 0)
    {
        if (s_cpu_max_lock == NULL)
        {
            result = ESP_ERR_INVALID_STATE;
        }
        else
        {
            result = esp_pm_lock_release(s_cpu_max_lock);
            if (result == ESP_OK)
            {
                s_cpu_lock_count = 0;
            }
        }
    }
    if (result == ESP_OK && s_cpu_max_lock != NULL)
    {
        result = esp_pm_lock_delete(s_cpu_max_lock);
        if (result == ESP_OK)
        {
            s_cpu_max_lock = NULL;
        }
    }

    if (s_cpu_lock_mutex != NULL)
    {
        xSemaphoreGive(s_cpu_lock_mutex);
    }
    return result;
}

static void _reset_stopped_resources(void)
{
    if (s_worker_events != NULL)
    {
        vEventGroupDelete(s_worker_events);
        s_worker_events = NULL;
    }
    if (s_command_mutex != NULL)
    {
        vSemaphoreDelete(s_command_mutex);
        s_command_mutex = NULL;
    }
    if (s_cpu_lock_mutex != NULL)
    {
        vSemaphoreDelete(s_cpu_lock_mutex);
        s_cpu_lock_mutex = NULL;
    }
    memset(&s_config, 0, sizeof(s_config));
    s_cpu_lock_count = 0;
    s_stopping = false;
    s_request_generation = 0;
    s_pending_completion_generation = 0;
    s_completed_generation = 0;
    s_request_complete_result = ESP_OK;
    atomic_store_explicit(&s_worker_event_tail_complete, true,
                          memory_order_release);
    taskENTER_CRITICAL(&s_state_lock);
    s_initialized = false;
    s_state = SYSTEM_PM_STATE_STOPPED;
    taskEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t _cleanup_stopped_resources(void)
{
    if (s_sleep_task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = _cleanup_cpu_lock();
    if (result == ESP_OK)
    {
        _reset_stopped_resources();
    }
    return result;
}

static void _rollback_init_resources(esp_err_t primary_error)
{
    (void)primary_error;
    taskENTER_CRITICAL(&s_state_lock);
    s_initialized = false;
    s_state = SYSTEM_PM_STATE_CLEANUP_PENDING;
    taskEXIT_CRITICAL(&s_state_lock);
    const esp_err_t cleanup_result = _cleanup_stopped_resources();
    if (cleanup_result != ESP_OK)
    {
        LOG_E("init rollback pending: primary=%d cleanup=%d",
              (int)primary_error, (int)cleanup_result);
    }
}

static bool _config_valid(const system_pm_config_t *config)
{
    bool valid = config != NULL;
    if (valid)
    {
        valid = config->wake_source_count > 0 &&
                config->wake_source_count <= SYSTEM_PM_MAX_WAKE_SOURCES &&
                config->prepare_sleep != NULL &&
                config->complete_sleep != NULL &&
                config->prepare_timeout_ms > 0 && config->task_priority > 0U &&
                config->task_priority < configMAX_PRIORITIES;
    }
    for (size_t i = 0; valid && i < config->wake_source_count; ++i)
    {
        const system_pm_wake_source_t *source = &config->wake_sources[i];
        if (!GPIO_IS_VALID_GPIO(source->gpio_num) ||
                !RTC_GPIO_IS_VALID_GPIO((gpio_num_t)source->gpio_num) ||
                (source->active_level != SYSTEM_PM_WAKE_LEVEL_LOW &&
                 source->active_level != SYSTEM_PM_WAKE_LEVEL_HIGH))
        {
            valid = false;
        }
        if (valid && i > 0 &&
                source->active_level != config->wake_sources[0].active_level)
        {
            valid = false;
        }
        for (size_t previous = 0; valid && previous < i; ++previous)
        {
            if (config->wake_sources[previous].gpio_num == source->gpio_num)
            {
                valid = false;
            }
        }
    }
    return valid;
}

static bool _config_equal(const system_pm_config_t *left,
                          const system_pm_config_t *right)
{
    bool equal = true;
    if (left->wake_source_count != right->wake_source_count ||
            left->prepare_sleep != right->prepare_sleep ||
            left->complete_sleep != right->complete_sleep ||
            left->sleep_hook_context != right->sleep_hook_context ||
            left->prepare_timeout_ms != right->prepare_timeout_ms ||
            left->wake_callback != right->wake_callback ||
            left->wake_callback_context != right->wake_callback_context ||
            left->commit_guard != right->commit_guard ||
            left->commit_callback != right->commit_callback ||
            left->commit_context != right->commit_context ||
            left->task_priority != right->task_priority)
    {
        equal = false;
    }
    for (size_t i = 0; equal && i < left->wake_source_count; ++i)
    {
        if (left->wake_sources[i].gpio_num != right->wake_sources[i].gpio_num ||
                left->wake_sources[i].active_level !=
                right->wake_sources[i].active_level)
        {
            equal = false;
        }
    }
    return equal;
}

static void _disable_wake_sources(void)
{
    esp_sleep_disable_ext1_wakeup_io(0);
}

static esp_err_t _enable_wake_sources(void)
{
    uint64_t gpio_mask = 0;
    for (size_t i = 0; i < s_config.wake_source_count; ++i)
    {
        gpio_mask |= UINT64_C(1) <<
                     (unsigned)s_config.wake_sources[i].gpio_num;
    }
    const esp_sleep_ext1_wakeup_mode_t mode =
        s_config.wake_sources[0].active_level == SYSTEM_PM_WAKE_LEVEL_LOW ?
        ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH;
    return esp_sleep_enable_ext1_wakeup_io(gpio_mask, mode);
}

static system_pm_wake_event_t _make_wake_event(esp_err_t prepare_result,
        esp_err_t sleep_result,
        esp_err_t complete_result,
        uint32_t wakeup_causes,
        uint64_t gpio_wakeup_mask)
{
    system_pm_wake_event_t event =
    {
        .reason = SYSTEM_PM_WAKE_REASON_UNKNOWN,
        .source_index = SYSTEM_PM_WAKE_SOURCE_NONE,
        .gpio_num = SYSTEM_PM_WAKE_SOURCE_NONE,
        .wakeup_causes = wakeup_causes,
        .gpio_wakeup_mask = gpio_wakeup_mask,
        .prepare_result = prepare_result,
        .sleep_result = sleep_result,
        .complete_result = complete_result,
    };
    if (prepare_result != ESP_OK || sleep_result != ESP_OK ||
            complete_result != ESP_OK)
    {
        event.reason = SYSTEM_PM_WAKE_REASON_SLEEP_ERROR;
    }
    else if ((wakeup_causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) != 0)
    {
        event.reason = SYSTEM_PM_WAKE_REASON_GPIO;
        for (size_t i = 0; i < s_config.wake_source_count; ++i)
        {
            const int gpio = s_config.wake_sources[i].gpio_num;
            if ((gpio_wakeup_mask & (UINT64_C(1) << (unsigned)gpio)) != 0)
            {
                event.source_index = (int)i;
                event.gpio_num = gpio;
                break;
            }
        }
    }
    else if ((wakeup_causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0)
    {
        event.reason = SYSTEM_PM_WAKE_REASON_TIMER;
    }
    else if (wakeup_causes != 0)
    {
        event.reason = SYSTEM_PM_WAKE_REASON_OTHER;
    }
    return event;
}

static void _notify_wake_if_admitted(const system_pm_wake_event_t *event)
{
    if (event->prepare_result != ESP_OK || event->sleep_result != ESP_OK ||
            event->complete_result != ESP_OK)
    {
        LOG_W("standby cycle failed: prepare=%d sleep=%d complete=%d",
              (int)event->prepare_result, (int)event->sleep_result,
              (int)event->complete_result);
    }
    system_pm_wake_callback_t callback = NULL;
    void *callback_context = NULL;
    taskENTER_CRITICAL(&s_state_lock);
    if (s_initialized && !s_stopping)
    {
        callback = s_config.wake_callback;
        callback_context = s_config.wake_callback_context;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (callback != NULL)
    {
        callback(event, callback_context);
    }
}

static bool _request_is_current(uint32_t generation)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool current = !s_stopping &&
                         s_state == SYSTEM_PM_STATE_PREPARING &&
                         s_request_generation == generation;
    taskEXIT_CRITICAL(&s_state_lock);
    return current;
}

static bool _commit_request(uint32_t generation)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool current = !s_stopping &&
                         s_state == SYSTEM_PM_STATE_PREPARING &&
                         s_request_generation == generation;
    if (current)
    {
        s_state = SYSTEM_PM_STATE_SLEEPING;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return current;
}

static uint32_t _next_generation_locked(void)
{
    ++s_request_generation;
    if (s_request_generation == 0)
    {
        s_request_generation = 1;
    }
    return s_request_generation;
}

static void _finish_request(uint32_t generation, esp_err_t complete_result)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_completed_generation = generation;
    s_request_complete_result = complete_result;
    if (!s_stopping)
    {
        s_state = complete_result == ESP_OK ?
                  SYSTEM_PM_STATE_IDLE : SYSTEM_PM_STATE_RECOVERY_PENDING;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    xEventGroupSetBits(s_worker_events, SYSTEM_PM_EVENT_REQUEST_COMPLETE);
}

static bool _completion_result(uint32_t generation, esp_err_t *result)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool completed = s_completed_generation == generation;
    if (completed)
    {
        *result = s_request_complete_result;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return completed;
}

static esp_err_t _wait_for_completion(EventGroupHandle_t events,
                                      uint32_t generation)
{
    esp_err_t result = ESP_ERR_TIMEOUT;
    TickType_t timeout = pdMS_TO_TICKS(s_config.prepare_timeout_ms);
    if (timeout == 0)
    {
        timeout = 1;
    }
    const TickType_t started = xTaskGetTickCount();
    for (;;)
    {
        if (_completion_result(generation, &result))
        {
            return result;
        }

        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout)
        {
            return ESP_ERR_TIMEOUT;
        }
        const EventBits_t bits = xEventGroupWaitBits(
                                     events, SYSTEM_PM_EVENT_REQUEST_COMPLETE, pdFALSE, pdTRUE,
                                     timeout - elapsed);
        if (_completion_result(generation, &result))
        {
            return result;
        }
        if ((bits & SYSTEM_PM_EVENT_REQUEST_COMPLETE) == 0)
        {
            return ESP_ERR_TIMEOUT;
        }

        xEventGroupClearBits(events, SYSTEM_PM_EVENT_REQUEST_COMPLETE);
    }
}

static bool _stop_requested(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool stopping = s_stopping;
    taskEXIT_CRITICAL(&s_state_lock);
    return stopping;
}

static bool _run_standby_request(uint32_t generation)
{
    const esp_err_t prepare_result = s_config.prepare_sleep(
                                         s_config.prepare_timeout_ms,
                                         s_config.sleep_hook_context);
    esp_err_t sleep_result = ESP_ERR_INVALID_STATE;
    uint32_t wakeup_causes = 0;
    uint64_t gpio_wakeup_mask = 0;
    bool wake_sources_attempted = false;
    if (prepare_result == ESP_OK && _request_is_current(generation))
    {
        sleep_result = _enable_wake_sources();
        wake_sources_attempted = true;
        if (sleep_result == ESP_OK && s_config.commit_guard != NULL &&
                !s_config.commit_guard(generation, s_config.commit_context))
        {
            sleep_result = ESP_ERR_INVALID_STATE;
        }
        if (sleep_result == ESP_OK && !_commit_request(generation))
        {
            sleep_result = ESP_ERR_INVALID_STATE;
        }
        if (sleep_result == ESP_OK)
        {
            if (s_config.commit_callback != NULL)
            {
                s_config.commit_callback(generation, s_config.commit_context);
            }
            LOG_I("entering light sleep");
            sleep_result = esp_light_sleep_start();
            if (sleep_result == ESP_OK)
            {
                wakeup_causes = esp_sleep_get_wakeup_causes();
                if ((wakeup_causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) != 0)
                {
                    gpio_wakeup_mask = esp_sleep_get_ext1_wakeup_status();
                }
            }
        }
    }
    if (wake_sources_attempted)
    {
        _disable_wake_sources();
    }

    const esp_err_t complete_result = s_config.complete_sleep(
                                          s_config.prepare_timeout_ms, s_config.sleep_hook_context);
    _finish_request(generation, complete_result);
    const system_pm_wake_event_t event = _make_wake_event(
            prepare_result, sleep_result, complete_result,
            wakeup_causes, gpio_wakeup_mask);
    _notify_wake_if_admitted(&event);
    return _stop_requested();
}

static void _sleep_task(void *context)
{
    (void)context;
    bool stop = false;
    while (!stop)
    {
        uint32_t commands = 0;
        xTaskNotifyWait(0, UINT32_MAX, &commands, portMAX_DELAY);
        if ((commands & SYSTEM_PM_CMD_STOP) != 0)
        {
            break;
        }
        if ((commands & SYSTEM_PM_CMD_RECOVER) != 0)
        {
            taskENTER_CRITICAL(&s_state_lock);
            const bool recover = !s_stopping &&
                                 s_state == SYSTEM_PM_STATE_CANCELING;
            const uint32_t generation = s_pending_completion_generation;
            taskEXIT_CRITICAL(&s_state_lock);
            if (!recover)
            {
                continue;
            }
            const esp_err_t complete_result = s_config.complete_sleep(
                                                  s_config.prepare_timeout_ms,
                                                  s_config.sleep_hook_context);
            _finish_request(generation, complete_result);
            continue;
        }
        if ((commands & SYSTEM_PM_CMD_REQUEST) == 0)
        {
            continue;
        }

        taskENTER_CRITICAL(&s_state_lock);
        if (!s_stopping && s_state == SYSTEM_PM_STATE_QUEUED)
        {
            s_state = SYSTEM_PM_STATE_PREPARING;
            const uint32_t generation = s_request_generation;
            taskEXIT_CRITICAL(&s_state_lock);
            stop = _run_standby_request(generation);
        }
        else
        {
            taskEXIT_CRITICAL(&s_state_lock);
        }
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_state = SYSTEM_PM_STATE_CLEANUP_PENDING;
    s_sleep_task = NULL;
    s_initialized = false;
    taskEXIT_CRITICAL(&s_state_lock);
    xEventGroupSetBits(s_worker_events, SYSTEM_PM_EVENT_STOPPED);
    atomic_store_explicit(&s_worker_event_tail_complete, true,
                          memory_order_release);
    vTaskDelete(NULL);
}

static esp_err_t _create_system_pm_resources(void)
{
    esp_err_t result;
    esp_pm_lock_handle_t lock = NULL;

    s_cpu_lock_count = 0;
    result = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0,
                                "system_pm_cpu_max", &lock);
    if (result != ESP_OK)
    {
        return result;
    }
    s_cpu_max_lock = lock;
    s_cpu_lock_mutex = xSemaphoreCreateMutex();
    if (s_cpu_lock_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    s_command_mutex = xSemaphoreCreateMutex();
    if (s_command_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    s_worker_events = xEventGroupCreate();
    if (s_worker_events == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    atomic_store_explicit(&s_worker_event_tail_complete, false,
                          memory_order_release);
    if (xTaskCreatePinnedToCore(
                _sleep_task, "system_pm_sleep",
                CONFIG_SYSTEM_PM_STANDBY_TASK_STACK, NULL,
                s_config.task_priority, &s_sleep_task,
                CONFIG_MAIN_PROJECT_TASK_CORE_ID) != pdPASS)
    {
        s_sleep_task = NULL;
        atomic_store_explicit(&s_worker_event_tail_complete, true,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0 || \
    CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1
    LOG_I("task affinity name=system_pm_sleep core=%d",
          (int)xTaskGetCoreID(s_sleep_task));
#endif
    return ESP_OK;
}

static void _activate_system_pm(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_state = SYSTEM_PM_STATE_IDLE;
    s_request_generation = 0;
    s_pending_completion_generation = 0;
    s_completed_generation = 0;
    s_request_complete_result = ESP_OK;
    s_stopping = false;
    s_initialized = true;
    taskEXIT_CRITICAL(&s_state_lock);
}

esp_err_t system_pm_init(const system_pm_config_t *config)
{
    esp_err_t result = ESP_OK;
    bool initialize = false;
    if (!_config_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state_lock);
    if (s_initialized)
    {
        result = _config_equal(&s_config, config) ?
                 ESP_OK : ESP_ERR_INVALID_STATE;
    }
    else if (s_state != SYSTEM_PM_STATE_STOPPED ||
             _cleanup_resources_present())
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        s_state = SYSTEM_PM_STATE_CLEANUP_PENDING;
        s_stopping = false;
        initialize = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (!initialize)
    {
        return result;
    }

    s_config = *config;
    result = _create_system_pm_resources();
    if (result != ESP_OK)
    {
        goto rollback;
    }

    _activate_system_pm();
    LOG_I("initialized with %u wake sources",
          (unsigned)s_config.wake_source_count);
    return ESP_OK;

rollback:
    _rollback_init_resources(result);
    return result;
}

esp_err_t system_pm_deinit(void)
{
    esp_err_t result = ESP_OK;
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    TaskHandle_t worker = NULL;
    EventGroupHandle_t events = NULL;
    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized && s_state == SYSTEM_PM_STATE_STOPPED &&
            !_cleanup_resources_present())
    {
        result = ESP_OK;
    }
    else if (current == s_sleep_task || s_state == SYSTEM_PM_STATE_SLEEPING ||
             s_state == SYSTEM_PM_STATE_CANCELING ||
             s_state == SYSTEM_PM_STATE_RECOVERY_PENDING)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else if (s_sleep_task != NULL)
    {
        if (s_worker_events == NULL || !s_initialized)
        {
            result = ESP_ERR_INVALID_STATE;
        }
        else
        {
            s_stopping = true;
            _next_generation_locked();
            worker = s_sleep_task;
            events = s_worker_events;
        }
    }
    else if (s_state != SYSTEM_PM_STATE_CLEANUP_PENDING ||
             !_cleanup_resources_present())
    {
        result = ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (result != ESP_OK)
    {
        return result;
    }

    if (worker != NULL)
    {
        xTaskNotify(worker, SYSTEM_PM_CMD_STOP, eSetBits);
        xEventGroupWaitBits(events, SYSTEM_PM_EVENT_STOPPED,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        _wait_for_worker_event_tail();
    }
    result = _cleanup_stopped_resources();
    return result;
}

esp_err_t system_pm_request_standby(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool command_owned = false;
    EventGroupHandle_t events = NULL;
    TaskHandle_t worker = NULL;
    if (s_command_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_command_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    command_owned = true;

    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || s_stopping || s_sleep_task == NULL ||
            s_worker_events == NULL || s_state != SYSTEM_PM_STATE_IDLE)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        _next_generation_locked();
        s_state = SYSTEM_PM_STATE_QUEUED;
        worker = s_sleep_task;
        events = s_worker_events;
        result = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (result == ESP_OK)
    {
        xEventGroupClearBits(events, SYSTEM_PM_EVENT_REQUEST_COMPLETE);
        xTaskNotify(worker, SYSTEM_PM_CMD_REQUEST, eSetBits);
    }

    if (command_owned)
    {
        xSemaphoreGive(s_command_mutex);
    }
    return result;
}

esp_err_t system_pm_cancel_standby(void)
{
    esp_err_t result = ESP_OK;
    bool command_owned = false;
    if (s_command_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_command_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    command_owned = true;

    EventGroupHandle_t events = NULL;
    TaskHandle_t worker = NULL;
    uint32_t completion_generation = 0;
    bool wait_for_result = false;
    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || s_stopping || s_sleep_task == NULL ||
            s_worker_events == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else if (s_state == SYSTEM_PM_STATE_SLEEPING)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else if (s_state == SYSTEM_PM_STATE_QUEUED)
    {
        _next_generation_locked();
        s_state = SYSTEM_PM_STATE_IDLE;
        s_request_complete_result = ESP_OK;
    }
    else if (s_state == SYSTEM_PM_STATE_PREPARING)
    {
        completion_generation = s_request_generation;
        s_pending_completion_generation = completion_generation;
        _next_generation_locked();
        s_state = SYSTEM_PM_STATE_CANCELING;
        events = s_worker_events;
        wait_for_result = true;
    }
    else if (s_state == SYSTEM_PM_STATE_CANCELING)
    {
        completion_generation = s_pending_completion_generation;
        events = s_worker_events;
        wait_for_result = true;
    }
    else if (s_state == SYSTEM_PM_STATE_RECOVERY_PENDING)
    {
        completion_generation = _next_generation_locked();
        s_pending_completion_generation = completion_generation;
        s_state = SYSTEM_PM_STATE_CANCELING;
        events = s_worker_events;
        worker = s_sleep_task;
        wait_for_result = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (wait_for_result)
    {
        xEventGroupClearBits(events, SYSTEM_PM_EVENT_REQUEST_COMPLETE);
        if (worker != NULL)
        {
            xTaskNotify(worker, SYSTEM_PM_CMD_RECOVER, eSetBits);
        }
        result = _wait_for_completion(events, completion_generation);
    }

    if (command_owned)
    {
        xSemaphoreGive(s_command_mutex);
    }
    return result;
}

esp_err_t system_pm_acquire_cpu_max_freq(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool mutex_owned = false;
    if (!s_initialized || s_cpu_max_lock == NULL || s_cpu_lock_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_cpu_lock_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    mutex_owned = true;
    result = ESP_OK;
    if (s_cpu_lock_count == 0)
    {
        result = esp_pm_lock_acquire(s_cpu_max_lock);
    }
    if (result == ESP_OK)
    {
        ++s_cpu_lock_count;
    }

    if (mutex_owned)
    {
        xSemaphoreGive(s_cpu_lock_mutex);
    }
    return result;
}

esp_err_t system_pm_release_cpu_max_freq(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool mutex_owned = false;
    if (!s_initialized || s_cpu_max_lock == NULL || s_cpu_lock_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_cpu_lock_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    mutex_owned = true;
    result = ESP_OK;
    if (s_cpu_lock_count == 0)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else if (s_cpu_lock_count == 1)
    {
        result = esp_pm_lock_release(s_cpu_max_lock);
        if (result == ESP_OK)
        {
            s_cpu_lock_count = 0;
        }
    }
    else
    {
        --s_cpu_lock_count;
    }

    if (mutex_owned)
    {
        xSemaphoreGive(s_cpu_lock_mutex);
    }
    return result;
}
