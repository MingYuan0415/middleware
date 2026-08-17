#ifndef __DEVICE_LINK_WIFI_ADAPTER_H__
#define __DEVICE_LINK_WIFI_ADAPTER_H__

#include <stdbool.h>
#include <stddef.h>

#include "connectivity_manager.h"
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

#ifdef UNIT_TEST_HOST
/** @brief Override descriptor lookup for fail-closed startup tests. */
void device_link_wifi_adapter_test_set_descriptor_result(esp_err_t result);
#endif

/**
 * @brief Start the asynchronous operation completion bridge.
 *
 * Subscribes the adapter to connectivity manager terminal snapshots and
 * forwards them into the Core v2 operation table through the link service.
 * Idempotent for the boot; the service calls this during init, after the
 * event bus exists, and calls stop during teardown.
 */
esp_err_t device_link_wifi_adapter_bridge_start(void);

/** @brief Stop the completion bridge (idempotent). */
void device_link_wifi_adapter_bridge_stop(void);

/**
 * @brief Encode a wifi.v1.WifiStatus operation result payload.
 *
 * Used by the Device Link completion bridge to attach the terminal status
 * snapshot to a SUCCEEDED operation record.
 *
 * @param[in]  status   Connectivity status snapshot.
 * @param[out] response Payload buffer.
 * @param[in]  capacity Buffer capacity.
 * @param[out] response_len Encoded length.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG for a malformed snapshot.
 */
esp_err_t device_link_wifi_adapter_encode_operation_result(
    const connectivity_manager_status_snapshot_t *status,
    uint8_t *response, size_t capacity, size_t *response_len);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_WIFI_ADAPTER_H__ */
