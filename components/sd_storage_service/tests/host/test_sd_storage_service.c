#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "sd_storage_service.h"

#define TEST_CLEANUP_ERROR ((esp_err_t)0x5501)

typedef struct fake_sd_adapter
{
    int mount_calls;
    int unmount_calls;
    bool mounted;
    bool format_requested;
    int max_files;
    size_t allocation_unit_size;
    char mount_path[64];
    bool check_lifecycle_guard;
    bool return_handle_on_error;
    unsigned unmount_failures;
    esp_err_t mount_result;
} fake_sd_adapter_t;

static int s_handle;
static const sd_storage_service_config_t s_config =
{
    .mount_path = "/sdcard",
    .max_files = 5,
    .allocation_unit_size = 16U * 1024U,
};

static esp_err_t _fake_mount(void *context,
                             const sd_storage_service_config_t *config,
                             sd_storage_service_mount_mode_t mode,
                             void **out_handle)
{
    fake_sd_adapter_t *adapter = context;
    adapter->mount_calls++;
    adapter->mounted = adapter->mount_result == ESP_OK;
    adapter->format_requested =
        mode == SD_STORAGE_SERVICE_MOUNT_RECOVER_FORMAT;
    adapter->max_files = config->max_files;
    adapter->allocation_unit_size = config->allocation_unit_size;
    strcpy(adapter->mount_path, config->mount_path);
    if (adapter->check_lifecycle_guard)
    {
        assert(sd_storage_service_init(&s_config) == ESP_ERR_INVALID_STATE);
        assert(sd_storage_service_deinit() == ESP_ERR_INVALID_STATE);
    }
    if (adapter->mounted || adapter->return_handle_on_error)
    {
        *out_handle = &s_handle;
    }
    return adapter->mount_result;
}

static esp_err_t _fake_unmount(void *context, void *handle)
{
    fake_sd_adapter_t *adapter = context;
    assert(handle == &s_handle);
    adapter->unmount_calls++;
    if (adapter->check_lifecycle_guard)
    {
        assert(sd_storage_service_init(&s_config) == ESP_ERR_INVALID_STATE);
        assert(sd_storage_service_deinit() == ESP_ERR_INVALID_STATE);
    }
    if (adapter->unmount_failures > 0U)
    {
        --adapter->unmount_failures;
        return TEST_CLEANUP_ERROR;
    }
    adapter->mounted = false;
    return ESP_OK;
}

static bool _fake_is_mounted(void *context, void *handle)
{
    fake_sd_adapter_t *adapter = context;
    return handle == &s_handle && adapter->mounted;
}

static void _test_successful_lifecycle(void)
{
    fake_sd_adapter_t adapter =
    {
        .check_lifecycle_guard = true,
    };
    const sd_storage_service_mount_ops_t ops =
    {
        .context = &adapter,
        .mount = _fake_mount,
        .unmount = _fake_unmount,
        .is_mounted = _fake_is_mounted,
    };

    assert(sd_storage_service_register_mount_ops(&ops) == ESP_OK);
    assert(sd_storage_service_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(sd_storage_service_init(&s_config) == ESP_OK);
    assert(sd_storage_service_init(&s_config) == ESP_OK);
    sd_storage_service_config_t different = s_config;
    different.max_files++;
    assert(sd_storage_service_init(&different) == ESP_ERR_INVALID_STATE);
    assert(adapter.mount_calls == 1);
    assert(sd_storage_service_is_mounted());
    assert(strcmp(sd_storage_service_get_mount_path(), "/sdcard") == 0);
    assert(strcmp(adapter.mount_path, "/sdcard") == 0);
    assert(!adapter.format_requested);
    assert(adapter.max_files == 5);
    assert(adapter.allocation_unit_size == 16U * 1024U);
    assert(sd_storage_service_get_handle() == &s_handle);

    sd_storage_service_config_t config = {0};
    assert(sd_storage_service_get_config(&config) == ESP_OK);
    assert(strcmp(config.mount_path, "/sdcard") == 0);
    assert(config.max_files == s_config.max_files);

    assert(sd_storage_service_register_mount_ops(&ops) ==
           ESP_ERR_INVALID_STATE);
    assert(sd_storage_service_deinit() == ESP_OK);
    assert(adapter.unmount_calls == 1);
    assert(!sd_storage_service_is_mounted());
    assert(sd_storage_service_get_mount_path() == NULL);
    assert(sd_storage_service_deinit() == ESP_OK);
}

static void _test_mount_rollback_cleanup_retry(void)
{
    fake_sd_adapter_t adapter =
    {
        .return_handle_on_error = true,
        .unmount_failures = 2U,
        .mount_result = ESP_FAIL,
    };
    const sd_storage_service_mount_ops_t ops =
    {
        .context = &adapter,
        .mount = _fake_mount,
        .unmount = _fake_unmount,
        .is_mounted = _fake_is_mounted,
    };

    assert(sd_storage_service_register_mount_ops(&ops) == ESP_OK);
    assert(sd_storage_service_init(&s_config) == TEST_CLEANUP_ERROR);
    assert(adapter.mount_calls == 1);
    assert(adapter.unmount_calls == 1);
    assert(!sd_storage_service_is_mounted());
    assert(sd_storage_service_get_handle() == &s_handle);
    assert(sd_storage_service_init(&s_config) == ESP_ERR_INVALID_STATE);
    assert(sd_storage_service_start(&s_config) == ESP_ERR_INVALID_STATE);

    assert(sd_storage_service_deinit() == TEST_CLEANUP_ERROR);
    assert(adapter.unmount_calls == 2);
    assert(sd_storage_service_get_handle() == &s_handle);
    assert(sd_storage_service_init(&s_config) == ESP_ERR_INVALID_STATE);

    assert(sd_storage_service_deinit() == ESP_OK);
    assert(adapter.unmount_calls == 3);
    assert(sd_storage_service_get_handle() == NULL);
}

static void _test_explicit_format_recovery(void)
{
    fake_sd_adapter_t adapter = {0};
    const sd_storage_service_mount_ops_t ops =
    {
        .context = &adapter,
        .mount = _fake_mount,
        .unmount = _fake_unmount,
        .is_mounted = _fake_is_mounted,
    };

    assert(sd_storage_service_register_mount_ops(&ops) == ESP_OK);
    assert(sd_storage_service_recover_and_mount(&s_config) == ESP_OK);
    assert(adapter.mount_calls == 1);
    assert(adapter.format_requested);
    assert(sd_storage_service_deinit() == ESP_OK);
}

int main(void)
{
    _test_successful_lifecycle();
    _test_mount_rollback_cleanup_retry();
    _test_explicit_format_recovery();
    return 0;
}
