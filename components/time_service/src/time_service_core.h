#ifndef __TIME_SERVICE_CORE_H__
#define __TIME_SERVICE_CORE_H__

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/**
 * @brief Validate the supported calendar fields of a UTC time value.
 *
 * @param value is the calendar value to validate.
 *
 * @return true when the value is supported; false otherwise.
 */
bool time_service_core_tm_valid(const struct tm *value);

/**
 * @brief Convert a UTC calendar value to Unix epoch seconds.
 *
 * @param value is the UTC calendar value to convert.
 * @param epoch receives Unix epoch seconds.
 *
 * @return true on success; false for invalid arguments or values.
 */
bool time_service_core_utc_to_epoch(const struct tm *value, int64_t *epoch);

/**
 * @brief Convert Unix epoch seconds to a normalized UTC calendar value.
 *
 * @param epoch is the Unix time to convert.
 * @param value receives the normalized UTC calendar value.
 *
 * @return true on success; false for an unsupported epoch or null output.
 */
bool time_service_core_epoch_to_utc(int64_t epoch, struct tm *value);

#endif /* __TIME_SERVICE_CORE_H__ */
