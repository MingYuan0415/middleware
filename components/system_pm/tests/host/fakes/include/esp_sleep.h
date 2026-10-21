#ifndef __SYSTEM_PM_HOST_ESP_SLEEP_H__
#define __SYSTEM_PM_HOST_ESP_SLEEP_H__

#include <stdint.h>

#include "esp_err.h"

#define BIT(nr) (UINT32_C(1) << (nr))

typedef enum
{
    ESP_EXT1_WAKEUP_ANY_LOW = 0,
    ESP_EXT1_WAKEUP_ANY_HIGH = 1,
} esp_sleep_ext1_wakeup_mode_t;

typedef enum
{
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_ALL,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
    ESP_SLEEP_WAKEUP_TOUCHPAD,
} esp_sleep_wakeup_cause_t;

esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t io_mask,
        esp_sleep_ext1_wakeup_mode_t level_mode);
esp_err_t esp_sleep_disable_ext1_wakeup_io(uint64_t io_mask);
esp_err_t esp_light_sleep_start(void);
uint32_t esp_sleep_get_wakeup_causes(void);
uint64_t esp_sleep_get_ext1_wakeup_status(void);

#endif /* __SYSTEM_PM_HOST_ESP_SLEEP_H__ */
