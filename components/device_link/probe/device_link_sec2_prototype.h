#ifndef __DEVICE_LINK_SEC2_PROTOTYPE_H__
#define __DEVICE_LINK_SEC2_PROTOTYPE_H__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Security 2 prototype configuration.
 *
 * The prototype owns NimBLE startup, the static session GATT service, and
 * one serialized Security 2 transaction per connection. It must not run
 * together with another NimBLE owner such as the provisioning service.
 */
typedef struct device_link_sec2_prototype_config
{
    const char *salt;          /**< SRP salt, never logged. */
    size_t salt_len;           /**< Salt length. */
    const char *verifier;      /**< SRP verifier, never logged. */
    size_t verifier_len;       /**< Verifier length. */
    const char *device_name;   /**< Advertised device name. */
    uint32_t task_stack_bytes; /**< Prototype worker stack size. */
    uint32_t task_priority;    /**< Prototype worker priority. */
} device_link_sec2_prototype_config_t;

/**
 * @brief Start the Security 2 prototype transport.
 *
 * Registers the static session service, starts NimBLE, and begins
 * connectable advertising. Requests are reassembled from session_rx
 * fragments and dispatched to Protocomm Security 2 one at a time; responses
 * are sent fragment by fragment on session_tx using indications.
 *
 * @param[in] config Prototype configuration, kept for the lifetime.
 * @return ESP_OK or an ESP-IDF error code.
 */
esp_err_t device_link_sec2_prototype_start(
    const device_link_sec2_prototype_config_t *config);

/**
 * @brief Stop the prototype transport and tear down NimBLE.
 */
void device_link_sec2_prototype_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_SEC2_PROTOTYPE_H__ */
