#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "ble_link_sec_state.h"

static void test_reset_clears_state(void)
{
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(!state.active);
    assert(!state.encrypted);
    assert(!state.identity_ready);
    assert(!state.finalized);
    assert(ble_link_sec_state_on_disconnect(&state) ==
           BLE_LINK_SEC_ACTION_NONE);
}

static void test_enc_change_before_identity_holds(void)
{
    /* Bound peer reconnect with RPA: ENC_CHANGE arrives while the
     * identity is still the unresolved RPA. Nothing may be deleted or
     * terminated. */
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(&state, false, false, false, false, false) ==
           BLE_LINK_SEC_ACTION_NONE);
    /* ENC with unresolved identity: store lookups fail, but the reducer
     * must hold, not classify as orphan. */
    const uint32_t actions =
        ble_link_sec_state_on_encrypted(&state, true, true, false);

    assert(actions == BLE_LINK_SEC_ACTION_NONE);
    assert(!state.finalized);
    assert((actions & BLE_LINK_SEC_ACTION_DELETE_BOND) == 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) == 0U);

    /* Identity resolves to the stored bond: the peer is admitted. */
    const uint32_t resolved =
        ble_link_sec_state_on_identity(&state, true, true, true);

    assert((resolved & BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED) != 0U);
    assert((resolved & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert((resolved & BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN) != 0U);
    assert((resolved & BLE_LINK_SEC_ACTION_DELETE_BOND) == 0U);
    assert((resolved & BLE_LINK_SEC_ACTION_TERMINATE) == 0U);
    assert(ble_link_sec_state_peer_admitted(&state));
}

static void test_identity_before_enc_change_converges(void)
{
    /* The same peer with the events in the opposite order converges to
     * the same admission. */
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(&state, false, false, false, false, false) ==
           BLE_LINK_SEC_ACTION_NONE);
    uint32_t actions =
        ble_link_sec_state_on_identity(&state, true, false, false);

    assert(actions == BLE_LINK_SEC_ACTION_NONE);
    assert(!state.finalized);
    actions = ble_link_sec_state_on_encrypted(&state, true, true, true);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_DELETE_BOND) == 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) == 0U);
    assert(ble_link_sec_state_peer_admitted(&state));
}

static void test_new_peer_in_window_is_admitted(void)
{
    /* A brand-new RPA peer pairing inside the open window is provisional:
     * the ENC event holds until the identity resolves, then the peer is
     * admitted. */
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(&state, true, false, false, false, false) ==
           BLE_LINK_SEC_ACTION_NONE);
    uint32_t actions =
        ble_link_sec_state_on_encrypted(&state, true, true, true);

    assert(actions == BLE_LINK_SEC_ACTION_NONE);
    assert(!state.finalized);
    actions = ble_link_sec_state_on_identity(&state, true, true, true);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) == 0U);
    assert(ble_link_sec_state_peer_admitted(&state));

    /* A second ENC event (duplicate) must not re-report. */
    const uint32_t again =
        ble_link_sec_state_on_encrypted(&state, true, true, true);

    assert(again == BLE_LINK_SEC_ACTION_NONE);
}

static void test_malformed_bond_deleted_and_terminated(void)
{
    /* A bond with incomplete or non-SC material is deleted and the
     * connection terminated, even inside a window. */
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(&state, true, true, false, false, false) ==
           BLE_LINK_SEC_ACTION_NONE);
    const uint32_t actions =
        ble_link_sec_state_on_encrypted(&state, true, true, false);

    assert((actions & BLE_LINK_SEC_ACTION_DELETE_BOND) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) == 0U);
    assert(!ble_link_sec_state_peer_admitted(&state));
}

static void test_unknown_peer_outside_window_terminated(void)
{
    /* An encrypted, bonded-less connection outside any window with no
     * prior bond is an unknown peer: terminate, do not admit. */
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(&state, false, true, true, true, true) ==
           BLE_LINK_SEC_ACTION_NONE);
    const uint32_t actions =
        ble_link_sec_state_on_encrypted(&state, true, false, false);

    assert((actions & BLE_LINK_SEC_ACTION_DELETE_BOND) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN) == 0U);
    assert(!ble_link_sec_state_peer_admitted(&state));
}

static void test_encryption_dropped_resets_decision(void)
{
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(&state, true, true, false, false, false) ==
           BLE_LINK_SEC_ACTION_NONE);
    const uint32_t up = ble_link_sec_state_on_encrypted(
                            &state, true, true, true);

    assert((up & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert(ble_link_sec_state_peer_admitted(&state));
    const uint32_t down = ble_link_sec_state_on_encrypted(
                              &state, false, false, false);

    assert(down == BLE_LINK_SEC_ACTION_NONE);
    assert(!ble_link_sec_state_peer_admitted(&state));
    /* Re-encryption re-admits. */
    const uint32_t reup = ble_link_sec_state_on_encrypted(
                              &state, true, true, true);

    assert((reup & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert(ble_link_sec_state_peer_admitted(&state));
}

static void test_static_identity_known_at_connect(void)
{
    /* A static/public address carries its identity at connect: admission
     * completes at ENC_CHANGE without IDENTITY_RESOLVED. */
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(&state, false, true, true, true, true) ==
           BLE_LINK_SEC_ACTION_NONE);
    const uint32_t actions =
        ble_link_sec_state_on_encrypted(&state, true, true, true);

    assert((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert(ble_link_sec_state_peer_admitted(&state));
}

int main(void)
{
    test_reset_clears_state();
    test_enc_change_before_identity_holds();
    test_identity_before_enc_change_converges();
    test_new_peer_in_window_is_admitted();
    test_malformed_bond_deleted_and_terminated();
    test_unknown_peer_outside_window_terminated();
    test_encryption_dropped_resets_decision();
    test_static_identity_known_at_connect();
    puts("ble_link_sec_state: all tests passed");
    return 0;
}
