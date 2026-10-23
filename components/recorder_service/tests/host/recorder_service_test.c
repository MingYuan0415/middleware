#include "recorder_service.h"

#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int64_t s_now_us;

int64_t esp_timer_get_time(void)
{
    s_now_us += 1000;
    return s_now_us;
}

static void _remove_directory_contents(const char *directory)
{
    DIR *dir = opendir(directory);
    if (dir == NULL)
    {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        char path[256];
        const int length = snprintf(path, sizeof(path), "%s/%s", directory,
                                    entry->d_name);
        if (length > 0 && (size_t)length < sizeof(path))
        {
            (void)unlink(path);
        }
    }
    (void)closedir(dir);
}

static void _write_empty_wav(const char *path)
{
    uint8_t wav[44U] = {0};
    const uint32_t riff_size = 36U;
    const uint32_t fmt_size = 16U;
    const uint16_t format = 1U;
    const uint16_t channels = 2U;
    const uint32_t sample_rate = 16000U;
    const uint32_t byte_rate = 64000U;
    const uint16_t block_align = 4U;
    const uint16_t bits = 16U;
    FILE *file;

    memcpy(&wav[0], "RIFF", 4U);
    memcpy(&wav[4], &riff_size, sizeof(riff_size));
    memcpy(&wav[8], "WAVE", 4U);
    memcpy(&wav[12], "fmt ", 4U);
    memcpy(&wav[16], &fmt_size, sizeof(fmt_size));
    memcpy(&wav[20], &format, sizeof(format));
    memcpy(&wav[22], &channels, sizeof(channels));
    memcpy(&wav[24], &sample_rate, sizeof(sample_rate));
    memcpy(&wav[28], &byte_rate, sizeof(byte_rate));
    memcpy(&wav[32], &block_align, sizeof(block_align));
    memcpy(&wav[34], &bits, sizeof(bits));
    memcpy(&wav[36], "data", 4U);
    file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(wav, 1U, sizeof(wav), file) == sizeof(wav));
    assert(fclose(file) == 0);
}

int main(void)
{
    char directory[] = "/tmp/mt-recorder-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    const recorder_service_config_t config =
    {
        .directory = directory,
        .max_duration_seconds = 60U,
        .minimum_free_bytes = 1U,
    };
    assert(recorder_service_init(&config) == ESP_OK);
    assert(recorder_service_start() == ESP_OK);
    recorder_service_snapshot_t snapshot;
    assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RECORDER_SERVICE_RECORDING);
    assert(recorder_service_start() == ESP_ERR_INVALID_STATE);
    assert(recorder_service_pause() == ESP_OK);
    assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RECORDER_SERVICE_PAUSED);
    assert(recorder_service_resume() == ESP_OK);
    assert(recorder_service_stop() == ESP_OK);
    assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RECORDER_SERVICE_IDLE);
    recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES];
    size_t count = 0U;
    assert(recorder_service_list(files, RECORDER_SERVICE_MAX_FILES,
                                 &count) == ESP_OK);
    assert(count == 1U);
    assert(strstr(files[0].name, ".wav") != NULL);
    /* The stop path finalized the header: the file holds only the 44-byte WAV
     * header, so it reports no body bytes and no duration. */
    assert(files[0].bytes == 44U);
    assert(files[0].duration_ms == 0U);
    assert(recorder_service_play(files[0].name) == ESP_OK);
    assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.state == RECORDER_SERVICE_IDLE);
    assert(snapshot.bytes == 0U);
    assert(snapshot.playback_duration_ms == 0U);
    assert(snapshot.playback_position_ms == 0U);
    const esp_err_t missing_result = recorder_service_play("/tmp/not-owned.wav");
    /* A path outside the recording directory is rejected before any lookup, so
     * only one error is correct here. */
    assert(missing_result == ESP_ERR_INVALID_ARG);
    assert(recorder_service_play(directory) == ESP_ERR_INVALID_ARG);
    assert(recorder_service_delete("/tmp/not-owned.wav") == ESP_ERR_INVALID_ARG);
    assert(recorder_service_delete(directory) == ESP_ERR_INVALID_ARG);
    assert(recorder_service_delete(files[0].name) == ESP_OK);

    /* Full paths must fit even when the leaf name uses its 63-byte limit. */
    {
        char long_name[64] =
            "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL.wav";
        char long_path[RECORDER_SERVICE_PATH_MAX];
        const int path_length = snprintf(long_path, sizeof(long_path), "%s/%s",
                                         directory, long_name);
        assert(path_length > 0 && (size_t)path_length < sizeof(long_path));
        _write_empty_wav(long_path);
        assert(recorder_service_deinit() == ESP_OK);
        assert(recorder_service_init(&config) == ESP_OK);
        assert(recorder_service_list(files, RECORDER_SERVICE_MAX_FILES,
                                     &count) == ESP_OK);
        assert(count == 1U);
        assert(strcmp(files[0].name, long_path) == 0);
        assert(recorder_service_play(files[0].name) == ESP_OK);
        assert(recorder_service_delete(files[0].name) == ESP_OK);
    }

    /* A WAV whose body is three seconds of 64 kB/s PCM must report 3000 ms:
     * the playback timeline divides by the 64000 B/s stream rate. */
    {
        char wav_path[256];
        (void)snprintf(wav_path, sizeof(wav_path), "%s/REC_%lld.wav",
                       directory, (long long)esp_timer_get_time());
        uint8_t wav[44U + 192000U];
        memset(wav, 0, sizeof(wav));
        memcpy(&wav[0], "RIFF", 4U);
        memcpy(&wav[4], &(uint32_t)
        {
            192000U + 36U
        }, 4U);
        memcpy(&wav[8], "WAVE", 4U);
        memcpy(&wav[12], "fmt ", 4U);
        memcpy(&wav[16], &(uint32_t)
        {
            16U
        }, 4U);
        const uint16_t pcm = 1U;
        const uint16_t channels = 2U;
        const uint32_t rate = 16000U;
        const uint32_t byte_rate = 64000U;
        const uint16_t align = 4U;
        const uint16_t bits = 16U;
        const uint32_t data_size = 192000U;
        memcpy(&wav[20], &pcm, 2U);
        memcpy(&wav[22], &channels, 2U);
        memcpy(&wav[24], &rate, 4U);
        memcpy(&wav[28], &byte_rate, 4U);
        memcpy(&wav[32], &align, 2U);
        memcpy(&wav[34], &bits, 2U);
        memcpy(&wav[36], "data", 4U);
        memcpy(&wav[40], &data_size, 4U);
        FILE *file = fopen(wav_path, "wb");

        assert(file != NULL);
        assert(fwrite(wav, 1U, sizeof(wav), file) == sizeof(wav));
        assert(fclose(file) == 0);

        assert(recorder_service_play(wav_path) == ESP_OK);
        assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.playback_duration_ms == 3000U);
        assert(snapshot.playback_position_ms == 3000U);
        assert(recorder_service_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE ||
               snapshot.state == RECORDER_SERVICE_IDLE);
        assert(recorder_service_delete(wav_path) == ESP_OK);
    }

    assert(recorder_service_deinit() == ESP_OK);
    assert(recorder_service_init(&config) == ESP_OK);
    assert(recorder_service_start() == ESP_OK);
    assert(recorder_service_deinit() == ESP_OK);
    assert(recorder_service_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    _remove_directory_contents(directory);
    assert(rmdir(directory) == 0);
    return 0;
}
