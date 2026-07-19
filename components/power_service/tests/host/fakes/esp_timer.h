#ifndef __POWER_SERVICE_HOST_ESP_TIMER_H__
#define __POWER_SERVICE_HOST_ESP_TIMER_H__

#include <stdint.h>

/** @brief Return host monotonic time in microseconds. */
int64_t esp_timer_get_time(void);

#endif /* __POWER_SERVICE_HOST_ESP_TIMER_H__ */
