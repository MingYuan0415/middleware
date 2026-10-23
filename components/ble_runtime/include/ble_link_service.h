#ifndef __BLE_LINK_SERVICE_H__
#define __BLE_LINK_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_operation.h"
#include "device_link_v1.h"
#include "device_link_v1_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_LINK_SERVICE_WORK_SLOTS 4U
#define BLE_LINK_SERVICE_MAX_SUBSCRIBERS 1U

typedef enum
{
    BLE_LINK_SERVICE_TX_SESSION = 0,
} ble_link_service_tx_channel_t;

typedef enum
{
    BLE_LINK_SERVICE_RX_SESSION = 0,
} ble_link_service_rx_channel_t;

typedef esp_err_t (*ble_link_service_output_t)(
    const uint8_t *value, size_t len,
    ble_link_service_tx_channel_t channel, bool is_last,
    const ble_link_operation_identity_t *identity, void *arg);

typedef struct ble_link_service_facts
{
    uint64_t active_boot_id;
    uint32_t connection_generation;
    uint32_t preferred_att_mtu;
    uint16_t conn_handle;
    bool encrypted;
    bool identity_known;
    bool secure_connections_bond_verified;
    bool pairing_window_open;
    uint8_t peer_addr_type;
    uint8_t peer_addr[6];
} ble_link_service_facts_t;

typedef struct ble_link_v1_owner_ops
{
    void (*fill_info)(device_link_v1_info_t *info, void *arg);
    device_link_v1_status_t (*submit_operation)(
        device_link_v1_operation_t operation,
        const device_link_v1_credentials_t *credentials,
        uint32_t operation_id, void *arg);
} ble_link_v1_owner_ops_t;

/** @brief One coherent local Numeric Comparison confirmation snapshot. */
typedef struct ble_link_confirmation_snapshot
{
    bool pending; /**< A local confirmation decision is required. */
    uint64_t token; /**< Exact local confirmation transaction token. */
    uint32_t numeric_comparison; /**< Six-digit value, valid while pending. */
} ble_link_confirmation_snapshot_t;

typedef struct ble_link_work ble_link_work_t;
typedef esp_err_t (*ble_link_work_submit_fn)(ble_link_work_t *work, void *arg);
typedef void (*ble_link_service_wake_fn_t)(void *arg);

esp_err_t ble_link_service_set_domain_descriptors(
    const void *domains, size_t domain_count);

void ble_link_service_init(
    uint64_t boot_id, ble_link_service_output_t output, void *arg,
    const void *security, size_t max_pending_frames);
void ble_link_service_reset(void);
void ble_link_service_set_v1_ops(const ble_link_v1_owner_ops_t *ops, void *arg);
void ble_link_service_set_pairing_window(bool open);

/**
 * @brief Update current server_tx indication admission.
 *
 * Stale connection identities are ignored. Execution checks this state under
 * the same lock that admits domain operations.
 *
 * @param[in] generation Current connection generation.
 * @param[in] conn_handle Current connection handle.
 * @param[in] admitted True when the server_tx indication CCCD is enabled.
 */
void ble_link_service_set_transport_admitted(
    uint32_t generation, uint16_t conn_handle, bool admitted);
void ble_link_service_observe_snapshot(const device_link_v1_snapshot_t *snapshot);
esp_err_t ble_link_service_complete_operation(
    uint32_t operation_id, device_link_v1_wifi_failure_t failure,
    const device_link_v1_network_t *networks, uint8_t count,
    const device_link_v1_snapshot_t *snapshot);

esp_err_t ble_link_service_accept(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const uint8_t *value, size_t len,
    ble_link_work_t **out_work);
esp_err_t ble_link_service_execute(ble_link_work_t *work);
void ble_link_service_release_work(ble_link_work_t *work);

void ble_link_service_set_worker_wake(
    ble_link_service_wake_fn_t wake, void *arg);
void ble_link_service_wake_owner(void);
esp_err_t ble_link_service_pump_tx(void);

/**
 * @brief Complete an ACTIVE record with TIMEOUT when its deadline is due.
 *
 * @param[in] now_ms Monotonic millisecond clock.
 */
void ble_link_service_tick(uint32_t now_ms);

/**
 * @brief Remaining milliseconds until the ACTIVE operation deadline.
 *
 * @return UINT32_MAX when no ACTIVE deadline is armed, 0 when due.
 */
uint32_t ble_link_service_operation_timeout_remaining_ms(void);

/**
 * @brief Record negotiated ATT MTU for the current connection only.
 *
 * Zero generation is INVALID_ARG; a retired connection is NOT_FOUND.
 * The MTU is clamped to at least 23.
 */
esp_err_t ble_link_service_set_att_mtu(
    uint32_t generation, uint16_t conn_handle, uint16_t att_mtu);
/**
 * @brief Retire one scheduler-qualified indication, waking the owner once.
 *
 * The scheduler owns token validation. The service matches the connection and
 * flow; malformed identities fail with INVALID_ARG and stale ones with NOT_FOUND.
 */
esp_err_t ble_link_service_tx_completed(
    const ble_link_operation_identity_t *identity, esp_err_t status);

/**
 * @brief True while a Write is reserved or a `server_tx` indication is
 * unconfirmed.
 *
 * GATT must reject a new Write with ATT `0xfe` when this is true.
 */
bool ble_link_service_write_blocked(void);
bool ble_link_service_response_in_flight(void);
void ble_link_service_abort_transactions(void);
/**
 * @brief Install an active session ACL, preserving duplicate connection state.
 *
 * Zero generation is INVALID_ARG; inactive or older ACLs are NOT_FOUND.
 * This fence does not require encryption or a committed bond.
 */
esp_err_t ble_link_service_on_connect(uint32_t generation, uint16_t conn_handle);
void ble_link_service_clear_session_state(void);
esp_err_t ble_link_service_clear_session_state_if_current(
    const ble_link_operation_identity_t *identity);

esp_err_t ble_link_service_confirm_binding(uint64_t token, bool accept);

/** @brief Return the current confirmation tuple from one locked read. */
ble_link_confirmation_snapshot_t ble_link_service_get_confirmation(void);

/** @brief Allocate a boot-unique token without publishing or waking the owner.
 * @return ESP_ERR_INVALID_ARG for NULL; ESP_ERR_INVALID_STATE if uninitialized
 * or exhausted (with output zero); otherwise ESP_OK.
 */
esp_err_t ble_link_service_allocate_confirmation_token(uint64_t *out_token);

/** @brief Publish an already-installed host offer and wake the product owner.
 * @return ESP_ERR_INVALID_ARG for zero token; ESP_ERR_INVALID_STATE before init.
 */
esp_err_t ble_link_service_publish_numeric_comparison(uint32_t passkey,
        uint64_t token);


#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_SERVICE_H__ */
