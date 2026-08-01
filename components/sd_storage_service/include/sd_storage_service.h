#ifndef __SD_STORAGE_SERVICE_H__
#define __SD_STORAGE_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Runtime SD mount settings supplied to the board adapter. */
typedef struct sd_storage_service_config
{
    const char *mount_path;
    int max_files;
    size_t allocation_unit_size;
} sd_storage_service_config_t;

/** @brief Mount behavior requested by the service lifecycle operation. */
typedef enum sd_storage_service_mount_mode
{
    SD_STORAGE_SERVICE_MOUNT_NORMAL = 0, /**< Never format after mount failure. */
    SD_STORAGE_SERVICE_MOUNT_RECOVER_FORMAT, /**< Allow explicit format recovery. */
} sd_storage_service_mount_mode_t;

/**
 * @brief Board adapter used by the independent SD storage service.
 *
 * The service deliberately does not own a board pin map.  The adapter may use
 * SDSPI, SDMMC, or another removable-media implementation while preserving
 * the same lifecycle and no-format-by-default policy.
 *
 * @note On error, a non-NULL out_handle requests an unmount cleanup attempt.
 *       The service retains that handle only when unmount also fails, then
 *       retries cleanup during deinitialization.
 */
typedef struct sd_storage_service_mount_ops
{
    void *context;
    esp_err_t (*mount)(void *context,
                       const sd_storage_service_config_t *config,
                       sd_storage_service_mount_mode_t mode,
                       void **out_handle);
    esp_err_t (*unmount)(void *context, void *handle);
    bool (*is_mounted)(void *context, void *handle);
} sd_storage_service_mount_ops_t;

/** @brief Register board mount operations before initialization. */
esp_err_t sd_storage_service_register_mount_ops(
    const sd_storage_service_mount_ops_t *ops);

/** @brief Start the service and mount without destructive recovery. */
esp_err_t sd_storage_service_init(const sd_storage_service_config_t *config);

/** @brief Explicitly mount with format-on-failure recovery enabled. */
esp_err_t sd_storage_service_recover_and_mount(
    const sd_storage_service_config_t *config);

/** @brief Unmount the card and release the board adapter resources. */
esp_err_t sd_storage_service_deinit(void);

/** @brief Explicit aliases for integrations that use start/stop terminology. */
esp_err_t sd_storage_service_start(const sd_storage_service_config_t *config);
esp_err_t sd_storage_service_stop(void);

/** @brief Return whether a filesystem is currently mounted. */
bool sd_storage_service_is_mounted(void);

/** @brief Return the configured VFS mount path. */
const char *sd_storage_service_get_mount_path(void);

/** @brief Return the board-owned adapter handle, or NULL when unmounted. */
void *sd_storage_service_get_handle(void);

/** @brief Copy the active service configuration. */
esp_err_t sd_storage_service_get_config(sd_storage_service_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* __SD_STORAGE_SERVICE_H__ */
