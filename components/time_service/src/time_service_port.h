#ifndef __TIME_SERVICE_PORT_H__
#define __TIME_SERVICE_PORT_H__

#include <stdint.h>
#include <sys/time.h>

#include "esp_err.h"

/**
 * @brief Callback invoked by the SNTP port after a clock update.
 *
 * @param value points to the synchronized system time.
 */
typedef void (*time_service_port_sync_cb_t)(struct timeval *value);

/**
 * @brief Set the platform clock from Unix epoch seconds.
 *
 * @param epoch is the Unix time to apply.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t time_service_port_clock_set(int64_t epoch);

/**
 * @brief Read Unix epoch seconds from the platform clock.
 *
 * @param epoch receives the current Unix time.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t time_service_port_clock_get(int64_t *epoch);

/**
 * @brief Start the SNTP client with the supplied update callback.
 *
 * @param callback is invoked after SNTP updates the system clock.
 *
 * @return ESP_OK when started, otherwise an ESP-IDF error.
 */
esp_err_t time_service_port_sntp_start(time_service_port_sync_cb_t callback);

/**
 * @brief Restart an initialized SNTP client.
 *
 * @return ESP_OK when restarted, otherwise an ESP-IDF error.
 */
esp_err_t time_service_port_sntp_restart(void);

/**
 * @brief Stop the SNTP client, detach its callback, and drain TCP/IP work.
 *
 * @return ESP_OK when callback work is drained, otherwise an ESP-IDF error.
 */
esp_err_t time_service_port_sntp_stop(void);

#endif /* __TIME_SERVICE_PORT_H__ */
