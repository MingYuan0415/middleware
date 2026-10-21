#ifndef __DEVICE_LINK_TEST_HOOKS_H__
#define __DEVICE_LINK_TEST_HOOKS_H__

/**
 * @file Private host-test hooks.
 *
 * These declarations are deliberately absent from the public header: they
 * only drive id exhaustion and reboot from a host test.
 */

#include <stdint.h>

#include "device_link_v1_engine.h"

#ifdef UNIT_TEST_HOST

/** @brief Start an operation with an explicit request id. */
device_link_v1_status_t device_link_v1_engine_start_with_id(
    device_link_v1_engine_t *engine, device_link_v1_operation_t operation,
    uint8_t request_id, const device_link_v1_credentials_t *credentials,
    uint32_t operation_id);

/** @brief Advance the engine's request-id counter to exhaustion. */
void device_link_v1_engine_test_exhaust_ids(device_link_v1_engine_t *engine);

/** @brief Drop volatile engine state as a reboot would. */
void device_link_v1_engine_test_reboot(device_link_v1_engine_t *engine);

#endif /* UNIT_TEST_HOST */

#endif /* __DEVICE_LINK_TEST_HOOKS_H__ */
