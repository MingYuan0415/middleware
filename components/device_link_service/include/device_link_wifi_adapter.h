#ifndef __DEVICE_LINK_WIFI_ADAPTER_H__
#define __DEVICE_LINK_WIFI_ADAPTER_H__

#include <stdbool.h>
#include <stddef.h>

#include "device_link_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return the startup-frozen Wi-Fi v1 descriptor.
 *
 * The descriptor is always available to host tests and registry validation,
 * but the Device Link service must not pass it to the runtime router until
 * the product capability gate says the adapter and its recovery tests are
 * complete.
 */
esp_err_t device_link_wifi_adapter_get_descriptor(
    const device_link_domain_descriptor_t **descriptor);

/** @brief Whether the owner has completed its runtime capability gate. */
bool device_link_wifi_adapter_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_WIFI_ADAPTER_H__ */
