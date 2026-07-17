#ifndef __TIME_SERVICE_HOST_ESP_NETIF_H__
#define __TIME_SERVICE_HOST_ESP_NETIF_H__

#include "esp_err.h"

/** @brief Callback executed synchronously in the fake TCP/IP context. */
typedef esp_err_t (*esp_netif_callback_fn)(void *context);

/** @brief Execute a callback after previously queued TCP/IP work. */
esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn callback, void *context);

#endif /* __TIME_SERVICE_HOST_ESP_NETIF_H__ */
