#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#include "nv_storage.h"

#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x1106
#define ESP_ERR_NVS_INVALID_STATE 0x1108

#define NV_STORAGE_FAKE_CAPACITY 512U
#define NV_STORAGE_FAKE_KEYS 4U
#define NV_STORAGE_FAKE_KEY_MAX 16U

typedef struct nv_storage_fake_entry
{
    char key[NV_STORAGE_FAKE_KEY_MAX];
    bool present;          /**< Committed entry exists (durable). */
    bool staged;           /**< A staged write/erase awaits commit. */
    size_t len;            /**< Committed payload length. */
    size_t staged_len;     /**< Staged payload length. */
    uint8_t data[NV_STORAGE_FAKE_CAPACITY];      /**< Committed payload. */
    uint8_t staged_data[NV_STORAGE_FAKE_CAPACITY]; /**< Staged payload. */
} nv_storage_fake_entry_t;

/* The fake mirrors real NVS write/commit semantics: set_blob stages the
 * value, erase_key stages the removal, and only commit() publishes the
 * staged state. A failed commit or a power cycle discards the staged
 * state, so the durable boundary the firmware relies on is testable. */
static nv_storage_fake_entry_t s_entries[NV_STORAGE_FAKE_KEYS];
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static esp_err_t s_fail_next_set;
static esp_err_t s_fail_next_get;
static esp_err_t s_fail_next_erase;
static esp_err_t s_fail_next_commit;

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

static nv_storage_fake_entry_t *_find_or_create_entry_locked(const char *key)
{
    nv_storage_fake_entry_t *entry = _find_entry_locked(key);

    if (entry != NULL)
    {
        return entry;
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

static nv_storage_fake_entry_t *_publish_entry_locked(
    nv_storage_fake_entry_t *entry)
{
    if (entry == NULL)
    {
        return NULL;
    }
    if (entry->staged && entry->staged_len > 0U)
    {
        memcpy(entry->data, entry->staged_data, entry->staged_len);
        entry->len = entry->staged_len;
        entry->staged = false;
    }
    else if (entry->staged)
    {
        /* Staged erase: the entry disappears on commit. */
        memset(entry, 0, sizeof(*entry));
    }
    return entry;
}

/* Production nv_storage_set_blob() = nvs_set_blob() + nvs_commit(): the
 * call fails (and the durable boundary is NOT crossed) when the commit
 * fails, but the staged value stays readable by the same handle. */
static esp_err_t _commit_locked(void)
{
    if (s_fail_next_commit != ESP_OK)
    {
        return s_fail_next_commit;
    }
    for (size_t i = 0U; i < NV_STORAGE_FAKE_KEYS; ++i)
    {
        if (s_entries[i].present && s_entries[i].staged)
        {
            _publish_entry_locked(&s_entries[i]);
        }
    }
    return ESP_OK;
}

esp_err_t nv_storage_set_blob(const char *key, const void *data, size_t len)
{
    if (key == NULL || data == NULL || len == 0U ||
            len > NV_STORAGE_FAKE_CAPACITY ||
            strlen(key) >= NV_STORAGE_FAKE_KEY_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _lock();
    if (s_fail_next_set != ESP_OK)
    {
        const esp_err_t result = s_fail_next_set;

        s_fail_next_set = ESP_OK;
        _unlock();
        return result;
    }
    nv_storage_fake_entry_t *entry = _find_or_create_entry_locked(key);

    if (entry == NULL)
    {
        _unlock();
        return ESP_FAIL;
    }
    entry->staged = true;
    memcpy(entry->staged_data, data, len);
    entry->staged_len = len;
    if (s_fail_next_commit != ESP_OK)
    {
        /* The staged write succeeded but the durable commit failed: the
         * value remains readable (NVS handle semantics) yet a power cycle
         * discards it. */
        const esp_err_t result = s_fail_next_commit;

        s_fail_next_commit = ESP_OK;
        _unlock();
        return result;
    }
    (void)_commit_locked();
    _unlock();
    return ESP_OK;
}

esp_err_t nv_storage_get_blob(const char *key, void *out, size_t *size)
{
    esp_err_t result = ESP_OK;

    if (key == NULL || out == NULL || size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _lock();
    if (s_fail_next_get != ESP_OK)
    {
        result = s_fail_next_get;
        s_fail_next_get = ESP_OK;
        _unlock();
        return result;
    }
    /* Reads observe the latest value (staged or committed), mirroring the
     * NVS handle: an uncommitted write is visible in memory but not
     * durable. */
    nv_storage_fake_entry_t *entry = _find_entry_locked(key);
    const uint8_t *read_data = NULL;
    size_t read_len = 0U;

    if (entry != NULL && entry->staged && entry->staged_len > 0U)
    {
        /* Staged write (commit pending or failed). */
        read_data = entry->staged_data;
        read_len = entry->staged_len;
    }
    else if (entry != NULL && entry->present && !entry->staged)
    {
        read_data = entry->data;
        read_len = entry->len;
    }
    if (read_data == NULL)
    {
        result = ESP_ERR_NVS_NOT_FOUND;
    }
    else if (*size < read_len)
    {
        *size = read_len;
        result = ESP_ERR_NVS_INVALID_LENGTH;
    }
    else
    {
        memcpy(out, read_data, read_len);
        *size = read_len;
    }
    _unlock();
    return result;
}

esp_err_t nv_storage_erase_key(const char *key)
{
    if (key == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _lock();
    if (s_fail_next_erase != ESP_OK)
    {
        const esp_err_t result = s_fail_next_erase;

        s_fail_next_erase = ESP_OK;
        _unlock();
        return result;
    }
    nv_storage_fake_entry_t *entry = _find_entry_locked(key);

    if (entry == NULL || entry->staged)
    {
        _unlock();
        return ESP_ERR_NVS_NOT_FOUND;
    }
    entry->staged = true;
    memset(entry->staged_data, 0, sizeof(entry->staged_data));
    entry->staged_len = 0U;
    if (s_fail_next_commit != ESP_OK)
    {
        const esp_err_t result = s_fail_next_commit;

        s_fail_next_commit = ESP_OK;
        _unlock();
        return result;
    }
    (void)_commit_locked();
    _unlock();
    return ESP_OK;
}

void nv_storage_fake_reset(void)
{
    _lock();
    memset(s_entries, 0, sizeof(s_entries));
    s_fail_next_set = ESP_OK;
    s_fail_next_get = ESP_OK;
    s_fail_next_erase = ESP_OK;
    s_fail_next_commit = ESP_OK;
    _unlock();
}

size_t nv_storage_fake_blob_len(void)
{
    size_t total = 0U;

    _lock();
    for (size_t i = 0U; i < NV_STORAGE_FAKE_KEYS; ++i)
    {
        if (s_entries[i].present && !s_entries[i].staged)
        {
            total += s_entries[i].len;
        }
    }
    _unlock();
    return total;
}

void nv_storage_fake_fail_next_set(esp_err_t result)
{
    _lock();
    s_fail_next_set = result;
    _unlock();
}

void nv_storage_fake_fail_next_get(esp_err_t result)
{
    _lock();
    s_fail_next_get = result;
    _unlock();
}

void nv_storage_fake_fail_next_erase(esp_err_t result)
{
    _lock();
    s_fail_next_erase = result;
    _unlock();
}

void nv_storage_fake_fail_next_commit(esp_err_t result)
{
    _lock();
    s_fail_next_commit = result;
    _unlock();
}

void nv_storage_fake_power_cycle(void)
{
    _lock();
    /* A power cut discards all staged (uncommitted) state; the previous
     * committed value survives (including when the staged write or erase
     * never committed). An entry created only by a staged write vanishes. */
    for (size_t i = 0U; i < NV_STORAGE_FAKE_KEYS; ++i)
    {
        if (s_entries[i].staged)
        {
            memset(s_entries[i].staged_data, 0,
                   sizeof(s_entries[i].staged_data));
            s_entries[i].staged_len = 0U;
            s_entries[i].staged = false;
            if (!s_entries[i].present || s_entries[i].len == 0U)
            {
                /* No committed value: the entry never existed durably. */
                memset(&s_entries[i], 0, sizeof(s_entries[i]));
            }
        }
    }
    _unlock();
}

bool nv_storage_fake_commit_pending(void)
{
    bool pending = false;

    _lock();
    for (size_t i = 0U; i < NV_STORAGE_FAKE_KEYS; ++i)
    {
        if (s_entries[i].present && s_entries[i].staged)
        {
            pending = true;
            break;
        }
    }
    _unlock();
    return pending;
}
