#ifndef __HOST_PROVISIONING_ESP_TIMER_H__
#define __HOST_PROVISIONING_ESP_TIMER_H__

#include <stdint.h>

/** @brief Return the host monotonic clock in microseconds. */
int64_t esp_timer_get_time(void);

#endif /* __HOST_PROVISIONING_ESP_TIMER_H__ */
