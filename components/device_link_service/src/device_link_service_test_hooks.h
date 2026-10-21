#ifndef __DEVICE_LINK_SERVICE_TEST_HOOKS_H__
#define __DEVICE_LINK_SERVICE_TEST_HOOKS_H__

/**
 * @file Private host-test hooks.
 *
 * These declarations are deliberately absent from the public header: they
 * only observe service admission from a host test.
 */

#include "esp_err.h"

#ifdef UNIT_TEST_HOST

/** @brief Barrier invoked after API admission samples its state. */
typedef void (*device_link_service_test_api_acquire_hook_t)(void *arg);

/** @brief Install a test-only API admission barrier. */
void device_link_service_test_set_api_acquire_hook(
    device_link_service_test_api_acquire_hook_t hook, void *arg);

/** @brief Force the firmware-descriptor read result. */
void device_link_wifi_adapter_test_set_descriptor_result(esp_err_t result);

#endif /* UNIT_TEST_HOST */

#endif /* __DEVICE_LINK_SERVICE_TEST_HOOKS_H__ */
