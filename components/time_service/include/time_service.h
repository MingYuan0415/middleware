#ifndef __TIME_SERVICE_H__
#define __TIME_SERVICE_H__

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Board RTC operations using UTC values.
 */
typedef struct time_service_rtc_ops
{
    bool (*is_available)(void);                /**< Optional availability probe. */
    esp_err_t (*read)(struct tm *utc_time);    /**< Optional UTC read operation. */
    esp_err_t (*write)(const struct tm *utc_time); /**< Optional UTC write. */
} time_service_rtc_ops_t;

/**
 * @brief Confidence level of the current system clock.
 */
typedef enum
{
    TIME_SERVICE_QUALITY_INVALID = 0, /**< Clock has no trusted source. */
    TIME_SERVICE_QUALITY_RTC,         /**< Clock was restored from RTC. */
    TIME_SERVICE_QUALITY_MANUAL,      /**< Clock was set by the user. */
    TIME_SERVICE_QUALITY_NTP,         /**< Clock was synchronized by NTP. */
} time_service_quality_t;

/**
 * @brief Register board RTC operations before service initialization.
 *
 * @param ops provides operations copied by the service.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for a null pointer;
 *         ESP_ERR_INVALID_STATE after initialization begins.
 */
esp_err_t time_service_register_rtc_ops(const time_service_rtc_ops_t *ops);

/**
 * @brief Initialize the clock, RTC bridge, and SNTP worker using CST-8.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 *
 * @warning Call from task context. Lifecycle calls must be serialized.
 */
esp_err_t time_service_init(void);

/**
 * @brief Stop SNTP, join the worker, and clear registered RTC operations.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE for an inconsistent state
 *         or when called from the SNTP worker.
 *
 * @warning This is a blocking task-context operation. The caller must prevent
 *          concurrent public API calls before deinitialization.
 */
esp_err_t time_service_deinit(void);

/**
 * @brief Read the current system clock as UTC.
 *
 * @param utc_time receives the normalized UTC time.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t time_service_get_utc(struct tm *utc_time);

/**
 * @brief Read the current system clock in the configured local timezone.
 *
 * @param local_time receives the normalized local time.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t time_service_get_local(struct tm *local_time);

/**
 * @brief Set local system time and persist the corresponding UTC value to RTC.
 *
 * @note RTC persistence is best effort and its result is available through
 *       time_service_get_last_rtc_error().
 *
 * @param local_time is the local calendar value to apply.
 *
 * @return ESP_OK when the system clock is updated, otherwise an ESP-IDF error.
 *
 * @warning This task-context operation may block on RTC I2C.
 */
esp_err_t time_service_set_local(const struct tm *local_time);

/**
 * @brief Return the confidence level of the current system clock.
 *
 * @return Current clock quality, or TIME_SERVICE_QUALITY_INVALID before init.
 */
time_service_quality_t time_service_get_quality(void);

/**
 * @brief Return the result of the most recent RTC read or write.
 *
 * @return Last RTC result; ESP_ERR_INVALID_STATE before initialization.
 */
esp_err_t time_service_get_last_rtc_error(void);

/**
 * @brief Start or restart an asynchronous SNTP synchronization request.
 *
 * @return ESP_OK when the request starts, otherwise an ESP-IDF error.
 */
esp_err_t time_service_request_sync(void);

/**
 * @brief Stop the outstanding or periodic SNTP client.
 *
 * @return ESP_OK when canceled; ESP_ERR_INVALID_STATE before initialization;
 *         otherwise an SNTP stop or callback-drain error.
 *
 * @warning Call from task context. The operation is idempotent.
 */
esp_err_t time_service_cancel_sync(void);

/**
 * @brief Wait for the current SNTP generation to complete.
 *
 * @note A timeout cancels the outstanding client so a later request can start
 *       immediately. UINT32_MAX waits forever.
 *
 * @param timeout_ms is the maximum wait in milliseconds.
 *
 * @return Synchronization result, ESP_ERR_TIMEOUT when the wait expires, or an
 *         SNTP stop error when timeout cleanup cannot be completed.
 *
 * @warning This is an explicitly blocking task-context operation.
 */
esp_err_t time_service_wait_sync(uint32_t timeout_ms);

/**
 * @brief Compatibility wrapper for time_service_get_local().
 *
 * @param timeinfo receives the normalized local time.
 *
 * @return The result from time_service_get_local().
 */
esp_err_t time_service_get_time(struct tm *timeinfo);

/**
 * @brief Compatibility wrapper for time_service_set_local().
 *
 * @param timeinfo is the local calendar value to apply.
 *
 * @return The result from time_service_set_local().
 */
esp_err_t time_service_set_time(const struct tm *timeinfo);

/**
 * @brief Request SNTP synchronization and wait up to 30 seconds.
 *
 * @return The request error or final synchronization result.
 */
esp_err_t time_service_sync_ntp(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __TIME_SERVICE_H__ */
