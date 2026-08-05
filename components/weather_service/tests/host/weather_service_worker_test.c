#include "weather_service.h"
#include "weather_service_host.h"

#include "esp_heap_caps.h"
#include "host_freertos.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
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

static bool _wait_for_location_requests(unsigned expected,
                                        unsigned timeout_ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    for (unsigned elapsed = 0U; elapsed < timeout_ms; ++elapsed)
    {
        if (weather_host_location_requests() >= expected)
        {
            return true;
        }
        (void)nanosleep(&delay, NULL);
    }
    return false;
}

static bool _wait_for_retry(weather_service_state_t state,
                            uint32_t retry_after, unsigned timeout_ms)
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
                status.state == state &&
                status.retry_after_seconds == retry_after)
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

static void _connect_with_manual_refresh(uint32_t ipv4_address)
{
    assert(weather_service_set_network_ready(true, ipv4_address) == ESP_OK);
    assert(weather_service_request_refresh() == ESP_OK);
}

static void _test_session_location_and_manual_refresh(void)
{
    _start();
    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_LOCATING, 3U, 1000U));
    assert(weather_host_location_requests() == 0U);
    weather_host_advance_milliseconds(2399);
    const struct timespec short_settle = {.tv_nsec = 10000000L};
    (void)nanosleep(&short_settle, NULL);
    assert(weather_host_location_requests() == 0U);
    weather_host_advance_milliseconds(1);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1500U));
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

    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    const struct timespec settle = {.tv_nsec = 10000000L};
    (void)nanosleep(&settle, NULL);
    assert(weather_host_location_requests() == 1U);

    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_LOCATING, 1000U));
    assert(weather_host_location_requests() == 1U);

    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_DAILY, 2U, 1000U));
    assert(weather_host_location_requests() == 2U);
    assert(weather_service_request_refresh() == ESP_ERR_TIMEOUT);

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    assert(weather_service_set_network_ready(true, UINT32_C(0x090a0b0c)) ==
           ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_LOCATING, 3U, 1000U));
    assert(weather_host_location_requests() == 2U);
    weather_host_set_now(1122);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_location_requests(3U, 1000U));
    _stop();
}

static void _test_location_backoff_and_success_reset(void)
{
    _start();
    weather_host_fail_location_transport(8U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_location_requests(2U, 1000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 1000U));

    static const int64_t retry_times[] = {1004, 1016, 1064};
    static const uint32_t retry_delays[] = {12U, 48U, 240U};
    for (size_t index = 0U;
            index < sizeof(retry_times) / sizeof(retry_times[0]); ++index)
    {
        weather_host_set_now(retry_times[index]);
        assert(_wait_for_location_requests((unsigned)(4U + index * 2U),
                                           1500U));
        assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR,
                               retry_delays[index], 1000U));
    }

    weather_host_set_now(1304);
    assert(_wait_for_location_requests(9U, 1500U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    weather_service_status_snapshot_t status;
    assert(weather_service_get_status(&status) == ESP_OK);
    assert(status.retry_after_seconds == 0U);
    _stop();
}

static void _test_location_manual_retry_and_old_fallback(void)
{
    _start();
    weather_host_fail_location_transport(4U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_location_requests(2U, 1000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 1000U));
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_location_requests(4U, 1000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 12U, 1000U));
    assert(weather_service_request_refresh() == ESP_ERR_TIMEOUT);
    _stop();

    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    weather_host_fail_location_transport(2U);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_location_requests(3U, 1000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_DEGRADED, 4U, 1000U));
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(snapshot->location.latitude_tenths == 225);
    assert(snapshot->location.reused);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_location_5xx_immediate_retry(void)
{
    _start();
    weather_host_set_location_status(503);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_location_requests(2U, 1000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 1000U));
    _stop();
}

static void _test_location_maximum_stabilization_and_manual_bypass(void)
{
    _start();
    weather_host_set_random(1200U);
    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_LOCATING, 4U, 1000U));
    assert(weather_host_location_requests() == 0U);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_location_requests(1U, 1000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    assert(weather_service_request_refresh() == ESP_ERR_TIMEOUT);
    _stop();
}

static void _test_retry_and_new_location_isolation(void)
{
    _start();
    weather_host_fail_location_transport(1U);
    weather_host_fail_weather_transport(WEATHER_SERVICE_KIND_CURRENT, 1U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
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
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_CURRENT, 4U, 1000U));
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
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_RATE_LIMITED, 1000U));
    weather_service_status_snapshot_t status;
    assert(weather_service_get_status(&status) == ESP_OK);
    assert(status.failure == WEATHER_SERVICE_FAILURE_RATE_LIMITED);
    assert(status.retry_after_seconds == 75U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 1U);
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_ERR_TIMEOUT);
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

static void _test_weather_response_failures(void)
{
    static const int auth_statuses[] = {401, 403};
    for (size_t index = 0U;
            index < sizeof(auth_statuses) / sizeof(auth_statuses[0]); ++index)
    {
        _start();
        weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT,
                                        auth_statuses[index], 0U);
        _connect_with_manual_refresh(UINT32_C(0x01020304));
        assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 1000U));
        assert(weather_host_weather_requests(
                   WEATHER_SERVICE_KIND_CURRENT) == 1U);
        _stop();
    }

    _start();
    weather_host_fail_weather_parse(WEATHER_SERVICE_KIND_CURRENT, 1U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_DEGRADED, 1000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 1U);
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    _stop();

    _start();
    weather_host_fail_weather_parse_no_mem(WEATHER_SERVICE_KIND_ALERTS, 1U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 1000U));
    weather_service_status_snapshot_t status;
    assert(weather_service_get_status(&status) == ESP_OK);
    assert(status.failure == WEATHER_SERVICE_FAILURE_INTERNAL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 1U);
    weather_host_set_now(1003);
    const struct timespec settle = {.tv_nsec = 10000000L};
    (void)nanosleep(&settle, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 1U);
    weather_host_set_now(1004);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_ALERTS, 2U, 1500U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    _stop();
}

static void _test_snapshot_allocation_failures(void)
{
    static const unsigned publish_offsets[] = {3U, 5U, 7U, 9U};
    for (size_t index = 0U;
            index < sizeof(publish_offsets) / sizeof(publish_offsets[0]);
            ++index)
    {
        _start();
        weather_host_fail_psram_after(publish_offsets[index]);
        _connect_with_manual_refresh(UINT32_C(0x01020304));
        assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 1000U));
        weather_service_status_snapshot_t status;
        assert(weather_service_get_status(&status) == ESP_OK);
        assert(status.failure == WEATHER_SERVICE_FAILURE_INTERNAL);
        assert(status.generation == index);
        assert(weather_host_weather_requests((weather_service_kind_t)index) ==
               1U);
        const struct timespec settle = {.tv_nsec = 10000000L};
        (void)nanosleep(&settle, NULL);
        assert(weather_host_weather_requests((weather_service_kind_t)index) ==
               1U);
        _stop();
    }

    _start();
    weather_host_fail_psram_after(0U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 1000U));
    assert(weather_host_location_requests() == 0U);
    _stop();

    _start();
    weather_host_fail_psram_after(1U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 1000U));
    assert(weather_host_location_requests() == 1U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 0U);
    _stop();
}

static void _test_monotonic_scheduling_and_expiration(void)
{
    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    unsigned current_requests = weather_host_weather_requests(
                                    WEATHER_SERVICE_KIND_CURRENT);
    weather_host_set_wall_seconds(INT64_C(2000000000));
    const struct timespec settle = {.tv_nsec = 10000000L};
    (void)nanosleep(&settle, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) ==
           current_requests);
    assert(weather_service_request_refresh() == ESP_ERR_TIMEOUT);
    weather_host_set_wall_seconds(1);
    weather_host_advance_milliseconds(60000);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_CURRENT,
                              current_requests + 1U, 1000U));
    _stop();

    weather_host_reset();
    weather_service_snapshot_t cached = {0};
    cached.available_mask = WEATHER_SERVICE_DATA_LOCATION |
                            WEATHER_SERVICE_DATA_CURRENT |
                            WEATHER_SERVICE_DATA_ALERTS |
                            WEATHER_SERVICE_DATA_HOURLY |
                            WEATHER_SERVICE_DATA_DAILY;
    cached.location.available = true;
    cached.location.latitude_tenths = 225;
    cached.location.longitude_tenths = 1141;
    cached.location.acquired_at = 1000;
    memcpy(cached.location.provider, "ipapi.is", sizeof("ipapi.is"));
    weather_service_dataset_meta_t *metadata[] =
    {
        &cached.current.meta,
        &cached.alerts.meta,
        &cached.hourly.meta,
        &cached.daily.meta,
    };
    for (size_t index = 0U; index < sizeof(metadata) / sizeof(metadata[0]);
            ++index)
    {
        metadata[index]->available = true;
        metadata[index]->fetched_at.epoch_seconds = 1000;
    }
    weather_host_set_cache_load(ESP_OK, &cached);
    assert(weather_service_init(&s_config) == ESP_OK);
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        weather_host_set_weather_status(kind, 503, 0U);
    }
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_DEGRADED, 1000U));
    weather_host_set_wall_seconds(1000 + 49 * 60 * 60);
    weather_host_advance_milliseconds(60000);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_DAILY, 4U, 1000U));
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(snapshot->current.meta.expired);
    assert(snapshot->alerts.meta.expired);
    assert(snapshot->hourly.meta.expired);
    assert(snapshot->daily.meta.expired);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_refresh_admission_and_cache_no_mem(void)
{
    weather_host_reset();
    weather_host_set_cache_load(ESP_ERR_NO_MEM, NULL);
    assert(weather_service_init(&s_config) == ESP_ERR_NO_MEM);
    assert(host_caps_task_count() == 0U);

    weather_service_config_t unconfigured = s_config;
    unconfigured.server_base_url = "";
    unconfigured.device_token = "";
    weather_host_reset();
    assert(weather_service_init(&unconfigured) == ESP_OK);
    assert(weather_service_request_refresh() == ESP_ERR_INVALID_STATE);
    _stop();

    _start();
    assert(weather_service_request_refresh() == ESP_ERR_INVALID_STATE);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    assert(weather_service_suspend(1000U) == ESP_OK);
    assert(weather_service_request_refresh() == ESP_ERR_INVALID_STATE);
    assert(weather_service_resume(1000U) == ESP_OK);
    _stop();
}

static void _test_http_cancellation_races(void)
{
    _start();
    weather_host_block_next_http();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(weather_host_wait_http_entered(1000U));
    assert(weather_service_suspend(1000U) == ESP_OK);
    assert(weather_service_request_refresh() == ESP_ERR_INVALID_STATE);
    assert(weather_service_resume(1000U) == ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    _stop();

    _start();
    weather_host_block_next_http();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(weather_host_wait_http_entered(1000U));
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 1000U));
    assert(weather_host_location_requests() == 1U);
    _stop();

    _start();
    weather_host_block_next_http();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(weather_host_wait_http_entered(1000U));
    assert(weather_service_deinit(1000U) == ESP_OK);
    assert(host_caps_task_count() == 0U);
}

int main(void)
{
    _test_session_location_and_manual_refresh();
    _test_location_backoff_and_success_reset();
    _test_location_manual_retry_and_old_fallback();
    _test_location_5xx_immediate_retry();
    _test_location_maximum_stabilization_and_manual_bypass();
    _test_retry_and_new_location_isolation();
    _test_rate_limit();
    _test_weather_response_failures();
    _test_snapshot_allocation_failures();
    _test_monotonic_scheduling_and_expiration();
    _test_refresh_admission_and_cache_no_mem();
    _test_http_cancellation_races();
    puts("weather worker host tests passed");
    return 0;
}
