#ifndef __TIME_SERVICE_HOST_ESP_SNTP_H__
#define __TIME_SERVICE_HOST_ESP_SNTP_H__

#include <stdbool.h>
#include <sys/time.h>

#define SNTP_OPMODE_POLL 0

/** @brief Fake SNTP synchronization callback type. */
typedef void (*sntp_sync_time_cb_t)(struct timeval *value);

/** @brief Configure the fake SNTP operating mode. */
void esp_sntp_setoperatingmode(int mode);
/** @brief Configure one fake SNTP server. */
void esp_sntp_setservername(int index, const char *server);
/** @brief Install or detach the fake synchronization callback. */
void esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t callback);
/** @brief Start the fake SNTP client. */
void esp_sntp_init(void);
/** @brief Restart the fake SNTP client. */
bool esp_sntp_restart(void);
/** @brief Queue fake SNTP shutdown. */
void esp_sntp_stop(void);

#endif /* __TIME_SERVICE_HOST_ESP_SNTP_H__ */
