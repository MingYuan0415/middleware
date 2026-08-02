#ifndef __PROVISIONING_HOST_ESP_MAC_H__
#define __PROVISIONING_HOST_ESP_MAC_H__

#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    ESP_MAC_EFUSE_FACTORY = 0,
} esp_mac_type_t;

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);

#endif /* __PROVISIONING_HOST_ESP_MAC_H__ */
