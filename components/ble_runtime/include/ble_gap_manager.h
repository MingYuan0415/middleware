#ifndef __BLE_GAP_MANAGER_H__
#define __BLE_GAP_MANAGER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Project-owned GAP event types, translated by the port. */
typedef enum
{
    BLE_GAP_MANAGER_EVENT_CONNECT = 0,
    BLE_GAP_MANAGER_EVENT_DISCONNECT,
    BLE_GAP_MANAGER_EVENT_MTU,
    BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE,
    BLE_GAP_MANAGER_EVENT_SUBSCRIBE,
    BLE_GAP_MANAGER_EVENT_ADV_COMPLETE,
} ble_gap_manager_event_type_t;

/** @brief One translated GAP event. */
typedef struct ble_gap_manager_event
{
    ble_gap_manager_event_type_t type;
    uint16_t conn_handle;
    int status;      /**< Connect status or other operation result. */
    uint16_t mtu;    /**< Negotiated ATT MTU on MTU events. */
    uint8_t reason;  /**< Disconnect reason on disconnect events. */
    uint16_t attr_handle; /**< Characteristic handle on subscribe events. */
    bool subscribed;      /**< Subscribe state on subscribe events. */
    bool encrypted;       /**< Actual link encryption on encrypt events. */
} ble_gap_manager_event_t;

/** @brief Snapshot of the active connection facts. */
typedef struct ble_gap_manager_snapshot
{
    uint16_t conn_handle;  /**< Current connection, or 0 when disconnected. */
    uint32_t generation;   /**< Connection generation, 0 before first link. */
    uint16_t mtu;          /**< Last negotiated ATT MTU, 23 default. */
    bool connected;
    bool encrypted;
    bool subscribed;       /**< Any characteristic subscription active. */
} ble_gap_manager_snapshot_t;

/** @brief Access filter callback asked whether a new ACL may be admitted. */
typedef bool (*ble_gap_manager_admission_cb_t)(void *arg);

/**
 * @brief Initialize the manager; no connection is active.
 */
void ble_gap_manager_init(void);

/**
 * @brief Set the admission callback for new connections.
 *
 * The default policy admits only the first ACL: a second connection attempt
 * while one is active is rejected. The callback, when set, can only tighten
 * admission further; an active ACL is always rejected regardless of the
 * callback result.
 *
 * @param[in] callback Callback, or NULL for the default single-ACL policy.
 * @param[in] arg      Callback argument.
 */
void ble_gap_manager_set_admission_cb(
    ble_gap_manager_admission_cb_t callback, void *arg);

/**
 * @brief Feed one translated GAP event into the manager.
 *
 * The port translates NimBLE events and calls this from its host task.
 * Events for a connection that is not the current one are ignored; the
 * manager relies on the NimBLE host task processing events serially and in
 * order, so no event from a retired connection arrives after its disconnect.
 *
 * When a new connection is rejected (ESP_ERR_NO_MEM), the port must actively
 * terminate that ACL with ble_gap_terminate(); the manager does not record
 * the connection.
 *
 * @param[in] event Translated event.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM when a new
 *         connection is rejected.
 */
esp_err_t ble_gap_manager_handle_event(
    const ble_gap_manager_event_t *event);

/**
 * @brief Query whether a characteristic is currently subscribed.
 *
 * @param[in] conn_handle Connection handle.
 * @param[in] attr_handle Characteristic value handle.
 * @return True when subscribed for the current connection.
 */
bool ble_gap_manager_is_subscribed(
    uint16_t conn_handle, uint16_t attr_handle);

/**
 * @brief Copy the current connection snapshot.
 *
 * Must be called from the same task that feeds events (the host task), or
 * from a task that is otherwise synchronized with it.
 *
 * @param[out] out Snapshot to fill.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t ble_gap_manager_get_snapshot(ble_gap_manager_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_GAP_MANAGER_H__ */
