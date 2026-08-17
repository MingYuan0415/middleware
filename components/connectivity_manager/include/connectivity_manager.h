#ifndef __CONNECTIVITY_MANAGER_H__
#define __CONNECTIVITY_MANAGER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONNECTIVITY_MANAGER_SSID_MAX_BYTES     32U
#define CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES 64U
#define CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS   5U
#define CONNECTIVITY_MANAGER_WAIT_FOREVER       UINT32_MAX
#define CONNECTIVITY_MANAGER_PROFILE_REVISION_INITIAL UINT64_C(1)

EVENT_BUS_DECLARE_ID(CONNECTIVITY_MANAGER_MSG);

/** @brief Product-owned connectivity worker configuration. */
typedef struct connectivity_manager_config
{
    uint32_t task_priority;       /**< Connectivity policy worker priority. */
    uint32_t wifi_task_priority;  /**< Low-level Wi-Fi worker priority. */
} connectivity_manager_config_t;

/** @brief Nonzero generation identifying one admitted foreground operation. */
typedef uint64_t connectivity_manager_operation_id_t;

/** @brief Public connectivity policy state. */
typedef enum
{
    CONNECTIVITY_MANAGER_STATE_OFFLINE = 0,
    CONNECTIVITY_MANAGER_STATE_IDLE,
    CONNECTIVITY_MANAGER_STATE_SCANNING,
    CONNECTIVITY_MANAGER_STATE_CONNECTING,
    CONNECTIVITY_MANAGER_STATE_WAITING_IP,
    CONNECTIVITY_MANAGER_STATE_IP_READY,
    CONNECTIVITY_MANAGER_STATE_RETRY_WAIT,
    CONNECTIVITY_MANAGER_STATE_SUSPENDED,
} connectivity_manager_state_t;

/** @brief Stable failure reason suitable for application presentation. */
typedef enum
{
    CONNECTIVITY_MANAGER_FAILURE_NONE = 0,
    CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION,
    CONNECTIVITY_MANAGER_FAILURE_AP_NOT_FOUND,
    CONNECTIVITY_MANAGER_FAILURE_ASSOCIATION_TIMEOUT,
    CONNECTIVITY_MANAGER_FAILURE_DHCP_TIMEOUT,
    CONNECTIVITY_MANAGER_FAILURE_LINK_LOST,
    CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE,
    CONNECTIVITY_MANAGER_FAILURE_STORAGE,
    CONNECTIVITY_MANAGER_FAILURE_CONFLICT,
    CONNECTIVITY_MANAGER_FAILURE_INTERNAL,
} connectivity_manager_failure_t;

/** @brief Network security classification accepted by the manager. */
typedef enum
{
    CONNECTIVITY_MANAGER_SECURITY_OPEN = 0,
    CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    CONNECTIVITY_MANAGER_SECURITY_UNSUPPORTED,
} connectivity_manager_security_t;

/** @brief Event-bus message subtypes published by the manager. */
typedef enum
{
    CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT = 1,
    CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
} connectivity_manager_msg_sub_type_t;

/** @brief Borrowed credentials copied during command admission. */
typedef struct connectivity_manager_credentials
{
    const char *ssid;                         /**< Borrowed SSID bytes. */
    size_t ssid_length;                       /**< Number of SSID bytes. */
    const char *password;                     /**< Borrowed password bytes. */
    size_t password_length;                   /**< Number of password bytes. */
    connectivity_manager_security_t security; /**< Requested security mode. */
} connectivity_manager_credentials_t;

/** @brief Nonzero client identity used to make profile sync idempotent. */
typedef uint64_t connectivity_manager_client_sync_id_t;

/** @brief One sanitized bounded scan result. */
typedef struct connectivity_manager_scan_record
{
    char ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES + 1U]; /**< SSID. */
    int8_t rssi;                                             /**< Signal dBm. */
    uint8_t channel;                                        /**< Wi-Fi channel. */
    connectivity_manager_security_t security;               /**< Security. */
    bool saved;                                             /**< Saved profile. */
} connectivity_manager_scan_record_t;

/** @brief Immutable global connectivity policy snapshot. */
typedef struct connectivity_manager_status_snapshot
{
    uint64_t generation;                            /**< Snapshot generation. */
    connectivity_manager_operation_id_t operation_id; /**< Foreground operation. */
    connectivity_manager_state_t state;             /**< Current policy state. */
    connectivity_manager_failure_t failure;         /**< Last classified failure. */
    int32_t last_error;                             /**< Diagnostic error value. */
    uint32_t ipv4_address;                          /**< Network-order IPv4. */
    uint32_t retry_delay_ms;                        /**< Remaining policy delay. */
    uint8_t retry_count;                            /**< Long retry attempt. */
    bool available;                                 /**< Manager is initialized. */
    bool radio_available;                           /**< Radio executor is ready. */
    bool saved_profile;                             /**< A valid profile exists. */
    bool profile_persisted;                         /**< Current target is saved. */
    bool auto_connect;                              /**< Persistent policy switch. */
    bool manual_hold;                               /**< Offline for this boot. */
    bool operation_complete;                        /**< Foreground terminal event. */
    bool operation_canceled;                        /**< Owner confirmed cancellation. */
    bool operation_conflict;                        /**< Sync identity conflict. */
    uint64_t profile_revision;                       /**< Durable profile revision. */
    connectivity_manager_client_sync_id_t
    applied_client_sync_id;                         /**< Last durable sync id. */
    char ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES + 1U]; /**< Target SSID. */
} connectivity_manager_status_snapshot_t;

/** @brief Immutable bounded scan snapshot. */
typedef struct connectivity_manager_scan_snapshot
{
    uint64_t generation;                            /**< Snapshot generation. */
    connectivity_manager_operation_id_t operation_id; /**< Scan operation. */
    int32_t last_error;                             /**< Scan result. */
    uint8_t record_count;                           /**< Valid records. */
    bool running;                                   /**< Scan is active. */
    bool truncated;                                 /**< Results were truncated. */
    bool operation_canceled;                        /**< Owner confirmed cancellation. */
    connectivity_manager_scan_record_t
    records[CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS]; /**< Scan results. */
} connectivity_manager_scan_snapshot_t;

/**
 * @brief Initialize the policy worker and low-level radio executor.
 *
 * @param config is copied before the worker starts.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for an invalid config;
 *         otherwise a lifecycle or allocation error.
 */
esp_err_t connectivity_manager_init(
    const connectivity_manager_config_t *config);

/**
 * @brief Idempotently erase the persisted Wi-Fi profile before init.
 *
 * This reset-domain API owns the private profile key and excludes a racing
 * manager initialization through the lifecycle state machine.
 *
 * @return ESP_OK when absent or erased; otherwise a lifecycle or NVS error.
 */
esp_err_t connectivity_manager_clear_persisted_profile(void);

/**
 * @brief Stop the policy worker and release the low-level radio executor.
 *
 * @param timeout_ms is the total wait or CONNECTIVITY_MANAGER_WAIT_FOREVER.
 * @return ESP_OK when offline; ESP_ERR_TIMEOUT when the deadline expires;
 *         otherwise the retained executor cleanup error.
 */
esp_err_t connectivity_manager_deinit(uint32_t timeout_ms);

/**
 * @brief Suspend policy timers and radio activity before standby.
 *
 * @param timeout_ms is the maximum wait or CONNECTIVITY_MANAGER_WAIT_FOREVER.
 * @return ESP_OK when suspended; otherwise a lifecycle or executor error.
 */
esp_err_t connectivity_manager_suspend(uint32_t timeout_ms);

/**
 * @brief Resume radio activity and policy timers after standby.
 *
 * @param timeout_ms is the maximum wait or CONNECTIVITY_MANAGER_WAIT_FOREVER.
 * @return ESP_OK when resumed; otherwise a lifecycle or executor error.
 */
esp_err_t connectivity_manager_resume(uint32_t timeout_ms);

/**
 * @brief Request one foreground Wi-Fi scan.
 *
 * @param out_operation_id receives the admitted operation generation.
 * @return ESP_OK when queued; otherwise an argument, lifecycle, or queue error.
 */
esp_err_t connectivity_manager_request_scan(
    connectivity_manager_operation_id_t *out_operation_id);

/**
 * @brief Connect with a deep copy of candidate credentials.
 *
 * @param credentials contains borrowed bytes copied before return.
 * @param out_operation_id receives the admitted operation generation.
 * @return ESP_OK when queued; otherwise an argument, lifecycle, or queue error.
 */
esp_err_t connectivity_manager_request_connect(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_operation_id_t *out_operation_id);

/** @brief Synchronize a saved profile without per-command confirmation. */
esp_err_t connectivity_manager_request_sync_profile(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_client_sync_id_t client_sync_id,
    bool auto_connect,
    connectivity_manager_operation_id_t *out_operation_id);

/**
 * @brief Disconnect and hold automatic connection for this boot.
 *
 * @param out_operation_id receives the admitted operation generation.
 * @return ESP_OK when queued; otherwise an argument, lifecycle, or queue error.
 */
esp_err_t connectivity_manager_request_disconnect(
    connectivity_manager_operation_id_t *out_operation_id);

/**
 * @brief Connect immediately using the saved profile.
 *
 * @param out_operation_id receives the admitted operation generation.
 * @return ESP_OK when queued; otherwise an argument, lifecycle, or queue error.
 */
esp_err_t connectivity_manager_request_reconnect_saved(
    connectivity_manager_operation_id_t *out_operation_id);

/**
 * @brief Forget the saved profile and disconnect.
 *
 * @param out_operation_id receives the admitted operation generation.
 * @return ESP_OK when queued; otherwise an argument, lifecycle, or queue error.
 */
esp_err_t connectivity_manager_request_forget(
    connectivity_manager_operation_id_t *out_operation_id);

/**
 * @brief Persistently enable or disable automatic connection.
 *
 * @param enabled selects the persistent automatic connection policy.
 * @param out_operation_id receives the admitted operation generation.
 * @return ESP_OK when queued; otherwise an argument, lifecycle, or queue error.
 */
esp_err_t connectivity_manager_set_auto_connect(
    bool enabled, connectivity_manager_operation_id_t *out_operation_id);

/**
 * @brief Cancel the exact foreground operation when it is still active.
 *
 * @param operation_id is the admitted foreground operation generation.
 * @return ESP_OK when queued; otherwise an argument, lifecycle, or queue error.
 */
esp_err_t connectivity_manager_cancel(
    connectivity_manager_operation_id_t operation_id);

/**
 * @brief Read the cached global connectivity snapshot.
 *
 * @param snapshot receives a password-free immutable copy.
 * @return ESP_OK on success; otherwise an argument or lifecycle error.
 */
esp_err_t connectivity_manager_get_status(
    connectivity_manager_status_snapshot_t *snapshot);

/**
 * @brief Read the cached scan snapshot.
 *
 * @param snapshot receives a bounded immutable copy.
 * @return ESP_OK on success; otherwise an argument or lifecycle error.
 */
esp_err_t connectivity_manager_get_scan_snapshot(
    connectivity_manager_scan_snapshot_t *snapshot);

/**
 * @brief Report whether the policy worker is initialized.
 *
 * @return true while public manager APIs are admitted.
 */
bool connectivity_manager_is_available(void);

#ifdef __cplusplus
}
#endif

#endif /* __CONNECTIVITY_MANAGER_H__ */
