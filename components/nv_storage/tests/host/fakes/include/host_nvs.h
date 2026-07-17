#ifndef __NV_STORAGE_HOST_NVS_CONTROL_H__
#define __NV_STORAGE_HOST_NVS_CONTROL_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Fake NVS operations available for call counting and fault injection. */
typedef enum host_nvs_operation
{
    HOST_NVS_FLASH_INIT = 0,
    HOST_NVS_FLASH_DEINIT,
    HOST_NVS_FLASH_ERASE,
    HOST_NVS_OPEN,
    HOST_NVS_GET_U8,
    HOST_NVS_SET_U8,
    HOST_NVS_GET_U16,
    HOST_NVS_SET_U16,
    HOST_NVS_GET_U32,
    HOST_NVS_SET_U32,
    HOST_NVS_GET_STR,
    HOST_NVS_SET_STR,
    HOST_NVS_GET_BLOB,
    HOST_NVS_SET_BLOB,
    HOST_NVS_ERASE_KEY,
    HOST_NVS_COMMIT,
    HOST_NVS_CLOSE,
    HOST_NVS_OPERATION_COUNT,
} host_nvs_operation_t;

/** @brief Reset all fake flash contents, handles, failures, and gates. */
void host_nvs_reset(void);
/**
 * @brief Fail an operation after a configured number of successes.
 * @param operation identifies the fake operation.
 * @param successes is the number of calls allowed before failure.
 * @param error is the injected error.
 */
void host_nvs_fail_after(host_nvs_operation_t operation, unsigned successes,
                         esp_err_t error);
/**
 * @brief Return the call count for one fake operation.
 * @param operation identifies the fake operation.
 * @return Number of recorded calls.
 */
unsigned host_nvs_call_count(host_nvs_operation_t operation);
/** @brief Return the number of invalid keys received by the fake NVS port. */
unsigned host_nvs_invalid_key_call_count(void);

/** @brief Seed one fake NVS blob. */
void host_nvs_seed_blob(const char *key, const void *data, size_t size);
/** @brief Read one fake NVS blob without opening a public handle. */
bool host_nvs_read_blob(const char *key, void *data, size_t *size);
/** @brief Grow one value immediately before its next nonnull get operation. */
bool host_nvs_grow_before_nonnull_get(host_nvs_operation_t operation,
                                      const char *key, const void *data,
                                      size_t size);

/** @brief Enable or release the fake nvs_open() gate. */
void host_nvs_block_open(bool blocked);
/** @brief Wait until at least one fake nvs_open() call is blocked. */
bool host_nvs_wait_for_blocked_open(uint32_t timeout_ms);

#endif /* __NV_STORAGE_HOST_NVS_CONTROL_H__ */
