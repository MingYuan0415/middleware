#ifndef __RECORDER_SERVICE_H__
#define __RECORDER_SERVICE_H__

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    const char *directory;
    uint32_t max_duration_seconds;
    uint64_t minimum_free_bytes;
} recorder_service_config_t;

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
    uint64_t free_bytes;
    char name[64];
    esp_err_t last_error;
} recorder_service_snapshot_t;

#define RECORDER_SERVICE_MAX_FILES 8U

/** @brief Bounded metadata for one completed WAV recording. */
typedef struct recorder_service_file
{
    char name[64];
    uint32_t duration_ms;
    uint64_t bytes;
} recorder_service_file_t;

esp_err_t recorder_service_init(const recorder_service_config_t *config);
esp_err_t recorder_service_deinit(void);
esp_err_t recorder_service_get_snapshot(recorder_service_snapshot_t *snapshot);
/** @brief Enumerate completed WAV files without exposing directory handles. */
esp_err_t recorder_service_list(recorder_service_file_t *files,
                                size_t capacity, size_t *count);
bool recorder_service_is_busy(void);
/** @brief Queue a recording command; completion is reported in the snapshot. */
esp_err_t recorder_service_start(void);
esp_err_t recorder_service_pause(void);
esp_err_t recorder_service_resume(void);
esp_err_t recorder_service_stop(void);
esp_err_t recorder_service_play(const char *name);
esp_err_t recorder_service_stop_playback(void);
esp_err_t recorder_service_delete(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __RECORDER_SERVICE_H__ */
