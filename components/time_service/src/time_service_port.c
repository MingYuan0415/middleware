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
    esp_err_t result = ESP_OK;
    const time_t native_epoch = (time_t)epoch;
    if ((int64_t)native_epoch != epoch)
    {
        result = ESP_ERR_INVALID_SIZE;
        goto exit;
    }
    const struct timeval value = { .tv_sec = native_epoch, .tv_usec = 0 };
    result = settimeofday(&value, NULL) == 0 ? ESP_OK : ESP_FAIL;

exit:
    return result;
}

esp_err_t time_service_port_clock_get(int64_t *epoch)
{
    esp_err_t result = ESP_OK;
    if (epoch == NULL)
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }

    struct timeval value;
    if (gettimeofday(&value, NULL) != 0)
    {
        result = ESP_FAIL;
        goto exit;
    }
    *epoch = (int64_t)value.tv_sec;

exit:
    return result;
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
