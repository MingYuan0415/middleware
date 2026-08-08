#ifndef __WEATHER_SERVICE_INTERNAL_H__
#define __WEATHER_SERVICE_INTERNAL_H__

#include "weather_service.h"

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    WEATHER_SERVICE_KIND_CURRENT = 0,
    WEATHER_SERVICE_KIND_ALERTS,
    WEATHER_SERVICE_KIND_HOURLY,
    WEATHER_SERVICE_KIND_DAILY,
    WEATHER_SERVICE_KIND_COUNT,
} weather_service_kind_t;

typedef struct weather_service_http_result
{
    uint8_t *body;
    size_t body_size;
    int status_code;
    uint32_t retry_after_seconds;
} weather_service_http_result_t;

typedef struct weather_service_fetch_result
{
    esp_err_t error;
    int status_code;
    uint32_t retry_after_seconds;
    bool scope_drifted;
} weather_service_fetch_result_t;

esp_err_t weather_service_port_http_get(
    const char *url, const char *token, size_t response_limit,
    uint32_t timeout_ms, uint64_t cancel_generation,
    weather_service_http_result_t *result);
void weather_service_port_http_result_release(
    weather_service_http_result_t *result);
uint64_t weather_service_port_cancel_generation(void);
void weather_service_port_cancel(void);
int64_t weather_service_port_now_seconds(void);
int64_t weather_service_port_now_milliseconds(void);
uint32_t weather_service_port_random_u32(void);
void *weather_service_port_psram_calloc(size_t count, size_t size);
void weather_service_port_psram_free(void *memory);

esp_err_t weather_service_parse_location(const uint8_t *body,
        size_t body_size, int64_t acquired_at,
        weather_service_location_t *location);
esp_err_t weather_service_parse_weather(weather_service_kind_t kind,
                                        const uint8_t *body, size_t body_size,
                                        weather_service_snapshot_t *snapshot, uint32_t *changed_mask);
void weather_service_parse_init(void);

esp_err_t weather_service_cache_load(const char *directory,
                                     weather_service_snapshot_t *snapshot, uint64_t *sequence);
esp_err_t weather_service_cache_store(const char *directory,
                                      const weather_service_snapshot_t *snapshot, uint64_t sequence);

#endif /* __WEATHER_SERVICE_INTERNAL_H__ */
