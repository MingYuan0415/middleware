#ifndef __BLE_NIMBLE_LINK_EVENTS_H__
#define __BLE_NIMBLE_LINK_EVENTS_H__

#include "ble_port_ops.h"

typedef struct ble_nimble_link_update
{
    ble_port_event_type_t type;
    ble_link_operation_identity_t identity;
    uint16_t mtu;
    bool indicate;
    uint8_t peer_addr_type;
    uint8_t peer_addr[6];
} ble_nimble_link_update_t;

typedef esp_err_t (*ble_nimble_link_prepare_fn)(
    const ble_port_event_t *event, ble_nimble_link_update_t *update, void *arg);

typedef struct ble_nimble_link_events_ops
{
    esp_err_t (*lock)(void *arg);
    void (*unlock)(void *arg);
    ble_nimble_link_prepare_fn prepare;
    void *arg;
} ble_nimble_link_events_ops_t;

/** @brief Capture a connection update under the port lock, then apply unlocked. */
esp_err_t ble_nimble_link_events_handle_update(
    const ble_port_event_t *event, const ble_nimble_link_events_ops_t *ops);

typedef struct ble_nimble_adv_event_state
{
    uint32_t generation;
    uint16_t conn_handle;
} ble_nimble_adv_event_state_t;

/** @brief Apply only advertising/lifecycle events, then wake the owner. */
void ble_nimble_link_events_adv_consumer(const ble_port_event_t *event, void *arg);

#endif /* __BLE_NIMBLE_LINK_EVENTS_H__ */
