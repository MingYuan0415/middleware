#include "host_time_port.h"
#include "time_service_port.h"

#include <pthread.h>
#include <stddef.h>

typedef struct host_time_port_state
{
    pthread_mutex_t lock;
    int64_t epoch;
    time_service_port_sync_cb_t callback;
    bool invoke_callback_on_stop;
    bool running;
    esp_err_t stop_result;
} host_time_port_state_t;

static host_time_port_state_t s_port =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

void host_time_port_reset(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.epoch = 0;
    s_port.callback = NULL;
    s_port.invoke_callback_on_stop = false;
    s_port.running = false;
    s_port.stop_result = ESP_OK;
    (void)pthread_mutex_unlock(&s_port.lock);
}

void host_time_port_invoke_callback_on_stop(bool enabled)
{
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.invoke_callback_on_stop = enabled;
    (void)pthread_mutex_unlock(&s_port.lock);
}

void host_time_port_set_stop_result(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.stop_result = result;
    (void)pthread_mutex_unlock(&s_port.lock);
}

bool host_time_port_complete(int64_t epoch)
{
    (void)pthread_mutex_lock(&s_port.lock);
    time_service_port_sync_cb_t callback =
        s_port.running ? s_port.callback : NULL;
    if (callback != NULL)
    {
        s_port.epoch = epoch;
    }
    (void)pthread_mutex_unlock(&s_port.lock);
    if (callback != NULL)
    {
        struct timeval value = {.tv_sec = (time_t)epoch, .tv_usec = 0};
        callback(&value);
    }
    return callback != NULL;
}

esp_err_t time_service_port_clock_set(int64_t epoch)
{
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.epoch = epoch;
    (void)pthread_mutex_unlock(&s_port.lock);
    return ESP_OK;
}

esp_err_t time_service_port_clock_get(int64_t *epoch)
{
    if (epoch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_port.lock);
    *epoch = s_port.epoch;
    (void)pthread_mutex_unlock(&s_port.lock);
    return ESP_OK;
}

esp_err_t time_service_port_sntp_start(const char *server,
                                       time_service_port_sync_cb_t callback)
{
    if (server == NULL || server[0] == '\0' || callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_port.lock);
    s_port.callback = callback;
    s_port.running = true;
    (void)pthread_mutex_unlock(&s_port.lock);
    return ESP_OK;
}

esp_err_t time_service_port_sntp_restart(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    (void)pthread_mutex_lock(&s_port.lock);
    if (s_port.callback != NULL)
    {
        s_port.running = true;
        result = ESP_OK;
    }
    (void)pthread_mutex_unlock(&s_port.lock);
    return result;
}

esp_err_t time_service_port_sntp_stop(void)
{
    (void)pthread_mutex_lock(&s_port.lock);
    time_service_port_sync_cb_t callback = s_port.invoke_callback_on_stop ?
                                           s_port.callback : NULL;
    const esp_err_t result = s_port.stop_result;
    s_port.callback = NULL;
    s_port.running = false;
    (void)pthread_mutex_unlock(&s_port.lock);
    if (callback != NULL)
    {
        struct timeval value = {.tv_sec = 0, .tv_usec = 0};
        callback(&value);
    }
    return result;
}
