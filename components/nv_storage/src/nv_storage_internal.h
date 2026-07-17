#ifndef __NV_STORAGE_INTERNAL_H__
#define __NV_STORAGE_INTERNAL_H__

#include <stdbool.h>

#include "esp_err.h"

/** @brief Storage bytes reserved for a 15-byte NVS key and terminator. */
#define NV_STORAGE_KEY_BYTES 16U

/**
 * @brief Validate one bounded NVS key.
 * @param key is the NUL-terminated key to validate.
 * @return true for a nonempty key of at most 15 bytes; false otherwise.
 */
bool nv_storage_internal_key_is_valid(const char *key);
/**
 * @brief Acquire one lifecycle access reference.
 * @return true while scalar access is admitted; false otherwise.
 */
bool nv_storage_internal_access_begin(void);
/** @brief Release one lifecycle access reference. */
void nv_storage_internal_access_end(void);
/**
 * @brief Acquire the exclusive blob-load lifecycle reference.
 * @return true when the load is admitted; false otherwise.
 */
bool nv_storage_internal_load_begin(void);
/** @brief Release the exclusive blob-load lifecycle reference. */
void nv_storage_internal_load_end(void);
/**
 * @brief Report whether a blob load is active.
 * @return true during blob loading; false otherwise.
 */
bool nv_storage_internal_load_is_active(void);
/**
 * @brief Lock the fixed blob registry.
 * @return ESP_OK when locked; ESP_ERR_INVALID_STATE before initialization.
 */
esp_err_t nv_storage_internal_registry_lock(void);
/** @brief Unlock the fixed blob registry. */
void nv_storage_internal_registry_unlock(void);
/** @brief Clear every registered blob entry. */
void nv_storage_blob_registry_reset(void);

#endif /* __NV_STORAGE_INTERNAL_H__ */
