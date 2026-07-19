#ifndef __IMU_SERVICE_HOST_TEST_COMMON_H__
#define __IMU_SERVICE_HOST_TEST_COMMON_H__

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "imu_service.h"

#define TEST_CHECK(expression)                                             \
    do                                                                     \
    {                                                                      \
        if (!(expression))                                                 \
        {                                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,       \
                    __LINE__, #expression);                                \
            return false;                                                  \
        }                                                                  \
    } while (0)

static inline void test_sleep_ms(uint32_t delay_ms)
{
    struct timespec delay =
    {
        .tv_sec = (time_t)(delay_ms / 1000U),
        .tv_nsec = (long)(delay_ms % 1000U) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
    {
    }
}

static inline bool test_wait_for_state(imu_service_state_t expected,
                                       uint32_t timeout_ms)
{
    for (uint32_t elapsed = 0U; elapsed < timeout_ms; ++elapsed)
    {
        if (imu_service_get_state() == expected)
        {
            return true;
        }
        test_sleep_ms(1U);
    }
    return imu_service_get_state() == expected;
}

#endif /* __IMU_SERVICE_HOST_TEST_COMMON_H__ */
