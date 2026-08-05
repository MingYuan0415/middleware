#include "weather_service.h"
#include "weather_service_host.h"

#include "esp_heap_caps.h"
#include "host_freertos.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

static const weather_service_config_t s_config =
{
    .server_base_url = "https://weather.example.com",
    .device_token = "test-token",
    .location_url = "https://api.ipapi.is/",
    .cache_directory = "/tmp",
    .task_priority = 4U,
    .current_refresh_seconds = 1200U,
    .alerts_refresh_seconds = 600U,
    .hourly_refresh_seconds = 3600U,
    .daily_refresh_seconds = 14400U,
    .manual_refresh_min_seconds = 60U,
};

static bool _wait_for_state(weather_service_state_t expected,
                            unsigned timeout_ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    for (unsigned elapsed = 0U; elapsed < timeout_ms; ++elapsed)
    {
        weather_service_status_snapshot_t status;
        if (weather_service_get_status(&status) == ESP_OK &&
                status.state == expected)
        {
            return true;
        }
        (void)nanosleep(&delay, NULL);
    }
    return false;
}

static bool _wait_for_requests(weather_service_kind_t kind,
                               unsigned expected, unsigned timeout_ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    for (unsigned elapsed = 0U; elapsed < timeout_ms; ++elapsed)
    {
        if (weather_host_weather_requests(kind) >= expected)
        {
            return true;
        }
        (void)nanosleep(&delay, NULL);
    }
    return false;
}

static void _start(void)
{
    weather_host_reset();
    assert(weather_service_init(&s_config) == ESP_OK);
    assert(host_caps_task_count() == 1U);
    assert(host_last_task_stack_caps() ==
           (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void _stop(void)
{
    assert(weather_service_deinit(1000U) == ESP_OK);
    assert(host_caps_task_count() == 0U);
    assert(host_caps_task_wrong_delete_count() == 0U);
}

static void _test_session_location_and_manual_refresh(void)
{
    _start();
    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    assert(weather_host_location_requests() == 1U);
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }
    assert(weather_host_cache_writes() == 1U);
    assert(weather_host_psram_allocations() >= 2U);

    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(snapshot != NULL);
    assert(snapshot->location.latitude_tenths == 225);
    assert(snapshot->current.temperature_tenths_c == 312);
    weather_service_snapshot_release(snapshot);

    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_DAILY, 2U, 1000U));
    assert(weather_host_location_requests() == 1U);
    assert(weather_service_request_refresh() == ESP_ERR_TIMEOUT);

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    for (unsigned wait = 0U; wait < 1000U &&
            weather_host_location_requests() < 2U; ++wait)
    {
        const struct timespec delay = {.tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    assert(weather_host_location_requests() == 2U);
    _stop();
}

static void _test_retry_and_new_location_isolation(void)
{
    _start();
    weather_host_fail_location_transport(1U);
    weather_host_fail_weather_transport(WEATHER_SERVICE_KIND_CURRENT, 1U);
    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    assert(weather_host_location_requests() == 2U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 2U);

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_location(311, 1215);
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 503, 0U);
    unsigned alerts_before = weather_host_weather_requests(
                                 WEATHER_SERVICE_KIND_ALERTS);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_DEGRADED, 1000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 4U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) ==
           alerts_before);
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(snapshot->location.latitude_tenths == 225);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_rate_limit(void)
{
    _start();
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 429, 75U);
    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_RATE_LIMITED, 1000U));
    weather_service_status_snapshot_t status;
    assert(weather_service_get_status(&status) == ESP_OK);
    assert(status.failure == WEATHER_SERVICE_FAILURE_RATE_LIMITED);
    assert(status.retry_after_seconds == 75U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 1U);
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 10000000L,
    };
    (void)nanosleep(&delay, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 1U);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_RATE_LIMITED, 100U));
    _stop();
}

int main(void)
{
    _test_session_location_and_manual_refresh();
    _test_retry_and_new_location_isolation();
    _test_rate_limit();
    puts("weather worker host tests passed");
    return 0;
}
