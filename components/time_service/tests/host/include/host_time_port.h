#ifndef __TIME_SERVICE_HOST_TIME_PORT_H__
#define __TIME_SERVICE_HOST_TIME_PORT_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Reset the fake clock and SNTP state. */
void host_time_port_reset(void);
/** @brief Select whether stop invokes the callback it is detaching. */
void host_time_port_invoke_callback_on_stop(bool enabled);
/** @brief Select the result returned by the fake SNTP stop barrier. */
void host_time_port_set_stop_result(esp_err_t result);
/** @brief Complete one fake SNTP synchronization. */
bool host_time_port_complete(int64_t epoch);
/** @brief Return whether the fake SNTP client is running. */
bool host_time_port_is_running(void);
/** @brief Return the number of SNTP start calls. */
unsigned host_time_port_start_count(void);
/** @brief Return the number of SNTP restart calls. */
unsigned host_time_port_restart_count(void);
/** @brief Return the number of SNTP stop calls. */
unsigned host_time_port_stop_count(void);

#endif /* __TIME_SERVICE_HOST_TIME_PORT_H__ */
