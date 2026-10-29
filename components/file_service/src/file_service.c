#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "file_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_storage_service.h"

#define FILE_SERVICE_QUEUE_DEPTH (FILE_SERVICE_MAX_SESSIONS + 1U)
#define FILE_SERVICE_TASK_STACK 6144U
#define FILE_SERVICE_STOP_TIMEOUT_MS 5000U

/* Slots and all large buffers live in one bounded, init-time PSRAM allocation.
 * Only the worker touches scratch. Readers copy the separate publication. */
typedef enum request_type
{
    REQUEST_LIST,
    REQUEST_DETAIL,
    REQUEST_DELETE,
    REQUEST_STORAGE,
} request_type_t;

typedef struct request_slot
{
    request_type_t type;
    bool active;
    bool cancelled;
    bool mutation_started;
    uint32_t owner;
    uint32_t operation;
    uint32_t generation;
    uint32_t revision;
    file_service_storage_action_t action;
    file_service_cursor_t cursor;
    char relative_path[FILE_SERVICE_PATH_BYTES];
    sd_storage_service_usage_t usage;
    file_service_result_t result;
} request_slot_t;

typedef struct browse_session
{
    bool open;
    bool has_batch;
    bool has_detail;
    uint32_t id;
    request_slot_t slot;
    file_service_cursor_t cursor;
    file_service_batch_t batch;
    file_service_detail_t detail;
} browse_session_t;

typedef struct worker_workspace
{
    char path[FILE_SERVICE_PATH_BYTES];
    char child[FILE_SERVICE_PATH_BYTES];
#ifndef ESP_PLATFORM
    /* POSIX realpath requires PATH_MAX, not the smaller public path budget. */
    char canonical[PATH_MAX];
#endif
    file_service_batch_t batch;
    file_service_detail_t detail;
    file_service_cursor_t cursor;
    file_service_space_t space;
    uint32_t space_generation;
    bool space_changed;
} worker_workspace_t;

typedef struct file_pool
{
    browse_session_t sessions[FILE_SERVICE_MAX_SESSIONS];
    request_slot_t storage;
    worker_workspace_t work;
} file_pool_t;

static struct
{
    bool initialized;
    bool suspended;
    bool stopping;
    uint32_t pending;
    char mount_path[FILE_SERVICE_PATH_BYTES];
    char root_path[FILE_SERVICE_PATH_BYTES];
    sd_storage_service_config_t config;
    file_service_space_t space;
    uint32_t space_generation;
    file_pool_t *pool;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t exited;
    QueueHandle_t queue;
    TaskHandle_t task;
} s_files;
/* IDs deliberately survive service reinitialization and never encode an epoch. */
static uint32_t s_next_session;
static uint32_t s_next_operation;

static void _lock(void)
{
    (void)xSemaphoreTake(s_files.lock, portMAX_DELAY);
}

static void _unlock(void)
{
    (void)xSemaphoreGive(s_files.lock);
}

static uint32_t _next_id(uint32_t *counter)
{
    if (++*counter == 0U) ++*counter;
    return *counter;
}

static browse_session_t *_session(uint32_t id)
{
    for (size_t n = 0U; n < FILE_SERVICE_MAX_SESSIONS; ++n)
    {
        browse_session_t *session = &s_files.pool->sessions[n];
        if (session->open && session->id == id) return session;
    }
    return NULL;
}

static file_service_error_reason_t _errno_reason(int error)
{
    switch (error)
    {
    case ENOENT:
    case ENOTDIR:
        return FILE_SERVICE_ERROR_NOT_FOUND;
    case ENOTEMPTY:
    case EEXIST:
        return FILE_SERVICE_ERROR_NOT_EMPTY;
    case EACCES:
    case EPERM:
    case EROFS:
        return FILE_SERVICE_ERROR_PERMISSION;
    case ENAMETOOLONG:
        return FILE_SERVICE_ERROR_PATH_TOO_LONG;
    case ENOSPC:
        return FILE_SERVICE_ERROR_NO_SPACE;
    case ENOMEM:
        return FILE_SERVICE_ERROR_NO_MEMORY;
    case EINVAL:
    case ELOOP:
        return FILE_SERVICE_ERROR_INVALID;
    default:
        return FILE_SERVICE_ERROR_IO;
    }
}

static esp_err_t _reason_error(file_service_error_reason_t reason)
{
    switch (reason)
    {
    case FILE_SERVICE_ERROR_NONE:
        return ESP_OK;
    case FILE_SERVICE_ERROR_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case FILE_SERVICE_ERROR_PATH_TOO_LONG:
        return ESP_ERR_INVALID_SIZE;
    case FILE_SERVICE_ERROR_NO_MEMORY:
        return ESP_ERR_NO_MEM;
    case FILE_SERVICE_ERROR_INVALID:
        return ESP_ERR_INVALID_ARG;
    case FILE_SERVICE_ERROR_BUSY:
    case FILE_SERVICE_ERROR_STALE:
    case FILE_SERVICE_ERROR_CANCELLED:
    case FILE_SERVICE_ERROR_NOT_EMPTY:
    case FILE_SERVICE_ERROR_UNAVAILABLE:
        return ESP_ERR_INVALID_STATE;
    default:
        return ESP_FAIL;
    }
}

static file_service_error_reason_t _esp_reason(esp_err_t error)
{
    switch (error)
    {
    case ESP_OK:
        return FILE_SERVICE_ERROR_NONE;
    case ESP_ERR_NO_MEM:
        return FILE_SERVICE_ERROR_NO_MEMORY;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_NOT_SUPPORTED:
        return FILE_SERVICE_ERROR_INVALID;
    case ESP_ERR_INVALID_SIZE:
        return FILE_SERVICE_ERROR_PATH_TOO_LONG;
    case ESP_ERR_NOT_FOUND:
        return FILE_SERVICE_ERROR_NOT_FOUND;
    case ESP_ERR_INVALID_STATE:
    case ESP_ERR_TIMEOUT:
        return FILE_SERVICE_ERROR_BUSY;
    default:
        return FILE_SERVICE_ERROR_IO;
    }
}

static esp_err_t _reject(file_service_result_t *result, esp_err_t error,
                         file_service_error_reason_t reason)
{
    if (result != NULL)
    {
        memset(result, 0, sizeof(*result));
        result->state = FILE_SERVICE_RESULT_FAILED;
        result->error = error;
        result->reason = reason;
    }
    return error;
}

static esp_err_t _check_relative(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    const size_t length = strnlen(path, FILE_SERVICE_PATH_BYTES);
    if (length == FILE_SERVICE_PATH_BYTES) return ESP_ERR_INVALID_SIZE;
    if (length == 0U) return ESP_OK;
    if (path[0] == '/' || path[length - 1U] == '/') return ESP_ERR_INVALID_ARG;
    const char *part = path;
    while (*part != '\0')
    {
        const char *end = strchr(part, '/');
        const size_t size = end != NULL ? (size_t)(end - part) : strlen(part);
        if (size == 0U || (size == 1U && part[0] == '.') ||
                (size == 2U && part[0] == '.' && part[1] == '.') ||
                memchr(part, '\\', size) != NULL) return ESP_ERR_INVALID_ARG;
        if (size >= FILE_SERVICE_NAME_BYTES) return ESP_ERR_INVALID_SIZE;
        if (end == NULL) break;
        part = end + 1;
    }
    return ESP_OK;
}

/* Validate every component, including the configured root. Even symlinks
 * resolving within the root are forbidden: unlink must never target an alias. */
static esp_err_t _check_backend(char *path, int *system_error)
{
#ifndef ESP_PLATFORM
    struct stat info;
    for (char *end = path + 1; ; ++end)
    {
        if (*end != '/' && *end != '\0') continue;
        const char saved = *end;
        *end = '\0';
        const int status = lstat(path, &info);
        const int error = status != 0 ? errno : 0;
        *end = saved;
        if (status != 0)
        {
            *system_error = error;
            return _reason_error(_errno_reason(error));
        }
        if (S_ISLNK(info.st_mode)) return ESP_ERR_INVALID_ARG;
        if (saved == '\0') break;
    }
    char *canonical = s_files.pool->work.canonical;
    if (realpath(path, canonical) == NULL)
    {
        *system_error = errno;
        return _reason_error(_errno_reason(*system_error));
    }
    if (strlen(canonical) >= FILE_SERVICE_PATH_BYTES) return ESP_ERR_INVALID_SIZE;
#else
    (void)path;
    (void)system_error;
#endif
    return ESP_OK;
}

static esp_err_t _path(request_slot_t *slot, int *system_error)
{
    char *path = s_files.pool->work.path;
    int length = slot->relative_path[0] == '\0' ?
                 snprintf(path, FILE_SERVICE_PATH_BYTES, "%s", s_files.root_path) :
                 snprintf(path, FILE_SERVICE_PATH_BYTES, "%s/%s", s_files.root_path,
                          slot->relative_path);
    if (length < 0 || (size_t)length >= FILE_SERVICE_PATH_BYTES)
        return ESP_ERR_INVALID_SIZE;
    return _check_backend(path, system_error);
}

static esp_err_t _child_stat(const char *directory, const char *name,
                             struct stat *info, int *system_error)
{
    if (strlen(name) >= FILE_SERVICE_NAME_BYTES) return ESP_ERR_INVALID_SIZE;
    char *child = s_files.pool->work.child;
    int length = snprintf(child, FILE_SERVICE_PATH_BYTES, "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= FILE_SERVICE_PATH_BYTES)
        return ESP_ERR_INVALID_SIZE;
#ifdef ESP_PLATFORM
    const int status = stat(child, info);
#else
    const int status = lstat(child, info);
#endif
    if (status != 0)
    {
        *system_error = errno;
        return _reason_error(_errno_reason(*system_error));
    }
#ifndef ESP_PLATFORM
    if (S_ISLNK(info->st_mode)) return ESP_ERR_INVALID_ARG;
#endif
    if (!S_ISDIR(info->st_mode) && !S_ISREG(info->st_mode)) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static bool _cancelled(request_slot_t *slot)
{
    _lock();
    const bool cancelled = slot->cancelled;
    _unlock();
    return cancelled;
}

static esp_err_t _list(request_slot_t *slot, int *system_error)
{
    worker_workspace_t *work = &s_files.pool->work;
    file_service_batch_t *batch = &work->batch;
    memset(batch, 0, sizeof(*batch));
    esp_err_t error = _path(slot, system_error);
    if (error != ESP_OK) return error;
    DIR *directory = opendir(work->path);
    if (directory == NULL)
    {
        *system_error = errno;
        return _reason_error(_errno_reason(*system_error));
    }
    bool files_pass = slot->cursor.valid && slot->cursor.files_pass;
    long offset = slot->cursor.valid ? (long)slot->cursor.offset : 0L;
    if (offset > 0L) seekdir(directory, offset);
    bool finished = false;
    bool has_more = false;
    while (batch->count < FILE_SERVICE_BATCH_ENTRIES)
    {
        if (_cancelled(slot)) break;
        errno = 0;
        const long position = telldir(directory);
        if (position < 0L)
        {
            *system_error = errno;
            error = _reason_error(_errno_reason(*system_error));
            break;
        }
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL)
        {
            const int saved = errno;
            if (saved != 0)
            {
                *system_error = saved;
                error = _reason_error(_errno_reason(saved));
                break;
            }
            if (!files_pass)
            {
                files_pass = true;
                rewinddir(directory);
                offset = 0L;
                continue;
            }
            finished = true;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        struct stat info;
        error = _child_stat(work->path, entry->d_name, &info, system_error);
        if (error != ESP_OK) break;
        const bool is_directory = S_ISDIR(info.st_mode);
        if (is_directory == files_pass) continue;
        file_service_entry_t *record = &batch->entries[batch->count++];
        strcpy(record->name, entry->d_name);
        record->kind = is_directory ? FILE_SERVICE_ENTRY_DIRECTORY : FILE_SERVICE_ENTRY_FILE;
        record->size_bytes = !is_directory && info.st_size > 0 ? (uint64_t)info.st_size : 0U;
        record->has_mtime = info.st_mtime > 0;
        record->modified_epoch_s = info.st_mtime > 0 ? (uint32_t)info.st_mtime : 0U;
        errno = 0;
        offset = telldir(directory);
        if (offset < 0L)
        {
            *system_error = errno;
            error = _reason_error(_errno_reason(*system_error));
            break;
        }
    }
    if (batch->count == FILE_SERVICE_BATCH_ENTRIES && error == ESP_OK &&
            !finished)
    {
        for (;;)
        {
            if (_cancelled(slot)) break;
            errno = 0;
            const long position = telldir(directory);
            if (position < 0L)
            {
                *system_error = errno;
                error = _reason_error(_errno_reason(*system_error));
                break;
            }
            errno = 0;
            struct dirent *entry = readdir(directory);
            if (entry == NULL)
            {
                const int saved = errno;
                if (saved != 0)
                {
                    *system_error = saved;
                    error = _reason_error(_errno_reason(saved));
                    break;
                }
                if (!files_pass)
                {
                    files_pass = true;
                    rewinddir(directory);
                    offset = 0L;
                    continue;
                }
                finished = true;
                break;
            }
            if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }
            struct stat info;
            error = _child_stat(work->path, entry->d_name, &info,
                                system_error);
            if (error != ESP_OK) break;
            if (S_ISDIR(info.st_mode) == files_pass) continue;
            offset = position;
            has_more = true;
            break;
        }
    }
    const int close_status = closedir(directory);
    if (close_status != 0 && error == ESP_OK)
    {
        *system_error = errno;
        error = _reason_error(_errno_reason(*system_error));
    }
    if (batch->count < FILE_SERVICE_BATCH_ENTRIES && error == ESP_OK)
    {
        has_more = !finished;
    }
    batch->has_more = has_more;
    batch->generation = slot->generation;
    batch->revision = slot->revision;
    work->cursor = (file_service_cursor_t)
    {
        .valid = true, .files_pass = files_pass,
        .offset = offset, .generation = slot->generation, .revision = slot->revision
    };
    return error;
}

static esp_err_t _detail(request_slot_t *slot, int *system_error)
{
    if (slot->relative_path[0] == '\0') return ESP_ERR_INVALID_ARG;
    esp_err_t error = _path(slot, system_error);
    if (error != ESP_OK) return error;
    struct stat info;
    if (stat(s_files.pool->work.path, &info) != 0)
    {
        *system_error = errno;
        return _reason_error(_errno_reason(*system_error));
    }
    if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) return ESP_ERR_INVALID_ARG;
    file_service_detail_t *detail = &s_files.pool->work.detail;
    memset(detail, 0, sizeof(*detail));
    const char *name = strrchr(slot->relative_path, '/');
    strcpy(detail->name, name != NULL ? name + 1 : slot->relative_path);
    strcpy(detail->relative_path, slot->relative_path);
    detail->kind = S_ISDIR(info.st_mode) ? FILE_SERVICE_ENTRY_DIRECTORY : FILE_SERVICE_ENTRY_FILE;
    detail->size_bytes = S_ISREG(info.st_mode) && info.st_size > 0 ? (uint64_t)info.st_size : 0U;
    detail->has_mtime = info.st_mtime > 0;
    detail->modified_epoch_s = info.st_mtime > 0 ? (uint32_t)info.st_mtime : 0U;
    detail->generation = slot->generation;
    detail->revision = slot->revision;
    return ESP_OK;
}

/* Some FAT adapters use EACCES for a nonempty directory. Distinguish it from
 * permission failure without overwriting the errno of the failed deletion. */
static bool _directory_nonempty(const char *path)
{
    DIR *directory = opendir(path);
    if (directory == NULL) return false;
    bool nonempty = false;
    for (;;)
    {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) break;
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
        {
            nonempty = true;
            break;
        }
    }
    (void)closedir(directory);
    return nonempty;
}

static esp_err_t _delete(request_slot_t *slot, int *system_error,
                         file_service_error_reason_t *reason)
{
    if (slot->relative_path[0] == '\0') return ESP_ERR_INVALID_ARG;
    esp_err_t error = _path(slot, system_error);
    if (error != ESP_OK) return error;
    struct stat info;
    const char *path = s_files.pool->work.path;
    if (stat(path, &info) != 0)
    {
        *system_error = errno;
        return _reason_error(_errno_reason(*system_error));
    }
    const bool is_directory = S_ISDIR(info.st_mode);
    if (!is_directory && !S_ISREG(info.st_mode)) return ESP_ERR_INVALID_ARG;
    _lock();
    const bool cancelled = slot->cancelled;
    if (!cancelled) slot->mutation_started = true;
    _unlock();
    if (cancelled)
    {
        *reason = FILE_SERVICE_ERROR_CANCELLED;
        return ESP_ERR_INVALID_STATE;
    }
    if ((is_directory ? rmdir(path) : unlink(path)) != 0)
    {
        *system_error = errno;
        *reason = _errno_reason(*system_error);
        if (is_directory && *system_error == EACCES && _directory_nonempty(path))
            *reason = FILE_SERVICE_ERROR_NOT_EMPTY;
        return _reason_error(*reason);
    }
    return sd_storage_service_note_content_change(&slot->usage);
}

static void _space(request_slot_t *slot, bool check_generation,
                   int *system_error)
{
    worker_workspace_t *work = &s_files.pool->work;
    memset(&work->space, 0, sizeof(work->space));
    work->space_changed = true;
    esp_err_t error = sd_storage_service_usage_acquire(
                          SD_STORAGE_SERVICE_USAGE_ACCESS, &slot->usage);
    work->space_generation = slot->usage.generation;
    if (error == ESP_OK && check_generation &&
            slot->usage.generation != slot->generation)
    {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK)
    {
        errno = 0;
        error = sd_storage_service_get_space(&slot->usage,
                                             &work->space.total_bytes,
                                             &work->space.free_bytes);
        if (error != ESP_OK)
        {
            *system_error = errno;
        }
    }
    work->space.valid = error == ESP_OK;
    work->space.last_error = error;
}

static esp_err_t _storage(request_slot_t *slot, int *system_error)
{
    esp_err_t error;
    switch (slot->action)
    {
    case FILE_SERVICE_STORAGE_MOUNT:
        error = sd_storage_service_init(&s_files.config);
        break;
    case FILE_SERVICE_STORAGE_RECOVER_FORMAT:
        error = sd_storage_service_recover_and_mount(&s_files.config,
            slot->generation);
        break;
    case FILE_SERVICE_STORAGE_UNMOUNT:
        error = sd_storage_service_unmount(slot->generation);
        break;
    case FILE_SERVICE_STORAGE_FORMAT:
        error = sd_storage_service_format(&s_files.config, slot->generation);
        break;
    case FILE_SERVICE_STORAGE_REFRESH_SPACE:
        _space(slot, true, system_error);
        return s_files.pool->work.space.last_error;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (error == ESP_OK && slot->action != FILE_SERVICE_STORAGE_UNMOUNT)
    {
        _space(slot, false, system_error);
        error = s_files.pool->work.space.last_error;
    }
    else
    {
        s_files.pool->work.space_changed = true;
        s_files.pool->work.space = (file_service_space_t)
        {
            .last_error = error == ESP_OK ? ESP_ERR_INVALID_STATE : error
        };
    }
    return error;
}


/* The single terminal path owns both obligations. Release never happens under
 * the state mutex; active retains the slot until release and publication end. */
static void _finish(request_slot_t *slot, esp_err_t error,
                    file_service_error_reason_t reason, int system_error,
                    bool publish_scratch)
{
    if (slot->usage.token != 0U)
    {
        if (system_error == EIO)
            (void)sd_storage_service_usage_report_fault(&slot->usage, error);
        else if (error == ESP_OK)
            (void)sd_storage_service_usage_report_success(&slot->usage);
        (void)sd_storage_service_usage_release(&slot->usage);
        slot->usage.token = 0U;
    }
    sd_storage_service_snapshot_t storage = {0};
    (void)sd_storage_service_get_snapshot(&storage);
    _lock();
    if (slot->active)
    {
        if (slot->cancelled && !slot->mutation_started)
        {
            error = ESP_ERR_INVALID_STATE;
            reason = FILE_SERVICE_ERROR_CANCELLED;
        }
        if (error == ESP_OK && (slot->type == REQUEST_LIST || slot->type == REQUEST_DETAIL) &&
                (storage.generation != slot->generation || storage.revision != slot->revision))
        {
            error = ESP_ERR_INVALID_STATE;
            reason = FILE_SERVICE_ERROR_STALE;
        }
        slot->result.state = reason == FILE_SERVICE_ERROR_CANCELLED ?
                             FILE_SERVICE_RESULT_CANCELLED : error == ESP_OK ?
                             FILE_SERVICE_RESULT_DONE : FILE_SERVICE_RESULT_FAILED;
        slot->result.error = error;
        slot->result.reason = reason;
        slot->result.system_errno = system_error;
        slot->result.generation = storage.generation;
        slot->result.revision = storage.revision;
        if (slot->type == REQUEST_STORAGE)
        {
            if (publish_scratch && s_files.pool->work.space_changed)
            {
                s_files.space = s_files.pool->work.space;
                s_files.space_generation = s_files.pool->work.space_generation;
            }
        }
        else
        {
            browse_session_t *session = _session(slot->owner);
            if (session != NULL && &session->slot == slot &&
                    session->slot.operation == slot->operation &&
                    error == ESP_OK && publish_scratch)
            {
                if (slot->type == REQUEST_LIST)
                {
                    session->batch = s_files.pool->work.batch;
                    session->cursor = s_files.pool->work.cursor;
                    session->has_batch = true;
                }
                else if (slot->type == REQUEST_DETAIL)
                {
                    session->detail = s_files.pool->work.detail;
                    session->has_detail = true;
                }
            }
        }
        slot->active = false;
        --s_files.pending;
    }
    _unlock();
}

static void _execute(request_slot_t *slot)
{
    _lock();
    const bool cancelled = slot->cancelled;
    slot->result.state = FILE_SERVICE_RESULT_RUNNING;
    _unlock();
    if (cancelled)
    {
        _finish(slot, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_CANCELLED, 0, false);
        return;
    }
    worker_workspace_t *work = &s_files.pool->work;
    work->space_changed = false;
    sd_storage_service_snapshot_t storage = {0};
    (void)sd_storage_service_get_snapshot(&storage);
    if (slot->generation != storage.generation ||
            (slot->type != REQUEST_STORAGE && slot->revision != storage.revision))
    {
        _finish(slot, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_STALE, 0, false);
        return;
    }
    esp_err_t error;
    int system_error = 0;
    file_service_error_reason_t reason = FILE_SERVICE_ERROR_NONE;
    if (slot->type == REQUEST_STORAGE)
        error = _storage(slot, &system_error);
    else
    {
        error = sd_storage_service_usage_acquire(slot->type == REQUEST_DELETE ?
            SD_STORAGE_SERVICE_USAGE_MAINTENANCE : SD_STORAGE_SERVICE_USAGE_ACCESS,
            &slot->usage);
        if (error == ESP_OK)
        {
            (void)sd_storage_service_get_snapshot(&storage);
            if (slot->usage.generation != slot->generation ||
                    storage.generation != slot->generation ||
                    storage.revision != slot->revision)
            {
                error = ESP_ERR_INVALID_STATE;
                reason = FILE_SERVICE_ERROR_STALE;
            }
            else if (slot->type == REQUEST_LIST)
            {
                error = _list(slot, &system_error);
            }
            else if (slot->type == REQUEST_DETAIL)
            {
                error = _detail(slot, &system_error);
            }
            else
            {
                error = _delete(slot, &system_error, &reason);
            }
        }
        else
        {
            (void)sd_storage_service_get_snapshot(&storage);
            if (storage.faulted)
            {
                reason = FILE_SERVICE_ERROR_IO;
            }
            else if (!storage.mounted)
            {
                reason = FILE_SERVICE_ERROR_UNAVAILABLE;
            }
            else
            {
                reason = FILE_SERVICE_ERROR_BUSY;
            }
        }
    }
    if (error == ESP_ERR_INVALID_STATE && reason == FILE_SERVICE_ERROR_NONE)
    {
        (void)sd_storage_service_get_snapshot(&storage);
        if (storage.generation != slot->generation ||
                (slot->type != REQUEST_STORAGE && storage.revision != slot->revision))
            reason = FILE_SERVICE_ERROR_STALE;
        else if (slot->type == REQUEST_STORAGE &&
                 slot->action == FILE_SERVICE_STORAGE_REFRESH_SPACE &&
                 slot->usage.token == 0U)
        {
            reason = storage.faulted ? FILE_SERVICE_ERROR_IO :
                     !storage.mounted ? FILE_SERVICE_ERROR_UNAVAILABLE :
                     FILE_SERVICE_ERROR_BUSY;
        }
    }
    if (error != ESP_OK && reason == FILE_SERVICE_ERROR_NONE)
        reason = system_error != 0 ? _errno_reason(system_error) : _esp_reason(error);
    _finish(slot, error, reason, system_error, true);
}

static void _worker(void *argument)
{
    (void)argument;
    for (;;)
    {
        request_slot_t *slot = NULL;
        if (xQueueReceive(s_files.queue, &slot, pdMS_TO_TICKS(50U)) == pdTRUE)
            _execute(slot);
        _lock();
        const bool stop = s_files.stopping && s_files.pending == 0U;
        _unlock();
        if (stop) break;
    }
    /* No runtime state is accessed after this completion handshake. */
    SemaphoreHandle_t exited = s_files.exited;
    (void)xSemaphoreGive(exited);
    for (;;)
    {
        vTaskDelay(portMAX_DELAY);
    }
}

static void _free_resources(void)
{
    if (s_files.queue != NULL) vQueueDelete(s_files.queue);
    if (s_files.exited != NULL) vSemaphoreDelete(s_files.exited);
    if (s_files.lock != NULL) vSemaphoreDelete(s_files.lock);
    heap_caps_free(s_files.pool);
    memset(&s_files, 0, sizeof(s_files));
}

/* Caller holds the state lock. Queue send is a nonblocking pointer copy, not
 * file I/O. Keeping it inside prevents close/reopen from racing queue failure. */
static bool _accept(request_slot_t *slot, file_service_result_t *result)
{
    slot->operation = _next_id(&s_next_operation);
    slot->active = true;
    slot->cancelled = false;
    slot->mutation_started = false;
    slot->usage.token = 0U;
    slot->result = (file_service_result_t)
    {
        .state = FILE_SERVICE_RESULT_ACCEPTED,
        .operation_id = slot->operation, .session = slot->owner,
        .generation = slot->generation, .revision = slot->revision
    };
    ++s_files.pending;
    if (result != NULL) *result = slot->result;
    return xQueueSend(s_files.queue, &slot, 0U) == pdTRUE;
}

esp_err_t file_service_init(const file_service_config_t *config)
{
    if (config == NULL || config->mount_path == NULL || config->mount_path[0] != '/' ||
            strlen(config->mount_path) >= FILE_SERVICE_PATH_BYTES ||
            (config->root_relative_path != NULL &&
             _check_relative(config->root_relative_path) != ESP_OK))
        return ESP_ERR_INVALID_ARG;
    if (s_files.initialized) return ESP_ERR_INVALID_STATE;
    memset(&s_files, 0, sizeof(s_files));
    strcpy(s_files.mount_path, config->mount_path);
    size_t length = strlen(s_files.mount_path);
    while (length > 1U && s_files.mount_path[length - 1U] == '/')
        s_files.mount_path[--length] = '\0';
    const char *relative = config->root_relative_path;
    int count = relative == NULL || relative[0] == '\0' ?
                snprintf(s_files.root_path, FILE_SERVICE_PATH_BYTES, "%s", s_files.mount_path) :
                snprintf(s_files.root_path, FILE_SERVICE_PATH_BYTES, "%s/%s", s_files.mount_path, relative);
    if (count < 0 || (size_t)count >= FILE_SERVICE_PATH_BYTES) return ESP_ERR_INVALID_SIZE;
    s_files.config = (sd_storage_service_config_t)
    {
        .mount_path = s_files.mount_path,
        .max_files = config->max_files > 0 ? config->max_files : 5,
        .allocation_unit_size = config->allocation_unit_size
    };
    s_files.pool = heap_caps_malloc(sizeof(*s_files.pool), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_files.pool == NULL)
        s_files.pool = heap_caps_malloc(sizeof(*s_files.pool), MALLOC_CAP_8BIT);
    if (s_files.pool == NULL) return ESP_ERR_NO_MEM;
    memset(s_files.pool, 0, sizeof(*s_files.pool));
    s_files.lock = xSemaphoreCreateMutex();
    s_files.exited = xSemaphoreCreateBinary();
    s_files.queue = xQueueCreate(FILE_SERVICE_QUEUE_DEPTH, sizeof(request_slot_t *));
    if (s_files.lock == NULL || s_files.exited == NULL || s_files.queue == NULL)
    {
        _free_resources();
        return ESP_ERR_NO_MEM;
    }
    sd_storage_service_snapshot_t storage = {0};
    (void)sd_storage_service_get_snapshot(&storage);
    request_slot_t *initial = &s_files.pool->storage;
    initial->type = REQUEST_STORAGE;
    initial->action = FILE_SERVICE_STORAGE_MOUNT;
    initial->generation = storage.generation;
    initial->revision = storage.revision;
    s_files.space.last_error = ESP_ERR_INVALID_STATE;
    /* Reserve/count initial mount before a worker or observer can see it. */
    _lock();
    const bool queued = _accept(initial, NULL);
    _unlock();
    if (!queued)
    {
        _finish(initial, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_BUSY, 0, false);
        _free_resources();
        return ESP_ERR_NO_MEM;
    }
    s_files.initialized = true;
    if (xTaskCreatePinnedToCoreWithCaps(_worker, "file_service", FILE_SERVICE_TASK_STACK,
                                        NULL, 4U, &s_files.task,
                                        CONFIG_MAIN_PROJECT_TASK_CORE_ID,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        _finish(initial, ESP_ERR_NO_MEM, FILE_SERVICE_ERROR_NO_MEMORY, 0, false);
        _free_resources();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t file_service_deinit(void)
{
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    _lock();
    s_files.suspended = true;
    s_files.stopping = true;
    for (size_t n = 0U; n < FILE_SERVICE_MAX_SESSIONS; ++n)
    {
        browse_session_t *session = &s_files.pool->sessions[n];
        session->open = false;
        if (session->slot.active) session->slot.cancelled = true;
    }
    _unlock();
    if (xSemaphoreTake(s_files.exited, pdMS_TO_TICKS(FILE_SERVICE_STOP_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    vTaskDeleteWithCaps(s_files.task);
    s_files.task = NULL;
    s_files.initialized = false;
    _free_resources();
    return ESP_OK;
}

esp_err_t file_service_suspend(uint32_t timeout_ms)
{
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    _lock();
    s_files.suspended = true;
    _unlock();
    const TickType_t start = xTaskGetTickCount();
    for (;;)
    {
        _lock();
        const bool busy = s_files.pending != 0U;
        _unlock();
        if (!busy) return ESP_OK;
        if ((TickType_t)(xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms))
            return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

esp_err_t file_service_resume(void)
{
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    _lock();
    const bool stopping = s_files.stopping;
    if (!stopping) s_files.suspended = false;
    _unlock();
    return stopping ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t file_service_get_snapshot(file_service_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    sd_storage_service_snapshot_t storage = {0};
    (void)sd_storage_service_get_snapshot(&storage);
    _lock();
    memset(snapshot, 0, sizeof(*snapshot));
    request_slot_t *slot = &s_files.pool->storage;
    const bool maintenance = slot->active && slot->action != FILE_SERVICE_STORAGE_REFRESH_SPACE;
    snapshot->volume = maintenance ? FILE_SERVICE_VOLUME_MAINTENANCE :
                       !storage.mounted ? FILE_SERVICE_VOLUME_UNAVAILABLE :
                       storage.session_users != 0U ? FILE_SERVICE_VOLUME_BUSY : FILE_SERVICE_VOLUME_READY;
    snapshot->space = s_files.space;
    if (!storage.mounted || s_files.space_generation != storage.generation)
    {
        snapshot->space.valid = false;
        snapshot->space.last_error = ESP_ERR_INVALID_STATE;
    }
    snapshot->generation = storage.generation;
    snapshot->revision = storage.revision;
    snapshot->faulted = storage.faulted;
    snapshot->last_error = storage.last_error;
    snapshot->busy = s_files.pending != 0U;
    snapshot->pending_requests = s_files.pending;
    snapshot->suspended = s_files.suspended;
    snapshot->storage = slot->result;
    for (size_t n = 0U; n < FILE_SERVICE_MAX_SESSIONS; ++n)
        if (s_files.pool->sessions[n].open) ++snapshot->open_sessions;
    _unlock();
    return ESP_OK;
}

bool file_service_is_busy(void)
{
    if (!s_files.initialized) return false;
    _lock();
    const bool busy = s_files.pending != 0U;
    _unlock();
    return busy;
}

esp_err_t file_service_request_storage(file_service_storage_action_t action,
                                       uint32_t expected_generation, file_service_result_t *result)
{
    if (!s_files.initialized)
        return _reject(result, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_BUSY);
    if (action < FILE_SERVICE_STORAGE_MOUNT || action > FILE_SERVICE_STORAGE_REFRESH_SPACE)
        return _reject(result, ESP_ERR_NOT_SUPPORTED, FILE_SERVICE_ERROR_INVALID);
    sd_storage_service_snapshot_t storage = {0};
    (void)sd_storage_service_get_snapshot(&storage);
    if (storage.generation != expected_generation)
        return _reject(result, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_STALE);
    _lock();
    request_slot_t *slot = &s_files.pool->storage;
    if (s_files.suspended || slot->active)
    {
        _unlock();
        return _reject(result, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_BUSY);
    }
    slot->type = REQUEST_STORAGE;
    slot->owner = 0U;
    slot->action = action;
    slot->generation = expected_generation;
    slot->revision = storage.revision;
    const bool accepted = _accept(slot, result);
    _unlock();
    if (!accepted)
    {
        _finish(slot, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_BUSY, 0, false);
        return _reject(result, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_BUSY);
    }
    return ESP_OK;
}

esp_err_t file_service_session_open(uint32_t *session_id)
{
    if (session_id == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    _lock();
    if (s_files.suspended)
    {
        _unlock();
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t n = 0U; n < FILE_SERVICE_MAX_SESSIONS; ++n)
    {
        browse_session_t *session = &s_files.pool->sessions[n];
        if (!session->open && !session->slot.active)
        {
            session->open = true;
            session->has_batch = false;
            session->has_detail = false;
            session->id = _next_id(&s_next_session);
            memset(&session->slot.result, 0, sizeof(session->slot.result));
            memset(&session->cursor, 0, sizeof(session->cursor));
            *session_id = session->id;
            _unlock();
            return ESP_OK;
        }
    }
    _unlock();
    return ESP_ERR_NO_MEM;
}

esp_err_t file_service_session_close(uint32_t id)
{
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    _lock();
    browse_session_t *session = _session(id);
    if (session == NULL)
    {
        _unlock();
        return ESP_ERR_INVALID_ARG;
    }
    session->open = false;
    if (session->slot.active) session->slot.cancelled = true;
    _unlock();
    return ESP_OK;
}

static esp_err_t _submit(uint32_t id, request_type_t type, const char *path,
                         const file_service_cursor_t *cursor,
                         const file_service_target_t *target, file_service_result_t *result)
{
    esp_err_t error = _check_relative(path);
    if (error != ESP_OK) return _reject(result, error, _esp_reason(error));
    if (!s_files.initialized)
        return _reject(result, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_BUSY);
    sd_storage_service_snapshot_t storage = {0};
    (void)sd_storage_service_get_snapshot(&storage);
    if ((target != NULL && (target->generation != storage.generation ||
                            target->revision != storage.revision)) ||
            (cursor != NULL && cursor->valid && (cursor->generation != storage.generation ||
                    cursor->revision != storage.revision)))
        return _reject(result, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_STALE);
    if (cursor != NULL && cursor->valid &&
            (cursor->offset < 0 || (uint64_t)cursor->offset > (uint64_t)LONG_MAX))
        return _reject(result, ESP_ERR_INVALID_ARG, FILE_SERVICE_ERROR_INVALID);
    _lock();
    browse_session_t *session = _session(id);
    if (session == NULL)
    {
        _unlock();
        return _reject(result, ESP_ERR_INVALID_ARG, FILE_SERVICE_ERROR_INVALID);
    }
    request_slot_t *slot = &session->slot;
    if (s_files.suspended || slot->active)
    {
        _unlock();
        return _reject(result, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_BUSY);
    }
    slot->type = type;
    slot->owner = id;
    slot->generation = storage.generation;
    slot->revision = storage.revision;
    slot->cursor = cursor != NULL ? *cursor : (file_service_cursor_t)
    {
        0
    };
    strcpy(slot->relative_path, path);
    const bool accepted = _accept(slot, result);
    _unlock();
    if (!accepted)
    {
        _finish(slot, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_BUSY, 0, false);
        return _reject(result, ESP_ERR_INVALID_STATE, FILE_SERVICE_ERROR_BUSY);
    }
    return ESP_OK;
}

esp_err_t file_service_session_list(uint32_t session, const char *path,
                                    const file_service_cursor_t *cursor, file_service_result_t *result)
{
    return _submit(session, REQUEST_LIST, path, cursor, NULL, result);
}

esp_err_t file_service_session_detail(uint32_t session, const file_service_target_t *target,
                                      file_service_result_t *result)
{
    if (target == NULL) return _reject(result, ESP_ERR_INVALID_ARG, FILE_SERVICE_ERROR_INVALID);
    return _submit(session, REQUEST_DETAIL, target->relative_path, NULL, target, result);
}

esp_err_t file_service_session_delete(uint32_t session, const file_service_target_t *target,
                                      file_service_result_t *result)
{
    if (target == NULL) return _reject(result, ESP_ERR_INVALID_ARG, FILE_SERVICE_ERROR_INVALID);
    return _submit(session, REQUEST_DELETE, target->relative_path, NULL, target, result);
}

esp_err_t file_service_session_cancel(uint32_t id)
{
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    _lock();
    browse_session_t *session = _session(id);
    if (session != NULL && session->slot.active) session->slot.cancelled = true;
    _unlock();
    return session != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t file_service_session_get_result(uint32_t id, file_service_result_t *result)
{
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    _lock();
    browse_session_t *session = _session(id);
    if (session != NULL) *result = session->slot.result;
    _unlock();
    return session != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t file_service_session_get_batch(uint32_t id, file_service_batch_t *batch)
{
    if (batch == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    _lock();
    browse_session_t *session = _session(id);
    esp_err_t error = session == NULL ? ESP_ERR_INVALID_ARG :
                      !session->has_batch ? ESP_ERR_NOT_FOUND : ESP_OK;
    if (error == ESP_OK) *batch = session->batch;
    _unlock();
    return error;
}

esp_err_t file_service_session_get_detail(uint32_t id, file_service_detail_t *detail)
{
    if (detail == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    _lock();
    browse_session_t *session = _session(id);
    esp_err_t error = session == NULL ? ESP_ERR_INVALID_ARG :
                      !session->has_detail ? ESP_ERR_NOT_FOUND : ESP_OK;
    if (error == ESP_OK) *detail = session->detail;
    _unlock();
    return error;
}

esp_err_t file_service_session_get_cursor(uint32_t id, file_service_cursor_t *cursor)
{
    if (cursor == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_files.initialized) return ESP_ERR_INVALID_STATE;
    _lock();
    browse_session_t *session = _session(id);
    if (session != NULL) *cursor = session->cursor;
    _unlock();
    return session != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}
