#ifndef __SYSTEM_PM_HOST_HAL_GPIO_TYPES_H__
#define __SYSTEM_PM_HOST_HAL_GPIO_TYPES_H__

#include <stdint.h>

/** @brief Host stand-in for the SoC GPIO count used by validity checks. */
#define SOC_GPIO_PIN_COUNT 49

/** @brief Host stand-in for the GPIO number type. */
typedef int gpio_num_t;

/** @brief Model the ESP32-S3 GPIO validity window. */
#define GPIO_IS_VALID_GPIO(gpio_num) \
    (((gpio_num) >= 0) && ((gpio_num) < SOC_GPIO_PIN_COUNT))

#endif /* __SYSTEM_PM_HOST_HAL_GPIO_TYPES_H__ */
