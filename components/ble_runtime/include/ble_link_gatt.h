#ifndef __BLE_LINK_GATT_H__
#define __BLE_LINK_GATT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_service.h"
#include "ble_tx_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Device Link ATT MTU cap (profile preferred_att_mtu). */
#define BLE_LINK_GATT_ATT_MTU_MAX 498U

/**
 * @brief GATT wiring configuration.
 */
typedef struct ble_link_gatt_config
{
    uint64_t boot_id;             /**< Fresh nonzero value for this boot. */
    uint32_t connection_generation; /**< Current connection generation. */
    uint16_t conn_handle;         /**< Active connection handle. */
    uint8_t peer_addr_type;       /**< SMP peer identity address type. */
    uint8_t peer_addr[6];         /**< SMP peer identity address. */
    uint16_t att_mtu;             /**< Negotiated ATT MTU, 23 default. */
    size_t tx_queue_depth;        /**< TX scheduler queue depth. */
} ble_link_gatt_config_t;

/**
 * @brief Install the worker sink for completed Link messages.
 *
 * Set before the host starts and clear only after it stops. A NULL sink keeps
 * synchronous execution for focused host tests.
 */
void ble_link_gatt_set_work_submit(ble_link_work_submit_fn submit, void *arg);

/**
 * @brief Register the Device Link service characteristics and initialize
 * the link service.
 *
 * Must be called before the registry is sealed. Registers command_rx (write)
 * and server_tx (indicate).
 *
 * @param[in] config Configuration, kept for the lifetime.
 * @return ESP_OK or an error from the registry or service init.
 */
esp_err_t ble_link_gatt_init(const ble_link_gatt_config_t *config);

/**
 * @brief Reset the GATT wiring (new boot or teardown).
 */
void ble_link_gatt_reset(void);

/**
 * @brief Re-initialize the transport service after a runtime restart.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_STATE when not previously initialized.
 */
esp_err_t ble_link_gatt_restart(void);

/**
 * @brief Refresh the characteristic handles from the registry.
 *
 * Called by the transport after the NimBLE database registration delivered
 * handle assignments. A characteristic without an assigned handle keeps its
 * previous value (0 before the first assignment).
 */
void ble_link_gatt_update_handles(void);

/**
 * @brief Advance the connection identity (new connection or generation).
 *
 * @param[in] generation New connection generation.
 * @param[in] conn_handle New connection handle.
 */
void ble_link_gatt_set_connection(
    uint32_t generation, uint16_t conn_handle,
    uint8_t peer_addr_type, const uint8_t peer_addr[6]);

/**
 * @brief Update the resolved peer identity for the current connection.
 *
 * Unlike set_connection(), this preserves the negotiated ATT MTU and all
 * other per-ACL delivery state.
 *
 * @param[in] generation Current connection generation.
 * @param[in] conn_handle Current NimBLE connection handle.
 * @param[in] peer_addr_type Normalized peer identity address type.
 * @param[in] peer_addr Normalized six-byte peer identity address.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NOT_FOUND for stale ACL.
 */
esp_err_t ble_link_gatt_update_identity(
    uint32_t generation, uint16_t conn_handle,
    uint8_t peer_addr_type, const uint8_t peer_addr[6]);

/**
 * @brief Update the negotiated ATT MTU.
 *
 * @param[in] mtu Negotiated ATT MTU, at least 23.
 */
void ble_link_gatt_set_att_mtu(uint16_t mtu);

/**
 * @brief Read the current ATT MTU.
 * @param[out] out_mtu Receives the clamped MTU.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t ble_link_gatt_get_att_mtu(uint32_t *out_mtu);

uint16_t ble_link_gatt_session_tx_handle(void);
uint16_t ble_link_gatt_session_rx_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_GATT_H__ */
