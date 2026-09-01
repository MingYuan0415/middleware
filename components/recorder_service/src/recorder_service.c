#define DBG_TAG "recorder_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "recorder_service.h"

#include "audio_service.h"
#include "esp_timer.h"
#include "sd_storage_service.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <stdatomic.h>

#ifdef ESP_PLATFORM
    #include "freertos/FreeRTOS.h"
    #include "freertos/queue.h"
    #include "freertos/task.h"
    #define RECORDER_TASK_CORE 1
#endif

#ifdef ESP_PLATFORM
typedef enum recorder_command_type
{
    RECORDER_COMMAND_START = 0,
    RECORDER_COMMAND_PAUSE,
    RECORDER_COMMAND_RESUME,
    RECORDER_COMMAND_STOP,
    RECORDER_COMMAND_PLAY,
    RECORDER_COMMAND_STOP_PLAYBACK,
    RECORDER_COMMAND_DELETE,
    RECORDER_COMMAND_SHUTDOWN,
} recorder_command_type_t;

typedef struct recorder_command
{
    recorder_command_type_t type;
    uint32_t operation_id;
    char name[64];
} recorder_command_t;
#endif

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
    recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES];
    size_t file_count;
#ifdef ESP_PLATFORM
    TaskHandle_t task;
    TaskHandle_t command_task;
    QueueHandle_t command_queue;
#endif
} recorder_runtime_t;

static recorder_runtime_t s_recorder;
#ifdef ESP_PLATFORM
    static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#else
    static atomic_flag s_lock = ATOMIC_FLAG_INIT;
#endif
static bool s_initialized;
#ifdef ESP_PLATFORM
static uint32_t s_next_operation_id;
#endif

#ifdef ESP_PLATFORM
static void _recorder_command_task(void *argument);
#endif

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

static uint64_t _recorder_free_bytes(const char *path)
{
    struct statvfs info;
    return path != NULL && statvfs(path, &info) == 0 ?
           (uint64_t)info.f_bavail * info.f_frsize : 0U;
}

static esp_err_t _recorder_ensure_directory(const char *directory)
{
    char path[128];
    const size_t length = directory != NULL ? strlen(directory) : 0U;
    if (length == 0U || length >= sizeof(path))
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s", directory);
    for (char *cursor = path + 1; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '/')
        {
            *cursor = '\0';
            (void)mkdir(path, 0775);
            *cursor = '/';
        }
    }
    return mkdir(path, 0775) == 0 || errno == EEXIST ? ESP_OK : ESP_FAIL;
}

static bool _recorder_path_is_owned(const char *name)
{
    const size_t directory_length = strlen(s_recorder.config.directory);
    const size_t name_length = strlen(name);
    return name_length > directory_length &&
           strncmp(name, s_recorder.config.directory, directory_length) == 0 &&
           name[directory_length] == '/' && strstr(name, "..") == NULL;
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

#ifdef ESP_PLATFORM
static void _recorder_snapshot_update(uint32_t duration_ms)
{
    const uint64_t free_bytes = _recorder_free_bytes(
                                    sd_storage_service_get_mount_path());
    _recorder_lock();
    if (!s_recorder.playback)
    {
        s_recorder.snapshot.duration_ms = duration_ms;
        s_recorder.snapshot.bytes = s_recorder.data_bytes;
    }
    s_recorder.snapshot.free_bytes = free_bytes;
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

static void _recorder_refresh_index(void)
{
    recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES] = {0};
    size_t file_count = 0U;
    DIR *directory = opendir(s_recorder.config.directory);
    if (directory != NULL)
    {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL &&
                file_count < RECORDER_SERVICE_MAX_FILES)
        {
            const size_t name_length = strlen(entry->d_name);
            if (name_length <= 4U ||
                    strcmp(entry->d_name + name_length - 4U, ".wav") != 0)
            {
                continue;
            }
            char path[sizeof(files[0].name)];
            const int length = snprintf(path, sizeof(path), "%s/%s",
                                        s_recorder.config.directory,
                                        entry->d_name);
            if (length < 0 || (size_t)length >= sizeof(path))
            {
                continue;
            }
            struct stat info;
            if (stat(path, &info) != 0 ||
                    info.st_size < (off_t)sizeof(recorder_wav_header_t))
            {
                continue;
            }
            FILE *file = fopen(path, "rb");
            recorder_wav_header_t header;
            const bool valid = file != NULL &&
                               fread(&header, 1U, sizeof(header), file) ==
                               sizeof(header) &&
                               _recorder_header_valid(&header,
                                   (uint64_t)info.st_size);
            if (file != NULL)
            {
                (void)fclose(file);
            }
            if (!valid)
            {
                continue;
            }
            recorder_service_file_t *record = &files[file_count++];
            (void)snprintf(record->name, sizeof(record->name), "%s", path);
            record->bytes = (uint64_t)info.st_size;
            record->duration_ms = (uint32_t)(((uint64_t)header.data_size) *
                                             1000U / 64000U);
        }
        (void)closedir(directory);
    }
    _recorder_lock();
    memcpy(s_recorder.files, files, sizeof(files));
    s_recorder.file_count = file_count;
    _recorder_unlock();
}

static void _recorder_finish(bool failed, esp_err_t error)
{
    _recorder_lock();
    if (!s_recorder.playback)
    {
        s_recorder.snapshot.duration_ms =
            _recorder_duration_locked(esp_timer_get_time());
    }
    FILE *file = s_recorder.file;
    const bool playback = s_recorder.playback;
    const uint64_t recorded_bytes = s_recorder.data_bytes;
    const uint32_t data_bytes = recorded_bytes > UINT32_MAX ? UINT32_MAX :
                                (uint32_t)recorded_bytes;
    char path[sizeof(s_recorder.snapshot.name)];
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
        if (final_error == ESP_OK && !playback &&
                _recorder_header_write(file, data_bytes) != ESP_OK)
        {
            final_error = ESP_FAIL;
        }
        if (fclose(file) != 0 && final_error == ESP_OK)
        {
            final_error = ESP_FAIL;
        }
        if (final_error == ESP_OK && !playback && path[0] != '\0')
        {
            const size_t length = strlen(path);
            if (length > 5U && strcmp(path + length - 5U, ".part") == 0)
            {
                char final_path[sizeof(path)];
                (void)snprintf(final_path, sizeof(final_path), "%.*s.wav",
                               (int)(length - 5U), path);
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
                    final_error = ESP_FAIL;
                }
            }
            else
            {
                final_error = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    _recorder_refresh_index();
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
static void _recorder_task(void *argument)
{
    (void)argument;
    static uint8_t buffer[4096];
    for (;;)
    {
        _recorder_lock();
        const bool stop = s_recorder.stop_requested;
        const bool paused = s_recorder.snapshot.state == RECORDER_SERVICE_PAUSED;
        const bool playback = s_recorder.playback;
        FILE *file = s_recorder.file;
        const uint32_t duration = _recorder_duration_locked(esp_timer_get_time());
        _recorder_unlock();
        if (stop || file == NULL)
        {
            _recorder_finish(false, ESP_OK);
            vTaskDelete(NULL);
            return;
        }
        if (paused && !playback)
        {
            vTaskDelay(pdMS_TO_TICKS(20U));
            continue;
        }
        if (!playback && duration >= s_recorder.config.max_duration_seconds * 1000U)
        {
            _recorder_finish(false, ESP_OK);
            vTaskDelete(NULL);
            return;
        }
        size_t received = 0U;
        bool io_failed = false;
        if (playback)
        {
            received = fread(buffer, 1U, sizeof(buffer), file);
            if (received == 0U)
            {
                if (ferror(file))
                {
                    io_failed = true;
                }
                else
                {
                    _recorder_finish(false, ESP_OK);
                    vTaskDelete(NULL);
                    return;
                }
            }
            if (!io_failed && audio_service_write(buffer, received, NULL, 250U) != ESP_OK)
            {
                io_failed = true;
            }
        }
        else if (audio_service_read(buffer, sizeof(buffer), &received, 250U) != ESP_OK ||
                 received == 0U || fwrite(buffer, 1U, received, file) != received)
        {
            io_failed = true;
        }
        if (io_failed)
        {
            _recorder_finish(true, ESP_ERR_INVALID_RESPONSE);
            vTaskDelete(NULL);
            return;
        }
        _recorder_lock();
        if (playback)
        {
            const uint32_t position = _recorder_elapsed_ms(
                s_recorder.started_us, esp_timer_get_time());
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
#endif

esp_err_t recorder_service_init(const recorder_service_config_t *config)
{
    if (config == NULL || config->directory == NULL ||
            config->directory[0] != '/' || config->max_duration_seconds == 0U)
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
    s_recorder.stopping = false;
    esp_err_t result = _recorder_ensure_directory(config->directory);
    if (result != ESP_OK)
    {
        return result;
    }
#ifdef ESP_PLATFORM
    s_recorder.command_queue = xQueueCreate(4U, sizeof(recorder_command_t));
    if (s_recorder.command_queue == NULL ||
            xTaskCreatePinnedToCore(_recorder_command_task, "recorder_cmd",
                                    4096, NULL, 5,
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
#endif
    s_initialized = true;
    _recorder_refresh_index();
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
    if (recorder_service_is_busy())
    {
        _recorder_lock();
        s_recorder.stop_requested = true;
        _recorder_unlock();
#ifndef ESP_PLATFORM
        _recorder_finish(false, ESP_OK);
#endif
    }
#ifdef ESP_PLATFORM
    const int64_t deadline = esp_timer_get_time() + 5000000LL;
    while (atomic_load_explicit(&s_recorder.task_running,
                                memory_order_acquire) &&
            esp_timer_get_time() < deadline)
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    if (atomic_load_explicit(&s_recorder.task_running, memory_order_acquire))
    {
        return ESP_ERR_TIMEOUT;
    }
    if (s_recorder.command_queue != NULL)
    {
        const recorder_command_t shutdown =
        {
            .type = RECORDER_COMMAND_SHUTDOWN,
        };
        (void)xQueueSend(s_recorder.command_queue, &shutdown,
                         pdMS_TO_TICKS(100U));
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
#endif
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
    _recorder_lock();
    *snapshot = s_recorder.snapshot;
    _recorder_unlock();
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
    _recorder_lock();
    const size_t available = s_recorder.file_count < capacity ?
                             s_recorder.file_count : capacity;
    memcpy(files, s_recorder.files, available * sizeof(files[0]));
    _recorder_unlock();
    *count = available;
    return ESP_OK;
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
    const char *mount = sd_storage_service_get_mount_path();
    if (mount == NULL || _recorder_free_bytes(mount) <
            s_recorder.config.minimum_free_bytes)
    {
        return ESP_ERR_NO_MEM;
    }
    audio_service_config_t audio_config;
    if (audio_service_get_config(&audio_config) != ESP_OK ||
            audio_config.sample_rate_hz != 16000U ||
            audio_config.bits_per_sample != 16U || audio_config.channels != 2U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    char path[sizeof(s_recorder.snapshot.name)];
    const int path_length = snprintf(path, sizeof(path), "%s/REC_%lld.part",
                                     s_recorder.config.directory,
                                     (long long)esp_timer_get_time());
    if (path_length < 0 || (size_t)path_length >= sizeof(path))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(path, "wb+");
    if (file == NULL)
    {
        return ESP_FAIL;
    }
    const recorder_wav_header_t empty = {0};
    if (fwrite(&empty, 1U, sizeof(empty), file) != sizeof(empty) ||
            fflush(file) != 0)
    {
        (void)fclose(file);
        (void)remove(path);
        return ESP_FAIL;
    }
    if (audio_service_start() != ESP_OK)
    {
        (void)fclose(file);
        (void)remove(path);
        return ESP_FAIL;
    }
    _recorder_lock();
    s_recorder.file = file;
    s_recorder.data_bytes = 0U;
    s_recorder.started_us = esp_timer_get_time();
    s_recorder.accumulated_duration_ms = 0U;
    s_recorder.stop_requested = false;
    s_recorder.playback = false;
    s_recorder.snapshot.state = RECORDER_SERVICE_RECORDING;
    (void)snprintf(s_recorder.snapshot.name,
                   sizeof(s_recorder.snapshot.name), "%s", path);
    ++s_recorder.snapshot.generation;
#ifdef ESP_PLATFORM
    atomic_store_explicit(&s_recorder.task_running, true,
                          memory_order_release);
#endif
    _recorder_unlock();
#ifdef ESP_PLATFORM
    if (xTaskCreatePinnedToCore(_recorder_task, "recorder", 4096, NULL, 5,
                                &s_recorder.task, RECORDER_TASK_CORE) != pdPASS)
    {
        _recorder_finish(true, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

static esp_err_t _recorder_pause_now(void)
{
    if (!s_initialized || s_recorder.snapshot.state != RECORDER_SERVICE_RECORDING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _recorder_lock();
    if (s_recorder.snapshot.state != RECORDER_SERVICE_RECORDING)
    {
        _recorder_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_recorder.accumulated_duration_ms =
        _recorder_duration_locked(esp_timer_get_time());
    s_recorder.started_us = 0;
    s_recorder.snapshot.duration_ms = s_recorder.accumulated_duration_ms;
    s_recorder.snapshot.state = RECORDER_SERVICE_PAUSED;
    ++s_recorder.snapshot.generation;
    _recorder_unlock();
    return ESP_OK;
}

static esp_err_t _recorder_resume_now(void)
{
    if (!s_initialized || s_recorder.snapshot.state != RECORDER_SERVICE_PAUSED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _recorder_lock();
    if (s_recorder.snapshot.state != RECORDER_SERVICE_PAUSED)
    {
        _recorder_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_recorder.started_us = esp_timer_get_time();
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
#ifdef ESP_PLATFORM
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
    _recorder_unlock();
    while (atomic_load_explicit(&s_recorder.task_running,
                                memory_order_acquire))
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
#else
    if (!recorder_service_is_busy())
    {
        return ESP_ERR_INVALID_STATE;
    }
    _recorder_finish(false, ESP_OK);
#endif
    return ESP_OK;
}

static esp_err_t _recorder_play_now(const char *name)
{
    if (!s_initialized || name == NULL)
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
    if (!_recorder_path_is_owned(name))
    {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(name, "rb");
    if (file == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    recorder_wav_header_t header;
    struct stat info;
    if (fread(&header, 1U, sizeof(header), file) != sizeof(header) ||
            fseek(file, (long)sizeof(recorder_wav_header_t), SEEK_SET) != 0 ||
            stat(name, &info) != 0 ||
            !_recorder_header_valid(&header, (uint64_t)info.st_size) ||
            audio_service_start() != ESP_OK)
    {
        (void)fclose(file);
        return ESP_FAIL;
    }
    _recorder_lock();
    s_recorder.file = file;
    s_recorder.data_bytes = 0U;
    s_recorder.started_us = esp_timer_get_time();
    s_recorder.accumulated_duration_ms = 0U;
    s_recorder.playback = true;
    s_recorder.stop_requested = false;
    s_recorder.snapshot.state = RECORDER_SERVICE_PLAYING;
    s_recorder.snapshot.playback_position_ms = 0U;
    s_recorder.snapshot.playback_duration_ms =
        (uint32_t)(((uint64_t)header.data_size) * 1000U / 64000U);
    (void)snprintf(s_recorder.snapshot.name, sizeof(s_recorder.snapshot.name),
                   "%s", name);
    ++s_recorder.snapshot.generation;
    atomic_store_explicit(&s_recorder.task_running, true,
                          memory_order_release);
    _recorder_unlock();
#ifdef ESP_PLATFORM
    if (xTaskCreatePinnedToCore(_recorder_task, "recorder", 4096, NULL, 5,
                                &s_recorder.task, RECORDER_TASK_CORE) != pdPASS)
    {
        _recorder_finish(true, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
#else
    _recorder_finish(false, ESP_OK);
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
#ifdef ESP_PLATFORM
    while (atomic_load_explicit(&s_recorder.task_running,
                                memory_order_acquire))
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
#else
    _recorder_finish(false, ESP_OK);
#endif
    return ESP_OK;
}

static esp_err_t _recorder_delete_now(const char *name)
{
    if (!s_initialized || name == NULL)
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
    if (!_recorder_path_is_owned(name))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = remove(name) == 0 ? ESP_OK : ESP_FAIL;
    if (result == ESP_OK)
    {
        _recorder_lock();
        if (strcmp(s_recorder.snapshot.name, name) == 0)
        {
            s_recorder.snapshot.name[0] = '\0';
            s_recorder.snapshot.duration_ms = 0U;
            s_recorder.snapshot.bytes = 0U;
            ++s_recorder.snapshot.generation;
        }
        _recorder_unlock();
        _recorder_refresh_index();
    }
    return result;
}

#ifdef ESP_PLATFORM
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
    while (xQueueReceive(s_recorder.command_queue, &command, portMAX_DELAY) ==
            pdTRUE)
    {
        if (command.type == RECORDER_COMMAND_SHUTDOWN)
        {
            break;
        }
        esp_err_t result = ESP_ERR_INVALID_ARG;
        _recorder_lock();
        const bool stopping = s_recorder.stopping;
        _recorder_unlock();
        if (stopping)
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
                result = _recorder_play_now(command.name);
                break;
            case RECORDER_COMMAND_STOP_PLAYBACK:
                result = _recorder_stop_playback_now();
                break;
            case RECORDER_COMMAND_DELETE:
                result = _recorder_delete_now(command.name);
                break;
            default:
                break;
            }
        }
        _recorder_command_complete(&command, result);
    }
    _recorder_lock();
    s_recorder.command_task = NULL;
    _recorder_unlock();
    vTaskDelete(NULL);
}

static esp_err_t _recorder_submit_command(recorder_command_type_t type,
        const char *name)
{
    if (!s_initialized || s_recorder.command_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    recorder_command_t command =
    {
        .type = type,
    };
    _recorder_lock();
    if (s_recorder.stopping || s_recorder.snapshot.operation_pending ||
            s_recorder.command_task == NULL)
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
    if (name != NULL)
    {
        (void)snprintf(command.name, sizeof(command.name), "%s", name);
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
#endif

esp_err_t recorder_service_start(void)
{
#ifdef ESP_PLATFORM
    return _recorder_submit_command(RECORDER_COMMAND_START, NULL);
#else
    return _recorder_start_now();
#endif
}

esp_err_t recorder_service_pause(void)
{
#ifdef ESP_PLATFORM
    return _recorder_submit_command(RECORDER_COMMAND_PAUSE, NULL);
#else
    return _recorder_pause_now();
#endif
}

esp_err_t recorder_service_resume(void)
{
#ifdef ESP_PLATFORM
    return _recorder_submit_command(RECORDER_COMMAND_RESUME, NULL);
#else
    return _recorder_resume_now();
#endif
}

esp_err_t recorder_service_stop(void)
{
#ifdef ESP_PLATFORM
    return _recorder_submit_command(RECORDER_COMMAND_STOP, NULL);
#else
    return _recorder_stop_now();
#endif
}

esp_err_t recorder_service_play(const char *name)
{
#ifdef ESP_PLATFORM
    return _recorder_submit_command(RECORDER_COMMAND_PLAY, name);
#else
    return _recorder_play_now(name);
#endif
}

esp_err_t recorder_service_stop_playback(void)
{
#ifdef ESP_PLATFORM
    return _recorder_submit_command(RECORDER_COMMAND_STOP_PLAYBACK, NULL);
#else
    return _recorder_stop_playback_now();
#endif
}

esp_err_t recorder_service_delete(const char *name)
{
#ifdef ESP_PLATFORM
    return _recorder_submit_command(RECORDER_COMMAND_DELETE, name);
#else
    return _recorder_delete_now(name);
#endif
}
