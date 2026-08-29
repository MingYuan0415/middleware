#ifndef __BLE_LINK_OPERATION_H__
#define __BLE_LINK_OPERATION_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Immutable asynchronous Device Link operation kinds. */
typedef enum ble_link_operation_kind
{
    BLE_LINK_OPERATION_INVALID = 0,
    BLE_LINK_OPERATION_TX_NOTIFY,
    BLE_LINK_OPERATION_TX_INDICATE,
    BLE_LINK_OPERATION_PROVISIONAL_DISCARD,
    BLE_LINK_OPERATION_PEER_CLEANUP,
    BLE_LINK_OPERATION_TERMINATE,
    BLE_LINK_OPERATION_CONNECT,
    BLE_LINK_OPERATION_DISCONNECT,
    BLE_LINK_OPERATION_RESET,
    BLE_LINK_OPERATION_MTU,
    BLE_LINK_OPERATION_ENCRYPT_CHANGE,
    BLE_LINK_OPERATION_SUBSCRIBE,
} ble_link_operation_kind_t;

/**
 * @brief Immutable identity carried across asynchronous owner boundaries.
 *
 * Fields that do not apply to an operation are zero. `conn_handle` remains
 * meaningful when zero because controller handle zero is valid; callers use
 * UINT16_MAX only for an explicit administrative wildcard.
 */
typedef struct ble_link_operation_identity
{
    uint32_t generation;
    uint32_t security_epoch;
    uint32_t flow_id;
    uint32_t token;
    ble_link_operation_kind_t kind;
    uint16_t conn_handle;
} ble_link_operation_identity_t;

/** @brief Compare every immutable operation identity field. */
static inline bool ble_link_operation_identity_equal(
    const ble_link_operation_identity_t *left,
    const ble_link_operation_identity_t *right)
{
    return left != NULL && right != NULL &&
           left->generation == right->generation &&
           left->security_epoch == right->security_epoch &&
           left->flow_id == right->flow_id &&
           left->token == right->token &&
           left->kind == right->kind &&
           left->conn_handle == right->conn_handle;
}

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_OPERATION_H__ */
