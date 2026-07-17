#include "esp_netif.h"
#include "esp_sntp.h"
#include "host_time_idf.h"

#include <assert.h>
#include <string.h>

#define HOST_TIME_IDF_MAX_CALLS 8U

static host_time_idf_call_t s_calls[HOST_TIME_IDF_MAX_CALLS];
static size_t s_call_count;
static esp_err_t s_barrier_result;

static void _record(host_time_idf_call_t call)
{
    assert(s_call_count < HOST_TIME_IDF_MAX_CALLS);
    s_calls[s_call_count++] = call;
}

void host_time_idf_reset(void)
{
    memset(s_calls, 0, sizeof(s_calls));
    s_call_count = 0;
    s_barrier_result = ESP_OK;
}

void host_time_idf_set_barrier_result(esp_err_t result)
{
    s_barrier_result = result;
}

size_t host_time_idf_call_count(void)
{
    return s_call_count;
}

host_time_idf_call_t host_time_idf_call_at(size_t index)
{
    assert(index < s_call_count);
    return s_calls[index];
}

void esp_sntp_setoperatingmode(int mode)
{
    (void)mode;
}

void esp_sntp_setservername(int index, const char *server)
{
    (void)index;
    (void)server;
}

void esp_sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t callback)
{
    if (callback == NULL)
    {
        _record(HOST_TIME_IDF_CALLBACK_DETACHED);
    }
}

void esp_sntp_init(void)
{
}

bool esp_sntp_restart(void)
{
    return true;
}

void esp_sntp_stop(void)
{
    _record(HOST_TIME_IDF_SNTP_STOPPED);
}

esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn callback, void *context)
{
    _record(HOST_TIME_IDF_TCPIP_BARRIER);
    return s_barrier_result == ESP_OK ? callback(context) : s_barrier_result;
}
