#ifndef __FILE_SERVICE_H__
#define __FILE_SERVICE_H__

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The cursor carries a stream offset so a resumed enumeration re-opens the
 * directory and seeks back to the saved position. */

/** @brief Maximum live browse sessions. */
#define FILE_SERVICE_MAX_SESSIONS 4U
/** @brief Entries returned by one directory batch. */
#define FILE_SERVICE_BATCH_ENTRIES 16U
/** @brief Bytes reserved for one UTF-8 path, including the terminator. */
#define FILE_SERVICE_PATH_BYTES 1024U
/** @brief Bytes reserved for one UTF-8 file name, including the terminator. */
#define FILE_SERVICE_NAME_BYTES 1021U

/** @brief Service tuning supplied at initialization. */
typedef struct file_service_config
{
    const char *mount_path; /**< Volume mount point, normally "/sdcard". */
    /**
     * @brief Relative path inside the volume exposed as the browse root.
     *
     * Empty exposes the volume root; a subdirectory limits every request to it.
     */
    const char *root_relative_path;
    /** Mount retry parameters; kept so a failed mount stays retryable. */
    int max_files;
    size_t allocation_unit_size;
} file_service_config_t;

/** @brief Volume ownership state published by the service. */
typedef enum file_service_volume_state
{
    FILE_SERVICE_VOLUME_UNAVAILABLE = 0, /**< No usable filesystem. */
    FILE_SERVICE_VOLUME_READY,           /**< Reads and deletes allowed. */
    FILE_SERVICE_VOLUME_BUSY,            /**< A business session owns the card. */
    FILE_SERVICE_VOLUME_MAINTENANCE,     /**< Unmount/format running. */
} file_service_volume_state_t;

/** @brief Asynchronous storage-control operation. */
typedef enum file_service_storage_action
{
    FILE_SERVICE_STORAGE_MOUNT = 0, /**< Re-mount; never formats. */
    FILE_SERVICE_STORAGE_RECOVER_FORMAT, /**< User-confirmed format recovery. */
    FILE_SERVICE_STORAGE_UNMOUNT,   /**< Safe unmount. */
    FILE_SERVICE_STORAGE_FORMAT,    /**< Erase the card and mount it again. */
    FILE_SERVICE_STORAGE_REFRESH_SPACE, /**< Re-read capacity only. */
} file_service_storage_action_t;

/** @brief Capacity snapshot; a failed query is never reported as zero. */
typedef struct file_service_space
{
    bool valid;           /**< Whether total/free carry a fresh reading. */
    uint64_t total_bytes; /**< Filesystem capacity when valid. */
    uint64_t free_bytes;  /**< Available bytes when valid. */
    esp_err_t last_error; /**< Query error while valid is false. */
} file_service_space_t;

/** @brief Entry kind reported by a directory batch. */
typedef enum file_service_entry_kind
{
    FILE_SERVICE_ENTRY_FILE = 0,
    FILE_SERVICE_ENTRY_DIRECTORY,
} file_service_entry_kind_t;

/** @brief One directory entry. */
typedef struct file_service_entry
{
    char name[FILE_SERVICE_NAME_BYTES]; /**< UTF-8 name without a path. */
    file_service_entry_kind_t kind;     /**< File or directory. */
    uint64_t size_bytes;                /**< Size for files; zero for dirs. */
    bool has_mtime;                     /**< Whether modified time is known. */
    uint32_t modified_epoch_s;          /**< UTC modified time when known. */
} file_service_entry_t;

/** @brief Batch position and outcome. */
typedef struct file_service_batch
{
    file_service_entry_t entries[FILE_SERVICE_BATCH_ENTRIES];
    size_t count;      /**< Entries in this batch. */
    bool has_more;     /**< More entries follow this batch. */
    uint32_t revision; /**< Content revision the batch was read at. */
    uint32_t generation; /**< Mount generation the batch was read at. */
} file_service_batch_t;

/** @brief Detail for one file or directory. */
typedef struct file_service_detail
{
    char name[FILE_SERVICE_NAME_BYTES];          /**< UTF-8 name. */
    char relative_path[FILE_SERVICE_PATH_BYTES]; /**< UTF-8 volume path. */
    file_service_entry_kind_t kind;
    uint64_t size_bytes;
    bool has_mtime;
    uint32_t modified_epoch_s;
    uint32_t revision;
    uint32_t generation;
} file_service_detail_t;

/** @brief Resume position of one directory enumeration. */
typedef struct file_service_cursor
{
    bool valid;           /**< False starts at the first entry. */
    bool files_pass;      /**< True once directories were fully returned. */
    int64_t offset;       /**< Stream position inside the current pass. */
    uint32_t generation;  /**< Mount generation the cursor belongs to. */
    uint32_t revision;    /**< Content revision the cursor belongs to. */
} file_service_cursor_t;

/** @brief Exact content epoch captured when the user selects an entry. */
typedef struct file_service_target
{
    char relative_path[FILE_SERVICE_PATH_BYTES];
    uint32_t generation;
    uint32_t revision;
} file_service_target_t;

/** @brief Stable error taxonomy; localized messages belong to the caller. */
typedef enum file_service_error_reason
{
    FILE_SERVICE_ERROR_NONE = 0,
    FILE_SERVICE_ERROR_BUSY,
    FILE_SERVICE_ERROR_STALE,
    FILE_SERVICE_ERROR_NOT_FOUND,
    FILE_SERVICE_ERROR_NOT_EMPTY,
    FILE_SERVICE_ERROR_PERMISSION,
    FILE_SERVICE_ERROR_PATH_TOO_LONG,
    FILE_SERVICE_ERROR_NO_SPACE,
    FILE_SERVICE_ERROR_IO,
    FILE_SERVICE_ERROR_CANCELLED,
    FILE_SERVICE_ERROR_INVALID,
    FILE_SERVICE_ERROR_NO_MEMORY,
    FILE_SERVICE_ERROR_UNAVAILABLE,
} file_service_error_reason_t;

/** @brief Request acceptance and terminal state. */
typedef enum file_service_result_state
{
    FILE_SERVICE_RESULT_NONE = 0, /**< No result published yet. */
    FILE_SERVICE_RESULT_ACCEPTED, /**< Queued; not started. */
    FILE_SERVICE_RESULT_RUNNING,  /**< Executing in the worker. */
    FILE_SERVICE_RESULT_DONE,     /**< Completed successfully. */
    FILE_SERVICE_RESULT_FAILED,   /**< Completed with an error. */
    FILE_SERVICE_RESULT_CANCELLED, /**< Dropped before completion. */
} file_service_result_state_t;

/** @brief Result of one request. */
typedef struct file_service_result
{
    file_service_result_state_t state;
    uint32_t operation_id; /**< Identifier returned by the request. */
    uint32_t session;      /**< Owning browse session, or 0 for storage. */
    uint32_t generation;   /**< Mount generation the result belongs to. */
    uint32_t revision;     /**< Content revision after the operation. */
    esp_err_t error;       /**< ESP_OK unless the request failed. */
    file_service_error_reason_t reason; /**< Structured failure classification. */
    int system_errno;                  /**< Saved backend errno, or zero. */
} file_service_result_t;

/** @brief Point-in-time service state. */
typedef struct file_service_snapshot
{
    file_service_volume_state_t volume; /**< Volume ownership state. */
    file_service_space_t space;         /**< Capacity reading. */
    uint32_t generation;                /**< Mount generation. */
    uint32_t revision;                  /**< Content revision. */
    uint32_t open_sessions;             /**< Live browse sessions. */
    bool busy;                          /**< Any request or maintenance running. */
    bool suspended;                     /**< Work admission closed. */
    uint32_t pending_requests;          /**< Requests queued or running. */
    file_service_result_t storage;      /**< Last storage-control result. */
    bool faulted;                      /**< Mirrors the SD owner's fault state. */
    esp_err_t last_error;               /**< Mirrors the SD owner's last error. */
} file_service_snapshot_t;

/**
 * @brief Initialize the service and its worker.
 *
 * The service starts even when no card is present, so a caller can always read
 * the snapshot and submit a mount retry. The initial mount is attempted without
 * formatting.
 *
 * @param config supplies the mount point and browse root.
 */
esp_err_t file_service_init(const file_service_config_t *config);

/**
 * @brief Stop the worker and release every session and buffer.
 *
 * @return ESP_ERR_TIMEOUT when the worker does not stop in time. Admission
 *         remains closed and resources retained; retry deinit to finish.
 */
esp_err_t file_service_deinit(void);

/**
 * @brief Close work admission and wait for in-flight requests to drain.
 *
 * Running work is not interrupted. Timeout leaves admission closed; the
 * caller explicitly resumes after a rolled-back standby handshake.
 */
esp_err_t file_service_suspend(uint32_t timeout_ms);

/** @brief Reopen work admission after wake or a rolled-back standby. */
esp_err_t file_service_resume(void);

/** @brief Read the service snapshot. */
esp_err_t file_service_get_snapshot(file_service_snapshot_t *snapshot);

/** @brief Report whether any request, delete, or maintenance is running. */
bool file_service_is_busy(void);

/**
 * @brief Submit a storage-control request.
 *
 * @param action selects mount, recovery mount, unmount, format, or refresh.
 * @param expected_generation is the snapshot epoch captured at user action.
 * @param result receives acceptance or a structured rejection reason.
 *
 * @return ESP_OK when accepted; ESP_ERR_INVALID_STATE when the queue is full,
 *         work admission is closed, or the action conflicts with the current
 *         volume state; ESP_ERR_NOT_SUPPORTED for an unknown action.
 */
esp_err_t file_service_request_storage(file_service_storage_action_t action,
                                       uint32_t expected_generation,
                                       file_service_result_t *result);

/**
 * @brief Open one browse session.
 *
 * @param session receives the session identifier.
 *
 * @return ESP_OK on success; ESP_ERR_NO_MEM when every session is in use.
 */
esp_err_t file_service_session_open(uint32_t *session);

/**
 * @brief Close a browse session and withdraw its published results.
 *
 * Pending work is cancelled, but its bounded slot stays reserved until lease
 * and pending settlement; a newly opened page cannot reuse an executing slot.
 */
esp_err_t file_service_session_close(uint32_t session);

/**
 * @brief Request one directory batch.
 *
 * @param session is an open browse session.
 * @param relative_path is the UTF-8 path inside the volume; "" is the root.
 * @param cursor is the position returned by the previous batch, or NULL to
 *               start at the beginning.
 * @param result receives the accepted operation identifier.
 */
esp_err_t file_service_session_list(uint32_t session,
                                    const char *relative_path,
                                    const file_service_cursor_t *cursor,
                                    file_service_result_t *result);

/** @brief Read detail only if the selected target's content epoch is current. */
esp_err_t file_service_session_detail(uint32_t session,
                                      const file_service_target_t *target,
                                      file_service_result_t *result);

/** @brief Delete a file or empty directory only at the selected content epoch. */
esp_err_t file_service_session_delete(uint32_t session,
                                      const file_service_target_t *target,
                                      file_service_result_t *result);

/** @brief Request cancellation of the session's in-flight request. */
esp_err_t file_service_session_cancel(uint32_t session);

/**
 * @brief Read the session's published result.
 *
 * A result stays readable until the session's next request replaces it or the
 * session is closed.
 */
esp_err_t file_service_session_get_result(uint32_t session,
        file_service_result_t *result);

/** @brief Copy the last published directory batch into caller-owned storage. */
esp_err_t file_service_session_get_batch(uint32_t session,
        file_service_batch_t *batch);

/** @brief Copy the last published detail into caller-owned storage. */
esp_err_t file_service_session_get_detail(uint32_t session,
        file_service_detail_t *detail);

/** @brief Read the position for the session's next directory batch. */
esp_err_t file_service_session_get_cursor(uint32_t session,
        file_service_cursor_t *cursor);


#ifdef __cplusplus
}
#endif

#endif /* __FILE_SERVICE_H__ */
