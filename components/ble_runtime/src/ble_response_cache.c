#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <stdlib.h>

#include "esp_err.h"

#include "ble_response_cache.h"

#define DBG_TAG "ble_response_cache"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

typedef struct ble_response_cache_entry
{
    uint32_t generation;
    uint8_t *key;
    size_t key_len;
    uint8_t *data;
    size_t data_len;
    uint32_t stored_ms;
    bool in_use;
} ble_response_cache_entry_t;

typedef struct ble_response_cache
{
    const ble_response_cache_config_t *config;
    ble_response_cache_entry_t *entries;
} ble_response_cache_t;

static ble_response_cache_t s_cache;

typedef struct ble_used_id
{
    uint32_t generation;
    uint32_t id;
    bool in_use;
} ble_used_id_t;

typedef struct ble_used_id_set
{
    const ble_response_cache_config_t *config;
    ble_used_id_t *ids;
    size_t head;
    size_t count;
} ble_used_id_set_t;

static ble_used_id_set_t s_used_ids;

static void _ble_cache_lock(void)
{
    if (s_cache.config != NULL && s_cache.config->lock != NULL)
    {
        s_cache.config->lock(s_cache.config->lock_arg);
    }
}

static void _ble_cache_unlock(void)
{
    if (s_cache.config != NULL && s_cache.config->unlock != NULL)
    {
        s_cache.config->unlock(s_cache.config->lock_arg);
    }
}

static void _ble_cache_free_entry(ble_response_cache_entry_t *entry)
{
    free(entry->key);
    free(entry->data);
    memset(entry, 0, sizeof(*entry));
}

static ble_response_cache_entry_t *_ble_cache_find(
    uint32_t generation, const uint8_t *key, size_t key_len)
{
    for (size_t i = 0U; i < s_cache.config->max_entries; ++i)
    {
        ble_response_cache_entry_t *entry = &s_cache.entries[i];

        if (entry->in_use && entry->generation == generation &&
                entry->key_len == key_len &&
                memcmp(entry->key, key, key_len) == 0)
        {
            return entry;
        }
    }
    return NULL;
}

static uint32_t _ble_cache_age(const ble_response_cache_entry_t *entry)
{
    return s_cache.config->now_ms() - entry->stored_ms;
}

static bool _ble_cache_expired(const ble_response_cache_entry_t *entry)
{
    if (s_cache.config->ttl_ms == 0U)
    {
        return false;
    }
    return _ble_cache_age(entry) >= s_cache.config->ttl_ms;
}

static ble_response_cache_entry_t *_ble_cache_evict_slot(void)
{
    size_t victim = 0U;
    bool any_free = false;

    for (size_t i = 0U; i < s_cache.config->max_entries; ++i)
    {
        ble_response_cache_entry_t *entry = &s_cache.entries[i];

        if (!entry->in_use)
        {
            return entry;
        }
        if (s_cache.config->ttl_ms != 0U && _ble_cache_expired(entry))
        {
            _ble_cache_free_entry(entry);
            return entry;
        }
        if (!any_free || _ble_cache_age(entry) >
                _ble_cache_age(&s_cache.entries[victim]))
        {
            victim = i;
            any_free = true;
        }
    }
    _ble_cache_free_entry(&s_cache.entries[victim]);
    return &s_cache.entries[victim];
}

void ble_response_cache_init(const ble_response_cache_config_t *config)
{
    memset(&s_cache, 0, sizeof(s_cache));
    s_cache.config = config;
    s_cache.entries = NULL;
}

void ble_response_cache_deinit(void)
{
    void (*unlock_cb)(void *) = s_cache.config != NULL
                                ? s_cache.config->unlock
                                : NULL;
    void *unlock_arg = s_cache.config != NULL
                       ? s_cache.config->lock_arg
                       : NULL;

    if (s_cache.config != NULL && s_cache.config->lock != NULL)
    {
        s_cache.config->lock(s_cache.config->lock_arg);
    }
    if (s_cache.config != NULL && s_cache.entries != NULL)
    {
        for (size_t i = 0U; i < s_cache.config->max_entries; ++i)
        {
            _ble_cache_free_entry(&s_cache.entries[i]);
        }
        free(s_cache.entries);
        s_cache.entries = NULL;
    }
    s_cache.config = NULL;
    if (unlock_cb != NULL)
    {
        unlock_cb(unlock_arg);
    }
}

esp_err_t ble_response_cache_put(
    uint32_t generation, const uint8_t *key, size_t key_len,
    const uint8_t *data, size_t len)
{
    ble_response_cache_entry_t *entry;

    if (key == NULL || key_len == 0U || data == NULL || len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_cache_lock();
    if (s_cache.config == NULL)
    {
        _ble_cache_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (key_len > s_cache.config->max_key_bytes ||
            len > s_cache.config->max_entry_bytes)
    {
        _ble_cache_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cache.entries == NULL)
    {
        s_cache.entries = calloc(s_cache.config->max_entries,
                                 sizeof(s_cache.entries[0]));
        if (s_cache.entries == NULL)
        {
            _ble_cache_unlock();
            return ESP_ERR_NO_MEM;
        }
    }
    entry = _ble_cache_find(generation, key, key_len);
    if (entry != NULL)
    {
        uint8_t *new_data = malloc(len);

        if (new_data == NULL)
        {
            _ble_cache_unlock();
            return ESP_ERR_NO_MEM;
        }
        free(entry->data);
        entry->data = new_data;
        entry->data_len = len;
        memcpy(entry->data, data, len);
        entry->stored_ms = s_cache.config->now_ms();
        _ble_cache_unlock();
        return ESP_OK;
    }
    entry = _ble_cache_evict_slot();
    entry->key = malloc(key_len);
    entry->data = malloc(len);
    if (entry->key == NULL || entry->data == NULL)
    {
        free(entry->key);
        free(entry->data);
        entry->key = NULL;
        entry->data = NULL;
        _ble_cache_unlock();
        return ESP_ERR_NO_MEM;
    }
    entry->generation = generation;
    entry->key_len = key_len;
    memcpy(entry->key, key, key_len);
    entry->data_len = len;
    memcpy(entry->data, data, len);
    entry->stored_ms = s_cache.config->now_ms();
    entry->in_use = true;
    _ble_cache_unlock();
    return ESP_OK;
}

esp_err_t ble_response_cache_get(
    uint32_t generation, const uint8_t *key, size_t key_len,
    uint8_t *out, size_t capacity, size_t *out_len)
{
    ble_response_cache_entry_t *entry;

    if (key == NULL || key_len == 0U || out == NULL ||
            capacity == 0U || out_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_cache_lock();
    if (s_cache.config == NULL)
    {
        _ble_cache_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cache.entries == NULL)
    {
        _ble_cache_unlock();
        *out_len = 0U;
        return ESP_ERR_NOT_FOUND;
    }
    entry = _ble_cache_find(generation, key, key_len);
    if (entry == NULL || _ble_cache_expired(entry))
    {
        _ble_cache_unlock();
        *out_len = 0U;
        return ESP_ERR_NOT_FOUND;
    }
    if (capacity < entry->data_len)
    {
        _ble_cache_unlock();
        *out_len = 0U;
        return ESP_ERR_NO_MEM;
    }
    memcpy(out, entry->data, entry->data_len);
    *out_len = entry->data_len;
    _ble_cache_unlock();
    return ESP_OK;
}

void ble_response_cache_clear_generation(uint32_t generation)
{
    _ble_cache_lock();
    if (s_cache.config != NULL && s_cache.entries != NULL)
    {
        for (size_t i = 0U; i < s_cache.config->max_entries; ++i)
        {
            ble_response_cache_entry_t *entry = &s_cache.entries[i];

            if (entry->in_use && entry->generation == generation)
            {
                _ble_cache_free_entry(entry);
            }
        }
    }
    _ble_cache_unlock();
}

void ble_response_cache_clear(void)
{
    _ble_cache_lock();
    if (s_cache.config != NULL && s_cache.entries != NULL)
    {
        for (size_t i = 0U; i < s_cache.config->max_entries; ++i)
        {
            _ble_cache_free_entry(&s_cache.entries[i]);
        }
    }
    _ble_cache_unlock();
}

static void _ble_used_ids_lock(void)
{
    if (s_used_ids.config != NULL && s_used_ids.config->lock != NULL)
    {
        s_used_ids.config->lock(s_used_ids.config->lock_arg);
    }
}

static void _ble_used_ids_unlock(void)
{
    if (s_used_ids.config != NULL && s_used_ids.config->unlock != NULL)
    {
        s_used_ids.config->unlock(s_used_ids.config->lock_arg);
    }
}

void ble_used_id_set_init(const ble_response_cache_config_t *config)
{
    memset(&s_used_ids, 0, sizeof(s_used_ids));
    s_used_ids.config = config;
    s_used_ids.ids = NULL;
}

void ble_used_id_set_deinit(void)
{
    void (*unlock_cb)(void *) = s_used_ids.config != NULL
                                ? s_used_ids.config->unlock
                                : NULL;
    void *unlock_arg = s_used_ids.config != NULL
                       ? s_used_ids.config->lock_arg
                       : NULL;

    if (s_used_ids.config != NULL && s_used_ids.config->lock != NULL)
    {
        s_used_ids.config->lock(s_used_ids.config->lock_arg);
    }
    if (s_used_ids.config != NULL && s_used_ids.ids != NULL)
    {
        free(s_used_ids.ids);
        s_used_ids.ids = NULL;
    }
    s_used_ids.config = NULL;
    s_used_ids.head = 0U;
    s_used_ids.count = 0U;
    if (unlock_cb != NULL)
    {
        unlock_cb(unlock_arg);
    }
}

esp_err_t ble_used_id_set_add(uint32_t generation, uint32_t id)
{
    _ble_used_ids_lock();
    if (s_used_ids.config == NULL)
    {
        _ble_used_ids_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_used_ids.ids == NULL)
    {
        s_used_ids.ids = calloc(s_used_ids.config->max_entries,
                                sizeof(s_used_ids.ids[0]));
        if (s_used_ids.ids == NULL)
        {
            _ble_used_ids_unlock();
            return ESP_ERR_NO_MEM;
        }
    }
    for (size_t i = 0U; i < s_used_ids.count; ++i)
    {
        if (s_used_ids.ids[i].generation == generation &&
                s_used_ids.ids[i].id == id)
        {
            _ble_used_ids_unlock();
            return ESP_OK;
        }
    }
    if (s_used_ids.count < s_used_ids.config->max_entries)
    {
        s_used_ids.ids[s_used_ids.count].generation = generation;
        s_used_ids.ids[s_used_ids.count].id = id;
        s_used_ids.ids[s_used_ids.count].in_use = true;
        s_used_ids.count++;
    }
    else
    {
        /* Evict the oldest id (index 0) and compact. */
        for (size_t i = 1U; i < s_used_ids.count; ++i)
        {
            s_used_ids.ids[i - 1U] = s_used_ids.ids[i];
        }
        s_used_ids.ids[s_used_ids.count - 1U].generation = generation;
        s_used_ids.ids[s_used_ids.count - 1U].id = id;
        s_used_ids.ids[s_used_ids.count - 1U].in_use = true;
    }
    _ble_used_ids_unlock();
    return ESP_OK;
}

bool ble_used_id_set_contains(uint32_t generation, uint32_t id)
{
    bool found = false;

    _ble_used_ids_lock();
    if (s_used_ids.config != NULL && s_used_ids.ids != NULL)
    {
        for (size_t i = 0U; i < s_used_ids.count; ++i)
        {
            if (s_used_ids.ids[i].in_use &&
                    s_used_ids.ids[i].generation == generation &&
                    s_used_ids.ids[i].id == id)
            {
                found = true;
                break;
            }
        }
    }
    _ble_used_ids_unlock();
    return found;
}

void ble_used_id_set_clear_generation(uint32_t generation)
{
    _ble_used_ids_lock();
    if (s_used_ids.config != NULL && s_used_ids.ids != NULL)
    {
        size_t write = 0U;

        for (size_t i = 0U; i < s_used_ids.count; ++i)
        {
            if (s_used_ids.ids[i].generation != generation)
            {
                s_used_ids.ids[write] = s_used_ids.ids[i];
                write++;
            }
        }
        s_used_ids.count = write;
        for (size_t i = write; i < s_used_ids.config->max_entries; ++i)
        {
            s_used_ids.ids[i].in_use = false;
        }
    }
    _ble_used_ids_unlock();
}

void ble_used_id_set_clear(void)
{
    _ble_used_ids_lock();
    if (s_used_ids.config != NULL && s_used_ids.ids != NULL)
    {
        memset(s_used_ids.ids, 0,
               s_used_ids.config->max_entries * sizeof(s_used_ids.ids[0]));
    }
    s_used_ids.head = 0U;
    s_used_ids.count = 0U;
    _ble_used_ids_unlock();
}
