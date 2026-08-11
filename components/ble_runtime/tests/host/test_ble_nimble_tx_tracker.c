#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_operation.h"
#include "ble_nimble_tx_tracker.h"

static ble_link_operation_identity_t _identity(uint32_t token)
{
    return (ble_link_operation_identity_t)
    {
        .generation = 7U,
        .security_epoch = 3U,
        .flow_id = token + 10U,
        .token = token,
        .kind = BLE_LINK_OPERATION_TX_INDICATE,
        .conn_handle = 0U,
    };
}

static void _test_timeout_tombstone_blocks_tuple_reuse(void)
{
    ble_nimble_tx_tracker_entry_t entries[3];
    ble_nimble_tx_tracker_t tracker;
    const ble_link_operation_identity_t old_identity = _identity(1U);
    const ble_link_operation_identity_t new_identity = _identity(2U);
    ble_link_operation_identity_t translated;

    ble_nimble_tx_tracker_init(&tracker, entries, 3U);
    assert(ble_nimble_tx_tracker_retain(
               &tracker, &old_identity, 0x1234U, true) == ESP_OK);
    memset(&translated, 0, sizeof(translated));
    assert(ble_nimble_tx_tracker_translate(
               &tracker, 0U, 0x1234U, true, false, &translated));
    assert(ble_link_operation_identity_equal(
               &translated, &old_identity));

    assert(ble_nimble_tx_tracker_retire_identity(
               &tracker, &old_identity));
    assert(ble_nimble_tx_tracker_retain(
               &tracker, &new_identity, 0x1234U, true) ==
           ESP_ERR_INVALID_STATE);

    /* The late terminal callback consumes the old identity. A scheduler
     * carrying the new full identity will therefore reject it as stale. */
    memset(&translated, 0, sizeof(translated));
    assert(ble_nimble_tx_tracker_translate(
               &tracker, 0U, 0x1234U, true, true, &translated));
    assert(ble_link_operation_identity_equal(
               &translated, &old_identity));

    assert(ble_nimble_tx_tracker_retain(
               &tracker, &new_identity, 0x1234U, true) == ESP_OK);
    assert(ble_nimble_tx_tracker_translate(
               &tracker, 0U, 0x1234U, true, false, &translated));
    assert(ble_link_operation_identity_equal(
               &translated, &new_identity));
    assert(ble_nimble_tx_tracker_translate(
               &tracker, 0U, 0x1234U, true, true, &translated));
    assert(ble_link_operation_identity_equal(
               &translated, &new_identity));
    assert(!ble_nimble_tx_tracker_translate(
               &tracker, 0U, 0x1234U, true, true, &translated));
}

static void _test_disconnect_clear_releases_tombstone(void)
{
    ble_nimble_tx_tracker_entry_t entries[1];
    ble_nimble_tx_tracker_t tracker;
    const ble_link_operation_identity_t old_identity = _identity(3U);
    const ble_link_operation_identity_t new_identity = _identity(4U);

    ble_nimble_tx_tracker_init(&tracker, entries, 1U);
    assert(ble_nimble_tx_tracker_retain(
               &tracker, &old_identity, 0x4321U, true) == ESP_OK);
    assert(ble_nimble_tx_tracker_retire_identity(
               &tracker, &old_identity));
    ble_nimble_tx_tracker_clear(&tracker);
    assert(ble_nimble_tx_tracker_retain(
               &tracker, &new_identity, 0x4321U, true) == ESP_OK);
}

int main(void)
{
    _test_timeout_tombstone_blocks_tuple_reuse();
    _test_disconnect_clear_releases_tombstone();
    return 0;
}
