#ifndef __PROVISIONING_HOST_PROTOCOMM_BLE_H__
#define __PROVISIONING_HOST_PROTOCOMM_BLE_H__

#include <stdint.h>
#include <sys/types.h>

#include "esp_event.h"
#include "protocomm.h"

#define BLE_UUID128_VAL_LENGTH 16U

ESP_EVENT_DECLARE_BASE(PROTOCOMM_TRANSPORT_BLE_EVENT);

typedef enum
{
    PROTOCOMM_TRANSPORT_BLE_CONNECTED = 0,
    PROTOCOMM_TRANSPORT_BLE_DISCONNECTED,
} protocomm_transport_ble_event_t;

typedef struct protocomm_ble_name_uuid
{
    const char *name;
    uint16_t uuid;
} protocomm_ble_name_uuid_t;

typedef struct protocomm_ble_config
{
    char device_name[30];
    uint8_t service_uuid[BLE_UUID128_VAL_LENGTH];
    uint8_t *manufacturer_data;
    ssize_t manufacturer_data_len;
    ssize_t nu_lookup_count;
    protocomm_ble_name_uuid_t *nu_lookup;
    unsigned ble_bonding : 1;
    unsigned ble_sm_sc : 1;
    unsigned ble_link_encryption : 1;
    uint8_t *ble_addr;
    unsigned keep_ble_on : 1;
    unsigned ble_notify : 1;
} protocomm_ble_config_t;

esp_err_t protocomm_ble_start(
    protocomm_t *protocomm, const protocomm_ble_config_t *config);
esp_err_t protocomm_ble_stop(protocomm_t *protocomm);

#endif /* __PROVISIONING_HOST_PROTOCOMM_BLE_H__ */
