#include <assert.h>

#include "ble_nimble_pairing_gate.h"

#define TEST_ASSERT_TRUE(value) assert(value)
#define TEST_ASSERT_FALSE(value) assert(!(value))

static void test_hold_overrides_queued_open_request(void)
{
    ble_nimble_pairing_gate_state_t state;

    ble_nimble_pairing_gate_reset(&state);
    ble_nimble_pairing_gate_request(&state, true);
    TEST_ASSERT_TRUE(ble_nimble_pairing_gate_requested_open(&state));

    /* A host event queued for the earlier OPEN reads effective state at
     * execution time, so a newer rejected-ACL hold keeps SMP closed. */
    TEST_ASSERT_TRUE(ble_nimble_pairing_gate_set_hold(
                         &state,
                         BLE_NIMBLE_PAIRING_GATE_HOLD_REJECTED_ACL, true));
    TEST_ASSERT_FALSE(ble_nimble_pairing_gate_effective_open(&state));
    TEST_ASSERT_TRUE(ble_nimble_pairing_gate_set_hold(
                         &state,
                         BLE_NIMBLE_PAIRING_GATE_HOLD_REJECTED_ACL, false));
    TEST_ASSERT_TRUE(ble_nimble_pairing_gate_effective_open(&state));
}

static void test_independent_holds_require_last_owner_release(void)
{
    ble_nimble_pairing_gate_state_t state;

    ble_nimble_pairing_gate_reset(&state);
    ble_nimble_pairing_gate_request(&state, true);
    TEST_ASSERT_TRUE(ble_nimble_pairing_gate_set_hold(
                         &state,
                         BLE_NIMBLE_PAIRING_GATE_HOLD_PEER_CLEANUP, true));
    TEST_ASSERT_TRUE(ble_nimble_pairing_gate_set_hold(
                         &state,
                         BLE_NIMBLE_PAIRING_GATE_HOLD_REJECTED_ACL, true));
    TEST_ASSERT_TRUE(ble_nimble_pairing_gate_set_hold(
                         &state,
                         BLE_NIMBLE_PAIRING_GATE_HOLD_PEER_CLEANUP, false));
    TEST_ASSERT_FALSE(ble_nimble_pairing_gate_effective_open(&state));
    TEST_ASSERT_TRUE(ble_nimble_pairing_gate_set_hold(
                         &state,
                         BLE_NIMBLE_PAIRING_GATE_HOLD_REJECTED_ACL, false));
    TEST_ASSERT_TRUE(ble_nimble_pairing_gate_effective_open(&state));

    ble_nimble_pairing_gate_request(&state, false);
    TEST_ASSERT_FALSE(ble_nimble_pairing_gate_effective_open(&state));
    TEST_ASSERT_FALSE(ble_nimble_pairing_gate_set_hold(
                          &state, (ble_nimble_pairing_gate_hold_t)0U, true));
}

int main(void)
{
    test_hold_overrides_queued_open_request();
    test_independent_holds_require_last_owner_release();
    return 0;
}
