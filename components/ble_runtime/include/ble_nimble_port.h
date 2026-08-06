#ifndef __BLE_NIMBLE_PORT_H__
#define __BLE_NIMBLE_PORT_H__

#include "host/ble_gap.h"

#include "ble_port_ops.h"
#include "ble_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the transport operations implementation.
 *
 * The production implementation is the port's own; host tests install a fake
 * before starting the runtime. May be replaced while the runtime is stopped.
 *
 * @param[in] ops Operations, or NULL to restore the production port ops.
 * @return ESP_OK or ESP_ERR_INVALID_STATE while the runtime is running.
 */
esp_err_t ble_nimble_port_set_ops(const ble_port_ops_t *ops);

/**
 * @brief Register an event consumer.
 *
 * The port translates host events and fans them out to all registered
 * consumers in registration order. The GAP manager, advertising manager, and
 * TX scheduler each register their own consumer. Consumers may be registered
 * while the runtime is stopped or running; registration, unregistration, and
 * dispatch all happen on the host task, so a consumer registered from another
 * task while the runtime is running must be synchronized with the host task.
 *
 * @param[in] callback Consumer callback.
 * @param[in] arg      Callback argument.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM when full.
 */
esp_err_t ble_nimble_port_register_event_cb(
    ble_port_event_cb_t callback, void *arg);

/**
 * @brief Unregister an event consumer.
 *
 * @param[in] callback Callback to remove.
 * @param[in] arg      Matching argument.
 * @return ESP_OK or ESP_ERR_NOT_FOUND.
 */
esp_err_t ble_nimble_port_unregister_event_cb(
    ble_port_event_cb_t callback, void *arg);

/**
 * @brief Get the active transport operations.
 *
 * Returns the production port ops until a fake is installed with set_ops.
 * Consumers (advertising manager, TX scheduler) call the returned operations
 * to submit advertising control and notifications/indications.
 *
 * @return Active operations.
 */
const ble_port_ops_t *ble_nimble_port_get_ops(void);

/**
 * @brief Get the host port implementation for the runtime.
 */
const ble_runtime_host_port_t *ble_nimble_port_get(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NIMBLE_PORT_H__ */
