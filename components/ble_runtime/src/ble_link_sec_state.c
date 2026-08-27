#include <string.h>

#include "ble_link_sec_state.h"
void ble_link_sec_state_reset(ble_link_sec_state_t *state)
{
    if (state != NULL)
    {
        memset(state, 0, sizeof(*state));
    }
}

/**
 * @brief Run the terminal decision once, only on the transition.
 *
 * @return True when this call newly finalized the decision (the caller
 *         emits the actions); false for a duplicate or incomplete event.
 */
static bool _ble_link_sec_state_finalize_locked(ble_link_sec_state_t *state)
{
    if (state->finalized || !state->encrypted || !state->identity_ready)
    {
        return false;
    }
    if (state->bond_verified && (state->connect_window_open ||
                                 state->had_bond))
    {
        /* The bond matches the contract and the peer is either pairing
         * inside a window or reconnecting with a stored bond: a known
         * peer. The reporting flag is set by the emitter, not here, so
         * the first emission is not skipped. */
        state->finalized = true;
        return true;
    }
    if (state->bonded)
    {
        /* A bond exists but its material is incomplete or non-SC: fail
         * closed and remove the malformed record. */
        state->finalized = true;
        return true;
    }
    if (state->connect_window_open)
    {
        /* Pairing in flight inside the window: hold until the bond is
         * stored and verified; never delete a provisional bond here. */
        return false;
    }
    /* Encrypted without a bond and outside any window: unknown peer. */
    state->finalized = true;
    return true;
}

uint32_t ble_link_sec_state_on_connect(
    ble_link_sec_state_t *state, bool window_open, bool identity_ready,
    bool had_bond, bool bonded, bool bond_verified)
{
    if (state == NULL)
    {
        return BLE_LINK_SEC_ACTION_NONE;
    }
    memset(state, 0, sizeof(*state));
    state->active = true;
    state->connect_window_open = window_open;
    state->identity_ready = identity_ready;
    state->had_bond = had_bond;
    state->bonded = bonded;
    state->bond_verified = bond_verified;
    (void)_ble_link_sec_state_finalize_locked(state);
    return BLE_LINK_SEC_ACTION_NONE;
}

uint32_t ble_link_sec_state_on_identity(
    ble_link_sec_state_t *state, bool bonded, bool bond_verified,
    bool refresh_had_bond, bool had_bond)
{
    uint32_t actions = BLE_LINK_SEC_ACTION_NONE;

    if (state == NULL || !state->active || state->identity_ready)
    {
        return actions;
    }
    state->identity_ready = true;
    if (refresh_had_bond && !state->had_bond)
    {
        state->had_bond = had_bond;
    }
    state->bonded = state->bonded || bonded;
    state->bond_verified = state->bond_verified || bond_verified;
    if (!_ble_link_sec_state_finalize_locked(state))
    {
        return actions;
    }
    if (state->bond_verified && (state->connect_window_open ||
                                 state->had_bond))
    {
        if (!state->link_encrypted_reported)
        {
            actions |= BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED;
            state->link_encrypted_reported = true;
        }
        actions |= BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED;
        actions |= BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN;
    }
    else
    {
        /* Malformed bond or unknown peer. */
        if (state->bonded || !state->connect_window_open)
        {
            actions |= BLE_LINK_SEC_ACTION_DELETE_BOND;
            actions |= BLE_LINK_SEC_ACTION_TERMINATE;
        }
    }
    return actions;
}

uint32_t ble_link_sec_state_on_encrypted(
    ble_link_sec_state_t *state, bool encrypted, bool bonded,
    bool bond_verified)
{
    uint32_t actions = BLE_LINK_SEC_ACTION_NONE;

    if (state == NULL || !state->active)
    {
        return actions;
    }
    if (!encrypted)
    {
        /* Encryption dropped: the session link security facts are cleared
         * by the transport teardown path (not through this reducer) and
         * the connection must re-verify before any report. The identity
         * facts survive (the peer did not change). */
        state->encrypted = false;
        state->bonded = false;
        state->bond_verified = false;
        state->finalized = false;
        state->link_encrypted_reported = false;
        return BLE_LINK_SEC_ACTION_NONE;
    }
    state->encrypted = true;
    state->bonded = bonded;
    state->bond_verified = bond_verified;
    if (!_ble_link_sec_state_finalize_locked(state))
    {
        return actions;
    }
    if (state->bond_verified && (state->connect_window_open ||
                                 state->had_bond))
    {
        if (!state->link_encrypted_reported)
        {
            actions |= BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED;
            state->link_encrypted_reported = true;
        }
        actions |= BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED;
        actions |= BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN;
    }
    else
    {
        /* Malformed bond or unknown peer outside a window. */
        if (state->bonded || !state->connect_window_open)
        {
            actions |= BLE_LINK_SEC_ACTION_DELETE_BOND;
            actions |= BLE_LINK_SEC_ACTION_TERMINATE;
        }
    }
    return actions;
}

uint32_t ble_link_sec_state_reconcile_snapshot(
    ble_link_sec_state_t *state, bool identity_ready, bool encrypted,
    bool bonded, bool bond_verified)
{
    uint32_t actions = BLE_LINK_SEC_ACTION_NONE;

    if (!encrypted)
    {
        return ble_link_sec_state_on_encrypted(
                   state, false, bonded, bond_verified);
    }
    if (identity_ready)
    {
        actions |= ble_link_sec_state_on_identity(
                       state, bonded, bond_verified, false, false);
    }
    actions |= ble_link_sec_state_on_encrypted(
                   state, encrypted, bonded, bond_verified);
    return actions;
}

uint32_t ble_link_sec_state_on_disconnect(ble_link_sec_state_t *state)
{
    if (state != NULL)
    {
        memset(state, 0, sizeof(*state));
    }
    return BLE_LINK_SEC_ACTION_NONE;
}

bool ble_link_sec_state_peer_admitted(const ble_link_sec_state_t *state)
{
    return state != NULL && state->active && state->finalized &&
           state->bond_verified && (state->connect_window_open ||
                                    state->had_bond);
}

bool ble_link_sec_state_provisional_bond_verified(
    const ble_link_sec_state_t *state)
{
    return ble_link_sec_state_peer_admitted(state) && !state->had_bond;
}
