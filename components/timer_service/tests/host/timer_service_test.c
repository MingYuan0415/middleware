#include "timer_service.h"

#include <assert.h>
#include <stdint.h>

static int64_t s_now_us;

static int64_t _test_now(void)
{
    return s_now_us;
}

int main(void)
{
    const timer_service_config_t config =
    {
        .monotonic_time_us = _test_now,
    };
    timer_service_snapshot_t snapshot;

    assert(timer_service_init(&config) == ESP_OK);
    assert(timer_service_countdown_start(10000U) == ESP_OK);
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
    assert(timer_service_countdown_start(1000U) == ESP_ERR_INVALID_STATE);
    s_now_us = 28500000;
    assert(timer_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.focus_phase == TIMER_SERVICE_FOCUS_WORK);
    assert(snapshot.focus_completed_cycles == 1U);
    assert(snapshot.focus_remaining_ms == 5000U);

    assert(timer_service_focus_reset() == ESP_OK);
    assert(timer_service_deinit() == ESP_OK);
    assert(timer_service_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    return 0;
}
