#include "ble_service.h"

EVENT_BUS_DEFINE_ID(BLE_SERVICE_MSG);

esp_err_t ble_service_init(void)
{
    return ESP_OK;
}

esp_err_t ble_service_deinit(void)
{
    return ESP_OK;
}

esp_err_t ble_service_enable(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_service_disable(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_service_scan(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

ble_service_state_t ble_service_get_state(void)
{
    return BLE_SERVICE_STATE_DISABLED;
}
