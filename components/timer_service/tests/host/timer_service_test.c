#include "timer_service.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static int64_t s_now_us;
static unsigned s_completion_count;
static timer_service_completion_type_t s_last_completion;

static void _completion(const timer_service_completion_event_t *event,
                        void *user_data)
{
    (void)user_data;
    assert(event != NULL);
    s_last_completion = event->type;
    ++s_completion_count;
}

static int64_t _test_now(void)
{
    return s_now_us;
}

int main(void)
{
    const timer_service_config_t config =
    {
        .monotonic_time_us = _test_now,
        .completion_cb = _completion,
    };
    timer_service_snapshot_t snapshot;

    assert(timer_service_init(&config) == ESP_OK);
    assert(timer_service_countdown_start(10000U) == ESP_OK);
    assert(timer_service_countdown_start(5000U) == ESP_ERR_INVALID_STATE);
    s_now_us = 4000000;
    assert(timer_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.countdown_remaining_ms == 6000U);
    assert(timer_service_countdown_pause() == ESP_OK);
    s_now_us = 9000000;
    assert(timer_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.countdown_remaining_ms == 6000U);
    assert(timer_service_countdown_resume() == ESP_OK);
    s_now_us = 15000000;
    assert(timer_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.countdown_state == TIMER_SERVICE_COMPLETED);
    assert(s_last_completion == TIMER_SERVICE_COMPLETION_COUNTDOWN);

    assert(timer_service_countdown_reset() == ESP_OK);
    assert(timer_service_stopwatch_start() == ESP_OK);
    s_now_us = 17500000;
    assert(timer_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.stopwatch_elapsed_ms == 2500U);
    assert(timer_service_stopwatch_pause() == ESP_OK);
    s_now_us = 20000000;
    assert(timer_service_stopwatch_start() == ESP_OK);
    s_now_us = 21500000;
    assert(timer_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.stopwatch_elapsed_ms == 4000U);

    assert(timer_service_stopwatch_reset() == ESP_OK);
    assert(timer_service_focus_start(5000U, 2000U) == ESP_OK);
    assert(timer_service_focus_start(5000U, 2000U) == ESP_ERR_INVALID_STATE);
    assert(timer_service_countdown_start(1000U) == ESP_ERR_INVALID_STATE);
    s_now_us = 50000000;
    assert(timer_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.focus_phase == TIMER_SERVICE_FOCUS_WORK);
    assert(snapshot.focus_completed_cycles == 4U);
    assert(snapshot.focus_remaining_ms == 4500U);

    assert(timer_service_focus_reset() == ESP_OK);
    assert(timer_service_focus_start(5000U, 2000U) == ESP_OK);
    s_now_us = 56000000;
    assert(timer_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.focus_phase == TIMER_SERVICE_FOCUS_BREAK);
    assert(snapshot.focus_remaining_ms == 1000U);
    assert(timer_service_focus_pause() == ESP_OK);
    s_now_us = 90000000;
    assert(timer_service_focus_resume() == ESP_OK);
    s_now_us = 91000000;
    assert(timer_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.focus_phase == TIMER_SERVICE_FOCUS_WORK);
    assert(snapshot.focus_remaining_ms == 5000U);
    assert(s_completion_count >= 2U);
    assert(s_last_completion == TIMER_SERVICE_COMPLETION_FOCUS_CYCLE);

    assert(timer_service_focus_reset() == ESP_OK);
    assert(timer_service_deinit() == ESP_OK);
    assert(timer_service_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    return 0;
}
