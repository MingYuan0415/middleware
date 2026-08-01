#ifndef __TIME_SERVICE_HOST_TEST_CONFIG_H__
#define __TIME_SERVICE_HOST_TEST_CONFIG_H__

#include "time_service.h"

static inline const time_service_config_t *test_time_config(void)
{
    static const time_service_config_t config =
    {
        .timezone = "CST-8",
        .sntp_server = "pool.ntp.org",
        .task_priority = 4U,
    };
    return &config;
}

#endif /* __TIME_SERVICE_HOST_TEST_CONFIG_H__ */
