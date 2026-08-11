#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nv_storage.h"

#define ESP_ERR_NVS_INVALID_LENGTH 0x1106
#define NV_STORAGE_FAKE_CAPACITY 64U
#define NV_STORAGE_FAKE_KEY_CAPACITY 32U

typedef struct nv_storage_fake_entry
{
    char key[NV_STORAGE_FAKE_KEY_CAPACITY];
    bool allocated;
    bool committed_present;
    bool staged;
    bool staged_erase;
    size_t committed_len;
    size_t staged_len;
    uint8_t committed[NV_STORAGE_FAKE_CAPACITY];
    uint8_t staged_data[NV_STORAGE_FAKE_CAPACITY];
} nv_storage_fake_entry_t;

static nv_storage_fake_entry_t s_entry;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_set_condition = PTHREAD_COND_INITIALIZER;
static esp_err_t s_fail_next_set;
static esp_err_t s_fail_next_get;
static esp_err_t s_fail_next_erase;
static esp_err_t s_fail_next_commit;
static bool s_block_next_set;
static bool s_set_blocked;

static void _nv_storage_fake_lock(void)
{
    (void)pthread_mutex_lock(&s_mutex);
}

static void _nv_storage_fake_unlock(void)
{
    (void)pthread_mutex_unlock(&s_mutex);
}

static bool _nv_storage_fake_key_matches(const char *key)
{
    return key != NULL && s_entry.allocated &&
           strcmp(s_entry.key, key) == 0;
}

static esp_err_t _nv_storage_fake_commit(void)
{
    if (s_fail_next_commit != ESP_OK)
    {
        const esp_err_t result = s_fail_next_commit;

        s_fail_next_commit = ESP_OK;
        return result;
    }
    if (s_entry.staged_erase)
    {
        memset(&s_entry, 0, sizeof(s_entry));
        return ESP_OK;
    }
    memcpy(s_entry.committed, s_entry.staged_data, s_entry.staged_len);
    s_entry.committed_len = s_entry.staged_len;
    s_entry.committed_present = true;
    s_entry.staged = false;
    s_entry.staged_erase = false;
    s_entry.staged_len = 0U;
    memset(s_entry.staged_data, 0, sizeof(s_entry.staged_data));
    return ESP_OK;
}

esp_err_t nv_storage_set_blob(const char *key, const void *data, size_t len)
{
    if (key == NULL || data == NULL || len == 0U ||
            len > NV_STORAGE_FAKE_CAPACITY ||
            strlen(key) >= NV_STORAGE_FAKE_KEY_CAPACITY)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _nv_storage_fake_lock();
    if (s_fail_next_set != ESP_OK)
    {
        const esp_err_t result = s_fail_next_set;

        s_fail_next_set = ESP_OK;
        _nv_storage_fake_unlock();
        return result;
    }
    if (s_block_next_set)
    {
        s_set_blocked = true;
        (void)pthread_cond_broadcast(&s_set_condition);
        while (s_block_next_set)
        {
            (void)pthread_cond_wait(&s_set_condition, &s_mutex);
        }
        s_set_blocked = false;
    }
    if (s_entry.allocated && !_nv_storage_fake_key_matches(key))
    {
        _nv_storage_fake_unlock();
        return ESP_FAIL;
    }
    if (!s_entry.allocated)
    {
        const size_t key_len = strlen(key);

        memset(&s_entry, 0, sizeof(s_entry));
        memcpy(s_entry.key, key, key_len + 1U);
        s_entry.allocated = true;
    }
    memcpy(s_entry.staged_data, data, len);
    s_entry.staged_len = len;
    s_entry.staged = true;
    s_entry.staged_erase = false;
    const esp_err_t result = _nv_storage_fake_commit();

    _nv_storage_fake_unlock();
    return result;
}

esp_err_t nv_storage_get_blob(const char *key, void *out, size_t *size)
{
    if (key == NULL || out == NULL || size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _nv_storage_fake_lock();
    if (s_fail_next_get != ESP_OK)
    {
        const esp_err_t result = s_fail_next_get;

        s_fail_next_get = ESP_OK;
        _nv_storage_fake_unlock();
        return result;
    }
    if (!_nv_storage_fake_key_matches(key) || s_entry.staged_erase ||
            (!s_entry.staged && !s_entry.committed_present))
    {
        _nv_storage_fake_unlock();
        return ESP_ERR_NVS_NOT_FOUND;
    }
    const uint8_t *data = s_entry.staged ? s_entry.staged_data :
                          s_entry.committed;
    const size_t len = s_entry.staged ? s_entry.staged_len :
                       s_entry.committed_len;

    if (*size < len)
    {
        *size = len;
        _nv_storage_fake_unlock();
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, data, len);
    *size = len;
    _nv_storage_fake_unlock();
    return ESP_OK;
}

esp_err_t nv_storage_erase_key(const char *key)
{
    if (key == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _nv_storage_fake_lock();
    if (s_fail_next_erase != ESP_OK)
    {
        const esp_err_t result = s_fail_next_erase;

        s_fail_next_erase = ESP_OK;
        _nv_storage_fake_unlock();
        return result;
    }
    if (!_nv_storage_fake_key_matches(key) ||
            (!s_entry.committed_present && !s_entry.staged))
    {
        _nv_storage_fake_unlock();
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_entry.staged = true;
    s_entry.staged_erase = true;
    s_entry.staged_len = 0U;
    memset(s_entry.staged_data, 0, sizeof(s_entry.staged_data));
    const esp_err_t result = _nv_storage_fake_commit();

    _nv_storage_fake_unlock();
    return result;
}

void nv_storage_fake_reset(void)
{
    _nv_storage_fake_lock();
    memset(&s_entry, 0, sizeof(s_entry));
    s_fail_next_set = ESP_OK;
    s_fail_next_get = ESP_OK;
    s_fail_next_erase = ESP_OK;
    s_fail_next_commit = ESP_OK;
    s_block_next_set = false;
    s_set_blocked = false;
    (void)pthread_cond_broadcast(&s_set_condition);
    _nv_storage_fake_unlock();
}

void nv_storage_fake_power_cycle(void)
{
    _nv_storage_fake_lock();
    s_entry.staged = false;
    s_entry.staged_erase = false;
    s_entry.staged_len = 0U;
    memset(s_entry.staged_data, 0, sizeof(s_entry.staged_data));
    if (!s_entry.committed_present)
    {
        memset(&s_entry, 0, sizeof(s_entry));
    }
    s_fail_next_set = ESP_OK;
    s_fail_next_get = ESP_OK;
    s_fail_next_erase = ESP_OK;
    s_fail_next_commit = ESP_OK;
    _nv_storage_fake_unlock();
}

void nv_storage_fake_fail_next_set(esp_err_t result)
{
    _nv_storage_fake_lock();
    s_fail_next_set = result;
    _nv_storage_fake_unlock();
}

void nv_storage_fake_fail_next_get(esp_err_t result)
{
    _nv_storage_fake_lock();
    s_fail_next_get = result;
    _nv_storage_fake_unlock();
}

void nv_storage_fake_fail_next_erase(esp_err_t result)
{
    _nv_storage_fake_lock();
    s_fail_next_erase = result;
    _nv_storage_fake_unlock();
}

void nv_storage_fake_fail_next_commit(esp_err_t result)
{
    _nv_storage_fake_lock();
    s_fail_next_commit = result;
    _nv_storage_fake_unlock();
}

void nv_storage_fake_block_next_set(void)
{
    _nv_storage_fake_lock();
    s_block_next_set = true;
    s_set_blocked = false;
    _nv_storage_fake_unlock();
}

void nv_storage_fake_wait_set_blocked(void)
{
    _nv_storage_fake_lock();
    while (!s_set_blocked)
    {
        (void)pthread_cond_wait(&s_set_condition, &s_mutex);
    }
    _nv_storage_fake_unlock();
}

void nv_storage_fake_release_blocked_set(void)
{
    _nv_storage_fake_lock();
    s_block_next_set = false;
    (void)pthread_cond_broadcast(&s_set_condition);
    _nv_storage_fake_unlock();
}

size_t nv_storage_fake_committed_blob_len(void)
{
    _nv_storage_fake_lock();
    const size_t len = s_entry.committed_present ?
                       s_entry.committed_len : 0U;

    _nv_storage_fake_unlock();
    return len;
}

bool nv_storage_fake_commit_pending(void)
{
    _nv_storage_fake_lock();
    const bool pending = s_entry.staged;

    _nv_storage_fake_unlock();
    return pending;
}
