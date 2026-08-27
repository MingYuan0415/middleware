#ifndef __BLE_NIMBLE_PORT_H__
#define __BLE_NIMBLE_PORT_H__

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

/** @brief Copy the boot-scoped public discovery instance identifier. */
esp_err_t ble_nimble_port_get_public_instance_id(uint8_t out_instance_id[3]);

/**
 * @brief Request a local binding revoke (bond/CCCD deletion on the host
 * core).
 *
 * The caller must journal the revoke intent and erase the authorization
 * record first; the port deletes the stored bond and CCCDs asynchronously
 * and clears the journal. The startup continuation resumes an interrupted
 * revoke before advertising.
 *
 * @return ESP_OK when the deletion was queued, otherwise a state error.
 */
esp_err_t ble_nimble_port_revoke_binding(void);

/** @brief Complete Numeric Comparison with the local Yes/No decision. */
esp_err_t ble_nimble_port_numeric_comparison_reply(bool accept);

/** @brief Drop a pending Numeric Comparison without injecting a reply. */
void ble_nimble_port_numeric_comparison_cancel(void);

/**
 * @brief Request a local disconnect of the current ACL.
 *
 * Serializes the terminate through the host-core owner task, so the
 * pairing window can be re-opened only after the existing ACL is gone.
 *
 * @return ESP_OK when the terminate was queued, otherwise a state error.
 */
esp_err_t ble_nimble_port_request_disconnect(void);

/**
 * @brief Close new-link admission and wait for a NimBLE host-event barrier.
 *
 * The barrier closes the SMP gate on the host core. Once it returns, every
 * host callback queued before the call has either retained its cleanup work or
 * completed. Repeated calls add a fresh barrier and are used by shutdown to
 * establish a stable empty fixed point before stopping the host.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE, or ESP_ERR_TIMEOUT.
 */
esp_err_t ble_nimble_port_begin_cleanup_drain(void);

/**
 * @brief Whether cleanup or terminal work remains owned by the NimBLE port.
 *
 * Device Link shutdown uses this retained-state query before stopping the
 * host. A true result means the store/host owner must remain alive until the
 * physical delete is confirmed or a bounded deinit caller times out.
 *
 * @return True while store cleanup, an ACL terminal fence, or an accepted or
 *         rejected termination remains pending.
 */
bool ble_nimble_port_cleanup_pending(void);

/**
 * @brief Synchronously set the pairing-window SMP admission gate.
 *
 * The change executes on the persistent NimBLE host event queue and is
 * acknowledged before return. Closed is fail-closed (`sm_sec_lvl=1`);
 * open admits Pairing Request (`sm_sec_lvl=0`).
 */
esp_err_t ble_nimble_port_set_pairing_window(bool open);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NIMBLE_PORT_H__ */
