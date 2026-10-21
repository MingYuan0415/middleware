#ifndef __FACTORY_RESET_SERVICE_TEST_HOOKS_H__
#define __FACTORY_RESET_SERVICE_TEST_HOOKS_H__

/**
 * @file Private host-test hooks.
 *
 * These declarations are deliberately absent from the public header: they
 * exist only to observe the API admission window from a host test and are
 * compiled in under UNIT_TEST_HOST.
 */

#ifdef UNIT_TEST_HOST

/** @brief Barrier invoked after API admission samples its state. */
typedef void (*factory_reset_service_test_api_acquire_hook_t)(void *arg);

/** @brief Install a test-only API admission barrier. */
void factory_reset_service_test_set_api_acquire_hook(
    factory_reset_service_test_api_acquire_hook_t hook, void *arg);

#endif /* UNIT_TEST_HOST */

#endif /* __FACTORY_RESET_SERVICE_TEST_HOOKS_H__ */
