#ifndef __BLE_NIMBLE_PORT_H__
#define __BLE_NIMBLE_PORT_H__

#include "host/ble_gap.h"

#include "ble_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief GAP event sink installed by the GAP manager (P1.4). */
typedef void (*ble_nimble_port_gap_cb_t)(
    struct ble_gap_event *event, void *arg);

/**
 * @brief Install the GAP event sink.
 *
 * The GAP manager (P1.4) installs this sink and passes the port's bridge as
 * the event callback of every GAP procedure it starts, so host events reach
 * the manager. The port itself starts no GAP procedures and therefore
 * produces no events before the manager is wired up. May be replaced while
 * the runtime is stopped.
 *
 * @param[in] callback Sink callback, or NULL to clear.
 * @param[in] arg      Callback argument.
 * @return ESP_OK or ESP_ERR_INVALID_STATE while the runtime is running.
 */
esp_err_t ble_nimble_port_set_gap_callback(
    ble_nimble_port_gap_cb_t callback, void *arg);

/**
 * @brief Get the host port implementation for the runtime.
 */
const ble_runtime_host_port_t *ble_nimble_port_get(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NIMBLE_PORT_H__ */
