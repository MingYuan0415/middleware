#ifndef BLE_NIMBLE_STORE_GUARD_H
#define BLE_NIMBLE_STORE_GUARD_H

#include <stdatomic.h>

#include "esp_err.h"

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

#endif /* BLE_NIMBLE_STORE_GUARD_H */
