#ifndef __IMU_SERVICE_HOST_EVENT_BUS_H__
#define __IMU_SERVICE_HOST_EVENT_BUS_H__

#include <stdbool.h>
#include <stdint.h>

#include "imu_service.h"

/** @brief Reset all fake event publication records. */
void host_event_bus_reset(void);
/** @brief Select the result returned by later publication attempts. */
void host_event_bus_set_result(esp_err_t result);
/** @brief Wait for successful publications of one subtype. */
bool host_event_bus_wait_for_count(uint32_t subtype, uint32_t count,
                                   uint32_t timeout_ms);
/** @brief Wait for publication attempts of one subtype. */
bool host_event_bus_wait_for_attempts(uint32_t subtype, uint32_t count,
                                      uint32_t timeout_ms);
/** @brief Return the successful publication count for one subtype. */
uint32_t host_event_bus_count(uint32_t subtype);
/** @brief Return the last flags supplied for one subtype. */
uint32_t host_event_bus_flags(uint32_t subtype);
/** @brief Copy the last snapshot payload for one snapshot subtype. */
bool host_event_bus_get_snapshot(uint32_t subtype,
                                 imu_service_snapshot_t *snapshot);
/** @brief Copy the last interrupt sample payload. */
bool host_event_bus_get_interrupt(imu_service_sample_t *sample);

#endif /* __IMU_SERVICE_HOST_EVENT_BUS_H__ */
