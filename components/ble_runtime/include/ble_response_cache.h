#ifndef __BLE_RESPONSE_CACHE_H__
#define __BLE_RESPONSE_CACHE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Cached response entry bound to one connection generation.
 *
 * The cache stores the last response payload for a (generation,
 * characteristic, key) triple so a re-read or a retried request is served
 * without re-execution. Entries are served until eviction, an explicit
 * clear, or timeout; a successful read does not invalidate the entry.
 *
 * Retired in production: v2 monotonic request-id replay is owned by the
 * device_link router. This module is compiled by host tests only.
 */
typedef struct ble_response_cache_config
{
    size_t max_entries;      /**< Bounded cache capacity. */
    size_t max_entry_bytes;  /**< Max payload per entry. */
    size_t max_key_bytes;    /**< Max key length. */
    uint32_t ttl_ms;         /**< Entry lifetime; 0 disables expiry. */
    uint32_t (*now_ms)(void); /**< Monotonic millisecond clock. */
    void (*lock)(void *arg);  /**< Optional serialization lock. */
    void (*unlock)(void *arg);
    void *lock_arg;
} ble_response_cache_config_t;

/**
 * @brief Initialize the cache; it is empty.
 *
 * @param[in] config Configuration, kept for the cache lifetime.
 */
void ble_response_cache_init(const ble_response_cache_config_t *config);

/**
 * @brief Release the cache and its configuration.
 *
 * After this call every entry point returns ESP_ERR_INVALID_STATE (or the
 * empty fallback for queries) until the next init.
 */
void ble_response_cache_deinit(void);

/**
 * @brief Store a response for one (generation, key).
 *
 * Replaces any existing entry with the same key. Expired entries are evicted
 * before insertion; when full the oldest entry is evicted.
 *
 * @param[in] generation Connection generation.
 * @param[in] key        Key bytes (e.g. request id).
 * @param[in] key_len    Key length.
 * @param[in] data       Response payload.
 * @param[in] len        Payload length.
 * @return ESP_OK, ESP_ERR_INVALID_ARG (bad arguments or payload/key too
 *         large), ESP_ERR_INVALID_STATE after deinit, or ESP_ERR_NO_MEM
 *         when allocation failed.
 */
esp_err_t ble_response_cache_put(
    uint32_t generation, const uint8_t *key, size_t key_len,
    const uint8_t *data, size_t len);

/**
 * @brief Copy a cached response.
 *
 * @param[in]  generation Connection generation.
 * @param[in]  key        Key bytes.
 * @param[in]  key_len    Key length.
 * @param[out] out        Destination buffer.
 * @param[in]  capacity   Destination capacity.
 * @param[out] out_len    Bytes copied.
 * @return ESP_OK, ESP_ERR_NOT_FOUND when absent or expired,
 *         ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE after deinit, or
 *         ESP_ERR_NO_MEM when the payload does not fit the destination.
 */
esp_err_t ble_response_cache_get(
    uint32_t generation, const uint8_t *key, size_t key_len,
    uint8_t *out, size_t capacity, size_t *out_len);

/**
 * @brief Drop every entry for one generation (disconnect, read, timeout).
 */
void ble_response_cache_clear_generation(uint32_t generation);

/**
 * @brief Drop every entry.
 */
void ble_response_cache_clear(void);

/**
 * @brief Separate set of already-used request ids.
 *
 * Kept deliberately apart from the response cache: an id is marked used as
 * soon as a request is accepted, while its response may be evicted earlier
 * or never cached. The set is bounded; adding beyond capacity evicts the
 * oldest id.
 *
 * @param[in] config Configuration, kept for the set lifetime.
 */
void ble_used_id_set_init(const ble_response_cache_config_t *config);

/**
 * @brief Release the used-id set.
 */
void ble_used_id_set_deinit(void);

/**
 * @brief Mark an id as used for one generation.
 *
 * @param[in] generation Connection generation.
 * @param[in] id         Request id.
 * @return ESP_OK, ESP_ERR_INVALID_STATE after deinit, or ESP_ERR_NO_MEM
 *         when allocation failed.
 */
esp_err_t ble_used_id_set_add(uint32_t generation, uint32_t id);

/**
 * @brief Query whether an id was already used for one generation.
 */
bool ble_used_id_set_contains(uint32_t generation, uint32_t id);

/**
 * @brief Drop every id for one generation.
 */
void ble_used_id_set_clear_generation(uint32_t generation);

/**
 * @brief Drop every id.
 */
void ble_used_id_set_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_RESPONSE_CACHE_H__ */
