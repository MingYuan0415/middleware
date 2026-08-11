#ifndef __FACTORY_RESET_SERVICE_H__
#define __FACTORY_RESET_SERVICE_H__

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Restart callback invoked after the reset journal is durable.
 *
 * The callback must not re-enter factory_reset_service APIs. Production
 * callbacks normally do not return.
 */
typedef void (*factory_reset_service_restart_fn)(void *context);

/** @brief Factory-reset journal service configuration. */
typedef struct factory_reset_service_config
{
    factory_reset_service_restart_fn restart; /**< Required restart action. */
    void *restart_context; /**< Opaque restart callback context. */
} factory_reset_service_config_t;

/**
 * @brief Initialize the factory-reset journal service.
 * @param config is copied for the service lifetime.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE.
 */
esp_err_t factory_reset_service_init(
    const factory_reset_service_config_t *config);

/**
 * @brief Deinitialize the journal service.
 * @return ESP_OK when stopped, otherwise ESP_ERR_INVALID_STATE.
 */
esp_err_t factory_reset_service_deinit(void);

/**
 * @brief Persist factory-reset intent and restart the device.
 *
 * No reset-domain mutation happens before the journal write commits. The
 * callback normally does not return in production; host fakes may return.
 * Journal operations are serialized; a concurrent operation is rejected.
 *
 * @return ESP_OK when the journal committed and restart was invoked;
 *         otherwise a lifecycle or storage error.
 */
esp_err_t factory_reset_service_request(void);

/**
 * @brief Read and validate the durable factory-reset intent.
 * @param pending receives true only for a valid journal marker.
 * @return ESP_OK, or a lifecycle/storage/validation error that must fail
 *         startup closed.
 */
esp_err_t factory_reset_service_recovery_pending(bool *pending);

/**
 * @brief Clear the journal after every reset domain converged.
 * @return ESP_OK when absent or erased; otherwise a lifecycle or storage error.
 */
esp_err_t factory_reset_service_complete_recovery(void);

#ifdef UNIT_TEST_HOST
/** @brief Test-only barrier invoked after API admission samples its state. */
typedef void (*factory_reset_service_test_api_acquire_hook_t)(void *arg);

/** @brief Install a test-only API admission barrier. */
void factory_reset_service_test_set_api_acquire_hook(
    factory_reset_service_test_api_acquire_hook_t hook, void *arg);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __FACTORY_RESET_SERVICE_H__ */
