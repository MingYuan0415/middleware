#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_operation.h"

#include "ble_nimble_tx_tracker.h"

static void _ble_nimble_tx_tracker_remove(
    ble_nimble_tx_tracker_t *tracker, size_t index)
{
    if (tracker == NULL || index >= tracker->count)
    {
        return;
    }
    memmove(&tracker->entries[index], &tracker->entries[index + 1U],
            (tracker->count - index - 1U) * sizeof(tracker->entries[0]));
    tracker->count--;
    memset(&tracker->entries[tracker->count], 0,
           sizeof(tracker->entries[0]));
}

static bool _ble_nimble_tx_tracker_same_tuple(
    const ble_nimble_tx_tracker_entry_t *entry,
    uint16_t conn_handle, uint16_t value_handle, bool indication)
{
    return entry->identity.conn_handle == conn_handle &&
           entry->value_handle == value_handle &&
           entry->indication == indication;
}

void ble_nimble_tx_tracker_init(
    ble_nimble_tx_tracker_t *tracker,
    ble_nimble_tx_tracker_entry_t *entries, size_t capacity)
{
    if (tracker == NULL)
    {
        return;
    }
    tracker->entries = entries;
    tracker->capacity = entries != NULL ? capacity : 0U;
    tracker->count = 0U;
    if (entries != NULL)
    {
        memset(entries, 0, capacity * sizeof(entries[0]));
    }
}

esp_err_t ble_nimble_tx_tracker_retain(
    ble_nimble_tx_tracker_t *tracker,
    const ble_link_operation_identity_t *identity,
    uint16_t value_handle, bool indication)
{
    if (tracker == NULL || identity == NULL || identity->generation == 0U ||
            identity->token == 0U ||
            identity->kind != (indication ?
                               BLE_LINK_OPERATION_TX_INDICATE :
                               BLE_LINK_OPERATION_TX_NOTIFY))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (indication)
    {
        for (size_t i = 0U; i < tracker->count; ++i)
        {
            if (_ble_nimble_tx_tracker_same_tuple(
                        &tracker->entries[i], identity->conn_handle,
                        value_handle, true))
            {
                /* NimBLE's terminal callback has no operation token. Keep
                 * one unambiguous indication per raw callback tuple; a
                 * retired timeout tombstone must be consumed first. */
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    if (tracker->entries == NULL || tracker->count >= tracker->capacity)
    {
        return ESP_ERR_NO_MEM;
    }
    ble_nimble_tx_tracker_entry_t *entry =
        &tracker->entries[tracker->count++];

    memset(entry, 0, sizeof(*entry));
    entry->identity = *identity;
    entry->value_handle = value_handle;
    entry->indication = indication;
    return ESP_OK;
}

bool ble_nimble_tx_tracker_remove_identity(
    ble_nimble_tx_tracker_t *tracker,
    const ble_link_operation_identity_t *identity)
{
    if (tracker == NULL || identity == NULL)
    {
        return false;
    }
    for (size_t i = 0U; i < tracker->count; ++i)
    {
        if (ble_link_operation_identity_equal(
                    &tracker->entries[i].identity, identity))
        {
            _ble_nimble_tx_tracker_remove(tracker, i);
            return true;
        }
    }
    return false;
}

bool ble_nimble_tx_tracker_retire_identity(
    ble_nimble_tx_tracker_t *tracker,
    const ble_link_operation_identity_t *identity)
{
    if (tracker == NULL || identity == NULL)
    {
        return false;
    }
    for (size_t i = 0U; i < tracker->count; ++i)
    {
        if (ble_link_operation_identity_equal(
                    &tracker->entries[i].identity, identity))
        {
            /* A missing SENT callback must not let a later SENT callback
             * attach to this retired operation. */
            tracker->entries[i].sent = true;
            tracker->entries[i].retired = true;
            return true;
        }
    }
    return false;
}

bool ble_nimble_tx_tracker_translate(
    ble_nimble_tx_tracker_t *tracker,
    uint16_t conn_handle, uint16_t value_handle,
    bool indication, bool terminal,
    ble_link_operation_identity_t *identity)
{
    if (tracker == NULL || identity == NULL)
    {
        return false;
    }
    size_t match = SIZE_MAX;

    for (size_t i = 0U; i < tracker->count; ++i)
    {
        ble_nimble_tx_tracker_entry_t *entry = &tracker->entries[i];

        if (!_ble_nimble_tx_tracker_same_tuple(
                    entry, conn_handle, value_handle, indication))
        {
            continue;
        }
        if (!indication || terminal || (!entry->sent && !entry->retired))
        {
            match = i;
            break;
        }
    }
    if (match == SIZE_MAX)
    {
        return false;
    }
    *identity = tracker->entries[match].identity;
    if (indication && !terminal)
    {
        tracker->entries[match].sent = true;
    }
    else
    {
        _ble_nimble_tx_tracker_remove(tracker, match);
    }
    return true;
}

void ble_nimble_tx_tracker_clear(ble_nimble_tx_tracker_t *tracker)
{
    if (tracker == NULL)
    {
        return;
    }
    if (tracker->entries != NULL)
    {
        memset(tracker->entries, 0,
               tracker->capacity * sizeof(tracker->entries[0]));
    }
    tracker->count = 0U;
}
