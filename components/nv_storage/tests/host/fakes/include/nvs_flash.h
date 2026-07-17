#ifndef __NV_STORAGE_HOST_NVS_FLASH_H__
#define __NV_STORAGE_HOST_NVS_FLASH_H__

#include "esp_err.h"

/** @brief Fake implementation of ESP-IDF nvs_flash_init(). */
esp_err_t nvs_flash_init(void);
/** @brief Fake implementation of ESP-IDF nvs_flash_deinit(). */
esp_err_t nvs_flash_deinit(void);
/** @brief Fake implementation of ESP-IDF nvs_flash_erase(). */
esp_err_t nvs_flash_erase(void);

#endif /* __NV_STORAGE_HOST_NVS_FLASH_H__ */
