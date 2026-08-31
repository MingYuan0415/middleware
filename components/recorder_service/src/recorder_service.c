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
#include <stdatomic.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define RECORDER_TASK_CORE 1
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
    bool stop_requested;
    bool task_running;
    bool playback;
#ifdef ESP_PLATFORM
    TaskHandle_t task;
#endif
} recorder_runtime_t;

static recorder_runtime_t s_recorder;
#ifdef ESP_PLATFORM
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#else
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
#endif
static bool s_initialized;

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

static void _recorder_header_write(FILE *file, uint32_t data_bytes)
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
    (void)fseek(file, 0L, SEEK_SET);
    (void)fwrite(&header, 1U, sizeof(header), file);
    (void)fflush(file);
}

static void _recorder_snapshot_update(uint32_t duration_ms)
{
    _recorder_lock();
    s_recorder.snapshot.duration_ms = duration_ms;
    s_recorder.snapshot.bytes = s_recorder.data_bytes;
    s_recorder.snapshot.free_bytes = _recorder_free_bytes(
                                         sd_storage_service_get_mount_path());
    _recorder_unlock();
}

static void _recorder_finish(bool failed, esp_err_t error)
{
    _recorder_lock();
    FILE *file = s_recorder.file;
    const bool playback = s_recorder.playback;
    const uint32_t data_bytes = (uint32_t)s_recorder.data_bytes;
    char path[sizeof(s_recorder.snapshot.name)];
    (void)snprintf(path, sizeof(path), "%s", s_recorder.snapshot.name);
    s_recorder.file = NULL;
    _recorder_unlock();
    if (file != NULL)
    {
        if (!failed && !playback)
        {
            _recorder_header_write(file, data_bytes);
        }
        (void)fclose(file);
        if (!failed && !playback && path[0] != '\0')
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
            }
        }
    }
    (void)audio_service_stop();
    _recorder_lock();
    s_recorder.snapshot.state = failed ? RECORDER_SERVICE_ERROR :
                                RECORDER_SERVICE_IDLE;
    s_recorder.snapshot.last_error = failed ? error : ESP_OK;
    ++s_recorder.snapshot.generation;
    s_recorder.task_running = false;
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
        const uint32_t duration = (uint32_t)((esp_timer_get_time() -
                                              s_recorder.started_us) / 1000LL);
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
        s_recorder.data_bytes += received;
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
    s_recorder.config = *config;
    s_recorder.snapshot.state = RECORDER_SERVICE_IDLE;
    s_recorder.snapshot.last_error = ESP_OK;
    esp_err_t result = _recorder_ensure_directory(config->directory);
    if (result != ESP_OK)
    {
        return result;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t recorder_service_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (recorder_service_is_busy())
    {
        (void)recorder_service_stop();
    }
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

bool recorder_service_is_busy(void)
{
    if (!s_initialized)
    {
        return false;
    }
    _recorder_lock();
    const bool busy = s_recorder.snapshot.state == RECORDER_SERVICE_RECORDING ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PAUSED ||
                      s_recorder.snapshot.state == RECORDER_SERVICE_PLAYING;
    _recorder_unlock();
    return busy;
}

esp_err_t recorder_service_start(void)
{
    if (!s_initialized || recorder_service_is_busy())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const char *mount = sd_storage_service_get_mount_path();
    if (mount == NULL || _recorder_free_bytes(mount) <
            s_recorder.config.minimum_free_bytes)
    {
        return ESP_ERR_NO_MEM;
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
    (void)fwrite(&empty, 1U, sizeof(empty), file);
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
    s_recorder.stop_requested = false;
    s_recorder.playback = false;
    s_recorder.snapshot.state = RECORDER_SERVICE_RECORDING;
    (void)snprintf(s_recorder.snapshot.name,
                   sizeof(s_recorder.snapshot.name), "%s", path);
    ++s_recorder.snapshot.generation;
#ifdef ESP_PLATFORM
    s_recorder.task_running = true;
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

esp_err_t recorder_service_pause(void)
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
    s_recorder.snapshot.state = RECORDER_SERVICE_PAUSED;
    ++s_recorder.snapshot.generation;
    _recorder_unlock();
    return ESP_OK;
}

esp_err_t recorder_service_resume(void)
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
    s_recorder.snapshot.state = RECORDER_SERVICE_RECORDING;
    ++s_recorder.snapshot.generation;
    _recorder_unlock();
    return ESP_OK;
}

esp_err_t recorder_service_stop(void)
{
    if (!s_initialized || !recorder_service_is_busy())
    {
        return ESP_ERR_INVALID_STATE;
    }
#ifdef ESP_PLATFORM
    _recorder_lock();
    s_recorder.stop_requested = true;
    _recorder_unlock();
    while (s_recorder.task_running)
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
#else
    _recorder_finish(false, ESP_OK);
#endif
    return ESP_OK;
}

esp_err_t recorder_service_play(const char *name)
{
    if (!s_initialized || name == NULL || recorder_service_is_busy())
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
    if (fseek(file, (long)sizeof(recorder_wav_header_t), SEEK_SET) != 0 ||
            audio_service_start() != ESP_OK)
    {
        (void)fclose(file);
        return ESP_FAIL;
    }
    _recorder_lock();
    s_recorder.file = file;
    s_recorder.playback = true;
    s_recorder.stop_requested = false;
    s_recorder.snapshot.state = RECORDER_SERVICE_PLAYING;
    (void)snprintf(s_recorder.snapshot.name, sizeof(s_recorder.snapshot.name),
                   "%s", name);
    ++s_recorder.snapshot.generation;
    s_recorder.task_running = true;
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

esp_err_t recorder_service_stop_playback(void)
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
    while (s_recorder.task_running)
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
#else
    _recorder_finish(false, ESP_OK);
#endif
    return ESP_OK;
}

esp_err_t recorder_service_delete(const char *name)
{
    if (!s_initialized || name == NULL || recorder_service_is_busy())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_recorder_path_is_owned(name))
    {
        return ESP_ERR_INVALID_ARG;
    }
    return remove(name) == 0 ? ESP_OK : ESP_FAIL;
}
