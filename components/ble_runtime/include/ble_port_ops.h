#ifndef __BLE_PORT_OPS_H__
#define __BLE_PORT_OPS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_operation.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Translated host event types. */
typedef enum
{
    BLE_PORT_EVENT_CONNECT = 0,
    BLE_PORT_EVENT_DISCONNECT,
    BLE_PORT_EVENT_MTU,
    BLE_PORT_EVENT_ENC_CHANGE,
    BLE_PORT_EVENT_SUBSCRIBE,
    BLE_PORT_EVENT_ADV_COMPLETE,
    BLE_PORT_EVENT_NOTIFY_TX,
    BLE_PORT_EVENT_SYNC,
    BLE_PORT_EVENT_RESET,
    BLE_PORT_EVENT_ADV_STARTED,
    BLE_PORT_EVENT_ADV_STOPPED,
} ble_port_event_type_t;

/** @brief Translated TX outcome, independent of NimBLE error values. */
typedef enum
{
    BLE_PORT_TX_SENT = 0,      /**< Notification/indication submitted. */
    BLE_PORT_TX_CONFIRMED,     /**< Indication acknowledgement received. */
    BLE_PORT_TX_TIMEOUT,       /**< Indication confirmation never arrived. */
    BLE_PORT_TX_ERROR,         /**< Other transmission failure. */
} ble_port_tx_result_t;

/** @brief One translated host event, independent of NimBLE types. */
typedef struct ble_port_event
{
    ble_port_event_type_t type;
    ble_link_operation_identity_t identity; /**< Async operation identity. */
    uint16_t conn_handle;
    uint16_t attr_handle;
    int status;      /**< Connect/operation result or raw TX status. */
    uint16_t mtu;    /**< Negotiated ATT MTU on MTU events. */
    uint8_t reason;  /**< Disconnect reason on disconnect events. */
    bool subscribed; /**< Subscribe state on subscribe events (either kind). */
    bool notify;     /**< Notification CCCD bit on subscribe events. */
    bool indicate;   /**< Indication CCCD bit on subscribe events. */
    bool encrypted;  /**< Actual link encryption on encrypt events. */
    bool indication; /**< TX was an indication on NOTIFY_TX events. */
    bool accepted;   /**< CONNECT passed connection admission. */
    bool host_reset_pending; /**< ADV_COMPLETE preceded the host reset callback. */
    ble_port_tx_result_t tx_result; /**< TX outcome on NOTIFY_TX events. */
    uint32_t generation; /**< Command generation on ADV_STARTED/STOPPED. */
} ble_port_event_t;

/**
 * @brief Advertising configuration carried by the port.
 *
 * The port converts this into host advertising fields and the host copies
 * the field pointers shallowly, so the configuration object and all buffers
 * it points to must stay valid until advertising is stopped. Fields and
 * payload must stay inside the legacy advertising limit.
 */
typedef struct ble_port_adv_config
{
    uint16_t interval_ms;      /**< Advertising interval. */
    const uint8_t *short_name; /**< Short local name bytes, optional. */
    size_t short_name_len;
    const uint8_t *service_uuid; /**< 16-byte 128-bit UUID, wire order. */
    const uint8_t *service_data; /**< Payload after the UUID, optional. */
    size_t service_data_len;
    uint32_t generation; /**< Caller-assigned command identity. */
} ble_port_adv_config_t;

/**
 * @brief Transport operations the port exposes.
 *
 * The production implementation lives in the NimBLE port and executes
 * advertising control on its own worker so start/stop never run inside a
 * host callback. Host tests register a fake implementation.
 *
 * All operations must be called serially from a single owner task and must
 * not race with runtime lifecycle transitions: while the port is stopping or
 * deinitializing, advertising operations return ESP_ERR_INVALID_STATE and
 * in-flight requests are drained before the queue is torn down.
 */
typedef struct ble_port_ops
{
    esp_err_t (*adv_start)(const ble_port_adv_config_t *config);
    esp_err_t (*adv_stop)(uint32_t generation);
    esp_err_t (*notify)(uint16_t conn_handle, uint16_t value_handle,
                        const uint8_t *data, size_t len);
    esp_err_t (*indicate)(uint16_t conn_handle, uint16_t value_handle,
                          const uint8_t *data, size_t len);
} ble_port_ops_t;

/**
 * @brief Event consumer callback.
 */
typedef void (*ble_port_event_cb_t)(const ble_port_event_t *event, void *arg);

/**
 * @brief Initialize the router; no consumers are registered.
 */
void ble_event_router_init(void);

/**
 * @brief Register an event consumer.
 *
 * @param[in] callback Callback, required.
 * @param[in] arg      Callback argument.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM when full.
 */
esp_err_t ble_event_router_register(
    ble_port_event_cb_t callback, void *arg);

/**
 * @brief Unregister an event consumer.
 *
 * @param[in] callback Callback to remove.
 * @param[in] arg      Matching argument.
 * @return ESP_OK or ESP_ERR_NOT_FOUND.
 */
esp_err_t ble_event_router_unregister(
    ble_port_event_cb_t callback, void *arg);

/**
 * @brief Deliver one event to every registered consumer in order.
 *
 * Registration order is preserved across unregister (slots are compacted,
 * not swapped). Dispatch snapshots the consumer list before invoking, so a
 * consumer that registers or unregisters during a callback does not disturb
 * the current delivery: newly registered consumers receive the next event,
 * and unregistration takes effect after the current event. All router calls
 * must be made from a single task or under the caller's own synchronization;
 * the production port calls dispatch from its host task.
 *
 * @param[in] event Event to deliver.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t ble_event_router_dispatch(const ble_port_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_PORT_OPS_H__ */
