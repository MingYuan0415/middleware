#ifndef __POWER_SERVICE_H__
#define __POWER_SERVICE_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Power information returned by the board PMU.
 */
typedef struct power_info
{
    uint16_t battery_voltage_mv; /**< Battery voltage in millivolts. */
    int8_t battery_percent;      /**< Estimated battery percentage. */
    bool is_charging;            /**< Whether charging is active. */
    bool is_vbus_connected;      /**< Whether an external VBUS is present. */
} power_info_t;

/**
 * @brief Cached power sample and its validity metadata.
 */
typedef struct power_service_snapshot
{
    power_info_t info;      /**< Most recent successful power sample. */
    int64_t sampled_at_ms;  /**< Monotonic sample time in milliseconds. */
    bool valid;             /**< Whether info contains a current sample. */
} power_service_snapshot_t;

/**
 * @brief Board operations used to sample power hardware.
 */
typedef struct power_service_power_ops
{
    bool (*is_available)(void);                 /**< Optional availability probe. */
    esp_err_t (*get_info)(power_info_t *info);  /**< Required sample operation. */
} power_service_power_ops_t;

EVENT_BUS_DECLARE_ID(POWER_SERVICE_MSG);

/**
 * @brief Event-bus message subtypes published by the power service.
 */
typedef enum
{
    POWER_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE = 1, /**< Latest UI snapshot. */
    POWER_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED, /**< Validity edge. */
} power_service_msg_sub_type_t;

/**
 * @brief Register the board power operations before service initialization.
 *
 * @param ops provides the operations to copy into the service.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for invalid operations;
 *         ESP_ERR_INVALID_STATE while the service is active.
 *
 * @warning Call from task context before power_service_init().
 */
esp_err_t power_service_register_power_ops(const power_service_power_ops_t *ops);

/**
 * @brief Start the sampling worker and take the first sample immediately.
 *
 * @return ESP_OK on success; ESP_ERR_NO_MEM when worker resources cannot be
 *         allocated; ESP_ERR_INVALID_STATE during another lifecycle change.
 *
 * @warning Call from task context.
 */
esp_err_t power_service_init(void);

/**
 * @brief Stop and join the worker, then clear registered board operations.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE when called from the worker
 *         or when the lifecycle state is inconsistent.
 *
 * @warning This is a blocking task-context operation. The caller must prevent
 *          concurrent public API calls before deinitialization.
 */
esp_err_t power_service_deinit(void);

/**
 * @brief Pause sampling after the current PMU transaction completes.
 *
 * @note A timeout cancels the pending pause and waits once more for RUNNING,
 *       while still returning ESP_ERR_TIMEOUT. UINT32_MAX waits forever.
 *
 * @param timeout_ms is the maximum wait in milliseconds.
 *
 * @return ESP_OK when paused; ESP_ERR_TIMEOUT on timeout;
 *         ESP_ERR_INVALID_STATE when the service cannot be paused.
 *
 * @warning This is a blocking task-context operation.
 */
esp_err_t power_service_suspend(uint32_t timeout_ms);

/**
 * @brief Resume sampling after a successful or pending pause.
 *
 * @param timeout_ms is the maximum wait in milliseconds; UINT32_MAX waits
 *                   forever.
 *
 * @return ESP_OK when running; ESP_ERR_TIMEOUT on timeout;
 *         ESP_ERR_INVALID_STATE when the service cannot be resumed.
 *
 * @warning This is a blocking task-context operation.
 */
esp_err_t power_service_resume(uint32_t timeout_ms);

/**
 * @brief Copy the cached snapshot without accessing the PMU.
 *
 * @param snapshot receives the cached sample and validity flag.
 *
 * @return ESP_OK while the service is initialized, including when the snapshot
 *         is invalid; otherwise ESP_ERR_INVALID_ARG or ESP_ERR_INVALID_STATE.
 */
esp_err_t power_service_get_snapshot(power_service_snapshot_t *snapshot);

/**
 * @brief Copy the most recent valid power information from the cache.
 *
 * @param info receives the cached power information.
 *
 * @return ESP_OK for a valid sample; otherwise ESP_ERR_INVALID_ARG or
 *         ESP_ERR_INVALID_STATE.
 */
esp_err_t power_service_get_info(power_info_t *info);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __POWER_SERVICE_H__ */
