#include <assert.h>

#include "ble_nimble_smp_policy.h"

#define TEST_ASSERT_TRUE(value) assert(value)
#define TEST_ASSERT_FALSE(value) assert(!(value))

static void test_passkey_accepts_only_numeric_comparison(void)
{
    TEST_ASSERT_TRUE(ble_nimble_smp_passkey_decide(
                         BLE_NIMBLE_SMP_PASSKEY_ACTION_NUMCMP) ==
                     BLE_NIMBLE_SMP_PASSKEY_ACCEPT_NUMCMP);
    TEST_ASSERT_TRUE(ble_nimble_smp_passkey_decide(0U) ==
                     BLE_NIMBLE_SMP_PASSKEY_TERMINATE);
    TEST_ASSERT_TRUE(ble_nimble_smp_passkey_decide(1U) ==
                     BLE_NIMBLE_SMP_PASSKEY_TERMINATE);
    TEST_ASSERT_TRUE(ble_nimble_smp_passkey_decide(2U) ==
                     BLE_NIMBLE_SMP_PASSKEY_TERMINATE);
    TEST_ASSERT_TRUE(ble_nimble_smp_passkey_decide(3U) ==
                     BLE_NIMBLE_SMP_PASSKEY_TERMINATE);
    TEST_ASSERT_TRUE(ble_nimble_smp_passkey_decide(5U) ==
                     BLE_NIMBLE_SMP_PASSKEY_TERMINATE);
}

static void test_repeat_retries_only_leftover_mitm_in_window(void)
{
    TEST_ASSERT_TRUE(ble_nimble_smp_repeat_decide(
                         true, true, true, BLE_NIMBLE_SMP_PAIR_KEY_SIZE_MAX,
                         true, false) == BLE_NIMBLE_SMP_REPEAT_RETRY);
    TEST_ASSERT_TRUE(ble_nimble_smp_repeat_decide(
                         false, true, true, BLE_NIMBLE_SMP_PAIR_KEY_SIZE_MAX,
                         true, false) == BLE_NIMBLE_SMP_REPEAT_IGNORE);
    TEST_ASSERT_TRUE(ble_nimble_smp_repeat_decide(
                         true, false, true, BLE_NIMBLE_SMP_PAIR_KEY_SIZE_MAX,
                         true, false) == BLE_NIMBLE_SMP_REPEAT_IGNORE);
    TEST_ASSERT_TRUE(ble_nimble_smp_repeat_decide(
                         true, true, false, BLE_NIMBLE_SMP_PAIR_KEY_SIZE_MAX,
                         true, false) == BLE_NIMBLE_SMP_REPEAT_IGNORE);
    TEST_ASSERT_TRUE(ble_nimble_smp_repeat_decide(
                         true, true, true, 7U, true, false) ==
                     BLE_NIMBLE_SMP_REPEAT_IGNORE);
    TEST_ASSERT_TRUE(ble_nimble_smp_repeat_decide(
                         true, true, true, BLE_NIMBLE_SMP_PAIR_KEY_SIZE_MAX,
                         false, false) == BLE_NIMBLE_SMP_REPEAT_IGNORE);
}

static void test_repeat_ignores_verified_store_bond(void)
{
    TEST_ASSERT_TRUE(ble_nimble_smp_repeat_decide(
                         true, true, true, BLE_NIMBLE_SMP_PAIR_KEY_SIZE_MAX,
                         true, true) == BLE_NIMBLE_SMP_REPEAT_IGNORE);
}

static void test_cancel_injects_reject_when_pending(void)
{
    TEST_ASSERT_TRUE(ble_nimble_smp_numeric_comparison_inject_required(
                         true, 1U));
    TEST_ASSERT_FALSE(ble_nimble_smp_numeric_comparison_inject_required(
                          false, 1U));
    TEST_ASSERT_FALSE(ble_nimble_smp_numeric_comparison_inject_required(
                          true, 0U));
}

int main(void)
{
    test_passkey_accepts_only_numeric_comparison();
    test_repeat_retries_only_leftover_mitm_in_window();
    test_repeat_ignores_verified_store_bond();
    test_cancel_injects_reject_when_pending();
    return 0;
}
