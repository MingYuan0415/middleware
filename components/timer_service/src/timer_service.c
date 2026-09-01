#include "timer_service.h"

#include <stdatomic.h>
#include <string.h>

#ifdef ESP_PLATFORM
    #include "freertos/FreeRTOS.h"
    #include "freertos/semphr.h"
    #include "freertos/task.h"
#endif

typedef struct timer_service_runtime
{
    timer_service_config_t config;
    timer_service_snapshot_t snapshot;
    int64_t countdown_started_us;
    int64_t stopwatch_started_us;
    int64_t focus_started_us;
    uint64_t stopwatch_accumulated_ms;
    uint32_t focus_work_ms;
    uint32_t focus_break_ms;
    bool completion_pending;
    timer_service_completion_event_t completion_event;
#ifdef ESP_PLATFORM
    TaskHandle_t worker;
    SemaphoreHandle_t stopped;
    StaticSemaphore_t stopped_storage;
    atomic_bool stop_requested;
#endif
} timer_service_runtime_t;

static timer_service_runtime_t s_timer;
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static atomic_bool s_initialized = ATOMIC_VAR_INIT(false);

static void _timer_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire))
    {
    }
}

static void _timer_unlock(void)
{
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
}

static uint32_t _timer_elapsed_ms(int64_t started_us, int64_t now_us)
{
    if (now_us <= started_us)
    {
        return 0U;
    }
    uint64_t elapsed = (uint64_t)(now_us - started_us) / 1000U;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void _timer_update_locked(int64_t now_us)
{
    if (s_timer.snapshot.countdown_state == TIMER_SERVICE_RUNNING)
    {
        const uint32_t elapsed = _timer_elapsed_ms(s_timer.countdown_started_us,
                                 now_us);
        if (elapsed >= s_timer.snapshot.countdown_remaining_ms)
        {
            s_timer.snapshot.countdown_remaining_ms = 0U;
            s_timer.snapshot.countdown_state = TIMER_SERVICE_COMPLETED;
            ++s_timer.snapshot.generation;
            s_timer.completion_pending = true;
            s_timer.completion_event.generation = s_timer.snapshot.generation;
            s_timer.completion_event.type = TIMER_SERVICE_COMPLETION_COUNTDOWN;
        }
        else
        {
            s_timer.snapshot.countdown_remaining_ms -= elapsed;
            s_timer.countdown_started_us = now_us;
        }
    }

    if (s_timer.snapshot.stopwatch_state == TIMER_SERVICE_RUNNING)
    {
        s_timer.snapshot.stopwatch_elapsed_ms =
            s_timer.stopwatch_accumulated_ms +
            _timer_elapsed_ms(s_timer.stopwatch_started_us, now_us);
    }

    if (s_timer.snapshot.focus_state == TIMER_SERVICE_RUNNING)
    {
        uint64_t elapsed = _timer_elapsed_ms(s_timer.focus_started_us,
                                             now_us);
        const uint64_t work = s_timer.focus_work_ms;
        const uint64_t pause = s_timer.focus_break_ms;
        uint32_t completed = s_timer.snapshot.focus_completed_cycles;
        timer_service_focus_phase_t phase = s_timer.snapshot.focus_phase;
        uint64_t remaining = s_timer.snapshot.focus_remaining_ms;
        const uint32_t completed_before = completed;
        if (remaining == 0U || work == 0U || pause == 0U)
        {
            elapsed = 0U;
        }
        else if (elapsed < remaining)
        {
            remaining -= elapsed;
            elapsed = 0U;
        }
        else
        {
            elapsed -= remaining;
            if (phase == TIMER_SERVICE_FOCUS_WORK)
            {
                ++completed;
                phase = TIMER_SERVICE_FOCUS_BREAK;
            }
            else
            {
                phase = TIMER_SERVICE_FOCUS_WORK;
            }
            if (phase == TIMER_SERVICE_FOCUS_BREAK)
            {
                if (elapsed < pause)
                {
                    remaining = pause - elapsed;
                    elapsed = 0U;
                }
                else
                {
                    elapsed -= pause;
                    phase = TIMER_SERVICE_FOCUS_WORK;
                }
            }
            if (phase == TIMER_SERVICE_FOCUS_WORK)
            {
                const uint64_t cycle = work + pause;
                if (elapsed >= cycle)
                {
                    const uint64_t cycles = elapsed / cycle;
                    const uint32_t add = cycles > UINT32_MAX ? UINT32_MAX :
                                         (uint32_t)cycles;
                    completed = completed > UINT32_MAX - add ? UINT32_MAX :
                                completed + add;
                    elapsed %= cycle;
                }
                if (elapsed < work)
                {
                    remaining = work - elapsed;
                }
                else
                {
                    elapsed -= work;
                    ++completed;
                    phase = TIMER_SERVICE_FOCUS_BREAK;
                    remaining = pause - elapsed;
                }
            }
            s_timer.snapshot.generation++;
            if (completed != completed_before)
            {
                s_timer.completion_pending = true;
                s_timer.completion_event.generation =
                    s_timer.snapshot.generation;
                s_timer.completion_event.type =
                    TIMER_SERVICE_COMPLETION_FOCUS_CYCLE;
            }
        }
        s_timer.snapshot.focus_phase = phase;
        s_timer.snapshot.focus_remaining_ms = (uint32_t)remaining;
        s_timer.snapshot.focus_completed_cycles = completed;
        s_timer.focus_started_us = now_us;
    }
}

static void _timer_emit_completion(void)
{
    timer_service_completion_event_t event;
    timer_service_completion_cb_t callback;
    void *user_data;
    _timer_lock();
    if (!s_timer.completion_pending)
    {
        _timer_unlock();
        return;
    }
    event = s_timer.completion_event;
    s_timer.completion_pending = false;
    callback = s_timer.config.completion_cb;
    user_data = s_timer.config.completion_user_data;
    _timer_unlock();
    if (callback != NULL)
    {
        callback(&event, user_data);
    }
}

#ifdef ESP_PLATFORM
static void _timer_worker(void *argument)
{
    (void)argument;
    while (!atomic_load_explicit(&s_timer.stop_requested,
                                 memory_order_acquire))
    {
        timer_service_snapshot_t snapshot;
        (void)timer_service_get_snapshot(&snapshot);
        vTaskDelay(pdMS_TO_TICKS(100U));
    }
    _timer_lock();
    if (s_timer.stopped != NULL)
    {
        xSemaphoreGive(s_timer.stopped);
    }
    s_timer.worker = NULL;
    _timer_unlock();
    vTaskDelete(NULL);
}
#endif

static esp_err_t _timer_require_initialized(void)
{
    return atomic_load_explicit(&s_initialized, memory_order_acquire) ? ESP_OK :
           ESP_ERR_INVALID_STATE;
}

esp_err_t timer_service_init(const timer_service_config_t *config)
{
    if (config == NULL || config->monotonic_time_us == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _timer_lock();
    if (atomic_load_explicit(&s_initialized, memory_order_relaxed))
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_timer, 0, sizeof(s_timer));
    s_timer.config = *config;
#ifdef ESP_PLATFORM
    atomic_init(&s_timer.stop_requested, false);
    s_timer.stopped = xSemaphoreCreateBinaryStatic(&s_timer.stopped_storage);
#endif
    atomic_store_explicit(&s_initialized, true, memory_order_release);
    _timer_unlock();
#ifdef ESP_PLATFORM
    if (s_timer.stopped == NULL ||
            xTaskCreatePinnedToCore(_timer_worker, "timer_service", 3072, NULL,
                                    4, &s_timer.worker, tskNO_AFFINITY) != pdPASS)
    {
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        _timer_lock();
        memset(&s_timer, 0, sizeof(s_timer));
        _timer_unlock();
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

esp_err_t timer_service_deinit(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
#ifdef ESP_PLATFORM
    if (xTaskGetCurrentTaskHandle() == s_timer.worker)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    TaskHandle_t worker = s_timer.worker;
    atomic_store_explicit(&s_timer.stop_requested, true, memory_order_release);
    _timer_unlock();
    if (worker != NULL)
    {
        (void)xSemaphoreTake(s_timer.stopped, portMAX_DELAY);
    }
#endif
    _timer_lock();
    memset(&s_timer, 0, sizeof(s_timer));
    atomic_store_explicit(&s_initialized, false, memory_order_release);
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_get_snapshot(timer_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    _timer_update_locked(s_timer.config.monotonic_time_us());
    *snapshot = s_timer.snapshot;
    _timer_unlock();
    _timer_emit_completion();
    return ESP_OK;
}

esp_err_t timer_service_countdown_start(uint32_t duration_ms)
{
    if (duration_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    if (s_timer.snapshot.countdown_state == TIMER_SERVICE_RUNNING ||
            s_timer.snapshot.countdown_state == TIMER_SERVICE_PAUSED)
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_timer.snapshot.focus_state == TIMER_SERVICE_RUNNING ||
            s_timer.snapshot.focus_state == TIMER_SERVICE_PAUSED)
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_timer.snapshot.countdown_state = TIMER_SERVICE_RUNNING;
    s_timer.snapshot.countdown_duration_ms = duration_ms;
    s_timer.snapshot.countdown_remaining_ms = duration_ms;
    s_timer.countdown_started_us = s_timer.config.monotonic_time_us();
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_countdown_pause(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    _timer_update_locked(s_timer.config.monotonic_time_us());
    if (s_timer.snapshot.countdown_state != TIMER_SERVICE_RUNNING)
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_timer.snapshot.countdown_state = TIMER_SERVICE_PAUSED;
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_countdown_resume(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    if (s_timer.snapshot.countdown_state != TIMER_SERVICE_PAUSED)
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_timer.countdown_started_us = s_timer.config.monotonic_time_us();
    s_timer.snapshot.countdown_state = TIMER_SERVICE_RUNNING;
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_countdown_reset(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    s_timer.snapshot.countdown_state = TIMER_SERVICE_IDLE;
    s_timer.snapshot.countdown_duration_ms = 0U;
    s_timer.snapshot.countdown_remaining_ms = 0U;
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_stopwatch_start(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    if (s_timer.snapshot.stopwatch_state == TIMER_SERVICE_RUNNING)
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_timer.stopwatch_started_us = s_timer.config.monotonic_time_us();
    s_timer.snapshot.stopwatch_state = TIMER_SERVICE_RUNNING;
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_stopwatch_pause(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    _timer_update_locked(s_timer.config.monotonic_time_us());
    if (s_timer.snapshot.stopwatch_state != TIMER_SERVICE_RUNNING)
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_timer.stopwatch_accumulated_ms =
        s_timer.snapshot.stopwatch_elapsed_ms;
    s_timer.snapshot.stopwatch_state = TIMER_SERVICE_PAUSED;
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_stopwatch_reset(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    s_timer.snapshot.stopwatch_state = TIMER_SERVICE_IDLE;
    s_timer.snapshot.stopwatch_elapsed_ms = 0U;
    s_timer.stopwatch_accumulated_ms = 0U;
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_focus_start(uint32_t work_ms, uint32_t break_ms)
{
    if (work_ms == 0U || break_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    if (s_timer.snapshot.focus_state == TIMER_SERVICE_RUNNING ||
            s_timer.snapshot.focus_state == TIMER_SERVICE_PAUSED)
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_timer.snapshot.countdown_state == TIMER_SERVICE_RUNNING ||
            s_timer.snapshot.countdown_state == TIMER_SERVICE_PAUSED)
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_timer.focus_work_ms = work_ms;
    s_timer.focus_break_ms = break_ms;
    s_timer.snapshot.focus_state = TIMER_SERVICE_RUNNING;
    s_timer.snapshot.focus_phase = TIMER_SERVICE_FOCUS_WORK;
    s_timer.snapshot.focus_remaining_ms = work_ms;
    s_timer.snapshot.focus_completed_cycles = 0U;
    s_timer.focus_started_us = s_timer.config.monotonic_time_us();
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_focus_pause(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    _timer_update_locked(s_timer.config.monotonic_time_us());
    if (s_timer.snapshot.focus_state != TIMER_SERVICE_RUNNING)
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_timer.snapshot.focus_state = TIMER_SERVICE_PAUSED;
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_focus_resume(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    if (s_timer.snapshot.focus_state != TIMER_SERVICE_PAUSED)
    {
        _timer_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_timer.focus_started_us = s_timer.config.monotonic_time_us();
    s_timer.snapshot.focus_state = TIMER_SERVICE_RUNNING;
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_focus_reset(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _timer_lock();
    s_timer.snapshot.focus_state = TIMER_SERVICE_IDLE;
    s_timer.snapshot.focus_phase = TIMER_SERVICE_FOCUS_WORK;
    s_timer.snapshot.focus_remaining_ms = 0U;
    s_timer.snapshot.focus_completed_cycles = 0U;
    ++s_timer.snapshot.generation;
    _timer_unlock();
    return ESP_OK;
}
