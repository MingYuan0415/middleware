#ifndef __SD_STORAGE_SERVICE_H__
#define __SD_STORAGE_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/** @brief Externally visible medium phase. */
typedef enum sd_storage_service_phase
{
    SD_STORAGE_SERVICE_PHASE_UNMOUNTED = 0, /**< Service ready, no filesystem. */
    SD_STORAGE_SERVICE_PHASE_MOUNTING,      /**< Mount transaction running. */
    SD_STORAGE_SERVICE_PHASE_MOUNTED,       /**< Filesystem usable. */
    SD_STORAGE_SERVICE_PHASE_UNMOUNTING,    /**< Unmount transaction running. */
    SD_STORAGE_SERVICE_PHASE_FORMATTING,    /**< Destructive format running. */
    SD_STORAGE_SERVICE_PHASE_ERROR,         /**< Last operation failed; retry allowed. */
} sd_storage_service_phase_t;

/**
 * @brief Board adapter used by the independent SD storage service.
 *
 * The service deliberately does not own a board pin map.  The adapter may use
 * SDSPI, SDMMC, or another removable-media implementation while preserving
 * the same lifecycle and no-format-by-default policy.
 *
 * @note On error, a non-NULL out_handle requests an unmount cleanup attempt.
 *       The service retains that handle only when unmount also fails, then
 *       retries cleanup on mount, unmount, or deinitialization.
 * @note format() is the explicit destructive path. The adapter must reformat
 *       the mounted medium and mount it again, and must return ESP_OK only
 *       when the resulting filesystem is usable again.
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
    esp_err_t (*format)(void *context, void *handle,
                        const sd_storage_service_config_t *config);
    esp_err_t (*get_space)(void *context, void *handle,
                           uint64_t *total_bytes, uint64_t *free_bytes);
} sd_storage_service_mount_ops_t;

/** @brief Point-in-time SD state with a transition generation. */
typedef struct sd_storage_service_snapshot
{
    bool mounted;                    /**< Whether a filesystem is currently usable. */
    sd_storage_service_phase_t phase; /**< Current medium phase. */
    uint32_t generation;             /**< Changes when the usable medium epoch changes. */
    uint32_t revision;               /**< Shared content revision across all consumers. */
    const char *mount_path;          /**< Active path while mounted; else NULL. */
    esp_err_t last_error;            /**< Result of the most recent operation. */
    uint32_t access_users;           /**< Live short-lived access leases. */
    uint32_t session_users;          /**< Live business-session leases. */
    bool maintenance;                /**< An exclusive maintenance lease is held. */
    bool faulted;                    /**< Medium I/O fault; remount required. */
} sd_storage_service_snapshot_t;

/** @brief Purpose of one volume usage lease. */
typedef enum sd_storage_service_usage_kind
{
    SD_STORAGE_SERVICE_USAGE_ACCESS = 0, /**< Short browse/query lease. */
    SD_STORAGE_SERVICE_USAGE_SESSION,    /**< Business session (record/play). */
    SD_STORAGE_SERVICE_USAGE_MAINTENANCE, /**< Exclusive destructive lease. */
} sd_storage_service_usage_kind_t;

/** @brief Generation-checked volume usage lease. */
typedef struct sd_storage_service_usage
{
    uint32_t token;      /**< Non-zero while held; zero is invalid. */
    uint32_t generation; /**< Mount generation at acquisition. */
} sd_storage_service_usage_t;

/** @brief Register board mount operations before initialization. */
esp_err_t sd_storage_service_register_mount_ops(
    const sd_storage_service_mount_ops_t *ops);

/** @brief Start the service and mount without destructive recovery. */
esp_err_t sd_storage_service_init(const sd_storage_service_config_t *config);

/**
 * @brief Explicitly mount with format-on-failure recovery enabled.
 *
 * Only a confirmed user action may reach this path: a plain mount failure is
 * never turned into a format.
 * A healthy mounted volume is left unchanged. Otherwise old adapter cleanup
 * must finish first, with no live lease. The expected snapshot generation is
 * checked atomically before reserving that lifecycle operation; a destructive
 * attempt invalidates the confirmation even if its later remount fails.
 */
esp_err_t sd_storage_service_recover_and_mount(
    const sd_storage_service_config_t *config, uint32_t expected_generation);

/**
 * @brief Safely unmount the card while keeping the service initialized.
 *
 * @param expected_generation is the snapshot epoch confirmed by the caller.
 * @return ESP_OK when unmounted (also on a repeated cleanup); stale epochs,
 *         active transitions, or live leases return ESP_ERR_INVALID_STATE.
 *         Adapter failures retain the resource handle for a later retry.
 */
esp_err_t sd_storage_service_unmount(uint32_t expected_generation);

/**
 * @brief Erase the mounted card and mount it again.
 *
 * Destructive: every user file on the medium is removed. Fails with
 * ESP_ERR_INVALID_STATE while any usage lease is held. A failed format keeps
 * the medium unusable and retains its adapter cleanup obligation for a later
 * mount, unmount, or deinit retry.
 *
 * @param config supplies the geometry to reformat with; NULL reuses the
 *               active configuration.
 * @param expected_generation is the snapshot epoch confirmed by the caller.
 *        Epoch validation and the no-live-leases check are atomic.
 */
esp_err_t sd_storage_service_format(
    const sd_storage_service_config_t *config, uint32_t expected_generation);

/**
 * @brief Read total and free bytes of the mounted filesystem.
 *
 * The caller must hold a valid ACCESS, SESSION, or MAINTENANCE lease for the
 * entire call. No additional lease slot is allocated; adapter I/O runs outside
 * the state lock, and the lease blocks lifecycle changes. A failed or stale
 * query leaves both outputs untouched.
 */
esp_err_t sd_storage_service_get_space(const sd_storage_service_usage_t *usage,
                                       uint64_t *total_bytes, uint64_t *free_bytes);

/** @brief Unmount the card and release the board adapter resources. */
esp_err_t sd_storage_service_deinit(void);

/** @brief Explicit aliases for integrations that use start/stop terminology. */
esp_err_t sd_storage_service_start(const sd_storage_service_config_t *config);
esp_err_t sd_storage_service_stop(void);

/** @brief Return whether a filesystem is currently mounted. */
bool sd_storage_service_is_mounted(void);

/** @brief Return the configured VFS mount path, or NULL while unmounted. */
const char *sd_storage_service_get_mount_path(void);

/**
 * @brief Read a consistent SD snapshot.
 *
 * The service has no card-detect signal on the current board. Mount, unmount,
 * format, and confirmed faults change the epoch; faults immediately clear
 * usability while keeping old lease tokens releasable.
 */
esp_err_t sd_storage_service_get_snapshot(
    sd_storage_service_snapshot_t *snapshot);

/** @brief Copy the active service configuration. */
esp_err_t sd_storage_service_get_config(sd_storage_service_config_t *config);

/** @brief Read the shared content revision without performing storage I/O. */
uint32_t sd_storage_service_content_revision(void);

/**
 * @brief Publish a successful file content or namespace change.
 * @param usage is a held ACCESS, SESSION, or MAINTENANCE lease.
 * @return ESP_OK after incrementing the revision; invalid or stale leases
 *         return ESP_ERR_INVALID_STATE (NULL returns ESP_ERR_INVALID_ARG).
 */
esp_err_t sd_storage_service_note_content_change(
    const sd_storage_service_usage_t *usage);

/**
 * @brief Acquire a volume usage lease.
 *
 * ACCESS and SESSION acquisitions are rejected once a maintenance lease is
 * held, while the medium is unmounted, or after a confirmed medium fault.
 * MAINTENANCE is granted only when no other lease is live, which closes the
 * "check busy, then act" race without doing any file I/O under the lock.
 *
 * @param kind selects the lease purpose.
 * @param usage receives the lease on success.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for a NULL output;
 *         ESP_ERR_INVALID_STATE when unusable or conflicting;
 *         ESP_ERR_NOT_SUPPORTED for an unknown kind.
 */
esp_err_t sd_storage_service_usage_acquire(
    sd_storage_service_usage_kind_t kind,
    sd_storage_service_usage_t *usage);

/**
 * @brief Release a volume usage lease.
 *
 * Original token+generation entries remain releasable after a fault invalidates
 * their epoch. Duplicate and already-released nonzero tokens are harmless.
 */
esp_err_t sd_storage_service_usage_release(
    const sd_storage_service_usage_t *usage);

/**
 * @brief Report a medium-level I/O fault for one lease.
 *
 * A report alone does not clear usability: one failed transfer is also what a
 * preemption or a marginal contact produces, and unmounting on the first one
 * turns a transient into a card the user has to remount by hand. The service
 * counts reports and only invalidates the medium once they keep arriving with
 * no successful access in between.
 *
 * A confirmed fault clears usability and advances the epoch. The reporting
 * consumer must still close its file handles and release its original lease
 * before lifecycle cleanup/remount can begin.
 * A report carrying a stale generation is ignored so a late failure cannot
 * poison a later mount.
 *
 * @param usage is the lease that observed the fault.
 * @param error is the failing operation result.
 */
esp_err_t sd_storage_service_usage_report_fault(
    const sd_storage_service_usage_t *usage, esp_err_t error);

/**
 * @brief Report one successful medium-level access for a lease.
 *
 * A successful read/directory/stat/open proves the card still responds. It
 * resets the fault-confirmation streak, so transient errors reported by
 * different consumers cannot accumulate into a false permanent fault.
 *
 * @param usage is the lease that observed the successful access.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for NULL/zero token;
 *         ESP_ERR_INVALID_STATE when the lease is stale.
 */
esp_err_t sd_storage_service_usage_report_success(
    const sd_storage_service_usage_t *usage);

#ifdef __cplusplus
}
#endif

#endif /* __SD_STORAGE_SERVICE_H__ */
