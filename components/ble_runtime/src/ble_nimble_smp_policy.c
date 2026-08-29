#include "ble_nimble_smp_policy.h"

ble_nimble_smp_passkey_decision_t ble_nimble_smp_passkey_decide(
    uint8_t action)
{
    if (action == BLE_NIMBLE_SMP_PASSKEY_ACTION_NUMCMP)
    {
        return BLE_NIMBLE_SMP_PASSKEY_ACCEPT_NUMCMP;
    }
    return BLE_NIMBLE_SMP_PASSKEY_TERMINATE;
}

ble_nimble_smp_repeat_decision_t ble_nimble_smp_repeat_decide(
    bool new_sc, bool new_bonding, bool new_authenticated,
    uint8_t new_key_size, bool bindable, bool durable_bond_present)
{
    if (!new_sc || !new_bonding || !new_authenticated ||
            new_key_size != BLE_NIMBLE_SMP_PAIR_KEY_SIZE_MAX ||
            !bindable || durable_bond_present)
    {
        return BLE_NIMBLE_SMP_REPEAT_IGNORE;
    }
    return BLE_NIMBLE_SMP_REPEAT_RETRY;
}

bool ble_nimble_smp_candidate_cleanup_required(
    bool had_bond, bool pairing_started, bool bond_committed)
{
    return !had_bond && pairing_started && !bond_committed;
}

bool ble_nimble_smp_numeric_comparison_inject_required(
    bool pending, uint16_t conn_handle)
{
    return pending && conn_handle != BLE_NIMBLE_SMP_CONN_HANDLE_NONE;
}

bool ble_nimble_smp_numeric_comparison_reply_committed(int inject_result)
{
    return inject_result == 0;
}

bool ble_nimble_smp_numeric_comparison_restore_pending(
    bool inject_committed,
    uint32_t begin_epoch,
    uint32_t current_epoch,
    uint16_t begin_handle,
    uint16_t current_handle)
{
    return !inject_committed &&
           begin_epoch == current_epoch &&
           begin_handle != BLE_NIMBLE_SMP_CONN_HANDLE_NONE &&
           begin_handle == current_handle;
}

bool ble_nimble_smp_numeric_comparison_clear_committed(
    bool inject_committed,
    uint32_t begin_epoch,
    uint32_t current_epoch,
    uint16_t begin_handle,
    uint16_t current_handle,
    bool current_pending)
{
    return inject_committed &&
           !current_pending &&
           begin_epoch == current_epoch &&
           begin_handle != BLE_NIMBLE_SMP_CONN_HANDLE_NONE &&
           begin_handle == current_handle;
}
