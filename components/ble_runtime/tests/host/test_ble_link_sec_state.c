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
    /* Bound peer reconnect: the controller resolves the identity at
     * CONNECT (stored IRK), so the had_bond snapshot is captured there;
     * ENC_CHANGE alone completes the admission. */
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(&state, false, true, true, true, true) ==
           BLE_LINK_SEC_ACTION_NONE);
    const uint32_t actions =
        ble_link_sec_state_on_encrypted(&state, true, true, true);

    assert((actions & BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_DELETE_BOND) == 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) == 0U);
    assert(ble_link_sec_state_peer_admitted(&state));
}

static void test_unresolved_rpa_outside_window_fails_closed(void)
{
    /* A peer whose identity did NOT resolve at CONNECT, that is not inside
     * a pairing window, and whose ACL has already started SMP cannot prove
     * a prior bond: refreshing had_bond from the store would let this
     * connection's pairing bypass a closed window. Fail closed. */
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(&state, false, false, false, false, false) ==
           BLE_LINK_SEC_ACTION_NONE);
    assert(ble_link_sec_state_on_encrypted(
               &state, true, true, true) == BLE_LINK_SEC_ACTION_NONE);
    /* Identity resolves with a stored bond, but the snapshot held no prior
     * bond and no window is open: this can only be this connection's own
     * fresh pairing, which must not be admitted. */
    const uint32_t actions =
        ble_link_sec_state_on_identity(&state, true, true, false, false);

    assert((actions & BLE_LINK_SEC_ACTION_DELETE_BOND) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) != 0U);
    assert(!ble_link_sec_state_peer_admitted(&state));
}

static void test_identity_before_enc_change_converges(void)
{
    /* IDF announces the normalized identity before persisting OUR/PEER keys.
     * The callback therefore cannot verify the bond yet; final ENC_CHANGE
     * must reconcile the now-durable descriptor without another callback. */
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(&state, true, false, false, false, false) ==
           BLE_LINK_SEC_ACTION_NONE);
    uint32_t actions =
        ble_link_sec_state_on_identity(&state, true, false, false, false);

    assert(actions == BLE_LINK_SEC_ACTION_NONE);
    assert(!state.finalized);
    actions = ble_link_sec_state_reconcile_snapshot(
                  &state, true, true, true, true);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_DELETE_BOND) == 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) == 0U);
    assert(ble_link_sec_state_peer_admitted(&state));
}

static void test_connect_bond_snapshot_is_not_overwritten(void)
{
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(
               &state, false, false, true, true, true) ==
           BLE_LINK_SEC_ACTION_NONE);
    assert(ble_link_sec_state_on_encrypted(
               &state, true, true, true) == BLE_LINK_SEC_ACTION_NONE);
    /* A late identity callback reports a fresh store entry created by the
     * current pairing; the had_bond fact is not re-derived and the CONNECT
     * snapshot still admits the original bonded peer. */
    const uint32_t actions = ble_link_sec_state_on_identity(
                                 &state, false, false, false, false);

    assert((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
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
    actions = ble_link_sec_state_on_identity(&state, true, true, false, false);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) == 0U);
    assert(ble_link_sec_state_peer_admitted(&state));
    assert(ble_link_sec_state_provisional_bond_verified(&state));

    /* A second ENC event (duplicate) must not re-report. */
    const uint32_t again =
        ble_link_sec_state_on_encrypted(&state, true, true, true);

    assert(again == BLE_LINK_SEC_ACTION_NONE);
}

static void test_unpaired_disconnect_does_not_create_provisional_bond(void)
{
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(
               &state, true, false, false, false, false) ==
           BLE_LINK_SEC_ACTION_NONE);
    assert(!ble_link_sec_state_provisional_bond_verified(&state));
    assert(ble_link_sec_state_on_disconnect(&state) ==
           BLE_LINK_SEC_ACTION_NONE);
    assert(!ble_link_sec_state_provisional_bond_verified(&state));
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

static void test_final_encryption_snapshot_recovers_fresh_identity(void)
{
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(
               &state, true, false, false, false, false) ==
           BLE_LINK_SEC_ACTION_NONE);

    /* NimBLE can expose the normalized peer_id_addr only in the descriptor
     * observed at final ENC_CHANGE. No separate identity callback is needed. */
    const uint32_t actions = ble_link_sec_state_reconcile_snapshot(
                                 &state, true, true, true, true);

    assert((actions & BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_DELETE_BOND) == 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) == 0U);
    assert(ble_link_sec_state_peer_admitted(&state));
    assert(ble_link_sec_state_reconcile_snapshot(
               &state, true, true, true, true) ==
           BLE_LINK_SEC_ACTION_NONE);
}

static void test_late_identity_refresh_admits_existing_bond(void)
{
    ble_link_sec_state_t state;

    ble_link_sec_state_reset(&state);
    assert(ble_link_sec_state_on_connect(
               &state, false, false, false, false, false) ==
           BLE_LINK_SEC_ACTION_NONE);
    assert(ble_link_sec_state_on_encrypted(
               &state, true, true, true) == BLE_LINK_SEC_ACTION_NONE);
    const uint32_t actions = ble_link_sec_state_on_identity(
                                 &state, true, true, true, true);

    assert((actions & BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED) != 0U);
    assert((actions & BLE_LINK_SEC_ACTION_DELETE_BOND) == 0U);
    assert((actions & BLE_LINK_SEC_ACTION_TERMINATE) == 0U);
    assert(ble_link_sec_state_peer_admitted(&state));
}

int main(void)
{
    test_reset_clears_state();
    test_enc_change_before_identity_holds();
    test_unresolved_rpa_outside_window_fails_closed();
    test_identity_before_enc_change_converges();
    test_connect_bond_snapshot_is_not_overwritten();
    test_new_peer_in_window_is_admitted();
    test_unpaired_disconnect_does_not_create_provisional_bond();
    test_malformed_bond_deleted_and_terminated();
    test_unknown_peer_outside_window_terminated();
    test_encryption_dropped_resets_decision();
    test_static_identity_known_at_connect();
    test_final_encryption_snapshot_recovers_fresh_identity();
    test_late_identity_refresh_admits_existing_bond();
    puts("ble_link_sec_state: all tests passed");
    return 0;
}
