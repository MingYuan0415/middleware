/**
 * @brief Background weather acquisition, caching, and immutable snapshots.
 */

#ifndef __WEATHER_SERVICE_H__
#define __WEATHER_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_SERVICE_WAIT_FOREVER       UINT32_MAX
#define WEATHER_SERVICE_MAX_HOURS          24U
#define WEATHER_SERVICE_MAX_DAYS           7U
#define WEATHER_SERVICE_MAX_ALERTS         16U
#define WEATHER_SERVICE_CITY_BYTES         64U
#define WEATHER_SERVICE_REGION_BYTES       64U
#define WEATHER_SERVICE_COUNTRY_BYTES      4U
#define WEATHER_SERVICE_TIMEZONE_BYTES     48U
#define WEATHER_SERVICE_PROVIDER_BYTES     33U
#define WEATHER_SERVICE_CONDITION_BYTES    40U
#define WEATHER_SERVICE_DIRECTION_BYTES    24U
#define WEATHER_SERVICE_SCALE_BYTES        16U
#define WEATHER_SERVICE_ALERT_TITLE_BYTES  193U
#define WEATHER_SERVICE_ALERT_TYPE_BYTES   65U
#define WEATHER_SERVICE_ALERT_TEXT_BYTES   1025U
#define WEATHER_SERVICE_ALERT_ADVICE_BYTES 513U

EVENT_BUS_DECLARE_ID(WEATHER_SERVICE_MSG);

/** @brief Weather-service event subtype. */
typedef enum
{
    WEATHER_SERVICE_MSG_SUB_TYPE_SNAPSHOT = 1,
} weather_service_msg_sub_type_t;

/** @brief Public lifecycle and acquisition state. */
typedef enum
{
    WEATHER_SERVICE_STATE_UNCONFIGURED = 0,
    WEATHER_SERVICE_STATE_WAITING_NETWORK,
    WEATHER_SERVICE_STATE_LOCATING,
    WEATHER_SERVICE_STATE_UPDATING,
    WEATHER_SERVICE_STATE_READY,
    WEATHER_SERVICE_STATE_DEGRADED,
    WEATHER_SERVICE_STATE_AUTH_ERROR,
    WEATHER_SERVICE_STATE_RATE_LIMITED,
    WEATHER_SERVICE_STATE_SUSPENDED,
    WEATHER_SERVICE_STATE_ERROR,
} weather_service_state_t;

/** @brief Stable failure classification suitable for application text. */
typedef enum
{
    WEATHER_SERVICE_FAILURE_NONE = 0,
    WEATHER_SERVICE_FAILURE_NOT_CONFIGURED,
    WEATHER_SERVICE_FAILURE_NETWORK,
    WEATHER_SERVICE_FAILURE_LOCATION,
    WEATHER_SERVICE_FAILURE_AUTHENTICATION,
    WEATHER_SERVICE_FAILURE_RATE_LIMITED,
    WEATHER_SERVICE_FAILURE_UPSTREAM,
    WEATHER_SERVICE_FAILURE_RESPONSE,
    WEATHER_SERVICE_FAILURE_STORAGE,
    WEATHER_SERVICE_FAILURE_INTERNAL,
} weather_service_failure_t;

/** @brief Dataset kind bit used in availability and update masks. */
typedef enum
{
    WEATHER_SERVICE_DATA_CURRENT = UINT32_C(1) << 0,
    WEATHER_SERVICE_DATA_ALERTS = UINT32_C(1) << 1,
    WEATHER_SERVICE_DATA_HOURLY = UINT32_C(1) << 2,
    WEATHER_SERVICE_DATA_DAILY = UINT32_C(1) << 3,
    WEATHER_SERVICE_DATA_LOCATION = UINT32_C(1) << 4,
} weather_service_data_mask_t;

/** @brief Timestamp stored independently from the process-global timezone. */
typedef struct weather_service_time
{
    int64_t epoch_seconds;   /**< UTC Unix timestamp. */
    int16_t offset_minutes;  /**< Offset carried by the source timestamp. */
} weather_service_time_t;

/** @brief Freshness metadata common to every weather dataset. */
typedef struct weather_service_dataset_meta
{
    weather_service_time_t fetched_at;  /**< Device fetch time. */
    weather_service_time_t updated_at;  /**< Provider update time. */
    weather_service_time_t valid_until; /**< Server freshness boundary. */
    bool available;                     /**< Dataset contains usable data. */
    bool stale;                         /**< Server returned stale data. */
    bool expired;                       /**< Local maximum stale age elapsed. */
} weather_service_dataset_meta_t;

/** @brief Coarse city-level position retained by the device. */
typedef struct weather_service_location
{
    char city[WEATHER_SERVICE_CITY_BYTES];
    char region[WEATHER_SERVICE_REGION_BYTES];
    char country[WEATHER_SERVICE_COUNTRY_BYTES];
    char timezone[WEATHER_SERVICE_TIMEZONE_BYTES];
    char provider[WEATHER_SERVICE_PROVIDER_BYTES];
    char location_key[17]; /**< Opaque grid scope identity, 16 lowercase hex. */
    int64_t acquired_at;
    bool available;
    bool reused;
} weather_service_location_t;

/** @brief Normalized current observation using compact fixed-point values. */
typedef struct weather_service_current
{
    weather_service_dataset_meta_t meta;
    weather_service_time_t observed_at;
    int16_t temperature_tenths_c;
    int16_t feels_like_tenths_c;
    uint16_t condition_code;
    char condition_text[WEATHER_SERVICE_CONDITION_BYTES];
    uint16_t wind_degrees;
    uint16_t wind_speed_tenths_kmh;
    char wind_direction[WEATHER_SERVICE_DIRECTION_BYTES];
    char wind_scale[WEATHER_SERVICE_SCALE_BYTES];
    uint8_t humidity_percent;
    uint16_t precipitation_tenths_mm;
    uint16_t pressure_hpa;
    uint16_t visibility_tenths_km;
} weather_service_current_t;

/** @brief One normalized hourly forecast. */
typedef struct weather_service_hour
{
    weather_service_time_t forecast_at;
    int16_t temperature_tenths_c;
    uint16_t condition_code;
    char condition_text[WEATHER_SERVICE_CONDITION_BYTES];
    uint16_t wind_speed_tenths_kmh;
    char wind_direction[WEATHER_SERVICE_DIRECTION_BYTES];
    uint8_t humidity_percent;
    uint8_t precipitation_chance_percent;
    uint16_t precipitation_tenths_mm;
} weather_service_hour_t;

/** @brief Complete bounded hourly dataset. */
typedef struct weather_service_hourly
{
    weather_service_dataset_meta_t meta;
    uint8_t count;
    weather_service_hour_t items[WEATHER_SERVICE_MAX_HOURS];
} weather_service_hourly_t;

/** @brief One normalized daily forecast. */
typedef struct weather_service_day
{
    char date[11];
    int16_t minimum_temperature_tenths_c;
    int16_t maximum_temperature_tenths_c;
    uint16_t day_condition_code;
    uint16_t night_condition_code;
    char day_condition_text[WEATHER_SERVICE_CONDITION_BYTES];
    char night_condition_text[WEATHER_SERVICE_CONDITION_BYTES];
    uint8_t humidity_percent;
    uint16_t precipitation_tenths_mm;
    uint16_t visibility_tenths_km;
    uint8_t uv_index;
} weather_service_day_t;

/** @brief Complete bounded daily dataset. */
typedef struct weather_service_daily
{
    weather_service_dataset_meta_t meta;
    uint8_t count;
    weather_service_day_t items[WEATHER_SERVICE_MAX_DAYS];
} weather_service_daily_t;

/** @brief One bounded weather warning. */
typedef struct weather_service_alert
{
    uint64_t key;
    weather_service_time_t issued_at;
    weather_service_time_t starts_at;
    weather_service_time_t ends_at;
    char title[WEATHER_SERVICE_ALERT_TITLE_BYTES];
    char type_name[WEATHER_SERVICE_ALERT_TYPE_BYTES];
    char severity[16];
    char status[16];
    char description[WEATHER_SERVICE_ALERT_TEXT_BYTES];
    char instruction[WEATHER_SERVICE_ALERT_ADVICE_BYTES];
    bool content_truncated;
} weather_service_alert_t;

/** @brief Complete bounded alert snapshot. */
typedef struct weather_service_alerts
{
    weather_service_dataset_meta_t meta;
    uint8_t count;
    bool truncated;
    weather_service_alert_t items[WEATHER_SERVICE_MAX_ALERTS];
} weather_service_alerts_t;

/** @brief Immutable service-owned weather snapshot. */
typedef struct weather_service_snapshot
{
    uint64_t generation;
    uint32_t available_mask;
    weather_service_location_t location;
    weather_service_current_t current;
    weather_service_hourly_t hourly;
    weather_service_daily_t daily;
    weather_service_alerts_t alerts;
} weather_service_snapshot_t;

/** @brief Small status copy safe for synchronous callers. */
typedef struct weather_service_status_snapshot
{
    uint64_t generation;
    weather_service_state_t state;
    weather_service_failure_t failure;
    uint32_t available_mask;
    uint32_t retry_after_seconds;
    bool initialized;
    bool configured;
    bool network_ready;
    bool location_reused;
} weather_service_status_snapshot_t;

/** @brief Coalescible UI notification; consumers acquire the full snapshot. */
typedef struct weather_service_event
{
    uint64_t generation;
    uint32_t changed_mask;
    weather_service_state_t state;
    weather_service_failure_t failure;
} weather_service_event_t;

/** @brief Product-owned weather acquisition policy. */
typedef struct weather_service_config
{
    const char *server_base_url;  /**< HTTPS mt-server Origin, or empty. */
    const char *device_token;     /**< Bearer token, or empty. */
    const char *cache_directory;  /**< Mounted writable cache directory. */
    uint32_t task_priority;
    uint32_t current_refresh_seconds;
    uint32_t alerts_refresh_seconds;
    uint32_t hourly_refresh_seconds;
    uint32_t daily_refresh_seconds;
    uint32_t manual_refresh_min_seconds;
    bool allow_private_http;
} weather_service_config_t;

/** @brief Initialize the singleton weather worker and load its cache. */
esp_err_t weather_service_init(const weather_service_config_t *config);

/** @brief Stop the worker and release all snapshot and transport resources. */
esp_err_t weather_service_deinit(uint32_t timeout_ms);

/** @brief Cancel network work and quiesce the worker before light sleep. */
esp_err_t weather_service_suspend(uint32_t timeout_ms);

/** @brief Resume scheduling after connectivity has resumed. */
esp_err_t weather_service_resume(uint32_t timeout_ms);

/**
 * @brief Notify the service of the current IPv4 connectivity level.
 *
 * A false-to-true transition starts one new location session, as does a
 * ready-state notification whose ipv4_address differs from the previous
 * one. Repeated notifications for the same ready state and address do not
 * repeat location lookup.
 */
esp_err_t weather_service_set_network_ready(bool ready,
        uint32_t ipv4_address);

/**
 * @brief Request one user-initiated refresh cycle.
 *
 * @return ESP_OK when queued; ESP_ERR_INVALID_STATE when the service cannot
 *         currently execute a refresh; ESP_ERR_TIMEOUT when limited locally
 *         or blocked by an upstream account Retry-After deadline.
 */
esp_err_t weather_service_request_refresh(void);

/** @brief Copy the current small service status. */
esp_err_t weather_service_get_status(
    weather_service_status_snapshot_t *status);

/**
 * @brief Acquire the current immutable snapshot.
 *
 * @param snapshot receives a service-owned pointer valid until release.
 * @return ESP_OK when a snapshot exists; ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t weather_service_snapshot_acquire(
    const weather_service_snapshot_t **snapshot);

/** @brief Release one pointer returned by weather_service_snapshot_acquire(). */
void weather_service_snapshot_release(
    const weather_service_snapshot_t *snapshot);

/** @brief Report whether public service APIs are currently admitted. */
bool weather_service_is_available(void);

#ifdef __cplusplus
}
#endif

#endif /* __WEATHER_SERVICE_H__ */
