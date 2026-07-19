#ifndef __TIME_SERVICE_HOST_EVENT_BUS_H__
#define __TIME_SERVICE_HOST_EVENT_BUS_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Reset fake event publication state. */
void host_event_bus_reset(void);

/** @brief Select the result returned by later publication attempts. */
void host_event_bus_set_result(esp_err_t result);

/** @brief Wait until at least count publication attempts have occurred. */
bool host_event_bus_wait_for_attempts(uint32_t count, uint32_t timeout_ms);

/** @brief Wait until at least count publications have succeeded. */
bool host_event_bus_wait_for_count(uint32_t count, uint32_t timeout_ms);

/** @brief Return the number of successful publications. */
uint32_t host_event_bus_count(void);

/** @brief Return flags supplied to the most recent successful publication. */
uint32_t host_event_bus_last_flags(void);

/** @brief Return the last alarm sequence copied by the fake. */
uint32_t host_event_bus_last_sequence(void);

#endif /* __TIME_SERVICE_HOST_EVENT_BUS_H__ */
