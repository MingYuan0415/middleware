#ifndef __BLE_LINK_SEC_STATE_H__
#define __BLE_LINK_SEC_STATE_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Actions the port must take after a state transition. */
typedef enum
{
    BLE_LINK_SEC_ACTION_NONE = 0,
    /** @brief Report BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED once. */
    BLE_LINK_SEC_ACTION_REPORT_LINK_ENCRYPTED = 1U << 0,
    /** @brief Report BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED once. */
    BLE_LINK_SEC_ACTION_REPORT_BOND_VERIFIED = 1U << 1,
    /** @brief Mark the session identity known (matched peer). */
    BLE_LINK_SEC_ACTION_SET_IDENTITY_KNOWN = 1U << 2,
    /** @brief Delete the peer bond (malformed or orphan). */
    BLE_LINK_SEC_ACTION_DELETE_BOND = 1U << 3,
    /** @brief Terminate the connection (unknown or malformed peer). */
    BLE_LINK_SEC_ACTION_TERMINATE = 1U << 4,
} ble_link_sec_action_t;

/**
 * @brief Per-connection Security 2 admission facts.
 *
 * Pure accumulator: the port feeds GAP events in arrival order; the
 * reducer converges to the same terminal actions regardless of whether
 * IDENTITY_RESOLVED precedes or follows ENC_CHANGE. No peer bond is
 * deleted and no session fact is reported before the peer identity is
 * known, so a legal RPA reconnect is never misclassified as an unknown
 * peer.
 */
typedef struct ble_link_sec_state
{
    bool active;              /**< Connection generation live. */
    bool connect_window_open; /**< Pairing window was open at CONNECT. */
    bool encrypted;           /**< Link encryption established. */
    bool identity_ready;      /**< Identity address resolved/known. */
    bool had_bond;            /**< Store held a bond for the identity. */
    bool bond_verified;       /**< Store bond is SC, 16-byte, keyed by ID. */
    bool bonded;              /**< Connection reports a stored bond. */
    bool finalized;           /**< Terminal decision emitted. */
    bool link_encrypted_reported; /**< LINK_ENCRYPTED emitted once. */
} ble_link_sec_state_t;

/** @brief Reset the state (new connection generation or teardown). */
void ble_link_sec_state_reset(ble_link_sec_state_t *state);

/**
 * @brief Feed a CONNECT event.
 *
 * @param[in] window_open Pairing window state at connect time.
 * @param[in] identity_ready True when the descriptor contains a normalized
 *                           public or static-random identity, false while the
 *                           identity is unresolved or otherwise invalid.
 * @param[in] had_bond Store held a bond for the (possibly unresolved)
 *                     identity; only meaningful when identity_ready.
 * @param[in] bonded Connection reports a stored bond.
 * @param[in] bond_verified Store bond material is valid.
 * @return Action mask (always NONE until later events).
 */
uint32_t ble_link_sec_state_on_connect(
    ble_link_sec_state_t *state, bool window_open, bool identity_ready,
    bool had_bond, bool bonded, bool bond_verified);

/**
 * @brief Feed an identity resolution event (IDENTITY_RESOLVED, or a
 * static identity known after connect).
 *
 * The had_bond fact captured at CONNECT is not overwritten unless
 * @p refresh_had_bond is true. Refresh only when this ACL has not started
 * SMP; otherwise a pairing created by this connection could look like a
 * pre-existing bond and bypass a closed pairing window.
 *
 * @param[in] bonded Connection reports a stored bond.
 * @param[in] bond_verified Store bond material is valid.
 * @param[in] refresh_had_bond True when this ACL has not attempted pairing.
 * @param[in] had_bond Store currently holds a bond for the resolved identity.
 * @return Action mask.
 */
uint32_t ble_link_sec_state_on_identity(
    ble_link_sec_state_t *state, bool bonded, bool bond_verified,
    bool refresh_had_bond, bool had_bond);

/**
 * @brief Feed an encryption change event.
 *
 * @param[in] encrypted True when the link is encrypted.
 * @param[in] bonded Connection reports a stored bond.
 * @param[in] bond_verified Store bond material is valid.
 * @return Action mask.
 */
uint32_t ble_link_sec_state_on_encrypted(
    ble_link_sec_state_t *state, bool encrypted, bool bonded,
    bool bond_verified);

/**
 * @brief Reconcile the latest NimBLE connection descriptor snapshot.
 *
 * Fresh pairing publishes the normalized identity before NimBLE persists the
 * peer and local keys, so the identity callback cannot verify the bond yet.
 * The final ENC_CHANGE descriptor is authoritative after key persistence;
 * reconciling both facts admits that same ACL without a reconnect.
 *
 * @param[in,out] state Security admission accumulator.
 * @param[in] identity_ready Descriptor contains a normalized identity.
 * @param[in] encrypted Link encryption is active.
 * @param[in] bonded Connection reports a stored bond.
 * @param[in] bond_verified Store bond material is valid.
 * @param[in] refresh_had_bond True when this ACL has not attempted pairing.
 * @param[in] had_bond Store currently holds a bond for the resolved identity.
 * @return Combined action mask.
 */
uint32_t ble_link_sec_state_reconcile_snapshot(
    ble_link_sec_state_t *state, bool identity_ready, bool encrypted,
    bool bonded, bool bond_verified, bool refresh_had_bond, bool had_bond);

/** @brief Feed a disconnect or host reset; resets all facts. */
uint32_t ble_link_sec_state_on_disconnect(ble_link_sec_state_t *state);

/**
 * @brief Whether the terminal decision admitted the peer as known.
 */
bool ble_link_sec_state_peer_admitted(const ble_link_sec_state_t *state);

/**
 * @brief Whether this ACL created a newly verified provisional bond.
 *
 * The prior-bond fact is frozen at CONNECT. This predicate becomes true only
 * after the current ACL has produced a verified bond, never merely because an
 * unbonded connection candidate was accepted.
 */
bool ble_link_sec_state_provisional_bond_verified(
    const ble_link_sec_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_SEC_STATE_H__ */
