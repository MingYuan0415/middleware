#ifndef __SYSTEM_PM_HOST_DRIVER_RTC_IO_H__
#define __SYSTEM_PM_HOST_DRIVER_RTC_IO_H__

#include <stdbool.h>

#include "hal/gpio_types.h"

/** @brief True when the host fixture models @p gpio_num as RTC capable. */
bool system_pm_fakes_rtc_gpio_is_valid(int gpio_num);

#define RTC_GPIO_IS_VALID_GPIO(gpio_num) \
    system_pm_fakes_rtc_gpio_is_valid(gpio_num)

#endif /* __SYSTEM_PM_HOST_DRIVER_RTC_IO_H__ */
