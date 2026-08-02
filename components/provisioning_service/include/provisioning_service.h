#ifndef __PROVISIONING_SERVICE_H__
#define __PROVISIONING_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROVISIONING_SERVICE_QR_MAX_BYTES 256U
#define PROVISIONING_SERVICE_DEVICE_NAME_MAX_BYTES 10U
#define PROVISIONING_SERVICE_WAIT_FOREVER UINT32_MAX

EVENT_BUS_DECLARE_ID(PROVISIONING_SERVICE_MSG);

/** @brief Product-owned manual provisioning policy. */
typedef struct provisioning_service_config
{
    uint32_t task_priority; /**< Provisioning worker priority. */
    uint32_t window_ms; /**< BLE admission window duration. */
    uint32_t success_grace_ms; /**< Delay before closing after saved success. */
    uint32_t finish_close_delay_ms; /**< Delay allowing FinishSession response. */
} provisioning_service_config_t;

/** @brief Provisioning transport lifecycle visible to applications. */
typedef enum
{
    PROVISIONING_SERVICE_STATE_DISABLED = 0,
    PROVISIONING_SERVICE_STATE_IDLE,
    PROVISIONING_SERVICE_STATE_OPENING,
    PROVISIONING_SERVICE_STATE_ADVERTISING,
    PROVISIONING_SERVICE_STATE_CONNECTED,
    PROVISIONING_SERVICE_STATE_CLOSING,
    PROVISIONING_SERVICE_STATE_SUSPENDED,
    PROVISIONING_SERVICE_STATE_ERROR,
} provisioning_service_state_t;

/** @brief Event-bus message subtypes published by provisioning. */
typedef enum
{
    PROVISIONING_SERVICE_MSG_SUB_TYPE_STATUS = 1,
} provisioning_service_msg_sub_type_t;

#define PROVISIONING_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT \
    PROVISIONING_SERVICE_MSG_SUB_TYPE_STATUS

/** @brief Password-free provisioning status snapshot. */
typedef struct provisioning_service_status
{
    uint64_t generation; /**< Monotonic status generation for this boot. */
    provisioning_service_state_t state; /**< Current transport state. */
    int32_t last_error; /**< Local diagnostic error, never sent over BLE. */
    uint32_t window_remaining_ms; /**< Remaining provisioning window time. */
    uint64_t wifi_operation_id; /**< Current phone-owned operation or zero. */
    bool available; /**< Service lifecycle is initialized. */
    bool active; /**< A provisioning window owns BLE resources. */
    bool client_connected; /**< One BLE transport client is connected. */
    bool wifi_operation_active; /**< A phone-owned Wi-Fi operation is active. */
    bool qr_ready; /**< QR bootstrap data may be copied. */
    char device_name[PROVISIONING_SERVICE_DEVICE_NAME_MAX_BYTES]; /**< BLE name. */
} provisioning_service_status_t;

typedef provisioning_service_status_t provisioning_service_snapshot_t;

/**
 * @brief Initialize the idle manual provisioning service.
 * @param config is copied before the worker starts.
 * @return ESP_OK on success; otherwise an argument, lifecycle, or allocation error.
 */
esp_err_t provisioning_service_init(
    const provisioning_service_config_t *config);

/**
 * @brief Stop provisioning and release all service resources.
 * @param timeout_ms is the maximum wait or PROVISIONING_SERVICE_WAIT_FOREVER.
 * @return ESP_OK when disabled; otherwise a lifecycle, cleanup, or timeout error.
 * @note A timeout after command admission may be retried until cleanup completes.
 */
esp_err_t provisioning_service_deinit(uint32_t timeout_ms);

/**
 * @brief Generate a fresh QR secret and open the configured BLE window.
 * @return ESP_OK when admitted, otherwise a lifecycle, queue, or state error.
 */
esp_err_t provisioning_service_open_window(void);

/**
 * @brief Explicitly cancel a phone-owned operation and close the window.
 * @return ESP_OK when admitted or already idle; otherwise a lifecycle error.
 */
esp_err_t provisioning_service_close_window(void);

/**
 * @brief Close provisioning admission before standby.
 * @param timeout_ms is reserved for lifecycle symmetry.
 * @return ESP_OK when suspended; ESP_ERR_INVALID_STATE while active.
 */
esp_err_t provisioning_service_suspend(uint32_t timeout_ms);

/**
 * @brief Reopen provisioning admission after standby rollback or wake.
 * @param timeout_ms is reserved for lifecycle symmetry.
 * @return ESP_OK when idle; otherwise a lifecycle error.
 */
esp_err_t provisioning_service_resume(uint32_t timeout_ms);

/**
 * @brief Copy the password-free provisioning status.
 * @param status receives the immutable status snapshot.
 * @return ESP_OK on success; otherwise an argument or lifecycle error.
 */
esp_err_t provisioning_service_get_status(
    provisioning_service_status_t *status);

/**
 * @brief Copy the active QR bootstrap JSON for display.
 * @param output receives the NUL-terminated QR JSON.
 * @param capacity is the output capacity in bytes.
 * @param out_length receives the JSON byte count excluding NUL.
 * @return ESP_OK on success; otherwise an argument, state, or size error.
 * @warning The caller must overwrite the output when the page is paused.
 */
esp_err_t provisioning_service_copy_qr(char *output, size_t capacity,
                                       size_t *out_length);

/**
 * @brief Report whether a provisioning window currently owns BLE.
 * @return true while a window owns BLE resources; false otherwise.
 */
bool provisioning_service_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* __PROVISIONING_SERVICE_H__ */
