#include "weather_service_host.h"

#include "esp_heap_caps.h"
#include "event_bus.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct weather_host_dataset
{
    atomic_uint requests;
    atomic_uint transport_failures;
    atomic_int status_code;
    atomic_uint retry_after_seconds;
} weather_host_dataset_t;

static atomic_llong s_now;
static atomic_int s_latitude_tenths;
static atomic_int s_longitude_tenths;
static atomic_uint s_location_requests;
static atomic_uint s_location_transport_failures;
static atomic_uint s_cache_writes;
static atomic_uint s_psram_allocations;
static weather_host_dataset_t s_datasets[WEATHER_SERVICE_KIND_COUNT];

static bool _weather_host_consume(atomic_uint *remaining)
{
    unsigned value = atomic_load_explicit(remaining, memory_order_acquire);
    while (value > 0U)
    {
        if (atomic_compare_exchange_weak_explicit(
                    remaining, &value, value - 1U,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

static weather_service_kind_t _weather_host_kind(const char *url)
{
    if (strstr(url, "/alerts") != NULL)
    {
        return WEATHER_SERVICE_KIND_ALERTS;
    }
    if (strstr(url, "/hourly") != NULL)
    {
        return WEATHER_SERVICE_KIND_HOURLY;
    }
    if (strstr(url, "/daily") != NULL)
    {
        return WEATHER_SERVICE_KIND_DAILY;
    }
    return WEATHER_SERVICE_KIND_CURRENT;
}

void weather_host_reset(void)
{
    atomic_store(&s_now, 1000);
    atomic_store(&s_latitude_tenths, 225);
    atomic_store(&s_longitude_tenths, 1141);
    atomic_store(&s_location_requests, 0U);
    atomic_store(&s_location_transport_failures, 0U);
    atomic_store(&s_cache_writes, 0U);
    atomic_store(&s_psram_allocations, 0U);
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        atomic_store(&s_datasets[kind].requests, 0U);
        atomic_store(&s_datasets[kind].transport_failures, 0U);
        atomic_store(&s_datasets[kind].status_code, 200);
        atomic_store(&s_datasets[kind].retry_after_seconds, 0U);
    }
}

void weather_host_set_now(int64_t now)
{
    atomic_store(&s_now, now);
}

void weather_host_set_location(int16_t latitude_tenths,
                               int16_t longitude_tenths)
{
    atomic_store(&s_latitude_tenths, latitude_tenths);
    atomic_store(&s_longitude_tenths, longitude_tenths);
}

void weather_host_fail_location_transport(unsigned count)
{
    atomic_store(&s_location_transport_failures, count);
}

void weather_host_fail_weather_transport(weather_service_kind_t kind,
        unsigned count)
{
    atomic_store(&s_datasets[kind].transport_failures, count);
}

void weather_host_set_weather_status(weather_service_kind_t kind,
                                     int status_code,
                                     uint32_t retry_after_seconds)
{
    atomic_store(&s_datasets[kind].status_code, status_code);
    atomic_store(&s_datasets[kind].retry_after_seconds, retry_after_seconds);
}

unsigned weather_host_location_requests(void)
{
    return atomic_load(&s_location_requests);
}

unsigned weather_host_weather_requests(weather_service_kind_t kind)
{
    return atomic_load(&s_datasets[kind].requests);
}

unsigned weather_host_cache_writes(void)
{
    return atomic_load(&s_cache_writes);
}

unsigned weather_host_psram_allocations(void)
{
    return atomic_load(&s_psram_allocations);
}

void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities)
{
    if ((capabilities & MALLOC_CAP_SPIRAM) != 0U)
    {
        atomic_fetch_add(&s_psram_allocations, 1U);
    }
    return calloc(count, size);
}

void heap_caps_free(void *memory)
{
    free(memory);
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "host error";
}

esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags)
{
    (void)msg_id;
    (void)sub_type;
    (void)payload;
    (void)payload_size;
    (void)flags;
    return ESP_OK;
}

esp_err_t weather_service_port_http_get(
    const char *url, const char *token,
    const weather_service_location_t *location, size_t response_limit,
    uint32_t timeout_ms, weather_service_http_result_t *result)
{
    (void)token;
    (void)response_limit;
    (void)timeout_ms;
    memset(result, 0, sizeof(*result));
    if (location == NULL)
    {
        atomic_fetch_add(&s_location_requests, 1U);
        if (_weather_host_consume(&s_location_transport_failures))
        {
            return ESP_FAIL;
        }
        result->status_code = 200;
    }
    else
    {
        weather_service_kind_t kind = _weather_host_kind(url);
        weather_host_dataset_t *dataset = &s_datasets[kind];
        atomic_fetch_add(&dataset->requests, 1U);
        if (_weather_host_consume(&dataset->transport_failures))
        {
            return ESP_FAIL;
        }
        result->status_code = atomic_load(&dataset->status_code);
        result->retry_after_seconds = atomic_load(
                                          &dataset->retry_after_seconds);
    }
    result->body = malloc(2U);
    if (result->body == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    result->body[0] = '{';
    result->body[1] = '}';
    result->body_size = 2U;
    return ESP_OK;
}

void weather_service_port_http_result_release(
    weather_service_http_result_t *result)
{
    free(result->body);
    memset(result, 0, sizeof(*result));
}

void weather_service_port_cancel(void)
{
}

int64_t weather_service_port_now_seconds(void)
{
    return atomic_load(&s_now);
}

uint32_t weather_service_port_random_u32(void)
{
    return 0U;
}

esp_err_t weather_service_parse_location(const uint8_t *body,
        size_t body_size, int64_t acquired_at,
        weather_service_location_t *location)
{
    (void)body;
    (void)body_size;
    memset(location, 0, sizeof(*location));
    memcpy(location->city, "Shenzhen", sizeof("Shenzhen"));
    memcpy(location->region, "Guangdong", sizeof("Guangdong"));
    memcpy(location->country, "CN", sizeof("CN"));
    memcpy(location->timezone, "Asia/Shanghai", sizeof("Asia/Shanghai"));
    memcpy(location->provider, "ipapi.is", sizeof("ipapi.is"));
    location->latitude_tenths = (int16_t)atomic_load(&s_latitude_tenths);
    location->longitude_tenths = (int16_t)atomic_load(&s_longitude_tenths);
    location->acquired_at = acquired_at;
    location->available = true;
    return ESP_OK;
}

esp_err_t weather_service_parse_weather(weather_service_kind_t kind,
                                        const uint8_t *body, size_t body_size,
                                        weather_service_snapshot_t *snapshot, uint32_t *changed_mask)
{
    (void)body;
    (void)body_size;
    weather_service_dataset_meta_t *metadata = NULL;
    uint32_t mask = 0U;
    switch (kind)
    {
    case WEATHER_SERVICE_KIND_CURRENT:
        metadata = &snapshot->current.meta;
        snapshot->current.temperature_tenths_c = 312;
        mask = WEATHER_SERVICE_DATA_CURRENT;
        break;
    case WEATHER_SERVICE_KIND_ALERTS:
        metadata = &snapshot->alerts.meta;
        mask = WEATHER_SERVICE_DATA_ALERTS;
        break;
    case WEATHER_SERVICE_KIND_HOURLY:
        metadata = &snapshot->hourly.meta;
        mask = WEATHER_SERVICE_DATA_HOURLY;
        break;
    case WEATHER_SERVICE_KIND_DAILY:
        metadata = &snapshot->daily.meta;
        mask = WEATHER_SERVICE_DATA_DAILY;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    metadata->available = true;
    metadata->fetched_at.epoch_seconds = atomic_load(&s_now);
    snapshot->available_mask |= mask | WEATHER_SERVICE_DATA_LOCATION;
    *changed_mask = mask | WEATHER_SERVICE_DATA_LOCATION;
    return ESP_OK;
}

void weather_service_parse_init(void)
{
}

esp_err_t weather_service_cache_load(const char *directory,
                                     weather_service_snapshot_t *snapshot, uint64_t *sequence)
{
    (void)directory;
    (void)snapshot;
    (void)sequence;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t weather_service_cache_store(const char *directory,
                                      const weather_service_snapshot_t *snapshot, uint64_t sequence)
{
    (void)directory;
    (void)snapshot;
    (void)sequence;
    atomic_fetch_add(&s_cache_writes, 1U);
    return ESP_OK;
}
