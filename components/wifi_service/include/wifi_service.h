#ifndef __WIFI_SERVICE_H__
#define __WIFI_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum SSID length accepted by the service. */
#define WIFI_SERVICE_SSID_MAX_BYTES 32U
/** @brief Maximum personal-network password length. */
#define WIFI_SERVICE_PASSWORD_MAX_BYTES 64U
/** @brief Maximum records retained in a scan snapshot. */
#define WIFI_SERVICE_MAX_SCAN_RECORDS 5U
/** @brief Maximum saved-network filters accepted by one filtered scan. */
#define WIFI_SERVICE_MAX_SCAN_TARGETS 6U
/** @brief Infinite timeout value for blocking lifecycle operations. */
#define WIFI_SERVICE_WAIT_FOREVER UINT32_MAX

EVENT_BUS_DECLARE_ID(WIFI_SERVICE_MSG);

/** @brief Product-owned Wi-Fi worker configuration. */
typedef struct wifi_service_config
{
    uint32_t task_priority; /**< FreeRTOS worker priority. */
} wifi_service_config_t;

/** @brief Nonzero generation identifying one logical client session. */
typedef uint64_t wifi_service_session_id_t;
/** @brief Nonzero generation identifying one admitted operation. */
typedef uint64_t wifi_service_operation_id_t;

/** @brief Linearized outcome of a lower-layer cancellation request. */
typedef enum
{
    WIFI_SERVICE_CANCEL_ACCEPTED = 0,
    WIFI_SERVICE_CANCEL_TERMINAL_ALREADY_CLAIMED,
    WIFI_SERVICE_CANCEL_NOT_FOUND,
    WIFI_SERVICE_CANCEL_FAILURE,
} wifi_service_cancel_disposition_t;

/** @brief Public connection state reported in status snapshots. */
typedef enum
{
    WIFI_SERVICE_STATE_OFFLINE = 0,
    WIFI_SERVICE_STATE_IDLE,
    WIFI_SERVICE_STATE_SCANNING,
    WIFI_SERVICE_STATE_CONNECTING,
    WIFI_SERVICE_STATE_WAITING_IP,
    WIFI_SERVICE_STATE_IP_READY,
    WIFI_SERVICE_STATE_RETRY_WAIT,
    WIFI_SERVICE_STATE_SUSPENDED,
} wifi_service_state_t;

/** @brief Classified connection failure reported by the radio executor. */
typedef enum
{
    WIFI_SERVICE_FAILURE_NONE = 0,
    WIFI_SERVICE_FAILURE_AUTHENTICATION,
    WIFI_SERVICE_FAILURE_AP_NOT_FOUND,
    WIFI_SERVICE_FAILURE_ASSOCIATION_TIMEOUT,
    WIFI_SERVICE_FAILURE_DHCP_TIMEOUT,
    WIFI_SERVICE_FAILURE_LINK_LOST,
    WIFI_SERVICE_FAILURE_DRIVER,
} wifi_service_failure_t;

/** @brief Public scan state reported in scan snapshots. */
typedef enum
{
    WIFI_SERVICE_SCAN_IDLE = 0,
    WIFI_SERVICE_SCAN_RUNNING,
    WIFI_SERVICE_SCAN_RESULTS,
    WIFI_SERVICE_SCAN_CANCELED,
    WIFI_SERVICE_SCAN_FAILED,
} wifi_service_scan_state_t;

/** @brief Security classification accepted and reported by the service. */
typedef enum
{
    WIFI_SERVICE_SECURITY_OPEN = 0,
    WIFI_SERVICE_SECURITY_PERSONAL,
    WIFI_SERVICE_SECURITY_UNSUPPORTED,
} wifi_service_security_t;

/** @brief Event-bus message subtypes published by the service. */
typedef enum
{
    WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT = 1,
    WIFI_SERVICE_MSG_SUB_TYPE_SCAN_SNAPSHOT,
    WIFI_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED,
} wifi_service_msg_sub_type_t;

/** @brief Borrowed credential bytes copied during command admission. */
typedef struct wifi_service_credentials
{
    const char *ssid;                   /**< SSID bytes owned by the caller. */
    size_t ssid_length;                 /**< Number of SSID bytes. */
    const char *password;               /**< Password bytes owned by the caller. */
    size_t password_length;             /**< Number of password bytes. */
    wifi_service_security_t security;   /**< Requested security mode. */
} wifi_service_credentials_t;

/**
 * @brief Validate the frozen Device Link Wi-Fi v1 credential policy.
 *
 * SSIDs are 1..32 bytes without NUL. OPEN requires an empty password.
 * PERSONAL accepts an 8..63 byte NUL-free passphrase or exactly 64 ASCII
 * hexadecimal characters; the latter remains an ASCII PSK for ESP-IDF.
 *
 * @param[in] credentials Borrowed credential bytes.
 * @return true when the complete cross-field policy is satisfied.
 */
bool wifi_service_credentials_valid(
    const wifi_service_credentials_t *credentials);

/**
 * @brief Per-attempt budget applied to one CONNECT.
 *
 * The executor still drives a single target; the budget only decides how long
 * one attempt may take before its terminal result is published. A caller that
 * owns candidate ordering uses a zero retry limit so a disappeared access point
 * reaches the policy layer immediately instead of consuming the internal
 * backoff sequence.
 */
typedef struct wifi_service_connect_options
{
    /** Completed internal retries before the attempt terminates (0..limit). */
    uint8_t retry_limit;
    /** Association deadline in ms; zero selects the compiled default. */
    uint16_t association_timeout_ms;
    /** DHCP deadline in ms; zero selects the compiled default. */
    uint16_t dhcp_timeout_ms;
} wifi_service_connect_options_t;

/** @brief One bounded scan result copied from the Wi-Fi driver. */
typedef struct wifi_service_scan_record
{
    char ssid[WIFI_SERVICE_SSID_MAX_BYTES + 1U]; /**< NUL-terminated SSID. */
    int8_t rssi;                                  /**< Signal level in dBm. */
    uint8_t channel;                              /**< Primary Wi-Fi channel. */
    wifi_service_security_t security;             /**< Advertised security. */
} wifi_service_scan_record_t;

/** @brief One saved-network filter applied while scanning. */
typedef struct wifi_service_scan_target
{
    uint8_t ssid[WIFI_SERVICE_SSID_MAX_BYTES]; /**< Raw SSID bytes. */
    uint8_t ssid_length;                       /**< Number of SSID bytes. */
    wifi_service_security_t security;          /**< Saved security. */
} wifi_service_scan_target_t;

/**
 * @brief Immutable operation result and independent current link facts.
 *
 * session_id/operation_id identify the CONNECT/DISCONNECT operation fields.
 * A completed operation's result, failure and cancellation flag remain unchanged
 * until the next CONNECT/DISCONNECT begins or the low-level runtime is initialized.
 * Link changes, IP renewal and scans do not retire or replace that terminal.
 */
typedef struct wifi_service_status_snapshot
{
    uint64_t generation;                       /**< Snapshot generation. */
    wifi_service_session_id_t session_id;      /**< Operation session or zero. */
    wifi_service_operation_id_t operation_id;  /**< CONNECT/DISCONNECT ID or zero. */
    wifi_service_state_t state;                /**< Current connection state. */
    wifi_service_failure_t failure;            /**< Classified last failure. */
    int32_t last_error;                        /**< Last ESP-IDF error value. */
    uint32_t ipv4_address;                     /**< Network-order IPv4 address. */
    uint16_t disconnect_reason;                /**< Last driver reason code. */
    uint8_t retry_count;                       /**< Completed retry attempts. */
    bool available;                            /**< Radio service is usable. */
    bool desired_connected;                    /**< Reconnect policy is active. */
    bool operation_canceled;                   /**< Lower layer confirmed cancel. */
    bool operation_complete;                   /**< Sticky CONNECT/DISCONNECT terminal. */
    esp_err_t operation_result;                 /**< Immutable terminal result. */
    wifi_service_failure_t operation_failure;   /**< Immutable terminal classification. */
    char ssid[WIFI_SERVICE_SSID_MAX_BYTES + 1U]; /**< Target SSID. */
} wifi_service_status_snapshot_t;

/** @brief Immutable bounded scan snapshot published and cached by Wi-Fi. */
typedef struct wifi_service_scan_snapshot
{
    uint64_t generation;                      /**< Snapshot generation. */
    wifi_service_session_id_t session_id;     /**< Related session or zero. */
    wifi_service_operation_id_t operation_id; /**< Related operation or zero. */
    wifi_service_scan_state_t state;          /**< Current scan state. */
    int32_t last_error;                       /**< Last ESP-IDF error value. */
    uint8_t record_count;                     /**< Number of valid records. */
    uint8_t target_count;                     /**< Number of scan filters. */
    uint8_t matched_mask;                     /**< Bit i set when target i was seen. */
    bool truncated;                           /**< Driver returned more records. */
    bool operation_canceled;                  /**< Lower layer confirmed cancel. */
    wifi_service_scan_record_t records[WIFI_SERVICE_MAX_SCAN_RECORDS];
} wifi_service_scan_snapshot_t;

/** @brief Edge event emitted when radio availability changes. */
typedef struct wifi_service_availability_event
{
    bool available; /**< New availability state. */
} wifi_service_availability_event_t;

/**
 * @brief Initialize the worker-owned Wi-Fi runtime.
 *
 * @return ESP_OK when Wi-Fi is ready; otherwise an ESP-IDF error.
 *
 * @warning Call from task context after ESP-NETIF and the default event loop
 *          are ready. A failure is degradable only when cleanup is not pending.
 */
esp_err_t wifi_service_init(const wifi_service_config_t *config);

/**
 * @brief Initialize the worker-owned Wi-Fi runtime with a bounded wait.
 *
 * @param config is copied before the worker starts.
 * @param timeout_ms is the maximum control wait or WIFI_SERVICE_WAIT_FOREVER.
 * @return ESP_OK when Wi-Fi is ready; ESP_ERR_TIMEOUT when the deadline expires;
 *         otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_init_with_timeout(
    const wifi_service_config_t *config, uint32_t timeout_ms);

/**
 * @brief Release all worker-owned Wi-Fi resources.
 *
 * @param timeout_ms is the maximum wait or WIFI_SERVICE_WAIT_FOREVER.
 *
 * @return ESP_OK when cleanup completes; ESP_ERR_TIMEOUT when retry is needed;
 *         otherwise an ESP-IDF error.
 *
 * @warning This is a blocking task-context operation.
 */
esp_err_t wifi_service_deinit(uint32_t timeout_ms);

/**
 * @brief Report whether the Wi-Fi radio runtime is available.
 *
 * @return true when the cached runtime is available; false otherwise.
 */
bool wifi_service_is_available(void);

/**
 * @brief Report whether terminal Wi-Fi cleanup must be retried.
 *
 * @return true while cleanup is pending or running; false otherwise.
 */
bool wifi_service_is_cleanup_pending(void);

/**
 * @brief Open a logical client session and invalidate the prior session.
 *
 * @param out_session_id receives a nonzero session generation.
 *
 * @return ESP_OK when opened; otherwise an ESP-IDF error.
 *
 * @note Opening a session does not disconnect an established connection.
 */
esp_err_t wifi_service_session_open(wifi_service_session_id_t *out_session_id);

/**
 * @brief Close the current logical client session.
 *
 * @param session_id identifies the session to close.
 *
 * @return ESP_OK when closed; ESP_ERR_NOT_FOUND for a stale session; otherwise
 *         an ESP-IDF error.
 */
esp_err_t wifi_service_session_close(wifi_service_session_id_t session_id);

/**
 * @brief Admit a non-blocking scan command for the current session.
 *
 * @param session_id identifies the current client session.
 * @param out_operation_id receives the admitted operation generation.
 *
 * @return ESP_OK when queued; otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_request_scan(
    wifi_service_session_id_t session_id,
    wifi_service_operation_id_t *out_operation_id);

/**
 * @brief Admit a scan that also reports whether saved networks are present.
 *
 * The scan snapshot still retains at most WIFI_SERVICE_MAX_SCAN_RECORDS UI
 * records, but matched_mask is computed against every visible access point
 * returned by the driver. Hidden SSIDs do not expose their SSID and therefore
 * cannot be matched by this filtered scan.
 *
 * @param session_id identifies the current client session.
 * @param targets contains saved-network filters copied before return.
 * @param target_count is the number of filters (0..WIFI_SERVICE_MAX_SCAN_TARGETS).
 * @param out_operation_id receives the admitted operation generation.
 *
 * @return ESP_OK when queued; otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_request_scan_targets(
    wifi_service_session_id_t session_id,
    const wifi_service_scan_target_t *targets, size_t target_count,
    wifi_service_operation_id_t *out_operation_id);

/**
 * @brief Deep-copy credentials and admit a non-blocking connect command.
 *
 * @param session_id identifies the current client session.
 * @param credentials contains borrowed credential bytes copied before return.
 * @param out_operation_id receives the admitted operation generation.
 *
 * @return ESP_OK when queued; otherwise an ESP-IDF error.
 *
 * @note The caller retains ownership and may scrub its buffers on every return.
 * @note Equivalent to wifi_service_request_connect_with_options() with a zeroed
 *       option block, which selects the compiled retry and phase defaults.
 */
esp_err_t wifi_service_request_connect(
    wifi_service_session_id_t session_id,
    const wifi_service_credentials_t *credentials,
    wifi_service_operation_id_t *out_operation_id);

/**
 * @brief Deep-copy credentials and admit a connect with an explicit budget.
 *
 * @param session_id identifies the current client session.
 * @param credentials contains borrowed credential bytes copied before return.
 * @param options is the per-attempt budget; NULL selects every compiled default.
 * @param out_operation_id receives the admitted operation generation.
 *
 * @return ESP_OK when queued; ESP_ERR_INVALID_ARG for an unsupported retry
 *         limit or an option value that does not fit its wire width; otherwise
 *         an ESP-IDF error.
 *
 * @note The budget belongs to this attempt only; a later attempt without
 *       options returns to the compiled defaults.
 */
esp_err_t wifi_service_request_connect_with_options(
    wifi_service_session_id_t session_id,
    const wifi_service_credentials_t *credentials,
    const wifi_service_connect_options_t *options,
    wifi_service_operation_id_t *out_operation_id);

/**
 * @brief Admit a non-blocking disconnect command for the current session.
 *
 * @param session_id identifies the current client session.
 * @param out_operation_id receives the admitted operation generation.
 *
 * @return ESP_OK when queued; otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_request_disconnect(
    wifi_service_session_id_t session_id,
    wifi_service_operation_id_t *out_operation_id);

/**
 * @brief Request cancellation of an admitted operation.
 *
 * @param session_id identifies the operation's session.
 * @param operation_id identifies the operation to cancel.
 * @param out_disposition receives the outcome linearized under the service
 *                        state lock.
 *
 * @return ESP_OK when a disposition was produced; otherwise an argument or
 *         lifecycle error with WIFI_SERVICE_CANCEL_FAILURE.
 */
esp_err_t wifi_service_cancel(wifi_service_session_id_t session_id,
                              wifi_service_operation_id_t operation_id,
                              wifi_service_cancel_disposition_t
                              *out_disposition);

/**
 * @brief Read the cached connection snapshot without accessing the driver.
 *
 * @param snapshot receives the current connection snapshot.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for NULL.
 */
esp_err_t wifi_service_get_status(wifi_service_status_snapshot_t *snapshot);

/**
 * @brief Read the cached scan snapshot without accessing the driver.
 *
 * @param snapshot receives the current scan snapshot.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for NULL.
 */
esp_err_t wifi_service_get_scan_snapshot(
    wifi_service_scan_snapshot_t *snapshot);

/**
 * @brief Suspend radio activity and reject new public commands.
 *
 * @param timeout_ms is the maximum wait or WIFI_SERVICE_WAIT_FOREVER.
 *
 * @return ESP_OK when suspended or already offline; otherwise an ESP-IDF error.
 *
 * @warning This is a blocking task-context operation.
 */
esp_err_t wifi_service_suspend(uint32_t timeout_ms);

/**
 * @brief Resume radio activity or complete retryable terminal cleanup.
 *
 * @param timeout_ms is the maximum wait or WIFI_SERVICE_WAIT_FOREVER.
 *
 * @return ESP_OK when ready or cleanly offline; otherwise an ESP-IDF error.
 *
 * @warning This is a blocking task-context operation.
 */
esp_err_t wifi_service_resume(uint32_t timeout_ms);

/**
 * @brief Clear sensitive bytes through volatile stores.
 *
 * @param memory points to the writable region to clear.
 * @param size is the number of bytes to clear.
 */
void wifi_service_secure_zero(void *memory, size_t size);

#ifdef WIFI_SERVICE_TESTING
/**
 * @brief Report whether all internal credential storage is zeroed.
 *
 * @return true when queued slots and worker credentials contain only zero.
 */
bool wifi_service_test_credentials_are_zero(void);
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __WIFI_SERVICE_H__ */
