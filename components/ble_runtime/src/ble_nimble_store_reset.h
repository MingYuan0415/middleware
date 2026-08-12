#ifndef __BLE_NIMBLE_STORE_RESET_H__
#define __BLE_NIMBLE_STORE_RESET_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One injectable step in the full NimBLE peer-store reset. */
typedef esp_err_t (*ble_nimble_store_reset_step_fn)(void *context);

/** @brief Durable and runtime operations required by the reset sequence. */
typedef struct ble_nimble_store_reset_ops
{
    ble_nimble_store_reset_step_fn prepare_runtime_cleanup;
    ble_nimble_store_reset_step_fn erase_namespace;
    ble_nimble_store_reset_step_fn clear_runtime;
    ble_nimble_store_reset_step_fn audit_empty;
    void *context;
} ble_nimble_store_reset_ops_t;

/**
 * @brief Reset the complete NimBLE peer store in a crash-recoverable order.
 *
 * Exact runtime cleanup keys are captured before the durable namespace is
 * erased. The erase then prevents a malformed blob from blocking deletion
 * through an IDF persistence callback. Runtime cleanup may re-persist
 * intermediate state, so the namespace is erased again before the empty-store
 * audit.
 *
 * @param[in] ops Reset operations and their shared context.
 * @return ESP_OK or the first failed operation result.
 */
esp_err_t ble_nimble_store_reset_run(
    const ble_nimble_store_reset_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NIMBLE_STORE_RESET_H__ */
