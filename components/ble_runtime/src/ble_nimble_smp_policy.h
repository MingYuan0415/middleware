#ifndef __BLE_NIMBLE_SMP_POLICY_H__
#define __BLE_NIMBLE_SMP_POLICY_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_NIMBLE_SMP_PASSKEY_ACTION_NUMCMP 4U
#define BLE_NIMBLE_SMP_PAIR_KEY_SIZE_MAX 16U

typedef enum ble_nimble_smp_passkey_decision
{
    BLE_NIMBLE_SMP_PASSKEY_ACCEPT_NUMCMP = 0,
    BLE_NIMBLE_SMP_PASSKEY_TERMINATE,
} ble_nimble_smp_passkey_decision_t;

typedef enum ble_nimble_smp_repeat_decision
{
    BLE_NIMBLE_SMP_REPEAT_IGNORE = 0,
    BLE_NIMBLE_SMP_REPEAT_RETRY,
} ble_nimble_smp_repeat_decision_t;

/**
 * @brief Decide whether a GAP passkey action is Numeric Comparison.
 *
 * Just Works and display/input actions are rejected so pairing stays MITM.
 *
 * @param action NimBLE `BLE_SM_IOACT_*` value.
 * @return ACCEPT_NUMCMP only for numeric comparison; otherwise TERMINATE.
 */
ble_nimble_smp_passkey_decision_t ble_nimble_smp_passkey_decide(
    uint8_t action);

/**
 * @brief Decide REPEAT_PAIRING under `local_clear_then_pair`.
 *
 * A verified store bond is a durable binding. Replacement requires a local
 * revoke; leftover incomplete material in an open bindable window may retry.
 *
 * @param new_sc Peer requested Secure Connections.
 * @param new_bonding Peer requested bonding.
 * @param new_authenticated Peer requested MITM.
 * @param new_key_size Proposed key size in bytes.
 * @param bindable Session BINDABLE (window open and not bound).
 * @param durable_bond_present Any verified store bond already exists.
 * @return RETRY only for MITM pairing of leftover material; otherwise IGNORE.
 */
ble_nimble_smp_repeat_decision_t ble_nimble_smp_repeat_decide(
    bool new_sc, bool new_bonding, bool new_authenticated,
    uint8_t new_key_size, bool bindable, bool durable_bond_present);

/**
 * @brief Return whether cancel must inject a Numeric Comparison reject.
 *
 * @param pending Passkey action is still outstanding.
 * @param conn_handle ACL that owns the pending action.
 * @return true when NimBLE must receive `numcmp_accept = 0`.
 */
bool ble_nimble_smp_numeric_comparison_inject_required(
    bool pending, uint16_t conn_handle);

/**
 * @brief Decide whether a Numeric Comparison inject consumed the pending
 * action.
 *
 * @param inject_result NimBLE `ble_sm_inject_io` return value.
 * @return true when the pending flag and connection handle may be cleared.
 */
bool ble_nimble_smp_numeric_comparison_reply_committed(int inject_result);

/**
 * @brief Decide whether a failed Numeric Comparison inject may restore
 * pending state.
 *
 * Restore only when the connection identity is unchanged. A disconnect or
 * a new passkey offer advances the epoch and must not revive a stale
 * pending flag.
 *
 * @param inject_committed True when `ble_sm_inject_io` consumed the action.
 * @param begin_epoch Epoch sampled when the reply cleared pending.
 * @param current_epoch Epoch after inject returned.
 * @param begin_handle Connection handle sampled with `begin_epoch`.
 * @param current_handle Connection handle after inject returned.
 * @return true when pending may be set again for the same action.
 */
bool ble_nimble_smp_numeric_comparison_restore_pending(
    bool inject_committed,
    uint32_t begin_epoch,
    uint32_t current_epoch,
    uint16_t begin_handle,
    uint16_t current_handle);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NIMBLE_SMP_POLICY_H__ */
