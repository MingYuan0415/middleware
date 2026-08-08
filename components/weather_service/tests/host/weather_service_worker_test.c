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
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    for (;;)
    {
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec))
        {
            return false;
        }
        (void)nanosleep(&delay, NULL);
        weather_service_status_snapshot_t status;
        if (weather_service_get_status(&status) == ESP_OK &&
                status.state == expected)
        {
            return true;
        }
    }
}

static bool _wait_for_requests(weather_service_kind_t kind,
                               unsigned expected, unsigned timeout_ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    for (;;)
    {
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec))
        {
            return false;
        }
        (void)nanosleep(&delay, NULL);
        if (weather_host_weather_requests(kind) >= expected)
        {
            return true;
        }
    }
}

static bool _wait_for_location_requests(unsigned expected,
                                        unsigned timeout_ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    for (;;)
    {
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec))
        {
            return false;
        }
        (void)nanosleep(&delay, NULL);
        if (weather_host_location_requests() >= expected)
        {
            return true;
        }
    }
}

static bool _wait_for_retry(weather_service_state_t state,
                            uint32_t retry_after, unsigned timeout_ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    for (;;)
    {
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec))
        {
            return false;
        }
        (void)nanosleep(&delay, NULL);
        weather_service_status_snapshot_t status;
        if (weather_service_get_status(&status) == ESP_OK &&
                status.state == state &&
                status.retry_after_seconds == retry_after)
        {
            return true;
        }
    }
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
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    for (unsigned elapsed = 0U; elapsed < 1000U; ++elapsed)
    {
        if (host_caps_task_count() == 0U)
        {
            break;
        }
        (void)nanosleep(&delay, NULL);
    }
    assert(host_caps_task_count() == 0U);
    assert(host_caps_task_wrong_delete_count() == 0U);
}

static void _wait_for_worker_cycle(void)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 8;
    weather_service_status_snapshot_t status;
    bool settled = false;
    for (;;)
    {
        assert(weather_service_get_status(&status) == ESP_OK);
        if (status.state == WEATHER_SERVICE_STATE_LOCATING ||
                status.state == WEATHER_SERVICE_STATE_ERROR)
        {
            settled = true;
            break;
        }
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec >= deadline.tv_sec)
        {
            break;
        }
        (void)nanosleep(&delay, NULL);
    }
    assert(settled);
}

static void _connect_with_manual_refresh(uint32_t ipv4_address)
{
    assert(weather_service_set_network_ready(true, ipv4_address) == ESP_OK);
    _wait_for_worker_cycle();
    assert(weather_service_request_refresh() == ESP_OK);
}

static void _test_session_location_and_manual_refresh(void)
{
    _start();
    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_LOCATING, 3U, 4000U));
    assert(weather_host_location_requests() == 0U);
    weather_host_advance_milliseconds(2399);
    const struct timespec short_settle = {.tv_nsec = 10000000L};
    (void)nanosleep(&short_settle, NULL);
    assert(weather_host_location_requests() == 0U);
    weather_host_advance_milliseconds(1);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    assert(weather_host_location_requests() == 1U);
    assert(weather_host_location_path_seen());
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
        assert(weather_host_weather_path_seen(kind));
    }
    assert(weather_host_token_seen());
    assert(!weather_host_unexpected_request());
    assert(weather_host_cache_writes() == 1U);
    assert(weather_host_psram_allocations() >= 2U);

    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(snapshot != NULL);
    assert(strcmp(snapshot->location.provider, "maxmind") == 0);
    assert(strcmp(snapshot->location.city, "Shenzhen") == 0);
    assert(strcmp(snapshot->location.location_key, "9f4a2b3c8d1e5f06") == 0);
    assert(snapshot->current.temperature_tenths_c == 312);
    weather_service_snapshot_release(snapshot);

    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    const struct timespec settle = {.tv_nsec = 10000000L};
    (void)nanosleep(&settle, NULL);
    assert(weather_host_location_requests() == 1U);

    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_LOCATING, 4000U));
    assert(weather_host_location_requests() == 1U);

    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_DAILY, 2U, 4000U));
    assert(weather_host_location_requests() == 2U);
    assert(weather_service_request_refresh() == ESP_ERR_TIMEOUT);

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    assert(weather_service_set_network_ready(true, UINT32_C(0x090a0b0c)) ==
           ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_LOCATING, 3U, 4000U));
    assert(weather_host_location_requests() == 2U);
    weather_host_set_now(1122);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_location_requests(3U, 4000U));
    _stop();
}

static void _test_location_backoff_and_success_reset(void)
{
    _start();
    weather_host_fail_location_transport(8U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_location_requests(2U, 4000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 4000U));

    static const int64_t retry_times[] = {1004, 1016, 1064};
    static const uint32_t retry_delays[] = {12U, 48U, 240U};
    for (size_t index = 0U;
            index < sizeof(retry_times) / sizeof(retry_times[0]); ++index)
    {
        weather_host_set_now(retry_times[index]);
        assert(_wait_for_location_requests((unsigned)(4U + index * 2U), 4000U));
        assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR,
                               retry_delays[index], 4000U));
    }

    weather_host_set_now(1304);
    assert(_wait_for_location_requests(9U, 4000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
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
    assert(_wait_for_location_requests(2U, 4000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 4000U));
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_location_requests(4U, 4000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 12U, 4000U));
    assert(weather_service_request_refresh() == ESP_ERR_TIMEOUT);
    _stop();

    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    weather_host_fail_location_transport(2U);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_LOCATING, 3U, 4000U));
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_location_requests(3U, 8000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_DEGRADED, 4U, 4000U));
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(strcmp(snapshot->location.provider, "maxmind") == 0);
    assert(snapshot->location.reused);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_location_5xx_immediate_retry(void)
{
    _start();
    weather_host_set_location_status(503);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_location_requests(2U, 4000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 4000U));
    _stop();
}

static void _test_location_auth_failure(void)
{
    static const int auth_statuses[] = {401, 403};
    for (size_t index = 0U;
            index < sizeof(auth_statuses) / sizeof(auth_statuses[0]); ++index)
    {
        _start();
        weather_host_set_location_status(auth_statuses[index]);
        _connect_with_manual_refresh(UINT32_C(0x01020304));
        assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
        weather_service_status_snapshot_t status;
        assert(weather_service_get_status(&status) == ESP_OK);
        assert(status.failure == WEATHER_SERVICE_FAILURE_AUTHENTICATION);
        assert(weather_host_location_requests() == 1U);
        const struct timespec freeze = {.tv_sec = 1, .tv_nsec = 200000000L};
        (void)nanosleep(&freeze, NULL);
        assert(weather_host_location_requests() == 1U);
        assert(weather_service_get_status(&status) == ESP_OK);
        assert(status.state == WEATHER_SERVICE_STATE_AUTH_ERROR);
        weather_host_set_now(1061);
        assert(weather_service_request_refresh() == ESP_OK);
        assert(_wait_for_location_requests(2U, 4000U));
        assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
        _stop();
    }

    _start();
    weather_host_set_location_status(401);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
    weather_host_set_location_status(200);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_LOCATING, 3U, 4000U));
    weather_host_advance_milliseconds(2400);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    assert(weather_host_location_requests() == 2U);
    _stop();
}

static void _test_location_maximum_stabilization_and_manual_bypass(void)
{
    _start();
    weather_host_set_random(1200U);
    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_LOCATING, 4U, 4000U));
    assert(weather_host_location_requests() == 0U);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_location_requests(1U, 4000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    assert(weather_service_request_refresh() == ESP_ERR_TIMEOUT);
    _stop();
}

static void _test_retry_and_new_location_isolation(void)
{
    _start();
    weather_host_fail_location_transport(1U);
    weather_host_fail_weather_transport(WEATHER_SERVICE_KIND_CURRENT, 1U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    assert(weather_host_location_requests() == 2U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 2U);

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_location("geo2", "Shenzhen");
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 503, 0U);
    unsigned alerts_before = weather_host_weather_requests(
                                 WEATHER_SERVICE_KIND_ALERTS);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_CURRENT, 4U, 4000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_DEGRADED, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) >= 4U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) ==
           alerts_before);
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(strcmp(snapshot->location.provider, "maxmind") == 0);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_rate_limit(void)
{
    _start();
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 429, 75U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_RATE_LIMITED, 4000U));
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
    assert(_wait_for_state(WEATHER_SERVICE_STATE_RATE_LIMITED, 1000U));
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
        assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
        assert(weather_host_weather_requests(
                   WEATHER_SERVICE_KIND_CURRENT) == 1U);
        _stop();
    }

    _start();
    weather_host_fail_weather_parse(WEATHER_SERVICE_KIND_CURRENT, 1U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_DEGRADED, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 1U);
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    _stop();

    _start();
    weather_host_fail_weather_parse_no_mem(WEATHER_SERVICE_KIND_ALERTS, 1U);
    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_LOCATING, 3U, 4000U));
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 4000U));
    weather_service_status_snapshot_t status;
    assert(weather_service_get_status(&status) == ESP_OK);
    assert(status.failure == WEATHER_SERVICE_FAILURE_INTERNAL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 1U);
    weather_host_set_now(1003);
    const struct timespec settle = {.tv_nsec = 10000000L};
    (void)nanosleep(&settle, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 1U);
    weather_host_set_now(1004);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_ALERTS, 2U, 4000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    _stop();
}

static void _test_snapshot_allocation_failures(void)
{
    /* Allocation offsets are relative to the deterministic sequence the
       first-cycle barrier produces: the locating cycle consumes the init
       task plus one staging clone, then the forced refresh cycle consumes
       one clone, one stage replacement, and per kind candidate/commit.
       Each offset below fails the commit allocation of the matching kind
       (3 current, 5 alerts, 7 hourly, 9 daily). */
    static const unsigned publish_offsets[] = {3U, 5U, 7U, 9U};
    for (size_t index = 0U;
            index < sizeof(publish_offsets) / sizeof(publish_offsets[0]);
            ++index)
    {
        _start();
        assert(weather_service_set_network_ready(true,
                UINT32_C(0x01020304)) ==
               ESP_OK);
        _wait_for_worker_cycle();
        weather_host_fail_psram_after(publish_offsets[index]);
        assert(weather_service_request_refresh() == ESP_OK);
        assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 4000U));
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
    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    weather_host_fail_psram_after(0U);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 4000U));
    assert(weather_host_location_requests() == 0U);
    _stop();

    _start();
    assert(weather_service_set_network_ready(true, UINT32_C(0x01020304)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    weather_host_fail_psram_after(1U);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_ERROR, 4U, 4000U));
    assert(weather_host_location_requests() == 1U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 0U);
    _stop();
}

static void _test_monotonic_scheduling_and_expiration(void)
{
    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
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
                              current_requests + 1U, 4000U));
    _stop();

    weather_host_reset();
    weather_service_snapshot_t cached = {0};
    cached.available_mask = WEATHER_SERVICE_DATA_LOCATION |
                            WEATHER_SERVICE_DATA_CURRENT |
                            WEATHER_SERVICE_DATA_ALERTS |
                            WEATHER_SERVICE_DATA_HOURLY |
                            WEATHER_SERVICE_DATA_DAILY;
    cached.location.available = true;
    memcpy(cached.location.city, "Shenzhen", sizeof("Shenzhen"));
    memcpy(cached.location.region, "Guangdong", sizeof("Guangdong"));
    memcpy(cached.location.country, "CN", sizeof("CN"));
    memcpy(cached.location.timezone, "Asia/Shanghai",
           sizeof("Asia/Shanghai"));
    cached.location.acquired_at = 1000;
    memcpy(cached.location.provider, "maxmind", sizeof("maxmind"));
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
    assert(_wait_for_state(WEATHER_SERVICE_STATE_DEGRADED, 4000U));
    weather_host_set_wall_seconds(1000 + 49 * 60 * 60);
    weather_host_advance_milliseconds(60000);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_CURRENT, 4U, 4000U));
    const struct timespec pending_settle = {.tv_nsec = 10000000L};
    (void)nanosleep(&pending_settle, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 0U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_HOURLY) == 0U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_DAILY) == 0U);
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(snapshot->current.meta.expired);
    assert(snapshot->alerts.meta.expired);
    assert(snapshot->hourly.meta.expired);
    assert(snapshot->daily.meta.expired);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_weather_location_drift(void)
{
    _start();
    weather_host_set_weather_location("maxmind", "Shenzhen");
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) ==
               (kind == WEATHER_SERVICE_KIND_CURRENT ? 2U : 1U));
    }
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(strcmp(snapshot->location.provider, "maxmind") == 0);
    assert(strcmp(snapshot->location.city, "Shenzhen") == 0);
    assert(strcmp(snapshot->location.location_key, "1a2b3c4d5e6f7080") == 0);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_key_only_identity_change(void)
{
    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_location("maxmind", "Shenzhen");
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_DAILY, 2U, 4000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) >= 2U);
    }
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(strcmp(snapshot->location.provider, "maxmind") == 0);
    assert(strcmp(snapshot->location.city, "Shenzhen") == 0);
    assert(strcmp(snapshot->location.location_key, "1a2b3c4d5e6f7080") == 0);
    weather_service_snapshot_release(snapshot);
    _stop();

    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    unsigned alerts_before = weather_host_weather_requests(
                                 WEATHER_SERVICE_KIND_ALERTS);
    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_location("maxmind", "Shenzhen");
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 503, 0U);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_CURRENT, 3U, 4000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_DEGRADED, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) >= 3U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) ==
           alerts_before);
    const weather_service_snapshot_t *old_snapshot = NULL;
    assert(weather_service_snapshot_acquire(&old_snapshot) == ESP_OK);
    assert(strcmp(old_snapshot->location.location_key,
                  "9f4a2b3c8d1e5f06") == 0);
    weather_service_snapshot_release(old_snapshot);
    _stop();
}

static void _test_non_current_drift_aborts_cycle(void)
{
    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_weather_location("maxmind", "Shenzhen");
    weather_host_set_weather_location_skip(1U);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 8000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 3U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 3U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_HOURLY) == 2U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_DAILY) == 2U);
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(strcmp(snapshot->location.provider, "maxmind") == 0);
    assert(strcmp(snapshot->location.city, "Shenzhen") == 0);
    assert(strcmp(snapshot->location.location_key, "1a2b3c4d5e6f7080") == 0);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_pending_scope_blocks_until_current(void)
{
    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 503, 0U);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_CURRENT, 3U, 4000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_DEGRADED, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 1U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_HOURLY) == 1U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_DAILY) == 1U);

    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 200, 0U);
    weather_host_advance_milliseconds(6000);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 4U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 2U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_HOURLY) == 2U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_DAILY) == 2U);
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(strcmp(snapshot->location.location_key, "9f4a2b3c8d1e5f06") == 0);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_account_limit_survives_session(void)
{
    _start();
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 429, 75U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_RATE_LIMITED, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 1U);

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    weather_host_advance_milliseconds(2400);
    const struct timespec tick = {.tv_sec = 1, .tv_nsec = 200000000L};
    (void)nanosleep(&tick, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 1U);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_RATE_LIMITED, 4000U));

    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 200, 0U);
    weather_host_advance_milliseconds(15000);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_CURRENT, 2U, 4000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    _stop();
}

static void _test_auth_freeze_survives_session(void)
{
    _start();
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 401, 0U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 1U);

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    weather_host_advance_milliseconds(2400);
    const struct timespec tick = {.tv_sec = 1, .tv_nsec = 200000000L};
    (void)nanosleep(&tick, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 1U);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));

    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_CURRENT, 2U, 4000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
    _stop();
}

static void _test_fallback_blocks_weather(void)
{
    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }
    weather_host_set_weather_location("maxmind", "Shenzhen");

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_fail_location_transport(2U);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_location_requests(3U, 8000U));
    assert(_wait_for_retry(WEATHER_SERVICE_STATE_DEGRADED, 4U, 4000U));
    const struct timespec tick = {.tv_sec = 1, .tv_nsec = 200000000L};
    (void)nanosleep(&tick, NULL);
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(strcmp(snapshot->location.location_key, "9f4a2b3c8d1e5f06") == 0);
    assert(snapshot->location.reused);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_any_kind_auth_freezes_all(void)
{
    _start();
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_ALERTS, 401, 0U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 1U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 1U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_HOURLY) == 0U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_DAILY) == 0U);
    const struct timespec tick = {.tv_sec = 1, .tv_nsec = 200000000L};
    (void)nanosleep(&tick, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_HOURLY) == 0U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_DAILY) == 0U);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_ALERTS, 2U, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_HOURLY) == 0U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_DAILY) == 0U);
    _stop();
}

static void _test_pending_frozen_expiration(void)
{
    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 401, 0U);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    weather_host_advance_milliseconds(2400);
    const struct timespec tick = {.tv_sec = 1, .tv_nsec = 200000000L};
    (void)nanosleep(&tick, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 2U);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));

    weather_host_set_wall_seconds(1000 + 7 * 60 * 60);
    (void)nanosleep(&tick, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 2U);
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(snapshot->current.meta.expired);
    assert(snapshot->alerts.meta.expired);
    weather_service_event_t event;
    assert(weather_host_last_event(&event));
    assert(event.changed_mask != 0U);
    assert(event.generation == snapshot->generation);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_consecutive_drift_backoff(void)
{
    _start();
    weather_host_set_weather_location_alternate("maxmind", "Shenzhen");
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    const struct timespec tick = {.tv_sec = 1, .tv_nsec = 200000000L};
    (void)nanosleep(&tick, NULL);
    (void)nanosleep(&tick, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) >= 2U);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_DEGRADED, 4000U));
    unsigned settled = weather_host_weather_requests(
                           WEATHER_SERVICE_KIND_CURRENT);
    (void)nanosleep(&tick, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) ==
           settled);
    weather_host_advance_milliseconds(15000);
    (void)nanosleep(&tick, NULL);
    unsigned after_backoff = weather_host_weather_requests(
                                 WEATHER_SERVICE_KIND_CURRENT);
    assert(after_backoff >= settled + 1U);
    assert(after_backoff <= settled + 2U);
    (void)nanosleep(&tick, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) <=
           after_backoff + 1U);
    _stop();
}

static void _test_auth_freeze_overrides_location_fallback(void)
{
    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_CURRENT, 401, 0U);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 2U);

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_fail_location_transport(2U);
    weather_host_set_now(1122);
    assert(weather_service_set_network_ready(true, UINT32_C(0x090a0b0c)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_location_requests(4U, 4000U));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
    weather_service_status_snapshot_t status;
    assert(weather_service_get_status(&status) == ESP_OK);
    assert(status.failure == WEATHER_SERVICE_FAILURE_AUTHENTICATION);
    const struct timespec tick = {.tv_sec = 1, .tv_nsec = 200000000L};
    (void)nanosleep(&tick, NULL);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 2U);
    _stop();
}

static void _test_auth_recovery_after_force(void)
{
    _start();
    weather_host_set_weather_status(WEATHER_SERVICE_KIND_ALERTS, 401, 0U);
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_AUTH_ERROR, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 1U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_HOURLY) == 0U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_DAILY) == 0U);

    weather_host_set_weather_status(WEATHER_SERVICE_KIND_ALERTS, 200, 0U);
    weather_host_set_now(1061);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_CURRENT) == 2U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_ALERTS) == 2U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_HOURLY) == 1U);
    assert(weather_host_weather_requests(WEATHER_SERVICE_KIND_DAILY) == 1U);
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert((snapshot->available_mask & WEATHER_SERVICE_DATA_ALERTS) != 0U);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_district_not_identity(void)
{
    _start();
    weather_host_set_location_district("Futian");
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }
    const weather_service_snapshot_t *first = NULL;
    assert(weather_service_snapshot_acquire(&first) == ESP_OK);
    assert(strcmp(first->location.district, "Futian") == 0);
    weather_service_snapshot_release(first);

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_weather_location_district("Nanshan");
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 2U);
    }
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(strcmp(snapshot->location.location_key, "9f4a2b3c8d1e5f06") == 0);
    assert(strcmp(snapshot->location.district, "Nanshan") == 0);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_district_drift_by_key_only(void)
{
    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }

    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_weather_location_district("Nanshan");
    weather_host_set_weather_location_skip(1U);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    _wait_for_worker_cycle();
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 2U);
    }
    const weather_service_snapshot_t *snapshot = NULL;
    assert(weather_service_snapshot_acquire(&snapshot) == ESP_OK);
    assert(strcmp(snapshot->location.location_key, "9f4a2b3c8d1e5f06") == 0);
    assert(strcmp(snapshot->location.district, "Nanshan") == 0);
    weather_service_snapshot_release(snapshot);
    _stop();
}

static void _test_new_session_refreshes_all_scopes(void)
{
    _start();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) == 1U);
    }
    assert(weather_service_set_network_ready(false, 0U) == ESP_OK);
    weather_host_set_now(1061);
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    assert(weather_service_request_refresh() == ESP_OK);
    assert(_wait_for_requests(WEATHER_SERVICE_KIND_DAILY, 2U, 4000U));
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) >= 2U);
    }
    const struct timespec tick = {.tv_sec = 1, .tv_nsec = 200000000L};
    (void)nanosleep(&tick, NULL);
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        assert(weather_host_weather_requests(kind) >= 2U);
    }
    assert(weather_host_location_requests() == 2U);
    _stop();
}

static void _test_config_validation(void)
{
    weather_host_reset();
    weather_service_config_t config = s_config;
    config.allow_private_http = true;
    config.server_base_url = "http://192.168.1.2/base";
    assert(weather_service_init(&config) == ESP_ERR_INVALID_ARG);

    config.server_base_url = "http://192.168.1.2:8080";
    assert(weather_service_init(&config) == ESP_OK);
    _stop();

    static const char *const invalid_private[] =
    {
        "http://192.168.1.2:",
        "http://192.168.1.2:0",
        "http://192.168.1.2:65536",
        "http://192.168.1.2:8080/base",
        "http://192.168.1.2:80x",
    };
    for (size_t index = 0U;
            index < sizeof(invalid_private) / sizeof(invalid_private[0]);
            ++index)
    {
        config = s_config;
        config.allow_private_http = true;
        config.server_base_url = invalid_private[index];
        assert(weather_service_init(&config) == ESP_ERR_INVALID_ARG);
    }

    config = s_config;
    config.server_base_url = "https://weather.example.com/api";
    assert(weather_service_init(&config) == ESP_ERR_INVALID_ARG);
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
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
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
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
    _stop();

    _start();
    weather_host_block_next_http();
    _connect_with_manual_refresh(UINT32_C(0x01020304));
    assert(weather_host_wait_http_entered(1000U));
    assert(weather_service_set_network_ready(true, UINT32_C(0x05060708)) ==
           ESP_OK);
    assert(_wait_for_state(WEATHER_SERVICE_STATE_READY, 4000U));
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
    _test_location_auth_failure();
    _test_location_maximum_stabilization_and_manual_bypass();
    _test_retry_and_new_location_isolation();
    _test_rate_limit();
    _test_weather_response_failures();
    _test_snapshot_allocation_failures();
    _test_monotonic_scheduling_and_expiration();
    _test_weather_location_drift();
    _test_key_only_identity_change();
    _test_district_not_identity();
    _test_district_drift_by_key_only();
    _test_non_current_drift_aborts_cycle();
    _test_pending_scope_blocks_until_current();
    _test_account_limit_survives_session();
    _test_auth_freeze_survives_session();
    _test_fallback_blocks_weather();
    _test_any_kind_auth_freezes_all();
    _test_pending_frozen_expiration();
    _test_consecutive_drift_backoff();
    _test_auth_freeze_overrides_location_fallback();
    _test_auth_recovery_after_force();
    _test_new_session_refreshes_all_scopes();
    _test_config_validation();
    _test_refresh_admission_and_cache_no_mem();
    _test_http_cancellation_races();
    puts("weather worker host tests passed");
    return 0;
}
