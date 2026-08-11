#ifndef BLE_NIMBLE_STORE_GUARD_H
#define BLE_NIMBLE_STORE_GUARD_H

#include <stdbool.h>
#include <stdatomic.h>

#include "esp_err.h"

union ble_store_value;

/** @brief NimBLE store writer signature without importing host internals. */
typedef int ble_nimble_store_write_callback_t(
    int object_type, const union ble_store_value *value);

/** @brief Lifecycle state for the project-owned store writer wrapper. */
typedef struct ble_nimble_store_callback_guard
{
    ble_nimble_store_write_callback_t *original;
    bool active;
} ble_nimble_store_callback_guard_t;

typedef struct ble_nimble_store_guard
{
    atomic_int error;
} ble_nimble_store_guard_t;

/** @brief Start a new host run with no latched store failure. */
void ble_nimble_store_guard_reset(ble_nimble_store_guard_t *guard);

/** @brief Return the first store failure observed in this host run. */
esp_err_t ble_nimble_store_guard_error(
    const ble_nimble_store_guard_t *guard);

/** @brief Latch the first non-OK store result for this host run. */
void ble_nimble_store_guard_finish(
    ble_nimble_store_guard_t *guard, esp_err_t result);

/**
 * @brief Record the result returned by the underlying NimBLE store writer.
 *
 * Capacity exhaustion is handled by NimBLE's store-status callback and may be
 * retried after replacement. Every other nonzero write result is a durability
 * failure and latches this host run fail closed.
 *
 * @param[in,out] guard Host-run store guard.
 * @param[in] result Underlying store writer result.
 * @param[in] retryable_capacity_result NimBLE capacity result.
 * @return The original writer result unchanged.
 */
int ble_nimble_store_guard_record_write_result(
    ble_nimble_store_guard_t *guard, int result,
    int retryable_capacity_result);

/**
 * @brief Arm a writer wrapper and retain an available callback it replaces.
 *
 * ESP-IDF cold boot leaves the slot NULL until host startup. In that case the
 * guard becomes active without changing the slot; reconcile captures and
 * wraps the first non-NULL startup callback.
 *
 * @param[in,out] guard Callback lifecycle state; must be inactive.
 * @param[in,out] callback_slot NimBLE's active writer callback slot.
 * @param[in] wrapper Project-owned writer wrapper.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG/INVALID_STATE.
 */
esp_err_t ble_nimble_store_callback_guard_install(
    ble_nimble_store_callback_guard_t *guard,
    ble_nimble_store_write_callback_t **callback_slot,
    ble_nimble_store_write_callback_t *wrapper);

/**
 * @brief Install or reinstall a writer wrapper after host startup.
 *
 * The first reconcile of an armed cold-boot guard captures the non-NULL IDF
 * writer. After capture, the callback is changed only when the slot contains
 * that exact original callback. An unknown later replacement is never
 * overwritten.
 *
 * @param[in,out] guard Active callback lifecycle state.
 * @param[in,out] callback_slot NimBLE's active writer callback slot.
 * @param[in] wrapper Project-owned writer wrapper.
 * @return ESP_OK when guarded, or ESP_ERR_INVALID_ARG/INVALID_STATE.
 */
esp_err_t ble_nimble_store_callback_guard_reconcile(
    ble_nimble_store_callback_guard_t *guard,
    ble_nimble_store_write_callback_t **callback_slot,
    ble_nimble_store_write_callback_t *wrapper);

/**
 * @brief Restore the captured writer when the wrapper still owns the slot.
 *
 * An unknown replacement remains untouched. The lifecycle state is cleared
 * in all cases so a later host run can install a fresh guard.
 *
 * @param[in,out] guard Callback lifecycle state.
 * @param[in,out] callback_slot NimBLE's active writer callback slot.
 * @param[in] wrapper Project-owned writer wrapper.
 */
void ble_nimble_store_callback_guard_restore(
    ble_nimble_store_callback_guard_t *guard,
    ble_nimble_store_write_callback_t **callback_slot,
    ble_nimble_store_write_callback_t *wrapper);

#endif /* BLE_NIMBLE_STORE_GUARD_H */
