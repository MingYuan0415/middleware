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
    BLE_LINK_SERVICE_TX_CONTROL_RESPONSE,
} ble_link_service_tx_channel_t;

typedef enum
{
    BLE_LINK_SERVICE_RX_SESSION = 0,
    BLE_LINK_SERVICE_RX_CONTROL,
} ble_link_service_rx_channel_t;

typedef esp_err_t (*ble_link_service_output_t)(
    const uint8_t *value, size_t len,
    ble_link_service_tx_channel_t channel, bool is_last, uint32_t flow_id,
    void *arg);

typedef struct ble_link_service_facts
{
    uint64_t active_boot_id;
    uint32_t connection_generation;
    uint32_t security_epoch;
    uint32_t preferred_att_mtu;
    uint16_t conn_handle;
    bool encrypted;
    bool session_authenticated;
    bool authorized;
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
 * @brief Record the negotiated ATT MTU used to size indications.
 *
 * @param[in] att_mtu Negotiated ATT MTU, clamped to at least 23.
 */
void ble_link_service_set_att_mtu(uint16_t att_mtu);
esp_err_t ble_link_service_response_completed(uint32_t flow_id, bool is_last);
bool ble_link_service_write_blocked(void);
bool ble_link_service_response_in_flight(void);
void ble_link_service_abort_transactions(void);
void ble_link_service_on_connect(uint32_t generation, uint16_t conn_handle);
void ble_link_service_clear_session_state(void);
esp_err_t ble_link_service_clear_session_state_if_current(
    const ble_link_operation_identity_t *identity);
esp_err_t ble_link_service_abort_tx_if_current(
    const ble_link_operation_identity_t *identity);

esp_err_t ble_link_service_confirm_binding(uint64_t token, bool accept);
bool ble_link_service_pending_confirmation(void);
uint64_t ble_link_service_confirmation_token(void);
uint32_t ble_link_service_numeric_comparison_value(void);
esp_err_t ble_link_service_offer_numeric_comparison(uint32_t passkey);

esp_err_t ble_link_service_register_remote_replacement(
    const ble_link_operation_identity_t *identity);
void ble_link_service_idle_timeout(uint32_t generation);
void ble_link_service_idle_timeout_epoch(uint32_t generation, uint32_t epoch);
bool ble_link_service_delayed_replacement_pending(uint32_t generation);
uint32_t ble_link_service_retained_retry_remaining_ms(void);
bool ble_link_service_retained_cleanup_pending(void);
uint32_t ble_link_service_auth_expiry_remaining_ms(void);
esp_err_t ble_link_service_auth_expiry_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_SERVICE_H__ */
