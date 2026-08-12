/** @file Controllable monotonic clock declaration for chore host tests. */
#ifndef __CHORE_SERVICE_HOST_ESP_TIMER_H__
#define __CHORE_SERVICE_HOST_ESP_TIMER_H__

#include <stdint.h>

/** @brief Return the fake monotonic time in microseconds. */
int64_t esp_timer_get_time(void);

#endif /* __CHORE_SERVICE_HOST_ESP_TIMER_H__ */
