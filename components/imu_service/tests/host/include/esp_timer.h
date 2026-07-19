#ifndef __IMU_SERVICE_HOST_ESP_TIMER_H__
#define __IMU_SERVICE_HOST_ESP_TIMER_H__

#include <stdint.h>

/** @brief Return a deterministic monotonic host timestamp. */
int64_t esp_timer_get_time(void);

#endif /* __IMU_SERVICE_HOST_ESP_TIMER_H__ */
