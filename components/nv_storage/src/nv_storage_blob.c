#define DBG_TAG "nv_storage"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "nv_storage.h"
#include "nv_storage_internal.h"

#include "nvs.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define NV_STORAGE_NAMESPACE      "microtech"
#define NV_STORAGE_BLOB_POOL_SIZE 16U

typedef struct nv_storage_blob_entry
{
    bool in_use;
    char key[NV_STORAGE_KEY_BYTES];
    void *data;
    size_t size;
    nv_storage_blob_default_cb_t default_cb;
    nv_storage_blob_validate_cb_t validate_cb;
} nv_storage_blob_entry_t;

static nv_storage_blob_entry_t s_blobs[NV_STORAGE_BLOB_POOL_SIZE];
static size_t s_blob_count;

void nv_storage_blob_registry_reset(void)
{
    memset(s_blobs, 0, sizeof(s_blobs));
    s_blob_count = 0;
}

static bool _nv_storage_blob_registration_matches(
    const nv_storage_blob_entry_t *entry, void *data, size_t size,
    nv_storage_blob_default_cb_t default_cb,
    nv_storage_blob_validate_cb_t validate_cb)
{
    return entry->data == data && entry->size == size &&
           entry->default_cb == default_cb &&
           entry->validate_cb == validate_cb;
}

esp_err_t nv_storage_blob_register(const char *key, void *data, size_t size,
                                   nv_storage_blob_default_cb_t default_cb,
                                   nv_storage_blob_validate_cb_t validate_cb)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool access_owned = false;
    bool lock_owned = false;
    if (!nv_storage_internal_key_is_valid(key) || data == NULL || size == 0 ||
            default_cb == NULL)
    {
        goto exit;
    }
    if (!nv_storage_internal_access_begin())
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    access_owned = true;
    result = nv_storage_internal_registry_lock();
    if (result != ESP_OK)
    {
        goto exit;
    }
    lock_owned = true;
    if (nv_storage_internal_load_is_active())
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    for (size_t index = 0; index < NV_STORAGE_BLOB_POOL_SIZE; index++)
    {
        nv_storage_blob_entry_t *entry = &s_blobs[index];
        if (entry->in_use && strcmp(entry->key, key) == 0)
        {
            result = _nv_storage_blob_registration_matches(
                         entry, data, size, default_cb, validate_cb) ?
                     ESP_OK : ESP_ERR_INVALID_STATE;
            goto exit;
        }
    }

    if (s_blob_count == NV_STORAGE_BLOB_POOL_SIZE)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }

    for (size_t index = 0; index < NV_STORAGE_BLOB_POOL_SIZE; index++)
    {
        nv_storage_blob_entry_t *entry = &s_blobs[index];
        if (!entry->in_use)
        {
            size_t key_length = strlen(key);
            memset(entry, 0, sizeof(*entry));
            memcpy(entry->key, key, key_length + 1U);
            entry->data = data;
            entry->size = size;
            entry->default_cb = default_cb;
            entry->validate_cb = validate_cb;
            entry->in_use = true;
            s_blob_count++;
            LOG_I("blob registered: '%s' slot=%u size=%u", entry->key,
                  (unsigned)index, (unsigned)size);
            result = ESP_OK;
            break;
        }
    }

exit:
    if (lock_owned)
    {
        nv_storage_internal_registry_unlock();
    }
    if (access_owned)
    {
        nv_storage_internal_access_end();
    }
    return result;
}

static esp_err_t _nv_storage_blob_store_default(
    nvs_handle_t handle, const nv_storage_blob_entry_t *entry,
    void *candidate)
{
    memset(candidate, 0, entry->size);
    esp_err_t result = entry->default_cb(candidate, entry->size);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = nvs_set_blob(handle, entry->key, candidate, entry->size);
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    if (result == ESP_OK)
    {
        memcpy(entry->data, candidate, entry->size);
    }

exit:
    return result;
}

static esp_err_t _nv_storage_blob_load_one(
    nvs_handle_t handle, const nv_storage_blob_entry_t *entry)
{
    void *candidate = NULL;
    size_t stored_size = 0;
    esp_err_t result = nvs_get_blob(handle, entry->key, NULL, &stored_size);
    bool use_default = result == ESP_ERR_NVS_NOT_FOUND ||
                       (result == ESP_OK && stored_size != entry->size);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
    {
        goto exit;
    }

    candidate = malloc(entry->size);
    if (candidate == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }

    if (!use_default)
    {
        size_t read_size = entry->size;
        result = nvs_get_blob(handle, entry->key, candidate, &read_size);
        if (result == ESP_OK && read_size != entry->size)
        {
            use_default = true;
        }
        else if (result != ESP_OK)
        {
            goto exit;
        }
        else if (entry->validate_cb != NULL &&
                 !entry->validate_cb(candidate, read_size))
        {
            use_default = true;
        }
        else
        {
            memcpy(entry->data, candidate, entry->size);
        }
    }

    if (use_default)
    {
        result = _nv_storage_blob_store_default(handle, entry, candidate);
    }

exit:
    free(candidate);
    return result;
}

esp_err_t nv_storage_blob_load_all(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool load_owned = false;
    bool lock_owned = false;
    if (!nv_storage_internal_load_begin())
    {
        goto exit;
    }
    load_owned = true;
    result = nv_storage_internal_registry_lock();
    if (result != ESP_OK)
    {
        goto exit;
    }
    lock_owned = true;
    if (s_blob_count == 0)
    {
        result = ESP_OK;
        goto exit;
    }

    /*
     * LOAD_ACTIVE rejects registry mutation. This lock boundary also waits
     * for a registration admitted before LOAD_ACTIVE was set, so s_blobs
     * remains immutable after the lock is released and until load_end().
     */
    nv_storage_internal_registry_unlock();
    lock_owned = false;

    nvs_handle_t handle;
    result = nvs_open(NV_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK)
    {
        for (size_t index = 0; index < NV_STORAGE_BLOB_POOL_SIZE; index++)
        {
            if (!s_blobs[index].in_use)
            {
                continue;
            }
            result = _nv_storage_blob_load_one(handle, &s_blobs[index]);
            if (result != ESP_OK)
            {
                break;
            }
        }
        nvs_close(handle);
    }

exit:
    if (lock_owned)
    {
        nv_storage_internal_registry_unlock();
    }
    if (load_owned)
    {
        nv_storage_internal_load_end();
    }
    return result;
}
