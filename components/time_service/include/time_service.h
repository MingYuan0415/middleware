#ifndef __TIME_SERVICE_H__
#define __TIME_SERVICE_H__

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum timezone bytes excluding the terminator. */
#define TIME_SERVICE_TIMEZONE_MAX_BYTES 63U
/** @brief Maximum SNTP server bytes excluding the terminator. */
#define TIME_SERVICE_SNTP_SERVER_MAX_BYTES 127U

/** @brief Product-owned time-service startup configuration. */
typedef struct time_service_config
{
    const char *timezone;    /**< POSIX timezone string copied at startup. */
    const char *sntp_server; /**< SNTP server name copied at startup. */
    uint32_t task_priority;  /**< FreeRTOS synchronization worker priority. */
} time_service_config_t;

/** @brief Calendar fields compared by the recurring RTC alarm. */
typedef struct time_service_alarm_config
{
    bool match_second; /**< Compare second when true. */
    uint8_t second;    /**< Second in the range 0 through 59. */
    bool match_minute; /**< Compare minute when true. */
    uint8_t minute;    /**< Minute in the range 0 through 59. */
    bool match_hour;   /**< Compare hour when true. */
    uint8_t hour;      /**< Hour in the range 0 through 23. */
    bool match_day;    /**< Compare day of month when true. */
    uint8_t day;       /**< Day of month in the range 1 through 31. */
    bool match_weekday; /**< Compare weekday when true. */
    uint8_t weekday;    /**< Weekday in the range 0 through 6. */
} time_service_alarm_config_t;

/** @brief Current RTC alarm and physical interrupt state. */
typedef struct time_service_alarm_status
{
    bool enabled;          /**< Alarm interrupt generation is enabled. */
    bool pending;          /**< Hardware alarm flag is currently latched. */
    bool interrupt_active; /**< Active-low RTC_INT is currently asserted. */
} time_service_alarm_status_t;

/** @brief Non-coalesced alarm edge payload published by the worker. */
typedef struct time_service_alarm_event
{
    uint32_t sequence; /**< Monotonic alarm edge sequence for this service run. */
} time_service_alarm_event_t;

/** @brief Board RTC operations using UTC values. */
typedef struct time_service_rtc_ops
{
    bool (*is_available)(void);                /**< Optional availability probe. */
    esp_err_t (*read)(struct tm *utc_time);    /**< Optional UTC read operation. */
    esp_err_t (*write)(const struct tm *utc_time); /**< Optional UTC write. */
    esp_err_t (*alarm_configure)(const time_service_alarm_config_t *config); /**< Arm alarm. */
    esp_err_t (*alarm_disable)(void); /**< Disable and clear alarm. */
    esp_err_t (*alarm_get_status)(time_service_alarm_status_t *status); /**< Read state. */
    esp_err_t (*alarm_clear)(void); /**< Clear hardware pending flag. */
    esp_err_t (*alarm_poll_interrupt)(bool *active); /**< Poll active-low RTC_INT. */
} time_service_rtc_ops_t;

EVENT_BUS_DECLARE_ID(TIME_SERVICE_MSG);

/** @brief Events published by time_service. */
typedef enum
{
    TIME_SERVICE_MSG_SUB_TYPE_RTC_ALARM = 1, /**< One non-coalesced RTC alarm edge. */
} time_service_msg_sub_type_t;

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
 * @brief Initialize the clock, RTC bridge, and SNTP worker.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 *
 * @warning Call from task context. Lifecycle calls must be serialized.
 */
esp_err_t time_service_init(const time_service_config_t *config);

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

/** @brief Sentinel accepted by suspend/resume to wait without a deadline. */
#define TIME_SERVICE_WAIT_FOREVER UINT32_MAX

/**
 * @brief Quiesce RTC hardware access before system sleep.
 *
 * New RTC and SNTP control operations are rejected before existing public and
 * worker-owned RTC transactions are drained. Pending SNTP completion remains
 * queued for processing after resume.
 *
 * @param timeout_ms is the total maximum wait in milliseconds, or
 *                   TIME_SERVICE_WAIT_FOREVER.
 *
 * @return ESP_OK when no RTC transaction can start; ESP_ERR_TIMEOUT when the
 *         transition must be recovered with time_service_resume(); otherwise
 *         an ESP-IDF error.
 *
 * @warning Call from task context. Lifecycle control calls must be serialized.
 */
esp_err_t time_service_suspend(uint32_t timeout_ms);

/**
 * @brief Resume RTC worker and public hardware operations after sleep.
 *
 * @param timeout_ms is the total maximum wait in milliseconds, or
 *                   TIME_SERVICE_WAIT_FOREVER.
 *
 * @return ESP_OK when RTC access is restored; ESP_ERR_TIMEOUT when recovery
 *         remains pending; otherwise an ESP-IDF error.
 *
 * @warning Call from task context. Lifecycle control calls must be serialized.
 */
esp_err_t time_service_resume(uint32_t timeout_ms);

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
 * @brief Configure and enable the recurring RTC calendar alarm.
 *
 * @note At least one comparison field must be enabled. The alarm is evaluated
 *       against the RTC's UTC calendar fields.
 *
 * @param config selects comparison fields and their values.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for invalid fields;
 *         ESP_ERR_NOT_SUPPORTED without complete RTC alarm operations;
 *         otherwise an ESP-IDF error.
 *
 * @warning This task-context operation may block on RTC I2C.
 */
esp_err_t time_service_alarm_configure(
    const time_service_alarm_config_t *config);

/**
 * @brief Disable the RTC alarm interrupt and clear its pending flag.
 *
 * @return ESP_OK on success; otherwise an ESP-IDF error.
 *
 * @warning This task-context operation may block on RTC I2C.
 */
esp_err_t time_service_alarm_disable(void);

/**
 * @brief Read alarm control, pending flag, and active-low RTC_INT state.
 *
 * @param status receives a complete snapshot after all hardware reads succeed.
 *
 * @return ESP_OK on success; otherwise an ESP-IDF error.
 *
 * @warning This task-context operation may block on RTC I2C.
 */
esp_err_t time_service_alarm_get_status(time_service_alarm_status_t *status);

/**
 * @brief Clear the hardware alarm flag without disabling future matches.
 *
 * @return ESP_OK on success; otherwise an ESP-IDF error.
 *
 * @warning This task-context operation may block on RTC I2C.
 */
esp_err_t time_service_alarm_clear(void);

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
