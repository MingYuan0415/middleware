#ifndef BLE_NIMBLE_PAIRING_GATE_H
#define BLE_NIMBLE_PAIRING_GATE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum ble_nimble_pairing_gate_hold
{
    BLE_NIMBLE_PAIRING_GATE_HOLD_PEER_CLEANUP = 1U << 0,
    BLE_NIMBLE_PAIRING_GATE_HOLD_REJECTED_ACL = 1U << 1,
    BLE_NIMBLE_PAIRING_GATE_HOLD_DRAIN = 1U << 2,
    BLE_NIMBLE_PAIRING_GATE_HOLD_REVOKE = 1U << 3,
} ble_nimble_pairing_gate_hold_t;

typedef struct ble_nimble_pairing_gate_state
{
    atomic_uint bits;
} ble_nimble_pairing_gate_state_t;

void ble_nimble_pairing_gate_reset(
    ble_nimble_pairing_gate_state_t *state);

void ble_nimble_pairing_gate_request(
    ble_nimble_pairing_gate_state_t *state, bool open);

bool ble_nimble_pairing_gate_set_hold(
    ble_nimble_pairing_gate_state_t *state,
    ble_nimble_pairing_gate_hold_t hold, bool active);

bool ble_nimble_pairing_gate_requested_open(
    const ble_nimble_pairing_gate_state_t *state);

bool ble_nimble_pairing_gate_effective_open(
    const ble_nimble_pairing_gate_state_t *state);

#endif /* BLE_NIMBLE_PAIRING_GATE_H */
