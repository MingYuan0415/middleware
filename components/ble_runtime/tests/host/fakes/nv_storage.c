#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#include "nv_storage.h"

#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x1106

#define NV_STORAGE_FAKE_CAPACITY 512U
#define NV_STORAGE_FAKE_KEYS 4U
#define NV_STORAGE_FAKE_KEY_MAX 16U

typedef struct nv_storage_fake_entry
{
    char key[NV_STORAGE_FAKE_KEY_MAX];
    bool present;
    size_t len;
    uint8_t data[NV_STORAGE_FAKE_CAPACITY];
} nv_storage_fake_entry_t;

static nv_storage_fake_entry_t s_entries[NV_STORAGE_FAKE_KEYS];
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static esp_err_t s_next_get_result = ESP_OK;

static void _lock(void)
{
    (void)pthread_mutex_lock(&s_mutex);
}

static void _unlock(void)
{
    (void)pthread_mutex_unlock(&s_mutex);
}

static nv_storage_fake_entry_t *_find_entry_locked(const char *key)
{
    if (key == NULL)
    {
        return NULL;
    }
    for (size_t i = 0U; i < NV_STORAGE_FAKE_KEYS; ++i)
    {
        if (s_entries[i].present &&
                strncmp(s_entries[i].key, key,
                        NV_STORAGE_FAKE_KEY_MAX - 1U) == 0)
        {
            return &s_entries[i];
        }
    }
    return NULL;
}

static nv_storage_fake_entry_t *_free_entry_locked(const char *key)
{
    if (key == NULL)
    {
        return NULL;
    }
    for (size_t i = 0U; i < NV_STORAGE_FAKE_KEYS; ++i)
    {
        if (!s_entries[i].present)
        {
            memset(&s_entries[i], 0, sizeof(s_entries[i]));
            strncpy(s_entries[i].key, key,
                    NV_STORAGE_FAKE_KEY_MAX - 1U);
            s_entries[i].present = true;
            return &s_entries[i];
        }
    }
    return NULL;
}

esp_err_t nv_storage_set_blob(const char *key, const void *data, size_t len)
{
    esp_err_t result = ESP_OK;

    if (key == NULL || data == NULL || len == 0U ||
            len > NV_STORAGE_FAKE_CAPACITY ||
            strlen(key) >= NV_STORAGE_FAKE_KEY_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _lock();
    nv_storage_fake_entry_t *entry = _find_entry_locked(key);

    if (entry == NULL)
    {
        entry = _free_entry_locked(key);
    }
    if (entry == NULL)
    {
        result = ESP_FAIL;
    }
    else
    {
        memcpy(entry->data, data, len);
        entry->len = len;
    }
    _unlock();
    return result;
}

esp_err_t nv_storage_get_blob(const char *key, void *out, size_t *size)
{
    esp_err_t result = ESP_OK;

    if (key == NULL || out == NULL || size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _lock();
    if (s_next_get_result != ESP_OK)
    {
        result = s_next_get_result;
        s_next_get_result = ESP_OK;
        _unlock();
        return result;
    }
    nv_storage_fake_entry_t *entry = _find_entry_locked(key);

    if (entry == NULL)
    {
        result = ESP_ERR_NVS_NOT_FOUND;
    }
    else if (*size < entry->len)
    {
        *size = entry->len;
        result = ESP_ERR_NVS_INVALID_LENGTH;
    }
    else
    {
        memcpy(out, entry->data, entry->len);
        *size = entry->len;
    }
    _unlock();
    return result;
}

esp_err_t nv_storage_erase_key(const char *key)
{
    esp_err_t result = ESP_OK;

    if (key == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _lock();
    nv_storage_fake_entry_t *entry = _find_entry_locked(key);

    if (entry == NULL)
    {
        result = ESP_ERR_NVS_NOT_FOUND;
    }
    else
    {
        memset(entry, 0, sizeof(*entry));
    }
    _unlock();
    return result;
}

void nv_storage_fake_reset(void)
{
    _lock();
    memset(s_entries, 0, sizeof(s_entries));
    s_next_get_result = ESP_OK;
    _unlock();
}

size_t nv_storage_fake_blob_len(void)
{
    size_t total = 0U;

    _lock();
    for (size_t i = 0U; i < NV_STORAGE_FAKE_KEYS; ++i)
    {
        if (s_entries[i].present)
        {
            total += s_entries[i].len;
        }
    }
    _unlock();
    return total;
}

void nv_storage_fake_fail_next_get(esp_err_t result)
{
    _lock();
    s_next_get_result = result;
    _unlock();
}
