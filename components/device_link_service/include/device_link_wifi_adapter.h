#ifndef __DEVICE_LINK_WIFI_ADAPTER_H__
#define __DEVICE_LINK_WIFI_ADAPTER_H__

#include <stdbool.h>
#include <stddef.h>

#include "device_link_v1.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Install the product compatibility firmware triple for GET_INFO. */
void device_link_wifi_adapter_set_firmware(uint8_t major, uint8_t minor,
        uint8_t patch);

/** @brief Fill GET_INFO firmware fields from the installed product version. */
void device_link_wifi_adapter_fill_info(device_link_v1_info_t *info, void *arg);

/** @brief Submit one admitted Device Link operation to connectivity_manager. */
device_link_v1_status_t device_link_wifi_adapter_submit(
    device_link_v1_operation_t operation,
    const device_link_v1_credentials_t *credentials,
    uint32_t operation_id, void *arg);

#ifdef UNIT_TEST_HOST
void device_link_wifi_adapter_test_set_descriptor_result(esp_err_t result);
#endif

esp_err_t device_link_wifi_adapter_get_descriptor(const void **descriptor);

esp_err_t device_link_wifi_adapter_bridge_start(void);
void device_link_wifi_adapter_bridge_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_WIFI_ADAPTER_H__ */
