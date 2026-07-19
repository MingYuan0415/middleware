#include "time_service_port.h"

#include <time.h>

#include "esp_netif.h"
#include "esp_sntp.h"

static esp_err_t _time_service_port_tcpip_barrier(void *context)
{
    (void)context;
    return ESP_OK;
}

esp_err_t time_service_port_clock_set(int64_t epoch)
{
    const time_t native_epoch = (time_t)epoch;
    if ((int64_t)native_epoch != epoch)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    const struct timeval value = { .tv_sec = native_epoch, .tv_usec = 0 };
    return settimeofday(&value, NULL) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t time_service_port_clock_get(int64_t *epoch)
{
    if (epoch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct timeval value;
    if (gettimeofday(&value, NULL) != 0)
    {
        return ESP_FAIL;
    }
    *epoch = (int64_t)value.tv_sec;
    return ESP_OK;
}

esp_err_t time_service_port_sntp_start(time_service_port_sync_cb_t callback)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(callback);
    esp_sntp_init();
    return ESP_OK;
}

esp_err_t time_service_port_sntp_restart(void)
{
    return esp_sntp_restart() ? ESP_OK : ESP_FAIL;
}

esp_err_t time_service_port_sntp_stop(void)
{
    esp_sntp_set_time_sync_notification_cb(NULL);
    esp_sntp_stop();
    /* esp_sntp_stop() only queues work; join the TCP/IP thread before return. */
    return esp_netif_tcpip_exec(_time_service_port_tcpip_barrier, NULL);
}
