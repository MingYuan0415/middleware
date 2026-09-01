#include "audio_service.h"
#include "sd_storage_service.h"

#include <string.h>

static bool s_audio_running;
static const audio_service_config_t s_audio_config =
{
    .sample_rate_hz = 16000U,
    .bits_per_sample = 16U,
    .channels = 2U,
};

esp_err_t audio_service_get_config(audio_service_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *config = s_audio_config;
    return ESP_OK;
}

esp_err_t audio_service_start(void)
{
    s_audio_running = true;
    return ESP_OK;
}

esp_err_t audio_service_stop(void)
{
    s_audio_running = false;
    return ESP_OK;
}

esp_err_t audio_service_read(void *data, size_t bytes, size_t *read,
                             uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!s_audio_running || data == NULL || bytes == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(data, 0, bytes);
    if (read != NULL)
    {
        *read = bytes;
    }
    return ESP_OK;
}

esp_err_t audio_service_write(void *data, size_t bytes, size_t *written,
                              uint32_t timeout_ms)
{
    (void)data;
    (void)timeout_ms;
    if (!s_audio_running || bytes == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (written != NULL)
    {
        *written = bytes;
    }
    return ESP_OK;
}

const char *sd_storage_service_get_mount_path(void)
{
    return "/tmp";
}
