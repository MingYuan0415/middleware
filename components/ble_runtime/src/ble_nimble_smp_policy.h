#ifndef __BLE_NIMBLE_SMP_POLICY_H__
#define __BLE_NIMBLE_SMP_POLICY_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_NIMBLE_SMP_PASSKEY_ACTION_NUMCMP 4U
#define BLE_NIMBLE_SMP_PAIR_KEY_SIZE_MAX 16U
#define BLE_NIMBLE_SMP_CONN_HANDLE_NONE 0xffffU

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
 * @brief Decide whether a disconnected pairing candidate needs bond cleanup.
 *
 * @return true only for a fresh pairing that started but never reached the
 *         verified durable-bond boundary.
 */
bool ble_nimble_smp_candidate_cleanup_required(
    bool had_bond, bool pairing_started, bool bond_committed);


#ifdef __cplusplus
}
#endif

#endif /* __BLE_NIMBLE_SMP_POLICY_H__ */
