#ifndef __TIME_SERVICE_HOST_TIME_IDF_H__
#define __TIME_SERVICE_HOST_TIME_IDF_H__

#include <stddef.h>

#include "esp_err.h"

/** @brief Calls recorded by the ESP-IDF SNTP lifecycle fake. */
typedef enum
{
    HOST_TIME_IDF_CALLBACK_DETACHED = 0,
    HOST_TIME_IDF_SNTP_STOPPED,
    HOST_TIME_IDF_TCPIP_BARRIER,
} host_time_idf_call_t;

/** @brief Clear the fake ESP-IDF call trace. */
void host_time_idf_reset(void);
/** @brief Select the fake TCP/IP barrier result. */
void host_time_idf_set_barrier_result(esp_err_t result);
/** @brief Return the number of recorded calls. */
size_t host_time_idf_call_count(void);
/** @brief Return one recorded call by index. */
host_time_idf_call_t host_time_idf_call_at(size_t index);

#endif /* __TIME_SERVICE_HOST_TIME_IDF_H__ */
