#include "ble_nimble_pairing_gate.h"

#include <stddef.h>

#define BLE_NIMBLE_PAIRING_GATE_REQUESTED_OPEN (1U << 31)
#define BLE_NIMBLE_PAIRING_GATE_HOLD_MASK \
    (BLE_NIMBLE_PAIRING_GATE_HOLD_PEER_CLEANUP | \
     BLE_NIMBLE_PAIRING_GATE_HOLD_REJECTED_ACL | \
     BLE_NIMBLE_PAIRING_GATE_HOLD_DRAIN | \
     BLE_NIMBLE_PAIRING_GATE_HOLD_REVOKE)

void ble_nimble_pairing_gate_reset(
    ble_nimble_pairing_gate_state_t *state)
{
    if (state != NULL)
    {
        atomic_store_explicit(&state->bits, 0U, memory_order_release);
    }
}

void ble_nimble_pairing_gate_request(
    ble_nimble_pairing_gate_state_t *state, bool open)
{
    if (state == NULL)
    {
        return;
    }
    if (open)
    {
        (void)atomic_fetch_or_explicit(
            &state->bits, BLE_NIMBLE_PAIRING_GATE_REQUESTED_OPEN,
            memory_order_acq_rel);
    }
    else
    {
        (void)atomic_fetch_and_explicit(
            &state->bits, ~BLE_NIMBLE_PAIRING_GATE_REQUESTED_OPEN,
            memory_order_acq_rel);
    }
}

bool ble_nimble_pairing_gate_set_hold(
    ble_nimble_pairing_gate_state_t *state,
    ble_nimble_pairing_gate_hold_t hold, bool active)
{
    const unsigned int reason = (unsigned int)hold;

    if (state == NULL || reason == 0U ||
            (reason & BLE_NIMBLE_PAIRING_GATE_HOLD_MASK) != reason ||
            (reason & (reason - 1U)) != 0U)
    {
        return false;
    }
    if (active)
    {
        (void)atomic_fetch_or_explicit(&state->bits, reason,
                                       memory_order_acq_rel);
    }
    else
    {
        (void)atomic_fetch_and_explicit(&state->bits, ~reason,
                                        memory_order_acq_rel);
    }
    return true;
}

bool ble_nimble_pairing_gate_requested_open(
    const ble_nimble_pairing_gate_state_t *state)
{
    if (state == NULL)
    {
        return false;
    }
    return (atomic_load_explicit(&state->bits, memory_order_acquire) &
            BLE_NIMBLE_PAIRING_GATE_REQUESTED_OPEN) != 0U;
}

bool ble_nimble_pairing_gate_effective_open(
    const ble_nimble_pairing_gate_state_t *state)
{
    if (state == NULL)
    {
        return false;
    }
    const unsigned int bits = atomic_load_explicit(
                                  &state->bits, memory_order_acquire);

    return (bits & BLE_NIMBLE_PAIRING_GATE_REQUESTED_OPEN) != 0U &&
           (bits & BLE_NIMBLE_PAIRING_GATE_HOLD_MASK) == 0U;
}
