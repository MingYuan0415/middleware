#include "timer_service.h"

#include <stdatomic.h>
#include <string.h>

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
        uint32_t elapsed = _timer_elapsed_ms(s_timer.focus_started_us, now_us);
        while (elapsed >= s_timer.snapshot.focus_remaining_ms &&
                s_timer.snapshot.focus_remaining_ms > 0U)
        {
            elapsed -= s_timer.snapshot.focus_remaining_ms;
            if (s_timer.snapshot.focus_phase == TIMER_SERVICE_FOCUS_WORK)
            {
                s_timer.snapshot.focus_phase = TIMER_SERVICE_FOCUS_BREAK;
                s_timer.snapshot.focus_remaining_ms = s_timer.focus_break_ms;
                ++s_timer.snapshot.focus_completed_cycles;
            }
            else
            {
                s_timer.snapshot.focus_phase = TIMER_SERVICE_FOCUS_WORK;
                s_timer.snapshot.focus_remaining_ms = s_timer.focus_work_ms;
            }
            ++s_timer.snapshot.generation;
        }
        if (elapsed < s_timer.snapshot.focus_remaining_ms)
        {
            s_timer.snapshot.focus_remaining_ms -= elapsed;
        }
        s_timer.focus_started_us = now_us;
    }
}

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
    atomic_store_explicit(&s_initialized, true, memory_order_release);
    _timer_unlock();
    return ESP_OK;
}

esp_err_t timer_service_deinit(void)
{
    if (_timer_require_initialized() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
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
