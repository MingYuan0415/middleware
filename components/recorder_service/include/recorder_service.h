#ifndef __RECORDER_SERVICE_H__
#define __RECORDER_SERVICE_H__

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bytes reserved for one full backend path, including the terminator. */
#define RECORDER_SERVICE_PATH_BYTES 192U

typedef enum
{
    RECORDER_SERVICE_IDLE = 0,
    RECORDER_SERVICE_RECORDING,
    RECORDER_SERVICE_PAUSED,
    RECORDER_SERVICE_PLAYING,
    RECORDER_SERVICE_ERROR,
} recorder_service_state_t;

typedef struct recorder_service_config
{
    /**
     * @brief Recording directory as a UTF-8 path relative to the volume root.
     *
     * "MicroTech/Recordings", for example. The service resolves the active
     * mount point through sd_storage_service on every use, so a remount to a
     * different path keeps working without reconfiguration.
     */
    const char *directory;
    uint32_t max_duration_seconds;
    uint64_t minimum_free_bytes;
} recorder_service_config_t;

/** @brief Exact storage epoch and full backend path selected from the index. */
typedef struct recorder_service_target
{
    char name[RECORDER_SERVICE_PATH_BYTES]; /**< Full backend UTF-8 path. */
    uint32_t generation; /**< Mount generation of the published metadata. */
    uint32_t revision; /**< Content revision of the published metadata. */
} recorder_service_target_t;
typedef struct recorder_service_snapshot
{
    uint32_t generation;
    uint32_t operation_id;
    bool operation_pending;
    recorder_service_state_t state;
    uint32_t duration_ms;
    uint32_t playback_position_ms;
    uint32_t playback_duration_ms;
    uint64_t bytes;
    uint64_t free_bytes;     /**< Remaining bytes when space_valid is true. */
    bool space_valid;        /**< Whether the capacity query succeeded. */
    esp_err_t space_error;   /**< Capacity query error while space_valid is false. */
    bool storage_ready;      /**< Whether a usable volume is mounted. */
    uint32_t storage_generation; /**< Current mount generation. */
    uint32_t content_revision; /**< Current storage content revision. */
    uint32_t index_generation; /**< Published index mount generation. */
    uint32_t index_revision; /**< Published index content revision. */
    bool index_valid; /**< Whether a complete index has been published. */
    size_t file_count; /**< Recordings published in the current index. */
    bool library_truncated; /**< The medium holds more recordings than the index. */
    uint32_t max_duration_ms; /**< Configured recording limit. */
    char name[RECORDER_SERVICE_PATH_BYTES]; /**< Full backend path of the file. */
    esp_err_t last_error;
} recorder_service_snapshot_t;

/** @brief Recordings retained by one published index, newest first. */
#define RECORDER_SERVICE_MAX_FILES 32U

/** @brief Bounded metadata for one completed WAV recording. */
typedef struct recorder_service_file
{
    recorder_service_target_t target; /**< Target captured with this metadata. */
    uint32_t duration_ms;
    uint64_t bytes;
    bool has_mtime;                     /**< Whether modified time is known. */
    uint32_t modified_epoch_s;          /**< UTC modified time when known. */
} recorder_service_file_t;

/**
 * @brief Initialize the recorder control service.
 *
 * The service initializes without a mounted card: recording is simply not
 * startable until the volume is ready. The recording directory is prepared by
 * the worker after a successful mount, never during initialization.
 */
esp_err_t recorder_service_init(const recorder_service_config_t *config);
esp_err_t recorder_service_deinit(void);
esp_err_t recorder_service_get_snapshot(recorder_service_snapshot_t *snapshot);
/**
 * @brief Copy cached completed WAV metadata without filesystem I/O.
 *
 * A stale mount/content index returns an empty list and requests an asynchronous
 * worker refresh. Device and host builds share this nonblocking read contract.
 */
esp_err_t recorder_service_list(recorder_service_file_t *files,
                                size_t capacity, size_t *count);
/**
 * @brief Ask the worker to republish a stale index.
 *
 * Non-consuming counterpart of recorder_service_list() for callers that only
 * read snapshot counters. Requests coalesce; a fresh index returns ESP_OK
 * without queueing work.
 */
esp_err_t recorder_service_refresh(void);
bool recorder_service_is_busy(void);
/** @brief Queue a recording command; completion is reported in the snapshot. */
esp_err_t recorder_service_start(void);
esp_err_t recorder_service_pause(void);
esp_err_t recorder_service_resume(void);
esp_err_t recorder_service_stop(void);
/** @brief Queue playback of an exact target; stale epochs fail before open. */
esp_err_t recorder_service_play(const recorder_service_target_t *target);
esp_err_t recorder_service_stop_playback(void);
/** @brief Queue deletion; ESP_OK is admission, not terminal success. */
esp_err_t recorder_service_delete(const recorder_service_target_t *target);

#ifdef __cplusplus
}
#endif

#endif /* __RECORDER_SERVICE_H__ */
