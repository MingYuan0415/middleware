#ifndef __WEATHER_SERVICE_HOST_H__
#define __WEATHER_SERVICE_HOST_H__

#include "weather_service_internal.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief Reset all weather worker fakes to successful defaults. */
void weather_host_reset(void);

/** @brief Set the fake wall clock used by refresh policy. */
void weather_host_set_now(int64_t now);

/** @brief Set only the fake UTC wall clock. */
void weather_host_set_wall_seconds(int64_t now);

/** @brief Set only the fake monotonic clock. */
void weather_host_set_monotonic_milliseconds(int64_t now_ms);

/** @brief Advance the fake monotonic clock used by stabilization policy. */
void weather_host_advance_milliseconds(int64_t milliseconds);

/** @brief Set the deterministic random value used by jitter policies. */
void weather_host_set_random(uint32_t value);

/** @brief Set the location returned by the next location parse. */
void weather_host_set_location(int16_t latitude_tenths,
                               int16_t longitude_tenths);

/** @brief Fail the next number of location HTTP requests. */
void weather_host_fail_location_transport(unsigned count);

/** @brief Set the HTTP status returned by the location endpoint. */
void weather_host_set_location_status(int status_code);

/** @brief Fail the next number of dataset HTTP requests. */
void weather_host_fail_weather_transport(weather_service_kind_t kind,
        unsigned count);

/** @brief Set the HTTP status and Retry-After for one dataset. */
void weather_host_set_weather_status(weather_service_kind_t kind,
                                     int status_code,
                                     uint32_t retry_after_seconds);

/** @brief Fail the next number of dataset parse operations. */
void weather_host_fail_weather_parse(weather_service_kind_t kind,
                                     unsigned count);

/** @brief Make the next dataset parse fail with an allocation error. */
void weather_host_fail_weather_parse_no_mem(weather_service_kind_t kind,
        unsigned count);

/** @brief Return the number of location HTTP requests. */
unsigned weather_host_location_requests(void);

/** @brief Return the number of HTTP requests for one dataset. */
unsigned weather_host_weather_requests(weather_service_kind_t kind);

/** @brief Return successful cache writes. */
unsigned weather_host_cache_writes(void);

/** @brief Return snapshot allocations which requested PSRAM. */
unsigned weather_host_psram_allocations(void);

/** @brief Fail one PSRAM allocation after the requested successful calls. */
void weather_host_fail_psram_after(unsigned successful_allocations);

/** @brief Configure cache-load behavior and optional cached snapshot. */
void weather_host_set_cache_load(esp_err_t result,
                                 const weather_service_snapshot_t *snapshot);

/** @brief Block the next fake HTTP request until cancellation. */
void weather_host_block_next_http(void);

/** @brief Wait until a blocked fake HTTP request has entered. */
bool weather_host_wait_http_entered(unsigned timeout_ms);

#endif /* __WEATHER_SERVICE_HOST_H__ */
