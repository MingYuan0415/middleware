#define DBG_TAG "nv_storage"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "nv_storage.h"
#include "nv_storage_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdatomic.h>
#include <string.h>

#define NV_STORAGE_NAMESPACE        "microtech"
#define NV_STORAGE_STATE_MASK       0x3U
#define NV_STORAGE_LOAD_ACTIVE      0x4U
#define NV_STORAGE_ACCESS_INCREMENT 0x8U
#define NV_STORAGE_ACCESS_MASK \
    (~(NV_STORAGE_STATE_MASK | NV_STORAGE_LOAD_ACTIVE))

typedef enum nv_storage_state
{
    NV_STORAGE_UNINITIALIZED = 0,
    NV_STORAGE_INITIALIZING,
    NV_STORAGE_READY,
    NV_STORAGE_DEINITIALIZING,
} nv_storage_state_t;

static atomic_uint s_lifecycle = ATOMIC_VAR_INIT(NV_STORAGE_UNINITIALIZED);
static atomic_flag s_cleanup_busy = ATOMIC_FLAG_INIT;
static SemaphoreHandle_t s_registry_mutex;
static StaticSemaphore_t s_registry_mutex_storage;
static bool s_flash_owned;

bool nv_storage_internal_key_is_valid(const char *key)
{
    return key != NULL && key[0] != '\0' &&
           strnlen(key, NV_STORAGE_KEY_BYTES) < NV_STORAGE_KEY_BYTES;
}

bool nv_storage_internal_access_begin(void)
{
    bool acquired = false;
    unsigned lifecycle = atomic_load(&s_lifecycle);
    while ((lifecycle & NV_STORAGE_STATE_MASK) == NV_STORAGE_READY)
    {
        if ((lifecycle & NV_STORAGE_ACCESS_MASK) == NV_STORAGE_ACCESS_MASK)
        {
            break;
        }
        if (atomic_compare_exchange_weak(
                    &s_lifecycle, &lifecycle,
                    lifecycle + NV_STORAGE_ACCESS_INCREMENT))
        {
            acquired = true;
            break;
        }
    }
    return acquired;
}

void nv_storage_internal_access_end(void)
{
    atomic_fetch_sub(&s_lifecycle, NV_STORAGE_ACCESS_INCREMENT);
}

bool nv_storage_internal_load_begin(void)
{
    bool acquired = false;
    unsigned lifecycle = atomic_load(&s_lifecycle);
    while ((lifecycle & NV_STORAGE_STATE_MASK) == NV_STORAGE_READY &&
            (lifecycle & NV_STORAGE_LOAD_ACTIVE) == 0U)
    {
        if ((lifecycle & NV_STORAGE_ACCESS_MASK) == NV_STORAGE_ACCESS_MASK)
        {
            break;
        }
        unsigned desired = lifecycle + NV_STORAGE_ACCESS_INCREMENT;
        desired |= NV_STORAGE_LOAD_ACTIVE;
        if (atomic_compare_exchange_weak(&s_lifecycle, &lifecycle, desired))
        {
            acquired = true;
            break;
        }
    }
    return acquired;
}

void nv_storage_internal_load_end(void)
{
    atomic_fetch_sub(&s_lifecycle,
                     NV_STORAGE_ACCESS_INCREMENT + NV_STORAGE_LOAD_ACTIVE);
}

bool nv_storage_internal_load_is_active(void)
{
    return (atomic_load(&s_lifecycle) & NV_STORAGE_LOAD_ACTIVE) != 0U;
}

esp_err_t nv_storage_internal_registry_lock(void)
{
    esp_err_t result = ESP_OK;
    if (s_registry_mutex == NULL ||
            xSemaphoreTake(s_registry_mutex, portMAX_DELAY) != pdTRUE)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    return result;
}

void nv_storage_internal_registry_unlock(void)
{
    if (s_registry_mutex != NULL)
    {
        (void)xSemaphoreGive(s_registry_mutex);
    }
}

static void _nv_storage_wait_for_accesses(void)
{
    while ((atomic_load(&s_lifecycle) & NV_STORAGE_ACCESS_MASK) != 0U)
    {
        vTaskDelay(1);
    }
}

esp_err_t nv_storage_init(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool cleanup_owned = !atomic_flag_test_and_set(&s_cleanup_busy);
    if (!cleanup_owned)
    {
        goto exit;
    }

    unsigned lifecycle = atomic_load(&s_lifecycle);
    nv_storage_state_t state = lifecycle & NV_STORAGE_STATE_MASK;
    if (state == NV_STORAGE_READY)
    {
        result = (lifecycle & NV_STORAGE_LOAD_ACTIVE) == 0U ?
                 ESP_OK : ESP_ERR_INVALID_STATE;
        goto exit;
    }

    unsigned expected = NV_STORAGE_UNINITIALIZED;
    if (!atomic_compare_exchange_strong(&s_lifecycle, &expected,
                                        NV_STORAGE_INITIALIZING))
    {
        goto exit;
    }

    s_registry_mutex = xSemaphoreCreateMutexStatic(&s_registry_mutex_storage);
    if (s_registry_mutex == NULL)
    {
        atomic_store(&s_lifecycle, NV_STORAGE_UNINITIALIZED);
        result = ESP_ERR_NO_MEM;
        goto exit;
    }

    result = nvs_flash_init();
    if (result != ESP_OK)
    {
        vSemaphoreDelete(s_registry_mutex);
        s_registry_mutex = NULL;
        atomic_store(&s_lifecycle, NV_STORAGE_UNINITIALIZED);
        LOG_E("NVS init failed: %d", (int)result);
        goto exit;
    }

    s_flash_owned = true;
    atomic_store(&s_lifecycle, NV_STORAGE_READY);
    LOG_I("initialized, namespace='%s'", NV_STORAGE_NAMESPACE);
    result = ESP_OK;

exit:
    if (cleanup_owned)
    {
        atomic_flag_clear(&s_cleanup_busy);
    }
    return result;
}

esp_err_t nv_storage_deinit(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool cleanup_owned = !atomic_flag_test_and_set(&s_cleanup_busy);
    if (!cleanup_owned)
    {
        goto exit;
    }

    unsigned lifecycle = atomic_load(&s_lifecycle);
    nv_storage_state_t state = lifecycle & NV_STORAGE_STATE_MASK;
    if (state == NV_STORAGE_UNINITIALIZED)
    {
        result = ESP_OK;
        goto exit;
    }
    if (state == NV_STORAGE_INITIALIZING)
    {
        goto exit;
    }

    if (state == NV_STORAGE_READY)
    {
        while (true)
        {
            if ((lifecycle & NV_STORAGE_LOAD_ACTIVE) != 0U)
            {
                goto exit;
            }
            unsigned desired =
                (lifecycle & ~NV_STORAGE_STATE_MASK) |
                NV_STORAGE_DEINITIALIZING;
            if (atomic_compare_exchange_weak(&s_lifecycle, &lifecycle,
                                             desired))
            {
                break;
            }
            if ((lifecycle & NV_STORAGE_STATE_MASK) != NV_STORAGE_READY)
            {
                goto exit;
            }
        }
    }

    _nv_storage_wait_for_accesses();
    result = s_flash_owned ? nvs_flash_deinit() : ESP_OK;
    if (result != ESP_OK)
    {
        LOG_E("NVS deinit failed: %d", (int)result);
        goto exit;
    }

    s_flash_owned = false;
    nv_storage_blob_registry_reset();
    if (s_registry_mutex != NULL)
    {
        vSemaphoreDelete(s_registry_mutex);
        s_registry_mutex = NULL;
    }
    atomic_store(&s_lifecycle, NV_STORAGE_UNINITIALIZED);
    result = ESP_OK;

exit:
    if (cleanup_owned)
    {
        atomic_flag_clear(&s_cleanup_busy);
    }
    return result;
}

static esp_err_t _nv_storage_open(nvs_handle_t *handle)
{
    esp_err_t result = nvs_open(NV_STORAGE_NAMESPACE, NVS_READWRITE, handle);
    if (result != ESP_OK)
    {
        LOG_E("nvs_open failed: %d", (int)result);
    }
    return result;
}

static esp_err_t _nv_storage_begin(const char *key, nvs_handle_t *handle)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (!nv_storage_internal_key_is_valid(key))
    {
        goto exit;
    }
    if (!nv_storage_internal_access_begin())
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = _nv_storage_open(handle);
    if (result != ESP_OK)
    {
        nv_storage_internal_access_end();
    }

exit:
    return result;
}

static void _nv_storage_finish(nvs_handle_t handle)
{
    nvs_close(handle);
    nv_storage_internal_access_end();
}

esp_err_t nv_storage_set_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = nvs_set_u8(handle, key, value);
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    if (result != ESP_OK)
    {
        LOG_E("set_u8 '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}

esp_err_t nv_storage_get_u8(const char *key, uint8_t *output)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (output == NULL)
    {
        goto exit;
    }
    nvs_handle_t handle;
    result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = nvs_get_u8(handle, key, output);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
    {
        LOG_E("get_u8 '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}

esp_err_t nv_storage_set_u16(const char *key, uint16_t value)
{
    nvs_handle_t handle;
    esp_err_t result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = nvs_set_u16(handle, key, value);
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    if (result != ESP_OK)
    {
        LOG_E("set_u16 '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}

esp_err_t nv_storage_get_u16(const char *key, uint16_t *output)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (output == NULL)
    {
        goto exit;
    }
    nvs_handle_t handle;
    result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = nvs_get_u16(handle, key, output);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
    {
        LOG_E("get_u16 '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}

esp_err_t nv_storage_set_u32(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    esp_err_t result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = nvs_set_u32(handle, key, value);
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    if (result != ESP_OK)
    {
        LOG_E("set_u32 '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}

esp_err_t nv_storage_get_u32(const char *key, uint32_t *output)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (output == NULL)
    {
        goto exit;
    }
    nvs_handle_t handle;
    result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = nvs_get_u32(handle, key, output);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
    {
        LOG_E("get_u32 '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}

esp_err_t nv_storage_set_str(const char *key, const char *value)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (value == NULL)
    {
        goto exit;
    }
    nvs_handle_t handle;
    result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = nvs_set_str(handle, key, value);
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    if (result != ESP_OK)
    {
        LOG_E("set_str '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}

esp_err_t nv_storage_get_str(const char *key, char *output, size_t *size)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (size == NULL)
    {
        goto exit;
    }
    nvs_handle_t handle;
    result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }

    result = nvs_get_str(handle, key, output, size);
    if (result == ESP_OK && output == NULL)
    {
        result = ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND &&
            result != ESP_ERR_NVS_INVALID_LENGTH)
    {
        LOG_E("get_str '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}

esp_err_t nv_storage_set_blob(const char *key, const void *data, size_t length)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (data == NULL)
    {
        goto exit;
    }
    nvs_handle_t handle;
    result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = nvs_set_blob(handle, key, data, length);
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    if (result != ESP_OK)
    {
        LOG_E("set_blob '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}

esp_err_t nv_storage_get_blob(const char *key, void *output, size_t *size)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (size == NULL)
    {
        goto exit;
    }
    nvs_handle_t handle;
    result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }

    result = nvs_get_blob(handle, key, output, size);
    if (result == ESP_OK && output == NULL)
    {
        result = ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND &&
            result != ESP_ERR_NVS_INVALID_LENGTH)
    {
        LOG_E("get_blob '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}

esp_err_t nv_storage_erase_key(const char *key)
{
    nvs_handle_t handle;
    esp_err_t result = _nv_storage_begin(key, &handle);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = nvs_erase_key(handle, key);
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
    {
        LOG_E("erase_key '%s' failed: %d", key, (int)result);
    }
    _nv_storage_finish(handle);

exit:
    return result;
}
