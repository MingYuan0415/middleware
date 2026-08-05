#ifndef __WEATHER_SERVICE_HOST_H__
#define __WEATHER_SERVICE_HOST_H__

#include "weather_service_internal.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief Reset all weather worker fakes to successful defaults. */
void weather_host_reset(void);

/** @brief Set the fake wall clock used by refresh policy. */
void weather_host_set_now(int64_t now);

/** @brief Set the location returned by the next location parse. */
void weather_host_set_location(int16_t latitude_tenths,
                               int16_t longitude_tenths);

/** @brief Fail the next number of location HTTP requests. */
void weather_host_fail_location_transport(unsigned count);

/** @brief Fail the next number of dataset HTTP requests. */
void weather_host_fail_weather_transport(weather_service_kind_t kind,
        unsigned count);

/** @brief Set the HTTP status and Retry-After for one dataset. */
void weather_host_set_weather_status(weather_service_kind_t kind,
                                     int status_code,
                                     uint32_t retry_after_seconds);

/** @brief Return the number of location HTTP requests. */
unsigned weather_host_location_requests(void);

/** @brief Return the number of HTTP requests for one dataset. */
unsigned weather_host_weather_requests(weather_service_kind_t kind);

/** @brief Return successful cache writes. */
unsigned weather_host_cache_writes(void);

/** @brief Return snapshot allocations which requested PSRAM. */
unsigned weather_host_psram_allocations(void);

#endif /* __WEATHER_SERVICE_HOST_H__ */
