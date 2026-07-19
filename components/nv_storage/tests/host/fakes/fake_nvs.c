#include "host_nvs.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define HOST_NVS_MAX_ENTRIES 64U
#define HOST_NVS_MAX_HANDLES 32U
#define HOST_NVS_MAX_VALUE   512U

typedef enum host_value_type
{
    HOST_VALUE_U8 = 0,
    HOST_VALUE_U16,
    HOST_VALUE_U32,
    HOST_VALUE_STR,
    HOST_VALUE_BLOB,
} host_value_type_t;

typedef struct host_entry
{
    bool used;
    char key[NVS_KEY_NAME_MAX_SIZE];
    host_value_type_t type;
    size_t size;
    uint8_t data[HOST_NVS_MAX_VALUE];
} host_entry_t;

typedef struct host_handle
{
    bool used;
    nvs_handle_t id;
} host_handle_t;

typedef struct host_failure
{
    bool enabled;
    unsigned successes;
    esp_err_t error;
} host_failure_t;

typedef struct host_growth
{
    bool enabled;
    host_nvs_operation_t operation;
    char key[NVS_KEY_NAME_MAX_SIZE];
    host_value_type_t type;
    size_t size;
    uint8_t data[HOST_NVS_MAX_VALUE];
} host_growth_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static host_entry_t s_entries[HOST_NVS_MAX_ENTRIES];
static host_handle_t s_handles[HOST_NVS_MAX_HANDLES];
static host_failure_t s_failures[HOST_NVS_OPERATION_COUNT];
static unsigned s_calls[HOST_NVS_OPERATION_COUNT];
static unsigned s_invalid_key_calls;
static nvs_handle_t s_next_handle = 1;
static bool s_flash_initialized;
static host_growth_t s_growth;

static pthread_mutex_t s_block_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_block_changed = PTHREAD_COND_INITIALIZER;
static bool s_open_blocked;
static unsigned s_blocked_open_count;

static struct timespec _host_nvs_deadline(uint32_t timeout_ms)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static esp_err_t _host_nvs_record_locked(host_nvs_operation_t operation)
{
    esp_err_t result = ESP_OK;
    s_calls[operation]++;
    host_failure_t *failure = &s_failures[operation];
    if (failure->enabled)
    {
        if (failure->successes > 0)
        {
            failure->successes--;
        }
        else
        {
            failure->enabled = false;
            result = failure->error;
        }
    }
    return result;
}

static bool _host_nvs_key_is_valid_locked(const char *key)
{
    bool valid = key != NULL && key[0] != '\0' &&
                 strnlen(key, NVS_KEY_NAME_MAX_SIZE) < NVS_KEY_NAME_MAX_SIZE;
    if (!valid)
    {
        s_invalid_key_calls++;
    }
    return valid;
}

static host_handle_t *_host_nvs_find_handle_locked(nvs_handle_t id)
{
    host_handle_t *handle = NULL;
    for (size_t index = 0; index < HOST_NVS_MAX_HANDLES; index++)
    {
        if (s_handles[index].used && s_handles[index].id == id)
        {
            handle = &s_handles[index];
            break;
        }
    }
    return handle;
}

static host_entry_t *_host_nvs_find_entry_locked(const char *key)
{
    host_entry_t *entry = NULL;
    for (size_t index = 0; index < HOST_NVS_MAX_ENTRIES; index++)
    {
        if (s_entries[index].used && strcmp(s_entries[index].key, key) == 0)
        {
            entry = &s_entries[index];
            break;
        }
    }
    return entry;
}

static host_entry_t *_host_nvs_allocate_entry_locked(const char *key)
{
    host_entry_t *entry = _host_nvs_find_entry_locked(key);
    for (size_t index = 0;
            entry == NULL && index < HOST_NVS_MAX_ENTRIES; index++)
    {
        if (!s_entries[index].used)
        {
            entry = &s_entries[index];
            memset(entry, 0, sizeof(*entry));
            entry->used = true;
            memcpy(entry->key, key, strlen(key) + 1U);
        }
    }
    return entry;
}

void host_nvs_reset(void)
{
    (void)pthread_mutex_lock(&s_lock);
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_handles, 0, sizeof(s_handles));
    memset(s_failures, 0, sizeof(s_failures));
    memset(s_calls, 0, sizeof(s_calls));
    memset(&s_growth, 0, sizeof(s_growth));
    s_invalid_key_calls = 0;
    s_next_handle = 1;
    s_flash_initialized = false;
    (void)pthread_mutex_unlock(&s_lock);

    (void)pthread_mutex_lock(&s_block_lock);
    s_open_blocked = false;
    s_blocked_open_count = 0;
    (void)pthread_cond_broadcast(&s_block_changed);
    (void)pthread_mutex_unlock(&s_block_lock);
}

void host_nvs_fail_after(host_nvs_operation_t operation, unsigned successes,
                         esp_err_t error)
{
    (void)pthread_mutex_lock(&s_lock);
    s_failures[operation] = (host_failure_t)
    {
        .enabled = true,
        .successes = successes,
        .error = error,
    };
    (void)pthread_mutex_unlock(&s_lock);
}

unsigned host_nvs_call_count(host_nvs_operation_t operation)
{
    (void)pthread_mutex_lock(&s_lock);
    unsigned count = s_calls[operation];
    (void)pthread_mutex_unlock(&s_lock);
    return count;
}

unsigned host_nvs_invalid_key_call_count(void)
{
    (void)pthread_mutex_lock(&s_lock);
    unsigned count = s_invalid_key_calls;
    (void)pthread_mutex_unlock(&s_lock);
    return count;
}

void host_nvs_seed_blob(const char *key, const void *data, size_t size)
{
    (void)pthread_mutex_lock(&s_lock);
    host_entry_t *entry = _host_nvs_allocate_entry_locked(key);
    if (entry != NULL && size <= sizeof(entry->data))
    {
        entry->type = HOST_VALUE_BLOB;
        entry->size = size;
        memcpy(entry->data, data, size);
    }
    (void)pthread_mutex_unlock(&s_lock);
}

bool host_nvs_read_blob(const char *key, void *data, size_t *size)
{
    (void)pthread_mutex_lock(&s_lock);
    host_entry_t *entry = _host_nvs_find_entry_locked(key);
    bool found = entry != NULL && entry->type == HOST_VALUE_BLOB;
    if (found && size != NULL)
    {
        if (data != NULL && *size >= entry->size)
        {
            memcpy(data, entry->data, entry->size);
        }
        else if (data != NULL)
        {
            found = false;
        }
        *size = entry->size;
    }
    (void)pthread_mutex_unlock(&s_lock);
    return found;
}

bool host_nvs_grow_before_nonnull_get(host_nvs_operation_t operation,
                                      const char *key, const void *data,
                                      size_t size)
{
    bool configured = false;
    host_value_type_t type = HOST_VALUE_U8;
    bool operation_valid = true;
    if (operation == HOST_NVS_GET_STR)
    {
        type = HOST_VALUE_STR;
    }
    else if (operation == HOST_NVS_GET_BLOB)
    {
        type = HOST_VALUE_BLOB;
    }
    else
    {
        operation_valid = false;
    }
    const bool input_valid = key != NULL && key[0] != '\0' &&
                             strnlen(key, NVS_KEY_NAME_MAX_SIZE) <
                             NVS_KEY_NAME_MAX_SIZE &&
                             data != NULL && size <= HOST_NVS_MAX_VALUE;
    if (operation_valid && input_valid)
    {
        (void)pthread_mutex_lock(&s_lock);
        memset(&s_growth, 0, sizeof(s_growth));
        s_growth.enabled = true;
        s_growth.operation = operation;
        memcpy(s_growth.key, key, strlen(key) + 1U);
        s_growth.type = type;
        s_growth.size = size;
        memcpy(s_growth.data, data, size);
        (void)pthread_mutex_unlock(&s_lock);
        configured = true;
    }
    return configured;
}

void host_nvs_block_open(bool blocked)
{
    (void)pthread_mutex_lock(&s_block_lock);
    s_open_blocked = blocked;
    if (!blocked)
    {
        (void)pthread_cond_broadcast(&s_block_changed);
    }
    (void)pthread_mutex_unlock(&s_block_lock);
}

bool host_nvs_wait_for_blocked_open(uint32_t timeout_ms)
{
    (void)pthread_mutex_lock(&s_block_lock);
    const struct timespec deadline = _host_nvs_deadline(timeout_ms);
    int wait_result = 0;
    while (s_blocked_open_count == 0 && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_block_changed, &s_block_lock,
                                             &deadline);
    }
    bool blocked = s_blocked_open_count > 0;
    (void)pthread_mutex_unlock(&s_block_lock);
    return blocked;
}

esp_err_t nvs_flash_init(void)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_record_locked(HOST_NVS_FLASH_INIT);
    if (result == ESP_OK)
    {
        s_flash_initialized = true;
    }
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_flash_deinit(void)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_record_locked(HOST_NVS_FLASH_DEINIT);
    if (result == ESP_OK && !s_flash_initialized)
    {
        result = ESP_ERR_NVS_NOT_INITIALIZED;
    }
    if (result == ESP_OK)
    {
        for (size_t index = 0; index < HOST_NVS_MAX_HANDLES; index++)
        {
            if (s_handles[index].used)
            {
                result = ESP_ERR_INVALID_STATE;
                break;
            }
        }
    }
    if (result == ESP_OK)
    {
        s_flash_initialized = false;
    }
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_flash_erase(void)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_record_locked(HOST_NVS_FLASH_ERASE);
    if (result == ESP_OK)
    {
        memset(s_entries, 0, sizeof(s_entries));
    }
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    (void)open_mode;
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_record_locked(HOST_NVS_OPEN);
    (void)pthread_mutex_unlock(&s_lock);
    if (result != ESP_OK)
    {
        return result;
    }

    (void)pthread_mutex_lock(&s_block_lock);
    if (s_open_blocked)
    {
        s_blocked_open_count++;
        (void)pthread_cond_broadcast(&s_block_changed);
        while (s_open_blocked)
        {
            (void)pthread_cond_wait(&s_block_changed, &s_block_lock);
        }
        s_blocked_open_count--;
    }
    (void)pthread_mutex_unlock(&s_block_lock);

    (void)pthread_mutex_lock(&s_lock);
    if (!s_flash_initialized)
    {
        result = ESP_ERR_NVS_NOT_INITIALIZED;
    }
    else if (namespace_name == NULL || strcmp(namespace_name, "microtech") != 0 ||
             out_handle == NULL)
    {
        result = ESP_ERR_INVALID_ARG;
    }
    else
    {
        result = ESP_ERR_NO_MEM;
        for (size_t index = 0; index < HOST_NVS_MAX_HANDLES; index++)
        {
            if (!s_handles[index].used)
            {
                memset(&s_handles[index], 0, sizeof(s_handles[index]));
                s_handles[index].used = true;
                s_handles[index].id = s_next_handle++;
                *out_handle = s_handles[index].id;
                result = ESP_OK;
                break;
            }
        }
    }
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

void nvs_close(nvs_handle_t handle)
{
    (void)pthread_mutex_lock(&s_lock);
    (void)_host_nvs_record_locked(HOST_NVS_CLOSE);
    host_handle_t *record = _host_nvs_find_handle_locked(handle);
    if (record != NULL)
    {
        memset(record, 0, sizeof(*record));
    }
    (void)pthread_mutex_unlock(&s_lock);
}

static esp_err_t _host_nvs_set_locked(
    nvs_handle_t handle, const char *key, host_value_type_t type,
    const void *data, size_t size, host_nvs_operation_t operation)
{
    esp_err_t result = _host_nvs_record_locked(operation);
    if (result != ESP_OK)
    {
        return result;
    }
    if (_host_nvs_find_handle_locked(handle) == NULL)
    {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!_host_nvs_key_is_valid_locked(key))
    {
        return ESP_ERR_NVS_KEY_TOO_LONG;
    }
    if (data == NULL || size > HOST_NVS_MAX_VALUE)
    {
        return ESP_ERR_INVALID_ARG;
    }
    host_entry_t *entry = _host_nvs_allocate_entry_locked(key);
    if (entry == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    entry->type = type;
    entry->size = size;
    memcpy(entry->data, data, size);
    return ESP_OK;
}

static void _host_nvs_apply_growth_locked(host_nvs_operation_t operation,
        const char *key, bool has_output)
{
    if (has_output && s_growth.enabled &&
            s_growth.operation == operation &&
            strcmp(s_growth.key, key) == 0)
    {
        host_entry_t *entry = _host_nvs_find_entry_locked(key);
        if (entry != NULL)
        {
            entry->type = s_growth.type;
            entry->size = s_growth.size;
            memcpy(entry->data, s_growth.data, s_growth.size);
        }
        memset(&s_growth, 0, sizeof(s_growth));
    }
}

static esp_err_t _host_nvs_get_locked(
    nvs_handle_t handle, const char *key, host_value_type_t type,
    void *data, size_t *size, host_nvs_operation_t operation)
{
    esp_err_t result = _host_nvs_record_locked(operation);
    if (result != ESP_OK)
    {
        return result;
    }
    if (_host_nvs_find_handle_locked(handle) == NULL)
    {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (!_host_nvs_key_is_valid_locked(key))
    {
        return ESP_ERR_NVS_KEY_TOO_LONG;
    }
    _host_nvs_apply_growth_locked(operation, key, data != NULL);
    host_entry_t *entry = _host_nvs_find_entry_locked(key);
    if (entry == NULL)
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (entry->type != type)
    {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    if (size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (data == NULL)
    {
        *size = entry->size;
        return ESP_OK;
    }
    if (*size < entry->size)
    {
        *size = entry->size;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(data, entry->data, entry->size);
    *size = entry->size;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_record_locked(HOST_NVS_COMMIT);
    if (result == ESP_OK && _host_nvs_find_handle_locked(handle) == NULL)
    {
        result = ESP_ERR_NVS_INVALID_HANDLE;
    }
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_set_locked(
                           handle, key, HOST_VALUE_U8, &value, sizeof(value),
                           HOST_NVS_SET_U8);
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *output)
{
    size_t size = sizeof(*output);
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_get_locked(
                           handle, key, HOST_VALUE_U8, output, &size,
                           HOST_NVS_GET_U8);
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_set_locked(
                           handle, key, HOST_VALUE_U16, &value, sizeof(value),
                           HOST_NVS_SET_U16);
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *output)
{
    size_t size = sizeof(*output);
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_get_locked(
                           handle, key, HOST_VALUE_U16, output, &size,
                           HOST_NVS_GET_U16);
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_set_locked(
                           handle, key, HOST_VALUE_U32, &value, sizeof(value),
                           HOST_NVS_SET_U32);
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *output)
{
    size_t size = sizeof(*output);
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_get_locked(
                           handle, key, HOST_VALUE_U32, output, &size,
                           HOST_NVS_GET_U32);
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    size_t size = value == NULL ? 0 : strlen(value) + 1U;
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_set_locked(
                           handle, key, HOST_VALUE_STR, value, size,
                           HOST_NVS_SET_STR);
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *output,
                      size_t *length)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_get_locked(
                           handle, key, HOST_VALUE_STR, output, length,
                           HOST_NVS_GET_STR);
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                       size_t length)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_set_locked(
                           handle, key, HOST_VALUE_BLOB, value, length,
                           HOST_NVS_SET_BLOB);
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *output,
                       size_t *length)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_get_locked(
                           handle, key, HOST_VALUE_BLOB, output, length,
                           HOST_NVS_GET_BLOB);
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = _host_nvs_record_locked(HOST_NVS_ERASE_KEY);
    if (result == ESP_OK && _host_nvs_find_handle_locked(handle) == NULL)
    {
        result = ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (result == ESP_OK && !_host_nvs_key_is_valid_locked(key))
    {
        result = ESP_ERR_NVS_KEY_TOO_LONG;
    }
    host_entry_t *entry = NULL;
    if (result == ESP_OK)
    {
        entry = _host_nvs_find_entry_locked(key);
    }
    if (result == ESP_OK && entry == NULL)
    {
        result = ESP_ERR_NVS_NOT_FOUND;
    }
    if (result == ESP_OK)
    {
        memset(entry, 0, sizeof(*entry));
    }
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}
