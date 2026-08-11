#ifndef __BLE_NIMBLE_TX_TRACKER_H__
#define __BLE_NIMBLE_TX_TRACKER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_operation.h"

typedef struct ble_nimble_tx_tracker_entry
{
    ble_link_operation_identity_t identity;
    uint16_t value_handle;
    bool indication;
    bool sent;
    bool retired;
} ble_nimble_tx_tracker_entry_t;

typedef struct ble_nimble_tx_tracker
{
    ble_nimble_tx_tracker_entry_t *entries;
    size_t capacity;
    size_t count;
} ble_nimble_tx_tracker_t;

void ble_nimble_tx_tracker_init(
    ble_nimble_tx_tracker_t *tracker,
    ble_nimble_tx_tracker_entry_t *entries, size_t capacity);

esp_err_t ble_nimble_tx_tracker_retain(
    ble_nimble_tx_tracker_t *tracker,
    const ble_link_operation_identity_t *identity,
    uint16_t value_handle, bool indication);

bool ble_nimble_tx_tracker_remove_identity(
    ble_nimble_tx_tracker_t *tracker,
    const ble_link_operation_identity_t *identity);

bool ble_nimble_tx_tracker_retire_identity(
    ble_nimble_tx_tracker_t *tracker,
    const ble_link_operation_identity_t *identity);

bool ble_nimble_tx_tracker_translate(
    ble_nimble_tx_tracker_t *tracker,
    uint16_t conn_handle, uint16_t value_handle,
    bool indication, bool terminal,
    ble_link_operation_identity_t *identity);

void ble_nimble_tx_tracker_clear(ble_nimble_tx_tracker_t *tracker);

#endif /* __BLE_NIMBLE_TX_TRACKER_H__ */
