#ifndef __BLE_RUNTIME_TEST_HOOKS_H__
#define __BLE_RUNTIME_TEST_HOOKS_H__

/**
 * @file Private host-test hooks.
 *
 * These declarations are deliberately absent from the public headers: they
 * only force counter near-exhaustion from a host test.
 */

#include <stdint.h>

#ifdef UNIT_TEST_HOST

    /** @brief Setter for exercising generation exhaustion. */
    void ble_gap_manager_test_set_generation(uint32_t generation);

    /** @brief Setter for exercising admission-token exhaustion. */
    void ble_gap_manager_test_set_admission_token(uint32_t token);

    /** @brief Setter for exercising scheduler-token exhaustion. */
    void ble_tx_scheduler_test_set_token(uint32_t value);

#endif /* UNIT_TEST_HOST */

#endif /* __BLE_RUNTIME_TEST_HOOKS_H__ */
