#ifndef __NV_STORAGE_HOST_NVS_H__
#define __NV_STORAGE_HOST_NVS_H__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NVS_KEY_NAME_MAX_SIZE 16

typedef uint32_t nvs_handle_t;

/** @brief Fake NVS handle open mode. */
typedef enum nvs_open_mode
{
    NVS_READONLY = 0,
    NVS_READWRITE,
} nvs_open_mode_t;

/** @brief Fake implementation of ESP-IDF nvs_open(). */
esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle);
/** @brief Fake implementation of ESP-IDF nvs_close(). */
void nvs_close(nvs_handle_t handle);
/** @brief Fake implementation of ESP-IDF nvs_commit(). */
esp_err_t nvs_commit(nvs_handle_t handle);

/** @brief Fake implementation of ESP-IDF nvs_set_u8(). */
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
/** @brief Fake implementation of ESP-IDF nvs_get_u8(). */
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *output);
/** @brief Fake implementation of ESP-IDF nvs_set_u16(). */
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value);
/** @brief Fake implementation of ESP-IDF nvs_get_u16(). */
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *output);
/** @brief Fake implementation of ESP-IDF nvs_set_u32(). */
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
/** @brief Fake implementation of ESP-IDF nvs_get_u32(). */
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *output);
/** @brief Fake implementation of ESP-IDF nvs_set_str(). */
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
/** @brief Fake implementation of ESP-IDF nvs_get_str(). */
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *output,
                      size_t *length);
/** @brief Fake implementation of ESP-IDF nvs_set_blob(). */
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length);
/** @brief Fake implementation of ESP-IDF nvs_get_blob(). */
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *output,
                       size_t *length);
/** @brief Fake implementation of ESP-IDF nvs_erase_key(). */
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);

#endif /* __NV_STORAGE_HOST_NVS_H__ */
