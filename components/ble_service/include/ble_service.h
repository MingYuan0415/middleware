#ifndef __BLE_SERVICE_H__
#define __BLE_SERVICE_H__

#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

EVENT_BUS_DECLARE_ID(BLE_SERVICE_MSG);

/** @brief BLE service lifecycle states. */
typedef enum
{
    BLE_SERVICE_STATE_DISABLED = 0,
    BLE_SERVICE_STATE_ENABLING,
    BLE_SERVICE_STATE_ENABLED,
    BLE_SERVICE_STATE_ERROR,
} ble_service_state_t;

/** @brief BLE service event-bus message subtypes. */
typedef enum
{
    BLE_SERVICE_MSG_SUB_TYPE_STATE_CHANGED = 1,
    BLE_SERVICE_MSG_SUB_TYPE_SCAN_RESULT,
} ble_service_msg_sub_type_t;

/**
 * @brief Initialize the current BLE service implementation.
 *
 * @return ESP_OK when the service stub is ready.
 */
esp_err_t ble_service_init(void);

/**
 * @brief Release resources owned by the current BLE service implementation.
 *
 * @return ESP_OK when the service is stopped.
 */
esp_err_t ble_service_deinit(void);

/**
 * @brief Enable BLE operation.
 *
 * @return ESP_ERR_NOT_SUPPORTED until a BLE backend is implemented.
 */
esp_err_t ble_service_enable(void);

/**
 * @brief Disable BLE operation.
 *
 * @return ESP_ERR_NOT_SUPPORTED until a BLE backend is implemented.
 */
esp_err_t ble_service_disable(void);

/**
 * @brief Start BLE discovery.
 *
 * @return ESP_ERR_NOT_SUPPORTED until a BLE backend is implemented.
 */
esp_err_t ble_service_scan(void);

/**
 * @brief Return the current BLE service state.
 *
 * @return BLE_SERVICE_STATE_DISABLED while the stub backend is active.
 */
ble_service_state_t ble_service_get_state(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __BLE_SERVICE_H__ */
