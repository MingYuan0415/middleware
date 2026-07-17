#ifndef __WIFI_SERVICE_PORT_H__
#define __WIFI_SERVICE_PORT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi_service.h"

/** @brief Events translated from ESP-IDF Wi-Fi and IP event loops. */
typedef enum
{
    WIFI_SERVICE_PORT_EVENT_SCAN_DONE = 0, /**< Driver scan completed. */
    WIFI_SERVICE_PORT_EVENT_STA_CONNECTED, /**< Station associated. */
    WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED, /**< Station disconnected. */
    WIFI_SERVICE_PORT_EVENT_GOT_IP,         /**< Station acquired IPv4. */
    WIFI_SERVICE_PORT_EVENT_LOST_IP,        /**< Station lost IPv4. */
} wifi_service_port_event_type_t;

/** @brief Aggregate ownership state of the ESP-IDF Wi-Fi adapter. */
typedef enum
{
    WIFI_SERVICE_PORT_STATE_CLEAN = 0, /**< No adapter resources remain. */
    WIFI_SERVICE_PORT_STATE_STOPPED,   /**< Initialized with radio stopped. */
    WIFI_SERVICE_PORT_STATE_STARTED,   /**< Radio and event flow are active. */
    WIFI_SERVICE_PORT_STATE_PARTIAL,   /**< Retryable partial ownership. */
} wifi_service_port_state_t;

/** @brief Bounded event payload submitted to the service worker. */
typedef struct wifi_service_port_event
{
    wifi_service_port_event_type_t type; /**< Translated event type. */
    uint64_t epoch;                      /**< Adapter ownership epoch. */
    int32_t status;                      /**< Driver or operation result. */
    uint32_t ipv4_address;               /**< IPv4 address in network order. */
    uint16_t disconnect_reason;          /**< ESP-IDF disconnect reason. */
    uint8_t scan_id;                     /**< ESP-IDF scan identifier. */
} wifi_service_port_event_t;

/** @brief One sanitized scan result returned by the ESP-IDF adapter. */
typedef struct wifi_service_port_scan_record
{
    uint8_t ssid[WIFI_SERVICE_SSID_MAX_BYTES]; /**< Raw SSID bytes. */
    uint8_t ssid_length;                       /**< Number of SSID bytes. */
    int8_t rssi;                               /**< Signal strength in dBm. */
    uint8_t channel;                           /**< Primary Wi-Fi channel. */
    wifi_service_security_t security;          /**< Mapped security class. */
} wifi_service_port_scan_record_t;

/** @brief Fixed-size credential copy consumed only by the worker. */
typedef struct wifi_service_port_credentials
{
    uint8_t ssid[WIFI_SERVICE_SSID_MAX_BYTES]; /**< Raw SSID bytes. */
    uint8_t ssid_length;                       /**< Number of SSID bytes. */
    uint8_t password[WIFI_SERVICE_PASSWORD_MAX_BYTES + 1U]; /**< Secret. */
    uint8_t password_length;                   /**< Number of secret bytes. */
    wifi_service_security_t security;          /**< Requested security. */
} wifi_service_port_credentials_t;

/**
 * @brief Submit one bounded ESP-IDF event to the service queue.
 *
 * @param event is the translated adapter event.
 *
 * @return ESP_OK when queued, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_submit_event(
    const wifi_service_port_event_t *event);

/**
 * @brief Initialize and start the worker-owned ESP-IDF Wi-Fi adapter.
 * @return ESP_OK when ready, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_init(void);

/**
 * @brief Release all adapter resources, retaining failures for retry.
 * @return ESP_OK when clean, otherwise the first retained cleanup error.
 */
esp_err_t wifi_service_port_deinit(void);

/**
 * @brief Report whether no adapter resource remains owned.
 * @return true when clean; false otherwise.
 */
bool wifi_service_port_is_clean(void);

/**
 * @brief Return the aggregate adapter ownership state.
 * @return Current adapter ownership state.
 */
wifi_service_port_state_t wifi_service_port_get_state(void);

/**
 * @brief Report whether the ESP-IDF scan list still requires release.
 * @return true while scan results are owned; false otherwise.
 */
bool wifi_service_port_scan_is_owned(void);

/**
 * @brief Return the current adapter event epoch.
 * @return Current nonzero event epoch, or zero before initialization.
 */
uint64_t wifi_service_port_get_epoch(void);

/**
 * @brief Start radio operation and event delivery.
 * @return ESP_OK when started or already active, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_start(void);

/**
 * @brief Stop radio operation and drain event delivery.
 * @return ESP_OK when stopped, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_stop(void);

/**
 * @brief Start one non-blocking active scan.
 * @return ESP_OK when started, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_scan_start(void);

/**
 * @brief Copy bounded scan results and release the ESP-IDF scan list.
 *
 * @param records receives at most capacity sanitized records.
 * @param capacity is the number of record slots supplied.
 * @param out_count receives the number of copied records.
 * @param out_truncated receives whether additional records were discarded.
 *
 * @return ESP_OK when copied and released, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_scan_finish(
    wifi_service_port_scan_record_t *records, size_t capacity,
    size_t *out_count, bool *out_truncated);

/**
 * @brief Abort a scan and release any owned scan list.
 * @return ESP_OK when released, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_scan_abort(void);

/**
 * @brief Copy credentials into the ESP-IDF station configuration.
 *
 * @param credentials contains the worker-owned credential copy.
 *
 * @return ESP_OK when configured, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_set_credentials(
    const wifi_service_port_credentials_t *credentials);

/**
 * @brief Clear credentials from the ESP-IDF station configuration.
 * @return ESP_OK when cleared, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_clear_credentials(void);

/**
 * @brief Request station association using configured credentials.
 * @return ESP_OK when requested, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_connect(void);

/**
 * @brief Disconnect the station; an inactive radio is a successful no-op.
 * @return ESP_OK when disconnected or inactive, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_port_disconnect(void);

#ifdef WIFI_SERVICE_TESTING
    /**
    * @brief Report whether all adapter credential storage is zeroed.
    * @return true when zeroed; false otherwise.
    */
    bool wifi_service_test_credentials_are_zero(void);
#endif

#endif /* __WIFI_SERVICE_PORT_H__ */
