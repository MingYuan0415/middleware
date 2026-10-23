#include "ble_nimble_link_events.h"
#include "ble_link_gatt.h"
#include "ble_link_service.h"
#include "ble_adv_manager.h"

#define DBG_TAG "ble_link_events"
#include "mt_log.h"

esp_err_t ble_nimble_link_events_handle_update(
    const ble_port_event_t *event, const ble_nimble_link_events_ops_t *ops)
{
    if (event == NULL || ops == NULL || ops->lock == NULL ||
            ops->unlock == NULL || ops->prepare == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->type != BLE_PORT_EVENT_CONNECT &&
            event->type != BLE_PORT_EVENT_MTU &&
            event->type != BLE_PORT_EVENT_SUBSCRIBE)
    {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t result = ops->lock(ops->arg);
    if (result != ESP_OK)
    {
        return result;
    }
    ble_nimble_link_update_t update = {0};
    result = ops->prepare(event, &update, ops->arg);
    ops->unlock(ops->arg);
    if (result != ESP_OK)
    {
        return result;
    }
    switch (update.type)
    {
    case BLE_PORT_EVENT_CONNECT:
        return ble_link_gatt_set_connection(
                   update.identity.generation, update.identity.conn_handle,
                   update.peer_addr_type, update.peer_addr);
    case BLE_PORT_EVENT_MTU:
        return ble_link_gatt_set_att_mtu(
                   update.identity.generation, update.identity.conn_handle, update.mtu);
    case BLE_PORT_EVENT_SUBSCRIBE:
        ble_link_service_set_transport_admitted(
            update.identity.generation, update.identity.conn_handle, update.indicate);
        return ESP_OK;
    default:
        return ESP_ERR_NOT_FOUND;
    }
}

void ble_nimble_link_events_adv_consumer(const ble_port_event_t *event, void *arg)
{
    ble_nimble_adv_event_state_t *state = arg;
    if (event == NULL || state == NULL)
    {
        return;
    }
    switch (event->type)
    {
    case BLE_PORT_EVENT_CONNECT:
        if (event->status == 0 && event->accepted &&
                event->identity.generation != 0U &&
                event->identity.kind == BLE_LINK_OPERATION_CONNECT &&
                event->identity.conn_handle == event->conn_handle &&
                event->identity.flow_id == 0U && event->identity.token == 0U)
        {
            state->conn_handle = event->conn_handle;
            state->generation = event->identity.generation;
        }
        break;
    case BLE_PORT_EVENT_DISCONNECT:
        /* GAP has already cleared its snapshot; retain the pre-clear ACL. */
        if (state->generation == 0U ||
                event->identity.generation != state->generation ||
                event->conn_handle != state->conn_handle ||
                event->identity.conn_handle != state->conn_handle ||
                event->identity.kind != BLE_LINK_OPERATION_DISCONNECT ||
                event->identity.flow_id != 0U || event->identity.token != 0U)
        {
            return;
        }
        state->conn_handle = 0U;
        state->generation = 0U;
        break;
    case BLE_PORT_EVENT_RESET:
        state->conn_handle = 0U;
        state->generation = 0U;
        break;
    case BLE_PORT_EVENT_SYNC:
    case BLE_PORT_EVENT_ADV_STARTED:
    case BLE_PORT_EVENT_ADV_STOPPED:
    case BLE_PORT_EVENT_ADV_COMPLETE:
        break;
    default:
        return;
    }
    const esp_err_t result = ble_adv_manager_handle_event(event);
    /* Wake only after the physical result is visible to the owner. */
    ble_link_service_wake_owner();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    {
        LOG_E("adv manager event failed result=%d", result);
    }
}
