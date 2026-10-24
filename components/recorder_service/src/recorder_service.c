#define DBG_TAG "recorder_service"
#include "mt_log.h"

#include "recorder_service.h"

#include "audio_service.h"
#include "esp_timer.h"
#include "sd_storage_service.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
    #include "freertos/FreeRTOS.h"
    #include "freertos/queue.h"
    #include "freertos/task.h"
    #include "freertos/idf_additions.h"
    #include "esp_attr.h"
    #include "esp_heap_caps.h"
    #define RECORDER_TASK_CORE 1
#else
    #include "recorder_host_worker.h"
#endif

/*
 * The command worker runs every index scan, and each scan enters
 * VFS -> FATFS -> diskio -> SDSPI from that stack. Measured frames on the chain
 * (O2, xtensa-esp32s3): refresh_index 304, vfs_fat_stat 336, f_stat 96,
 * sdspi_host_start_command 272, start_command_default 96, plus newlib printf and
 * the per-call sector buffers. 4096 left too little margin, so the limit is
 * stated here instead of being rediscovered by an overflow.
 */
#define RECORDER_COMMAND_STACK 6144

typedef enum recorder_command_type
{
    RECORDER_COMMAND_START = 0,
    RECORDER_COMMAND_PAUSE,
    RECORDER_COMMAND_RESUME,
    RECORDER_COMMAND_STOP,
    RECORDER_COMMAND_PLAY,
    RECORDER_COMMAND_STOP_PLAYBACK,
    RECORDER_COMMAND_DELETE,
    RECORDER_COMMAND_REFRESH_INDEX,
    RECORDER_COMMAND_SHUTDOWN,
} recorder_command_type_t;

typedef struct recorder_command
{
    recorder_command_type_t type;
    uint32_t operation_id;
    recorder_service_target_t target;
} recorder_command_t;

typedef struct recorder_wav_header
{
    char riff[4];
    uint32_t file_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} recorder_wav_header_t;

/**
 * @brief One scan's working set, allocated per scan for one reason.
 *
 * The command worker runs every scan, and its stack must stay small enough for
 * the FATFS/VFS/SDSPI call chain it enters from here. Two 192-byte path buffers
 * plus a 216-byte record on that stack were enough to overflow it on the first
 * boot-time index refresh, so the whole working set lives in this bundle.
 */
typedef struct recorder_scan
{
    recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES];
    char directory_path[RECORDER_SERVICE_PATH_BYTES];
    char path[RECORDER_SERVICE_PATH_BYTES];
} recorder_scan_t;

typedef struct recorder_runtime
{
    recorder_service_config_t config;
    recorder_service_snapshot_t snapshot;
    FILE *file;
    uint64_t data_bytes;
    int64_t started_us;
    uint32_t accumulated_duration_ms;
    bool stop_requested;
    bool stopping;
    atomic_bool task_running;
    bool playback;
    size_t file_count;
    bool index_valid;
    bool refresh_requested;
    uint32_t index_generation;
    uint32_t index_revision;
    uint32_t index_publish_version;
    uint32_t space_generation;
    sd_storage_service_usage_t usage;
    bool usage_held;
    char directory_path[RECORDER_SERVICE_PATH_BYTES];
    TaskHandle_t task;
    TaskHandle_t command_task;
    QueueHandle_t command_queue;
} recorder_runtime_t;

static recorder_runtime_t s_recorder;
/**
 * @brief Published index, kept out of internal DRAM on purpose.
 *
 * At 32 entries this array is 7 KB. Internal DRAM is also where the display's
 * SPI bounce buffers and the I2S DMA rings come from; holding the index there
 * left too little contiguous DMA-capable memory for a single 10-row LCD chunk
 * once recording started, and every frame then failed with ESP_ERR_NO_MEM.
 * The index is read by the UI and written once per scan, so PSRAM is the right
 * home for it.
 */
#ifdef ESP_PLATFORM
    static EXT_RAM_BSS_ATTR recorder_service_file_t
    s_index_files[RECORDER_SERVICE_MAX_FILES];
#else
    static recorder_service_file_t s_index_files[RECORDER_SERVICE_MAX_FILES];
#endif
#ifdef ESP_PLATFORM
    static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#else
    static atomic_flag s_lock = ATOMIC_FLAG_INIT;
#endif
static bool s_initialized;
static uint32_t s_next_operation_id;

static void _recorder_command_task(void *argument);

static void _recorder_lock(void)
{
#ifdef ESP_PLATFORM
    portENTER_CRITICAL(&s_lock);
#else
    while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire))
    {
    }
#endif
}

static void _recorder_unlock(void)
{
#ifdef ESP_PLATFORM
    portEXIT_CRITICAL(&s_lock);
#else
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
#endif
}

/**
 * @brief Build the absolute backend path from a volume-relative directory.
 *
 * The recorder never hard-codes the mount point: it always resolves through
 * the storage service so a remount to another path keeps working.
 */
static esp_err_t _recorder_volume_path(const char *relative_path, char *out,
                                       size_t out_size)
{
    const char *mount = sd_storage_service_get_mount_path();
    if (mount == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const int length = relative_path[0] == '\0' ?
                       snprintf(out, out_size, "%s", mount) :
                       snprintf(out, out_size, "%s/%s", mount, relative_path);
    if (length < 0 || (size_t)length >= out_size)
    {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/**
 * @brief Whether a target names one direct child of the recording directory.
 *
 * The basename must not contain a separator, so nothing can escape the
 * directory, and "." / ".." are rejected as entries. A filename merely
 * containing two dots in the middle ("memo..2.wav") is legitimate: scanning
 * lists it, so play and delete must accept it too. Rejecting it outright left
 * such rows visible but permanently inoperable.
 */
/**
 * @brief Whether a target names one direct child of the recording directory.
 *
 * The basename must contain no separator, so nothing can escape the directory,
 * and "." / ".." are rejected as entries. A filename with two dots inside it
 * ("REC_..take.wav") is legitimate: the scan lists it, so play and delete must
 * accept it too. Rejecting it outright left such rows visible but permanently
 * inoperable.
 */
static bool _recorder_path_is_owned(const char *name)
{
    const size_t directory_length = strlen(s_recorder.directory_path);
    if (directory_length == 0U || name == NULL ||
            strncmp(name, s_recorder.directory_path, directory_length) != 0 ||
            name[directory_length] != '/')
    {
        return false;
    }
    const char *const base = name + directory_length + 1U;
    if (base[0] == '\0' || strchr(base, '/') != NULL)
    {
        return false;
    }
    return !(base[0] == '.' && (base[1] == '\0' ||
                                (base[1] == '.' && base[2] == '\0')));
}

static esp_err_t _recorder_prepare_directory(bool create)
{
    char path[RECORDER_SERVICE_PATH_BYTES];
    esp_err_t result = _recorder_volume_path(s_recorder.config.directory,
        path, sizeof(path));
    if (result != ESP_OK)
    {
        return result;
    }
    if (!create)
    {
        (void)snprintf(s_recorder.directory_path,
                       sizeof(s_recorder.directory_path), "%s", path);
        return ESP_OK;
    }
    for (char *cursor = path + 1; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '/')
        {
            *cursor = '\0';
            (void)mkdir(path, 0775);
            *cursor = '/';
        }
    }
    if (mkdir(path, 0775) != 0 && errno != EEXIST)
    {
        return ESP_FAIL;
    }
    (void)snprintf(s_recorder.directory_path,
                   sizeof(s_recorder.directory_path), "%s", path);
    return ESP_OK;
}

static esp_err_t _recorder_header_write(FILE *file, uint32_t data_bytes)
{
    const recorder_wav_header_t header =
    {
        .riff = {'R', 'I', 'F', 'F'},
        .file_size = data_bytes + 36U,
        .wave = {'W', 'A', 'V', 'E'},
        .fmt = {'f', 'm', 't', ' '},
        .fmt_size = 16U,
        .format = 1U,
        .channels = 2U,
        .sample_rate = 16000U,
        .byte_rate = 64000U,
        .block_align = 4U,
        .bits_per_sample = 16U,
        .data = {'d', 'a', 't', 'a'},
        .data_size = data_bytes,
    };
    if (fseek(file, 0L, SEEK_SET) != 0 ||
            fwrite(&header, 1U, sizeof(header), file) != sizeof(header) ||
            fflush(file) != 0)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool _recorder_header_valid(const recorder_wav_header_t *header,
                                   uint64_t file_size)
{
    return header != NULL && file_size >= sizeof(*header) &&
           memcmp(header->riff, "RIFF", 4U) == 0 &&
           memcmp(header->wave, "WAVE", 4U) == 0 &&
           memcmp(header->fmt, "fmt ", 4U) == 0 &&
           memcmp(header->data, "data", 4U) == 0 &&
           header->format == 1U && header->channels == 2U &&
           header->sample_rate == 16000U && header->bits_per_sample == 16U &&
           header->block_align == 4U && header->byte_rate == 64000U &&
           (uint64_t)header->file_size + 8U <= file_size &&
           (uint64_t)header->data_size + sizeof(*header) <= file_size;
}

/**
 * @brief Refresh the cached capacity reading from the storage service.
 *
 * Only the recorder worker (and the host test build) runs this. The UI reads
 * the resulting snapshot without touching the filesystem. A failed query is
 * cached as invalid instead of being published as "no space left".
 */
static esp_err_t _recorder_space_refresh(const sd_storage_service_usage_t *usage)
{
    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    const esp_err_t result = sd_storage_service_get_space(usage, &total_bytes,
        &free_bytes);
    _recorder_lock();
    s_recorder.snapshot.space_valid = result == ESP_OK;
    s_recorder.snapshot.space_error = result;
    s_recorder.snapshot.free_bytes = result == ESP_OK ? free_bytes : 0U;
    s_recorder.space_generation = usage->generation;
    _recorder_unlock();
    return result;
}

#ifdef ESP_PLATFORM
static void _recorder_snapshot_update(uint32_t duration_ms)
{
    _recorder_lock();
    if (!s_recorder.playback)
    {
        s_recorder.snapshot.duration_ms = duration_ms;
        s_recorder.snapshot.bytes = s_recorder.data_bytes;
    }
    _recorder_unlock();
}
#endif

static uint32_t _recorder_duration_locked(int64_t now_us)
{
    uint64_t duration = s_recorder.accumulated_duration_ms;
    if (s_recorder.snapshot.state == RECORDER_SERVICE_RECORDING &&
            now_us > s_recorder.started_us)
    {
        duration += (uint64_t)(now_us - s_recorder.started_us) / 1000U;
    }
    return duration > UINT32_MAX ? UINT32_MAX : (uint32_t)duration;
}

#ifdef ESP_PLATFORM
static uint32_t _recorder_elapsed_ms(int64_t started_us, int64_t now_us)
{
    if (now_us <= started_us)
    {
        return 0U;
    }
    const uint64_t elapsed = (uint64_t)(now_us - started_us) / 1000U;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}
#endif

/** @brief Earliest plausible wall-clock stamp; earlier ones mean no clock. */
#define RECORDER_MIN_EPOCH_S 1577836800U /* 2020-01-01T00:00:00Z */

/**
 * @brief Order a scanned candidate against one published record.
 *
 * Modified time is authoritative when both sides report one; the file name is
 * the tie-break because timestamp names sort chronologically. A longer name is
 * the newer boot-counter name, so length is compared before content.
 *
 * The candidate is passed as fields instead of a whole record: the scan runs on
 * the command worker's stack, where a second 208-byte record is not affordable.
 */
static bool _recorder_record_newer(const char *name, bool has_mtime,
                                   uint32_t epoch_s,
                                   const recorder_service_file_t *row)
{
    if (has_mtime != row->has_mtime)
    {
        return has_mtime;
    }
    if (has_mtime && epoch_s != row->modified_epoch_s)
    {
        return epoch_s > row->modified_epoch_s;
    }
    const size_t length = strlen(name);
    const size_t row_length = strlen(row->target.name);
    if (length != row_length)
    {
        return length > row_length;
    }
    return strcmp(name, row->target.name) > 0;
}

/** @brief Scan under a held lease and publish only a complete, current index. */
static void _recorder_refresh_index(const sd_storage_service_usage_t *held_usage)
{
    sd_storage_service_usage_t access = {0};
    const sd_storage_service_usage_t *usage = held_usage;
    if (usage == NULL)
    {
        if (sd_storage_service_usage_acquire(SD_STORAGE_SERVICE_USAGE_ACCESS,
                                             &access) != ESP_OK)
        {
            return;
        }
        usage = &access;
    }
    _recorder_lock();
    const uint32_t publish_version = s_recorder.index_publish_version;
    _recorder_unlock();
    recorder_scan_t *scan = calloc(1U, sizeof(*scan));
    if (scan == NULL)
    {
        _recorder_lock();
        if (publish_version == s_recorder.index_publish_version)
        {
            ++s_recorder.index_publish_version;
            s_recorder.index_valid = false;
            s_recorder.file_count = 0U;
            s_recorder.snapshot.library_truncated = false;
            s_recorder.snapshot.space_valid = false;
            s_recorder.snapshot.space_error = ESP_ERR_NO_MEM;
            ++s_recorder.snapshot.generation;
        }
        _recorder_unlock();
        if (held_usage == NULL)
        {
            (void)sd_storage_service_usage_release(&access);
        }
        return;
    }
    recorder_service_file_t *const files = scan->files;
    size_t file_count = 0U;
    sd_storage_service_snapshot_t before = {0};
    const bool usable = sd_storage_service_get_snapshot(&before) == ESP_OK &&
                        before.mounted && before.generation == usage->generation;
    const bool resolved = usable &&
                          _recorder_volume_path(s_recorder.config.directory,
                              scan->directory_path,
                              sizeof(scan->directory_path)) == ESP_OK;
    uint64_t total_bytes = 0U;
    uint64_t free_bytes = 0U;
    errno = 0;
    const esp_err_t space_error = resolved ?
                                  sd_storage_service_get_space(usage, &total_bytes, &free_bytes) :
                                  ESP_ERR_INVALID_STATE;
    bool medium_eio = space_error != ESP_OK && errno == EIO;
    errno = 0;
    DIR *directory = resolved ? opendir(scan->directory_path) : NULL;
    /* The directory is created lazily by the first recording, so on a fresh
     * card it is legitimately absent. That is an empty library, not a scan
     * failure: treating ENOENT as failure would keep index_valid false forever
     * and leave the list reading the medium it already finished reading. */
    const bool directory_absent = directory == NULL &&
                                  (errno == ENOENT || errno == ENOTDIR);
    bool scan_ok = directory != NULL || directory_absent;
    medium_eio |= resolved && directory == NULL && errno == EIO;
    bool truncated = false;
    if (directory != NULL)
    {
        for (;;)
        {
            errno = 0;
            struct dirent *entry = readdir(directory);
            if (entry == NULL)
            {
                scan_ok = errno == 0;
                medium_eio |= errno == EIO;
                break;
            }
            const size_t name_length = strlen(entry->d_name);
            if (name_length <= 4U ||
                    strcmp(entry->d_name + name_length - 4U, ".wav") != 0)
            {
                continue;
            }
            char *const path = scan->path;
            const int length = snprintf(path, sizeof(scan->path), "%s/%s",
                                        scan->directory_path, entry->d_name);
            if (length < 0 || (size_t)length >= sizeof(scan->path))
            {
                continue;
            }
            struct stat info;
            errno = 0;
            if (stat(path, &info) != 0)
            {
                scan_ok = false;
                medium_eio |= errno == EIO;
                break;
            }
            if (info.st_size < (off_t)sizeof(recorder_wav_header_t))
            {
                continue;
            }
            errno = 0;
            FILE *file = fopen(path, "rb");
            if (file == NULL)
            {
                scan_ok = false;
                medium_eio |= errno == EIO;
                break;
            }
            recorder_wav_header_t header;
            errno = 0;
            const size_t read_bytes = fread(&header, 1U, sizeof(header), file);
            if (ferror(file))
            {
                scan_ok = false;
                medium_eio |= errno == EIO;
            }
            const bool valid = read_bytes == sizeof(header) &&
                               _recorder_header_valid(&header,
                                   (uint64_t)info.st_size);
            errno = 0;
            if (fclose(file) != 0)
            {
                scan_ok = false;
                medium_eio |= errno == EIO;
            }
            if (!scan_ok)
            {
                break;
            }
            if (!valid)
            {
                continue;
            }
            /* The record is written straight into its slot: a stack-local
             * copy would add 216 bytes to the command worker's frame, which
             * only has room for the FATFS call chain below it. */
            const bool has_mtime = info.st_mtime > 0;
            const uint32_t epoch_s = has_mtime ? (uint32_t)info.st_mtime : 0U;
            /* The bound keeps the newest recordings, so a full medium loses
             * its oldest entries instead of an arbitrary readdir subset. */
            if (file_count == RECORDER_SERVICE_MAX_FILES)
            {
                truncated = true;
                if (!_recorder_record_newer(path, has_mtime, epoch_s,
                                            &files[file_count - 1U]))
                {
                    continue;
                }
            }
            const size_t last = file_count < RECORDER_SERVICE_MAX_FILES ?
                                file_count : RECORDER_SERVICE_MAX_FILES - 1U;
            size_t position = 0U;
            /* Skip every row that must stay ahead of the candidate. */
            while (position < last &&
                    !_recorder_record_newer(path, has_mtime, epoch_s,
                                            &files[position]))
            {
                ++position;
            }
            for (size_t i = last; i > position; --i)
            {
                files[i] = files[i - 1U];
            }
            recorder_service_file_t *row = &files[position];
            (void)snprintf(row->target.name, sizeof(row->target.name), "%s",
                           path);
            row->bytes = (uint64_t)info.st_size;
            row->duration_ms = (uint32_t)(((uint64_t)header.data_size) *
                                          1000U / 64000U);
            row->has_mtime = has_mtime;
            row->modified_epoch_s = epoch_s;
            if (file_count < RECORDER_SERVICE_MAX_FILES)
            {
                ++file_count;
            }
        }
        errno = 0;
        if (closedir(directory) != 0)
        {
            scan_ok = false;
            medium_eio |= errno == EIO;
        }
    }
    sd_storage_service_snapshot_t after = {0};
    const bool fresh = scan_ok && !medium_eio &&
                       sd_storage_service_get_snapshot(&after) == ESP_OK &&
                       after.mounted && before.generation == after.generation &&
                       before.revision == after.revision;
    if (medium_eio)
    {
        (void)sd_storage_service_usage_report_fault(usage, ESP_FAIL);
    }
    _recorder_lock();
    /* A concurrent finalization/command scan may have published since this
     * scan began. Losing that race must not overwrite or invalidate its cache. */
    if (publish_version == s_recorder.index_publish_version)
    {
        ++s_recorder.index_publish_version;
        if (fresh)
        {
            bool changed = !s_recorder.index_valid ||
                           s_recorder.index_generation != before.generation ||
                           s_recorder.index_revision != before.revision ||
                           s_recorder.file_count != file_count ||
                           s_recorder.snapshot.library_truncated != truncated;
            for (size_t i = 0U; i < file_count; ++i)
            {
                files[i].target.generation = before.generation;
                files[i].target.revision = before.revision;
                if (!changed)
                {
                    changed = strcmp(s_index_files[i].target.name,
                                     files[i].target.name) != 0 ||
                              s_index_files[i].bytes != files[i].bytes ||
                              s_index_files[i].duration_ms != files[i].duration_ms ||
                              s_index_files[i].modified_epoch_s !=
                              files[i].modified_epoch_s ||
                              s_index_files[i].has_mtime != files[i].has_mtime;
                }
            }
            /* Copy only the live rows, and only when something changed. The
             * array is 7 KB; the copy runs with interrupts disabled, so an
             * unconditional full-array memcpy cost every scan ~100 us of ISR
             * latency for bytes no reader would observe. */
            if (changed)
            {
                memcpy(s_index_files, files, file_count * sizeof(files[0]));
            }
            s_recorder.file_count = file_count;
            s_recorder.index_valid = true;
            s_recorder.index_generation = before.generation;
            s_recorder.index_revision = before.revision;
            s_recorder.snapshot.library_truncated = truncated;
            s_recorder.space_generation = before.generation;
            s_recorder.snapshot.space_valid = space_error == ESP_OK;
            s_recorder.snapshot.space_error = space_error;
            s_recorder.snapshot.free_bytes = space_error == ESP_OK ? free_bytes : 0U;
            if (changed)
            {
                ++s_recorder.snapshot.generation;
            }
        }
        else
        {
            if (s_recorder.index_valid || s_recorder.file_count != 0U)
            {
                ++s_recorder.snapshot.generation;
            }
            s_recorder.index_valid = false;
            s_recorder.file_count = 0U;
            s_recorder.snapshot.library_truncated = false;
            s_recorder.snapshot.space_valid = false;
            s_recorder.snapshot.space_error = ESP_ERR_INVALID_STATE;
            s_recorder.snapshot.free_bytes = 0U;
        }
    }
    _recorder_unlock();
    free(scan);
    if (held_usage == NULL)
    {
        (void)sd_storage_service_usage_release(&access);
    }
}

/**
 * @brief Report whether the cached index still matches the medium.
 *
 * A remount or a content change made by another owner invalidates the cached
 * list without any filesystem access at query time.
 */
static bool _recorder_index_is_fresh(void)
{
    sd_storage_service_snapshot_t storage = {0};
    const esp_err_t storage_result = sd_storage_service_get_snapshot(&storage);
    const uint32_t generation = storage_result == ESP_OK ? storage.generation :
                                0U;
    const bool mounted = storage_result == ESP_OK && storage.mounted;
    const uint32_t revision = storage.revision;
    _recorder_lock();
    const bool fresh = s_recorder.index_valid && mounted &&
                       s_recorder.index_generation == generation &&
                       s_recorder.index_revision == revision;
    _recorder_unlock();
    return fresh;
}

/** @brief Ask the worker for one index refresh, if it can be queued now. */
static esp_err_t _recorder_request_refresh(void)
{
    if (s_recorder.command_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const recorder_command_t refresh =
    {
        .type = RECORDER_COMMAND_REFRESH_INDEX,
    };
    return xQueueSend(s_recorder.command_queue, &refresh, 0U) == pdTRUE ?
           ESP_OK : ESP_ERR_NO_MEM;
}

/**
 * @brief Rebuild the cached index and capacity reading.
 *
 * Runs on the recorder worker only: the UI never performs file I/O. The
 * directory path is resolved (never created) for the current mount epoch, so a
 * boot after a successful mount can list existing recordings without starting
 * a new one.
 */
static void _recorder_refresh_from_medium(void)
{
    if (!_recorder_index_is_fresh())
    {
        _recorder_refresh_index(NULL);
    }
}

/** @brief Acquire the business session lease that covers a full operation. */
static esp_err_t _recorder_usage_acquire(void)
{
    sd_storage_service_usage_t usage = {0};
    const esp_err_t result = sd_storage_service_usage_acquire(
                                 SD_STORAGE_SERVICE_USAGE_SESSION, &usage);
    if (result != ESP_OK)
    {
        return result;
    }
    _recorder_lock();
    s_recorder.usage = usage;
    s_recorder.usage_held = true;
    _recorder_unlock();
    return ESP_OK;
}

/** @brief Release the business session lease after the last handle closed. */
static void _recorder_usage_release(bool medium_eio, esp_err_t error)
{
    _recorder_lock();
    const sd_storage_service_usage_t usage = s_recorder.usage;
    const bool held = s_recorder.usage_held;
    s_recorder.usage_held = false;
    memset(&s_recorder.usage, 0, sizeof(s_recorder.usage));
    _recorder_unlock();
    if (!held)
    {
        return;
    }
    if (medium_eio)
    {
        (void)sd_storage_service_usage_report_fault(&usage, error);
    }
    (void)sd_storage_service_usage_release(&usage);
}

static void _recorder_finish(bool failed, esp_err_t error, bool medium_eio)
{
    const int64_t now_us = esp_timer_get_time();
    _recorder_lock();
    if (!s_recorder.playback)
    {
        s_recorder.snapshot.duration_ms =
            _recorder_duration_locked(now_us);
    }
    FILE *file = s_recorder.file;
    const bool playback = s_recorder.playback;
    const uint64_t recorded_bytes = s_recorder.data_bytes;
    const uint32_t data_bytes = recorded_bytes > UINT32_MAX ? UINT32_MAX :
                                (uint32_t)recorded_bytes;
    char path[RECORDER_SERVICE_PATH_BYTES];
    (void)snprintf(path, sizeof(path), "%s", s_recorder.snapshot.name);
    s_recorder.file = NULL;
    _recorder_unlock();
    esp_err_t final_error = failed ? error : ESP_OK;
    const esp_err_t audio_error = audio_service_stop();
    if (final_error == ESP_OK && audio_error != ESP_OK)
    {
        final_error = audio_error;
    }
    if (file != NULL)
    {
        if (final_error == ESP_OK && !playback && recorded_bytes >
                UINT32_MAX - 36U)
        {
            final_error = ESP_ERR_INVALID_SIZE;
        }
        errno = 0;
        if (final_error == ESP_OK && !playback &&
                _recorder_header_write(file, data_bytes) != ESP_OK)
        {
            medium_eio |= errno == EIO;
            final_error = ESP_FAIL;
        }
        errno = 0;
        if (fclose(file) != 0)
        {
            medium_eio |= errno == EIO;
            if (final_error == ESP_OK)
            {
                final_error = ESP_FAIL;
            }
        }
        if (final_error != ESP_OK && !playback && path[0] != '\0')
        {
            /* A rejected take is not a recording: keeping the working file
             * would leave .part names the library never lists and the user can
             * never delete, slowly consuming the card. */
            if (remove(path) != 0)
            {
                medium_eio |= errno == EIO;
            }
        }
        if (final_error == ESP_OK && !playback && path[0] != '\0')
        {
            const size_t length = strlen(path);
            if (length > 5U && strcmp(path + length - 5U, ".part") == 0)
            {
                char final_path[RECORDER_SERVICE_PATH_BYTES];
                (void)snprintf(final_path, sizeof(final_path), "%.*s.wav",
                               (int)(length - 5U), path);
                errno = 0;
                if (rename(path, final_path) == 0)
                {
                    _recorder_lock();
                    (void)snprintf(s_recorder.snapshot.name,
                                   sizeof(s_recorder.snapshot.name), "%s",
                                   final_path);
                    _recorder_unlock();
                }
                else
                {
                    medium_eio |= errno == EIO;
                    final_error = ESP_FAIL;
                }
            }
            else
            {
                final_error = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    /* Finalize content while SESSION is still held. Readers must never observe
     * IDLE (or accept another start) before invalidation and lease release. */
    if (!playback && file != NULL)
    {
        (void)sd_storage_service_note_content_change(&s_recorder.usage);
    }
    if (final_error == ESP_OK)
    {
        _recorder_refresh_index(&s_recorder.usage);
    }
    _recorder_usage_release(medium_eio, final_error);
    _recorder_lock();
    s_recorder.snapshot.state = final_error != ESP_OK ? RECORDER_SERVICE_ERROR :
                                RECORDER_SERVICE_IDLE;
    if (playback && final_error == ESP_OK)
    {
        s_recorder.snapshot.playback_position_ms =
            s_recorder.snapshot.playback_duration_ms;
    }
    s_recorder.snapshot.last_error = final_error;
    ++s_recorder.snapshot.generation;
    atomic_store_explicit(&s_recorder.task_running, false,
                          memory_order_release);
    _recorder_unlock();
}

#ifdef ESP_PLATFORM
static EXT_RAM_BSS_ATTR uint8_t s_recorder_pcm[4096];

static void _recorder_stream_park(void)
{
    for (;;)
    {
        vTaskDelay(portMAX_DELAY);
    }
}

static void _recorder_task(void *argument)
{
    (void)argument;
    /* One stream at a time; the command owner reaps before reusing PCM. */
    for (;;)
    {
        const int64_t now_us = esp_timer_get_time();
        _recorder_lock();
        const bool stop = s_recorder.stop_requested;
        const bool paused = s_recorder.snapshot.state == RECORDER_SERVICE_PAUSED;
        const bool playback = s_recorder.playback;
        FILE *file = s_recorder.file;
        const uint32_t duration = _recorder_duration_locked(now_us);
        _recorder_unlock();
        if (stop || file == NULL)
        {
            _recorder_finish(false, ESP_OK, false);
            _recorder_stream_park();
            return;
        }
        if (paused && !playback)
        {
            /* The capture device keeps running while paused. Not reading it
             * leaves the pre-pause interval queued in the I2S ring, which the
             * first reads after resume then append to the file: the take would
             * contain audible paused audio and its data size would exceed the
             * reported duration. Drain and discard instead. */
            size_t discarded = 0U;
            if (audio_service_read(s_recorder_pcm, sizeof(s_recorder_pcm),
                                   &discarded, 250U) != ESP_OK)
            {
                _recorder_finish(true, ESP_ERR_INVALID_RESPONSE, false);
                _recorder_stream_park();
                return;
            }
            continue;
        }
        if (!playback && duration >= s_recorder.config.max_duration_seconds * 1000U)
        {
            _recorder_finish(false, ESP_OK, false);
            _recorder_stream_park();
            return;
        }
        size_t received = 0U;
        bool io_failed = false;
        bool medium_eio = false;
        if (playback)
        {
            errno = 0;
            received = fread(s_recorder_pcm, 1U, sizeof(s_recorder_pcm), file);
            if (ferror(file))
            {
                io_failed = true;
                medium_eio = errno == EIO;
            }
            else if (received == 0U)
            {
                _recorder_finish(false, ESP_OK, false);
                _recorder_stream_park();
                return;
            }
            if (!io_failed && audio_service_write(s_recorder_pcm, received, NULL, 250U) != ESP_OK)
            {
                io_failed = true;
            }
        }
        else if (audio_service_read(s_recorder_pcm, sizeof(s_recorder_pcm), &received, 250U) != ESP_OK ||
                 received == 0U)
        {
            io_failed = true;
        }
        else
        {
            errno = 0;
            if (fwrite(s_recorder_pcm, 1U, received, file) != received)
            {
                io_failed = true;
                medium_eio = errno == EIO;
            }
        }
        if (io_failed)
        {
            _recorder_finish(true, ESP_ERR_INVALID_RESPONSE, medium_eio);
            _recorder_stream_park();
            return;
        }
        const int64_t position_us = esp_timer_get_time();
        _recorder_lock();
        if (playback)
        {
            const uint32_t position = _recorder_elapsed_ms(
                                          s_recorder.started_us, position_us);
            s_recorder.snapshot.playback_position_ms =
                position < s_recorder.snapshot.playback_duration_ms ?
                position : s_recorder.snapshot.playback_duration_ms;
        }
        else
        {
            s_recorder.data_bytes += received;
        }
        _recorder_unlock();
        _recorder_snapshot_update(duration);
    }
}

static esp_err_t _recorder_stream_create(void)
{
    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(_recorder_task, "recorder", 4096, NULL, 5,
                                        &task, RECORDER_TASK_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    /* Only the command task creates/reaps, even if completion preceded return. */
    _recorder_lock();
    s_recorder.task = task;
    _recorder_unlock();
    return ESP_OK;
}

static void _recorder_stream_reap(void)
{
    TaskHandle_t task = NULL;
    _recorder_lock();
    if (s_recorder.task != NULL &&
            !atomic_load_explicit(&s_recorder.task_running, memory_order_acquire))
    {
        task = s_recorder.task;
        s_recorder.task = NULL;
    }
    _recorder_unlock();
    if (task != NULL)
    {
        vTaskDeleteWithCaps(task);
    }
}
#endif

static bool _recorder_relative_path_valid(const char *path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/' ||
            strlen(path) >= RECORDER_SERVICE_PATH_BYTES)
    {
        return false;
    }
    const char *cursor = path;
    while (*cursor != '\0')
    {
        const char *end = strchr(cursor, '/');
        const size_t component = end != NULL ? (size_t)(end - cursor) :
                                 strlen(cursor);
        if (component == 0U ||
                (component == 1U && cursor[0] == '.') ||
                (component == 2U && cursor[0] == '.' && cursor[1] == '.'))
        {
            return false;
        }
        if (end == NULL)
        {
            break;
        }
        cursor = end + 1;
    }
    return true;
}

esp_err_t recorder_service_init(const recorder_service_config_t *config)
{
    if (config == NULL || !_recorder_relative_path_valid(config->directory) ||
            config->max_duration_seconds == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_recorder, 0, sizeof(s_recorder));
    atomic_init(&s_recorder.task_running, false);
    s_recorder.config = *config;
    s_recorder.snapshot.state = RECORDER_SERVICE_IDLE;
    s_recorder.snapshot.last_error = ESP_OK;
    s_recorder.snapshot.space_error = ESP_ERR_INVALID_STATE;
    s_recorder.snapshot.max_duration_ms = config->max_duration_seconds >
                                          UINT32_MAX / 1000U ? UINT32_MAX :
                                          config->max_duration_seconds * 1000U;
    s_recorder.stopping = false;
    /* The directory is prepared by the worker after a mount succeeds; a boot
     * without a card must not create filesystem paths. */
    s_recorder.command_queue = xQueueCreate(4U, sizeof(recorder_command_t));
    if (s_recorder.command_queue == NULL ||
            xTaskCreatePinnedToCore(_recorder_command_task, "recorder_cmd",
                                    RECORDER_COMMAND_STACK, NULL, 5,
                                    &s_recorder.command_task,
                                    RECORDER_TASK_CORE) != pdPASS)
    {
        if (s_recorder.command_queue != NULL)
        {
            vQueueDelete(s_recorder.command_queue);
            s_recorder.command_queue = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    /* The worker indexes the medium; initialization never touches the card,
     * so a boot without one stays valid and a later mount is picked up. */
    const recorder_command_t refresh =
    {
        .type = RECORDER_COMMAND_REFRESH_INDEX,
    };
    (void)xQueueSend(s_recorder.command_queue, &refresh, 0U);
    return ESP_OK;
}

esp_err_t recorder_service_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _recorder_lock();
    s_recorder.stopping = true;
    _recorder_unlock();
    const int64_t deadline = esp_timer_get_time() + 5000000LL;
    /* Serialize the last close behind any command already running. */
    const recorder_command_t stop = {.type = RECORDER_COMMAND_STOP};
    if (xQueueSend(s_recorder.command_queue, &stop, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    if (s_recorder.command_queue != NULL)
    {
        const recorder_command_t shutdown =
        {
            .type = RECORDER_COMMAND_SHUTDOWN,
        };
        if (xQueueSend(s_recorder.command_queue, &shutdown,
                       pdMS_TO_TICKS(100U)) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }
        for (;;)
        {
            _recorder_lock();
            const bool command_running = s_recorder.command_task != NULL;
            _recorder_unlock();
            if (!command_running || esp_timer_get_time() >= deadline)
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10U));
        }
        _recorder_lock();
        const bool command_running = s_recorder.command_task != NULL;
        _recorder_unlock();
        if (command_running)
        {
            return ESP_ERR_TIMEOUT;
        }
        vQueueDelete(s_recorder.command_queue);
        s_recorder.command_queue = NULL;
    }
    /* A retained session lease would block the following unmount. */
    _recorder_usage_release(false, ESP_OK);
    s_initialized = false;
    return ESP_OK;
}

esp_err_t recorder_service_get_snapshot(recorder_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t now_us = esp_timer_get_time();
    _recorder_lock();
    *snapshot = s_recorder.snapshot;
    snapshot->index_generation = s_recorder.index_generation;
    snapshot->index_revision = s_recorder.index_revision;
    snapshot->index_valid = s_recorder.index_valid;
    snapshot->file_count = s_recorder.file_count;
    const uint32_t space_generation = s_recorder.space_generation;
    if (snapshot->state == RECORDER_SERVICE_RECORDING)
    {
        snapshot->duration_ms = _recorder_duration_locked(now_us);
    }
    _recorder_unlock();
    sd_storage_service_snapshot_t storage = {0};
    const esp_err_t storage_result = sd_storage_service_get_snapshot(&storage);
    snapshot->storage_ready = storage_result == ESP_OK && storage.mounted;
    snapshot->storage_generation = storage_result == ESP_OK ? storage.generation : 0U;
    snapshot->content_revision = storage_result == ESP_OK ? storage.revision : 0U;
    if (!snapshot->storage_ready || space_generation != storage.generation)
    {
        snapshot->space_valid = false;
        snapshot->space_error = ESP_ERR_INVALID_STATE;
        snapshot->free_bytes = 0U;
    }
    return ESP_OK;
}

esp_err_t recorder_service_list(recorder_service_file_t *files,
                                size_t capacity, size_t *count)
{
    if (files == NULL || count == NULL || capacity == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* SD metadata is read outside the recorder lock; validation and copying
     * then observe one index publication under the same critical section. */
    sd_storage_service_snapshot_t storage = {0};
    const bool mounted = sd_storage_service_get_snapshot(&storage) == ESP_OK &&
                         storage.mounted;
    _recorder_lock();
    const bool fresh = mounted && s_recorder.index_valid &&
                       s_recorder.index_generation == storage.generation &&
                       s_recorder.index_revision == storage.revision;
    const size_t available = !fresh ? 0U :
                             (s_recorder.file_count < capacity ? s_recorder.file_count : capacity);
    memcpy(files, s_index_files, available * sizeof(files[0]));
    const bool request_refresh = !fresh && !s_recorder.refresh_requested;
    if (request_refresh)
    {
        s_recorder.refresh_requested = true;
    }
    _recorder_unlock();
    if (request_refresh && _recorder_request_refresh() != ESP_OK)
    {
        _recorder_lock();
        s_recorder.refresh_requested = false;
        _recorder_unlock();
    }
    *count = available;
    return ESP_OK;
}

esp_err_t recorder_service_refresh(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (_recorder_index_is_fresh())
    {
        return ESP_OK;
    }
    _recorder_lock();
    const bool request = !s_recorder.refresh_requested;
    if (request)
    {
        s_recorder.refresh_requested = true;
    }
    _recorder_unlock();
    if (!request)
    {
        return ESP_OK;
    }
    const esp_err_t result = _recorder_request_refresh();
    if (result != ESP_OK)
    {
        _recorder_lock();
        s_recorder.refresh_requested = false;
        _recorder_unlock();
    }
    return result;
}

bool recorder_service_is_busy(void)
{
    if (!s_initialized)
    {
        return false;
    }
    _recorder_lock();
    const bool busy = s_recorder.snapshot.operation_pending ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_RECORDING ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PAUSED ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PLAYING;
    _recorder_unlock();
    return busy;
}

/**
 * @brief Build a human-readable recording stem for the current instant.
 *
 * A calibrated clock yields a local timestamp that stays meaningful after the
 * file leaves this device. An uncalibrated clock (before SNTP) would produce a
 * 1970 stamp, so the monotonic boot counter is used instead.
 */
static void _recorder_name_stem(char *out, size_t out_size)
{
    const time_t now = time(NULL);
    struct tm local_time;
    if (now >= (time_t)RECORDER_MIN_EPOCH_S &&
            localtime_r(&now, &local_time) != NULL &&
            strftime(out, out_size, "REC_%Y%m%d_%H%M%S", &local_time) > 0U)
    {
        return;
    }
    (void)snprintf(out, out_size, "REC_%lld", (long long)esp_timer_get_time());
}

/**
 * @brief Create the working file for a new recording without overwriting.
 *
 * The exclusive open both reserves the final stem and proves the name is
 * free, so a restart inside the same second never truncates an existing
 * recording. The descriptor is released because the WAV writer needs a stream.
 */
static esp_err_t _recorder_create_file(char *out, size_t out_size)
{
    char stem[64];
    _recorder_name_stem(stem, sizeof(stem));
    for (unsigned attempt = 0U; attempt < 100U; ++attempt)
    {
        const int length = attempt == 0U ?
                           snprintf(out, out_size, "%s/%s.part",
                                    s_recorder.directory_path, stem) :
                           snprintf(out, out_size, "%s/%s-%u.part",
                                    s_recorder.directory_path, stem,
                                    attempt + 1U);
        if (length < 0 || (size_t)length >= out_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        errno = 0;
        const int fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (fd < 0)
        {
            if (errno != EEXIST)
            {
                return errno == EIO ? ESP_FAIL : ESP_ERR_INVALID_STATE;
            }
            continue;
        }
        (void)close(fd);
        /* A finalized sibling in the same second owns this stem; giving it up
         * keeps the visible name unique too, not just the working file. The
         * probe rewrites the suffix in place and restores it, because this
         * runs on the command stack and cannot afford a second path buffer. */
        char suffix[6];
        char *const tail = out + (size_t)length - 5U;
        memcpy(suffix, tail, 5U);
        suffix[5] = '\0';
        memcpy(tail, ".wav\0", 5U);
        const bool taken = access(out, F_OK) == 0;
        memcpy(tail, suffix, 5U);
        if (taken)
        {
            (void)remove(out);
            continue;
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t _recorder_start_now(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _recorder_lock();
    const bool busy = s_recorder.snapshot.state == RECORDER_SERVICE_RECORDING ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PAUSED ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PLAYING;
    _recorder_unlock();
    if (busy)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* Occupancy covers directory preparation, file creation, the whole
     * recording, the WAV finalize and the index refresh. */
    const esp_err_t usage_result = _recorder_usage_acquire();
    if (usage_result != ESP_OK)
    {
        return usage_result;
    }
    errno = 0;
    esp_err_t setup_result = _recorder_prepare_directory(true);
    if (setup_result != ESP_OK)
    {
        _recorder_usage_release(errno == EIO, setup_result);
        return setup_result;
    }
    errno = 0;
    const esp_err_t space_result = _recorder_space_refresh(&s_recorder.usage);
    if (space_result != ESP_OK)
    {
        /* A failed query is not "the card is full". */
        _recorder_usage_release(errno == EIO, space_result);
        return space_result;
    }
    _recorder_lock();
    const uint64_t free_bytes = s_recorder.snapshot.free_bytes;
    _recorder_unlock();
    if (free_bytes < s_recorder.config.minimum_free_bytes)
    {
        _recorder_usage_release(false, ESP_OK);
        return ESP_ERR_NO_MEM;
    }
    audio_service_config_t audio_config;
    if (audio_service_get_config(&audio_config) != ESP_OK ||
            audio_config.sample_rate_hz != 16000U ||
            audio_config.bits_per_sample != 16U || audio_config.channels != 2U)
    {
        _recorder_usage_release(false, ESP_OK);
        return ESP_ERR_INVALID_STATE;
    }
    char path[sizeof(s_recorder.snapshot.name)];
    const esp_err_t create_result = _recorder_create_file(path, sizeof(path));
    if (create_result != ESP_OK)
    {
        _recorder_usage_release(errno == EIO, create_result);
        return create_result;
    }
    errno = 0;
    FILE *file = fopen(path, "wb+");
    if (file == NULL)
    {
        const bool medium_eio = errno == EIO;
        (void)remove(path);
        _recorder_usage_release(medium_eio, ESP_FAIL);
        return ESP_FAIL;
    }
    const recorder_wav_header_t empty = {0};
    errno = 0;
    if (fwrite(&empty, 1U, sizeof(empty), file) != sizeof(empty) ||
            fflush(file) != 0)
    {
        bool medium_eio = errno == EIO;
        errno = 0;
        if (fclose(file) != 0)
        {
            medium_eio |= errno == EIO;
        }
        errno = 0;
        if (remove(path) != 0)
        {
            medium_eio |= errno == EIO;
        }
        _recorder_usage_release(medium_eio, ESP_FAIL);
        return ESP_FAIL;
    }
    if (audio_service_start() != ESP_OK)
    {
        errno = 0;
        const int close_result = fclose(file);
        bool medium_eio = close_result != 0 && errno == EIO;
        errno = 0;
        if (remove(path) != 0)
        {
            medium_eio |= errno == EIO;
        }
        _recorder_usage_release(medium_eio, ESP_FAIL);
        return ESP_FAIL;
    }
    const int64_t started_us = esp_timer_get_time();
    _recorder_lock();
    s_recorder.file = file;
    s_recorder.data_bytes = 0U;
    s_recorder.started_us = started_us;
    s_recorder.accumulated_duration_ms = 0U;
    s_recorder.stop_requested = false;
    s_recorder.playback = false;
    s_recorder.snapshot.state = RECORDER_SERVICE_RECORDING;
    (void)snprintf(s_recorder.snapshot.name,
                   sizeof(s_recorder.snapshot.name), "%s", path);
    ++s_recorder.snapshot.generation;
    atomic_store_explicit(&s_recorder.task_running, true,
                          memory_order_release);
    _recorder_unlock();
#ifdef ESP_PLATFORM
    if (_recorder_stream_create() != ESP_OK)
    {
        _recorder_finish(true, ESP_ERR_NO_MEM, false);
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

static esp_err_t _recorder_pause_now(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t now_us = esp_timer_get_time();
    _recorder_lock();
    if (s_recorder.snapshot.state != RECORDER_SERVICE_RECORDING)
    {
        _recorder_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_recorder.accumulated_duration_ms =
        _recorder_duration_locked(now_us);
    s_recorder.started_us = 0;
    s_recorder.snapshot.duration_ms = s_recorder.accumulated_duration_ms;
    s_recorder.snapshot.state = RECORDER_SERVICE_PAUSED;
    ++s_recorder.snapshot.generation;
    _recorder_unlock();
    return ESP_OK;
}

static esp_err_t _recorder_resume_now(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t now_us = esp_timer_get_time();
    _recorder_lock();
    if (s_recorder.snapshot.state != RECORDER_SERVICE_PAUSED)
    {
        _recorder_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_recorder.started_us = now_us;
    s_recorder.snapshot.state = RECORDER_SERVICE_RECORDING;
    ++s_recorder.snapshot.generation;
    _recorder_unlock();
    return ESP_OK;
}

static esp_err_t _recorder_stop_now(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _recorder_lock();
    const bool busy = s_recorder.snapshot.state == RECORDER_SERVICE_RECORDING ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PAUSED ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PLAYING;
    if (!busy)
    {
        _recorder_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_recorder.stop_requested = true;
#ifndef ESP_PLATFORM
    _recorder_unlock();
    _recorder_finish(false, ESP_OK, false);
    return ESP_OK;
#else
    _recorder_unlock();
    while (atomic_load_explicit(&s_recorder.task_running,
                                memory_order_acquire))
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
#endif
    return ESP_OK;
}

static esp_err_t _recorder_play_now(const recorder_service_target_t *target)
{
    if (!s_initialized || target == NULL ||
            memchr(target->name, '\0', sizeof(target->name)) == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _recorder_lock();
    const bool busy = s_recorder.snapshot.state == RECORDER_SERVICE_RECORDING ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PAUSED ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PLAYING;
    _recorder_unlock();
    if (busy)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* Playback holds the business lease from open to close. */
    const esp_err_t usage_result = _recorder_usage_acquire();
    if (usage_result != ESP_OK)
    {
        return usage_result;
    }
    sd_storage_service_snapshot_t storage = {0};
    const bool target_current = sd_storage_service_get_snapshot(&storage) == ESP_OK &&
                                storage.mounted &&
                                target->generation == s_recorder.usage.generation &&
                                target->generation == storage.generation &&
                                target->revision == storage.revision;
    if (!target_current)
    {
        _recorder_usage_release(false, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (_recorder_prepare_directory(false) != ESP_OK ||
            !_recorder_path_is_owned(target->name))
    {
        _recorder_usage_release(false, ESP_OK);
        return ESP_ERR_INVALID_ARG;
    }
    errno = 0;
    FILE *file = fopen(target->name, "rb");
    if (file == NULL)
    {
        const bool medium_eio = errno == EIO;
        _recorder_usage_release(medium_eio, ESP_FAIL);
        return medium_eio ? ESP_FAIL : ESP_ERR_NOT_FOUND;
    }
    recorder_wav_header_t header;
    struct stat info;
    errno = 0;
    const bool file_valid = fread(&header, 1U, sizeof(header), file) == sizeof(header) &&
                            fseek(file, (long)sizeof(recorder_wav_header_t), SEEK_SET) == 0 &&
                            stat(target->name, &info) == 0 &&
                            _recorder_header_valid(&header, (uint64_t)info.st_size);
    bool medium_eio = !file_valid && errno == EIO;
    if (!file_valid || audio_service_start() != ESP_OK)
    {
        errno = 0;
        if (fclose(file) != 0)
        {
            medium_eio |= errno == EIO;
        }
        _recorder_usage_release(medium_eio, ESP_FAIL);
        return ESP_FAIL;
    }
    const int64_t started_us = esp_timer_get_time();
    _recorder_lock();
    s_recorder.file = file;
    s_recorder.data_bytes = 0U;
    s_recorder.started_us = started_us;
    s_recorder.accumulated_duration_ms = 0U;
    s_recorder.playback = true;
    s_recorder.stop_requested = false;
    s_recorder.snapshot.state = RECORDER_SERVICE_PLAYING;
    s_recorder.snapshot.playback_position_ms = 0U;
    s_recorder.snapshot.playback_duration_ms =
        (uint32_t)(((uint64_t)header.data_size) * 1000U / 64000U);
    (void)snprintf(s_recorder.snapshot.name, sizeof(s_recorder.snapshot.name),
                   "%s", target->name);
    ++s_recorder.snapshot.generation;
    atomic_store_explicit(&s_recorder.task_running, true,
                          memory_order_release);
    _recorder_unlock();
#ifdef ESP_PLATFORM
    if (_recorder_stream_create() != ESP_OK)
    {
        _recorder_finish(true, ESP_ERR_NO_MEM, false);
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

static esp_err_t _recorder_stop_playback_now(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _recorder_lock();
    const bool playing = s_recorder.snapshot.state == RECORDER_SERVICE_PLAYING;
    s_recorder.stop_requested = playing;
    _recorder_unlock();
    if (!playing)
    {
        return ESP_ERR_INVALID_STATE;
    }
#ifndef ESP_PLATFORM
    _recorder_finish(false, ESP_OK, false);
#else
    while (atomic_load_explicit(&s_recorder.task_running,
                                memory_order_acquire))
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
#endif
    return ESP_OK;
}

static esp_err_t _recorder_delete_now(const recorder_service_target_t *target)
{
    if (!s_initialized || target == NULL ||
            memchr(target->name, '\0', sizeof(target->name)) == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _recorder_lock();
    const bool busy = s_recorder.snapshot.state == RECORDER_SERVICE_RECORDING ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PAUSED ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PLAYING;
    _recorder_unlock();
    if (busy)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* Deletion is a maintenance operation: it becomes visible as "busy" to the
     * storage service so an unmount or format cannot race it. */
    sd_storage_service_usage_t usage = {0};
    const esp_err_t usage_result = sd_storage_service_usage_acquire(
                                       SD_STORAGE_SERVICE_USAGE_MAINTENANCE,
                                       &usage);
    if (usage_result != ESP_OK)
    {
        return usage_result;
    }
    sd_storage_service_snapshot_t storage = {0};
    const bool target_current = sd_storage_service_get_snapshot(&storage) == ESP_OK &&
                                storage.mounted &&
                                target->generation == usage.generation &&
                                target->generation == storage.generation &&
                                target->revision == storage.revision;
    if (!target_current)
    {
        (void)sd_storage_service_usage_release(&usage);
        return ESP_ERR_INVALID_STATE;
    }
    if (_recorder_prepare_directory(false) != ESP_OK ||
            !_recorder_path_is_owned(target->name))
    {
        (void)sd_storage_service_usage_release(&usage);
        return ESP_ERR_INVALID_ARG;
    }
    errno = 0;
    const esp_err_t result = remove(target->name) == 0 ? ESP_OK : ESP_FAIL;
    if (result == ESP_OK)
    {
        _recorder_lock();
        if (strcmp(s_recorder.snapshot.name, target->name) == 0)
        {
            s_recorder.snapshot.name[0] = '\0';
            s_recorder.snapshot.duration_ms = 0U;
            s_recorder.snapshot.bytes = 0U;
            ++s_recorder.snapshot.generation;
        }
        _recorder_unlock();
        (void)sd_storage_service_note_content_change(&usage);
        _recorder_refresh_index(&usage);
    }
    else if (errno == EIO)
    {
        (void)sd_storage_service_usage_report_fault(&usage, result);
    }
    (void)sd_storage_service_usage_release(&usage);
    return result;
}

static void _recorder_command_complete(const recorder_command_t *command,
                                       esp_err_t result)
{
    _recorder_lock();
    if (s_recorder.snapshot.operation_id == command->operation_id)
    {
        s_recorder.snapshot.operation_pending = false;
        if (result != ESP_OK || s_recorder.snapshot.state != RECORDER_SERVICE_ERROR)
        {
            s_recorder.snapshot.last_error = result;
        }
        ++s_recorder.snapshot.generation;
    }
    _recorder_unlock();
}

static void _recorder_command_task(void *argument)
{
    (void)argument;
    recorder_command_t command;
    for (;;)
    {
#ifdef ESP_PLATFORM
        TickType_t timeout = portMAX_DELAY;
        _recorder_stream_reap();
        _recorder_lock();
        const bool stream_owned = s_recorder.task != NULL;
        _recorder_unlock();
        if (stream_owned) timeout = pdMS_TO_TICKS(50U);
#else
        const uint32_t timeout = portMAX_DELAY;
#endif
        if (xQueueReceive(s_recorder.command_queue, &command, timeout) != pdTRUE)
        {
            continue;
        }
#ifdef ESP_PLATFORM
        _recorder_stream_reap();
#endif
        if (command.type == RECORDER_COMMAND_SHUTDOWN)
        {
            break;
        }
        esp_err_t result = ESP_ERR_INVALID_ARG;
        _recorder_lock();
        const bool stopping = s_recorder.stopping;
        _recorder_unlock();
        if (stopping && command.type != RECORDER_COMMAND_STOP)
        {
            result = ESP_ERR_INVALID_STATE;
        }
        else
        {
            switch (command.type)
            {
            case RECORDER_COMMAND_START:
                result = _recorder_start_now();
                break;
            case RECORDER_COMMAND_PAUSE:
                result = _recorder_pause_now();
                break;
            case RECORDER_COMMAND_RESUME:
                result = _recorder_resume_now();
                break;
            case RECORDER_COMMAND_STOP:
                result = _recorder_stop_now();
                break;
            case RECORDER_COMMAND_PLAY:
                result = _recorder_play_now(&command.target);
                break;
            case RECORDER_COMMAND_STOP_PLAYBACK:
                result = _recorder_stop_playback_now();
                break;
            case RECORDER_COMMAND_DELETE:
                result = _recorder_delete_now(&command.target);
                break;
            case RECORDER_COMMAND_REFRESH_INDEX:
                /* Housekeeping has no user operation result. Index publication
                 * itself advances snapshot generation when its content changes.
                 * Clear the flag so a stale/failed scan remains retryable. */
                _recorder_lock();
                s_recorder.refresh_requested = false;
                _recorder_unlock();
                _recorder_refresh_from_medium();
#ifdef ESP_PLATFORM
                _recorder_stream_reap();
#endif
                continue;
            default:
                break;
            }
        }
#ifdef ESP_PLATFORM
        _recorder_stream_reap();
#endif
        _recorder_command_complete(&command, result);
    }
#ifdef ESP_PLATFORM
    _recorder_stream_reap();
#endif
    _recorder_lock();
    s_recorder.command_task = NULL;
    _recorder_unlock();
    vTaskDelete(NULL);
}

static esp_err_t _recorder_submit_command(recorder_command_type_t type,
        const recorder_service_target_t *target)
{
    if (!s_initialized || s_recorder.command_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if ((type == RECORDER_COMMAND_PLAY || type == RECORDER_COMMAND_DELETE) &&
            (target == NULL ||
             memchr(target->name, '\0', sizeof(target->name)) == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    recorder_command_t command =
    {
        .type = type,
    };
    _recorder_lock();
    if (s_recorder.stopping || s_recorder.snapshot.operation_pending ||
            s_recorder.command_task == NULL ||
            ((type == RECORDER_COMMAND_START || type == RECORDER_COMMAND_PLAY ||
              type == RECORDER_COMMAND_DELETE) &&
             atomic_load_explicit(&s_recorder.task_running, memory_order_acquire)))
    {
        _recorder_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ++s_next_operation_id;
    if (s_next_operation_id == 0U)
    {
        ++s_next_operation_id;
    }
    command.operation_id = s_next_operation_id;
    if (target != NULL)
    {
        command.target = *target;
    }
    s_recorder.snapshot.operation_id = command.operation_id;
    s_recorder.snapshot.operation_pending = true;
    ++s_recorder.snapshot.generation;
    _recorder_unlock();
    if (xQueueSend(s_recorder.command_queue, &command, 0U) != pdTRUE)
    {
        _recorder_lock();
        s_recorder.snapshot.operation_pending = false;
        s_recorder.snapshot.last_error = ESP_ERR_NO_MEM;
        ++s_recorder.snapshot.generation;
        _recorder_unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t recorder_service_start(void)
{
    return _recorder_submit_command(RECORDER_COMMAND_START, NULL);
}

esp_err_t recorder_service_pause(void)
{
    return _recorder_submit_command(RECORDER_COMMAND_PAUSE, NULL);
}

esp_err_t recorder_service_resume(void)
{
    return _recorder_submit_command(RECORDER_COMMAND_RESUME, NULL);
}

esp_err_t recorder_service_stop(void)
{
    return _recorder_submit_command(RECORDER_COMMAND_STOP, NULL);
}

esp_err_t recorder_service_play(const recorder_service_target_t *target)
{
    return _recorder_submit_command(RECORDER_COMMAND_PLAY, target);
}

esp_err_t recorder_service_stop_playback(void)
{
    return _recorder_submit_command(RECORDER_COMMAND_STOP_PLAYBACK, NULL);
}

esp_err_t recorder_service_delete(const recorder_service_target_t *target)
{
    return _recorder_submit_command(RECORDER_COMMAND_DELETE, target);
}
