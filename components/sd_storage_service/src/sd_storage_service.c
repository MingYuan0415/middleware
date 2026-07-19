#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#define DBG_TAG "sd_storage"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "sd_storage_service.h"

#ifndef CONFIG_SD_STORAGE_SERVICE_ENABLE
    #define CONFIG_SD_STORAGE_SERVICE_ENABLE 1
#endif
#ifndef CONFIG_SD_STORAGE_SERVICE_MOUNT_PATH
    #define CONFIG_SD_STORAGE_SERVICE_MOUNT_PATH "/sdcard"
#endif
#ifndef CONFIG_SD_STORAGE_SERVICE_FORMAT_IF_MOUNT_FAILED
    #define CONFIG_SD_STORAGE_SERVICE_FORMAT_IF_MOUNT_FAILED 0
#endif
#ifndef CONFIG_SD_STORAGE_SERVICE_MAX_FILES
    #define CONFIG_SD_STORAGE_SERVICE_MAX_FILES 5
#endif
#ifndef CONFIG_SD_STORAGE_SERVICE_ALLOCATION_UNIT_SIZE
    #define CONFIG_SD_STORAGE_SERVICE_ALLOCATION_UNIT_SIZE (16 * 1024)
#endif

#define SD_STORAGE_SERVICE_PATH_MAX (64U)

static sd_storage_service_mount_ops_t s_ops;
static bool s_ops_registered;
static bool s_mounted;
static void *s_handle;
static char s_mount_path[SD_STORAGE_SERVICE_PATH_MAX];
static sd_storage_service_config_t s_config;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

typedef enum sd_storage_service_state
{
    SD_STORAGE_SERVICE_STATE_STOPPED = 0,
    SD_STORAGE_SERVICE_STATE_STARTING,
    SD_STORAGE_SERVICE_STATE_STARTED,
    SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING,
    SD_STORAGE_SERVICE_STATE_STOPPING,
} sd_storage_service_state_t;

static sd_storage_service_state_t s_state = SD_STORAGE_SERVICE_STATE_STOPPED;

static bool _allocation_unit_valid(size_t size)
{
    return size == 0U || (size >= 512U && size <= 65536U &&
                          (size & (size - 1U)) == 0U);
}

static esp_err_t _sd_storage_service_validate_ops(
    const sd_storage_service_mount_ops_t *ops)
{
    if (ops == NULL || ops->mount == NULL || ops->unmount == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t sd_storage_service_register_mount_ops(
    const sd_storage_service_mount_ops_t *ops)
{
    esp_err_t result = _sd_storage_service_validate_ops(ops);
    if (result != ESP_OK)
    {
        return result;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != SD_STORAGE_SERVICE_STATE_STOPPED)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        s_ops = *ops;
        s_ops_registered = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return result;
}

esp_err_t sd_storage_service_init(void)
{
#if !CONFIG_SD_STORAGE_SERVICE_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    const char *configured_path = CONFIG_SD_STORAGE_SERVICE_MOUNT_PATH;
    if (configured_path == NULL || configured_path[0] != '/' ||
            strlen(configured_path) >= sizeof(s_mount_path) ||
            CONFIG_SD_STORAGE_SERVICE_MAX_FILES <= 0 ||
            !_allocation_unit_valid(
                CONFIG_SD_STORAGE_SERVICE_ALLOCATION_UNIT_SIZE))
    {
        return ESP_ERR_INVALID_ARG;
    }

    sd_storage_service_mount_ops_t ops = {0};
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state == SD_STORAGE_SERVICE_STATE_STARTED)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    if (s_state == SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != SD_STORAGE_SERVICE_STATE_STOPPED || !s_ops_registered)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = SD_STORAGE_SERVICE_STATE_STARTING;
    ops = s_ops;
    taskEXIT_CRITICAL(&s_state_lock);

    memcpy(s_mount_path, configured_path, strlen(configured_path) + 1U);
    const sd_storage_service_config_t config =
    {
        .mount_path = s_mount_path,
        .format_if_mount_failed =
        CONFIG_SD_STORAGE_SERVICE_FORMAT_IF_MOUNT_FAILED != 0,
        .max_files = CONFIG_SD_STORAGE_SERVICE_MAX_FILES,
        .allocation_unit_size = CONFIG_SD_STORAGE_SERVICE_ALLOCATION_UNIT_SIZE,
    };

    void *handle = NULL;
    esp_err_t result = ops.mount(ops.context, &config, &handle);
    if (result != ESP_OK)
    {
        esp_err_t cleanup_result = ESP_OK;
        if (handle != NULL)
        {
            cleanup_result = ops.unmount(ops.context, handle);
        }
        taskENTER_CRITICAL(&s_state_lock);
        if (cleanup_result == ESP_OK)
        {
            s_state = SD_STORAGE_SERVICE_STATE_STOPPED;
        }
        else
        {
            s_ops = ops;
            s_config = config;
            s_handle = handle;
            s_mounted = false;
            s_state = SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING;
        }
        taskEXIT_CRITICAL(&s_state_lock);
        if (cleanup_result != ESP_OK)
        {
            LOG_W("SD mount rollback pending: 0x%x", cleanup_result);
            return cleanup_result;
        }
        return result;
    }
    if (handle == NULL)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_state = SD_STORAGE_SERVICE_STATE_STOPPED;
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const bool mounted = ops.is_mounted == NULL ? true :
                         ops.is_mounted(ops.context, handle);
    if (!mounted)
    {
        esp_err_t unmount_result = ops.unmount(ops.context, handle);
        taskENTER_CRITICAL(&s_state_lock);
        if (unmount_result == ESP_OK)
        {
            s_state = SD_STORAGE_SERVICE_STATE_STOPPED;
        }
        else
        {
            s_ops = ops;
            s_config = config;
            s_handle = handle;
            s_mounted = false;
            s_state = SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING;
        }
        taskEXIT_CRITICAL(&s_state_lock);
        if (unmount_result != ESP_OK)
        {
            LOG_W("SD adapter reported unmounted state; cleanup failed: 0x%x",
                  unmount_result);
            return unmount_result;
        }
        return ESP_FAIL;
    }

    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != SD_STORAGE_SERVICE_STATE_STARTING)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        (void)ops.unmount(ops.context, handle);
        return ESP_ERR_INVALID_STATE;
    }
    s_ops = ops;
    s_config = config;
    s_handle = handle;
    s_mounted = true;
    s_state = SD_STORAGE_SERVICE_STATE_STARTED;
    taskEXIT_CRITICAL(&s_state_lock);
    LOG_I("SD filesystem mounted at %s", s_mount_path);
    return ESP_OK;
#endif
}

esp_err_t sd_storage_service_deinit(void)
{
    sd_storage_service_mount_ops_t ops = {0};
    void *handle = NULL;
    sd_storage_service_state_t previous_state;
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state == SD_STORAGE_SERVICE_STATE_STOPPED)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    if (s_state != SD_STORAGE_SERVICE_STATE_STARTED &&
            s_state != SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    previous_state = s_state;
    s_state = SD_STORAGE_SERVICE_STATE_STOPPING;
    ops = s_ops;
    handle = s_handle;
    taskEXIT_CRITICAL(&s_state_lock);

    esp_err_t result = ESP_OK;
    if (handle != NULL)
    {
        result = ops.unmount(ops.context, handle);
    }

    taskENTER_CRITICAL(&s_state_lock);
    if (result != ESP_OK)
    {
        s_state = previous_state;
        taskEXIT_CRITICAL(&s_state_lock);
        return result;
    }
    s_handle = NULL;
    s_mounted = false;
    s_state = SD_STORAGE_SERVICE_STATE_STOPPED;
    taskEXIT_CRITICAL(&s_state_lock);
    LOG_I("SD filesystem unmounted");
    return ESP_OK;
}

esp_err_t sd_storage_service_start(void)
{
    return sd_storage_service_init();
}

esp_err_t sd_storage_service_stop(void)
{
    return sd_storage_service_deinit();
}

bool sd_storage_service_is_mounted(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool mounted = s_state == SD_STORAGE_SERVICE_STATE_STARTED &&
                         s_mounted;
    taskEXIT_CRITICAL(&s_state_lock);
    return mounted;
}

const char *sd_storage_service_get_mount_path(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool owns_handle = s_state == SD_STORAGE_SERVICE_STATE_STARTED ||
                             s_state == SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING;
    const char *mount_path = owns_handle ? s_mount_path : NULL;
    taskEXIT_CRITICAL(&s_state_lock);
    return mount_path;
}

void *sd_storage_service_get_handle(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool owns_handle = s_state == SD_STORAGE_SERVICE_STATE_STARTED ||
                             s_state == SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING;
    void *handle = owns_handle ? s_handle : NULL;
    taskEXIT_CRITICAL(&s_state_lock);
    return handle;
}

esp_err_t sd_storage_service_get_config(sd_storage_service_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != SD_STORAGE_SERVICE_STATE_STARTED &&
            s_state != SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *config = s_config;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}
