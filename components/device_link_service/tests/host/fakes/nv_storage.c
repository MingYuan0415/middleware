#include <stdbool.h>
#include <string.h>

#include "nv_storage.h"

#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x1106

#define NV_STORAGE_FAKE_CAPACITY 512U

static uint8_t s_blob[NV_STORAGE_FAKE_CAPACITY];
static size_t s_blob_len;
static bool s_present;

esp_err_t nv_storage_set_blob(const char *key, const void *data, size_t len)
{
    if (key == NULL || data == NULL || len == 0U ||
            len > NV_STORAGE_FAKE_CAPACITY)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_blob, data, len);
    s_blob_len = len;
    s_present = true;
    return ESP_OK;
}

esp_err_t nv_storage_get_blob(const char *key, void *out, size_t *size)
{
    if (key == NULL || out == NULL || size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_present)
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*size < s_blob_len)
    {
        *size = s_blob_len;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, s_blob, s_blob_len);
    *size = s_blob_len;
    return ESP_OK;
}

esp_err_t nv_storage_erase_key(const char *key)
{
    if (key == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_present)
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memset(s_blob, 0, sizeof(s_blob));
    s_blob_len = 0U;
    s_present = false;
    return ESP_OK;
}

void nv_storage_fake_reset(void)
{
    memset(s_blob, 0, sizeof(s_blob));
    s_blob_len = 0U;
    s_present = false;
}

size_t nv_storage_fake_blob_len(void)
{
    return s_present ? s_blob_len : 0U;
}
