#include "recorder_service.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern void fake_sd_set_mount_path(const char *path);
extern void fake_sd_space(esp_err_t error, uint64_t bytes);
extern void fake_sd_mutate(bool remount);

static void _delay(void)
{
    const struct timespec delay = {.tv_nsec = 2000000L};
    nanosleep(&delay, NULL);
}

static void _write_wav(const char *path)
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

static void _write_wav_at(const char *path, time_t epoch)
{
    _write_wav(path);
    const struct timespec stamps[2] =
    {
        {.tv_sec = epoch, .tv_nsec = 0},
        {.tv_sec = epoch, .tv_nsec = 0},
    };
    assert(utimensat(AT_FDCWD, path, stamps, 0) == 0);
}

static size_t _list(recorder_service_file_t *files,
                    recorder_service_snapshot_t *snapshot)
{
    size_t count = 0U;
    assert(recorder_service_list(files, RECORDER_SERVICE_MAX_FILES, &count) == ESP_OK);
    assert(recorder_service_get_snapshot(snapshot) == ESP_OK);
    return count;
}

/* A card whose recording directory does not exist yet is an empty library, not
 * a failed scan: the Page must leave "reading the medium" instead of waiting
 * for a publication that never comes. */
static void _test_missing_directory_publishes_empty(void)
{
    char volume[] = "/tmp/mt-recorder-nodir-XXXXXX";
    assert(mkdtemp(volume) != NULL);
    fake_sd_set_mount_path(volume);
    fake_sd_space(ESP_OK, UINT64_MAX);
    const recorder_service_config_t config =
    {
        .directory = "MicroTech/Recordings",
        .max_duration_seconds = 1800U,
        .minimum_free_bytes = 1U,
    };
    assert(recorder_service_init(&config) == ESP_OK);
    recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES];
    recorder_service_snapshot_t snapshot = {0};
    size_t count = 0U;
    bool published = false;
    for (unsigned attempt = 0U; attempt < 5000U; ++attempt)
    {
        count = _list(files, &snapshot);
        if (snapshot.index_valid)
        {
            published = true;
            break;
        }
        fake_sd_mutate(false);
        _delay();
    }
    assert(published);
    assert(count == 0U);
    assert(snapshot.file_count == 0U);
    assert(!snapshot.library_truncated);
    assert(snapshot.space_valid);
    assert(recorder_service_deinit() == ESP_OK);
    char cleanup[512];
    assert(snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", volume) > 0);
    assert(system(cleanup) == 0);
}

/* A name with two dots inside it is a normal recording the scan lists, so play
 * and delete must accept it instead of finding it "not owned". */
static void _test_dotted_name_is_owned(void)
{
    char volume[] = "/tmp/mt-recorder-dots-XXXXXX";
    assert(mkdtemp(volume) != NULL);
    char recordings[256];
    assert(snprintf(recordings, sizeof(recordings), "%s/MicroTech/Recordings",
                    volume) > 0);
    char parent[256];
    assert(snprintf(parent, sizeof(parent), "%s/MicroTech", volume) > 0);
    assert(mkdir(parent, 0775) == 0);
    assert(mkdir(recordings, 0775) == 0);
    char path[RECORDER_SERVICE_PATH_BYTES];
    assert(snprintf(path, sizeof(path), "%s/REC_..take.wav", recordings) > 0);
    _write_wav_at(path, 1716700000);
    fake_sd_set_mount_path(volume);
    fake_sd_space(ESP_OK, UINT64_MAX);
    const recorder_service_config_t config =
    {
        .directory = "MicroTech/Recordings", .max_duration_seconds = 1800U,
        .minimum_free_bytes = 1U,
    };
    assert(recorder_service_init(&config) == ESP_OK);
    recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES];
    recorder_service_snapshot_t snapshot = {0};
    size_t count = 0U;
    recorder_service_target_t target = {0};
    for (unsigned attempt = 0U; attempt < 5000U; ++attempt)
    {
        count = _list(files, &snapshot);
        if (count == 1U)
        {
            target = files[0].target;
            break;
        }
        fake_sd_mutate(false);
        _delay();
    }
    assert(count == 1U);
    /* Deleting it proves the ownership check accepts the dotted basename. */
    assert(recorder_service_delete(&target) == ESP_OK);
    for (unsigned attempt = 0U; attempt < 5000U; ++attempt)
    {
        assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
        if (!snapshot.operation_pending)
        {
            break;
        }
        _delay();
    }
    assert(snapshot.last_error == ESP_OK);
    assert(access(path, F_OK) != 0);
    assert(recorder_service_deinit() == ESP_OK);
    char cleanup[512];
    assert(snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", volume) > 0);
    assert(system(cleanup) == 0);
}

int main(void)
{
    char volume[] = "/tmp/mt-recorder-library-XXXXXX";
    assert(mkdtemp(volume) != NULL);
    char recordings[256];
    assert(snprintf(recordings, sizeof(recordings), "%s/MicroTech/Recordings",
                    volume) > 0);
    char parent[256];
    assert(snprintf(parent, sizeof(parent), "%s/MicroTech", volume) > 0);
    assert(mkdir(parent, 0775) == 0);
    assert(mkdir(recordings, 0775) == 0);
    /* One more recording than the index retains, stamped one minute apart. */
    const size_t total = RECORDER_SERVICE_MAX_FILES + 3U;
    const time_t base = 1716700000;
    for (size_t i = 0U; i < total; ++i)
    {
        char path[RECORDER_SERVICE_PATH_BYTES];
        assert(snprintf(path, sizeof(path), "%s/REC_%zu.wav", recordings, i) > 0);
        _write_wav_at(path, base + (time_t)i * 60);
    }
    fake_sd_set_mount_path(volume);
    fake_sd_space(ESP_OK, UINT64_MAX);
    const recorder_service_config_t config =
    {
        .directory = "MicroTech/Recordings",
        .max_duration_seconds = 1800U,
        .minimum_free_bytes = 1U,
    };
    assert(recorder_service_init(&config) == ESP_OK);
    recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES];
    recorder_service_snapshot_t snapshot = {0};
    size_t count = 0U;
    for (unsigned attempt = 0U; attempt < 5000U; ++attempt)
    {
        count = _list(files, &snapshot);
        if (count == RECORDER_SERVICE_MAX_FILES && snapshot.library_truncated)
        {
            break;
        }
        fake_sd_mutate(false);
        _delay();
    }
    assert(count == RECORDER_SERVICE_MAX_FILES);
    assert(snapshot.library_truncated);
    assert(snapshot.file_count == RECORDER_SERVICE_MAX_FILES);
    assert(snapshot.max_duration_ms == 1800000U);
    /* The oldest three are dropped, and the retained rows are newest first. */
    for (size_t i = 0U; i < count; ++i)
    {
        const size_t expected = total - 1U - i;
        char path[RECORDER_SERVICE_PATH_BYTES];
        assert(snprintf(path, sizeof(path), "%s/REC_%zu.wav", recordings,
                        expected) > 0);
        assert(strcmp(files[i].target.name, path) == 0);
        assert(files[i].has_mtime);
        assert(files[i].modified_epoch_s == (uint32_t)(base + (time_t)expected * 60));
        if (i > 0U)
        {
            assert(files[i - 1U].modified_epoch_s > files[i].modified_epoch_s);
        }
    }
    /* Deleting down to the limit clears the truncation warning. */
    for (size_t i = 0U; i < 3U; ++i)
    {
        char path[RECORDER_SERVICE_PATH_BYTES];
        assert(snprintf(path, sizeof(path), "%s/REC_%zu.wav", recordings, i) > 0);
        assert(remove(path) == 0);
    }
    fake_sd_mutate(false);
    for (unsigned attempt = 0U; attempt < 5000U; ++attempt)
    {
        count = _list(files, &snapshot);
        if (count == RECORDER_SERVICE_MAX_FILES && !snapshot.library_truncated)
        {
            break;
        }
        fake_sd_mutate(false);
        _delay();
    }
    assert(count == RECORDER_SERVICE_MAX_FILES);
    assert(!snapshot.library_truncated);
    assert(strstr(files[count - 1U].target.name, "/REC_3.wav") != NULL);
    /* Two recordings started inside the same second keep distinct names, so a
     * restart never trades one memo for another. */
    char first[RECORDER_SERVICE_PATH_BYTES] = {0};
    char second[RECORDER_SERVICE_PATH_BYTES] = {0};
    for (unsigned cycle = 0U; cycle < 2U; ++cycle)
    {
        assert(recorder_service_start() == ESP_OK);
        for (unsigned attempt = 0U; attempt < 5000U; ++attempt)
        {
            assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
            if (snapshot.state == RECORDER_SERVICE_RECORDING)
            {
                break;
            }
            _delay();
        }
        assert(snapshot.state == RECORDER_SERVICE_RECORDING);
        assert(recorder_service_stop() == ESP_OK);
        for (unsigned attempt = 0U; attempt < 5000U; ++attempt)
        {
            assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
            if (!snapshot.operation_pending)
            {
                break;
            }
            _delay();
        }
        assert(snapshot.last_error == ESP_OK);
        assert(snapshot.state == RECORDER_SERVICE_IDLE);
        (void)snprintf(cycle == 0U ? first : second, RECORDER_SERVICE_PATH_BYTES,
                       "%s", snapshot.name);
    }
    assert(first[0] != '\0' && second[0] != '\0');
    assert(strcmp(first, second) != 0);
    assert(access(first, F_OK) == 0 && access(second, F_OK) == 0);
    assert(recorder_service_deinit() == ESP_OK);
    char cleanup[512];
    assert(snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", volume) > 0);
    assert(system(cleanup) == 0);
    _test_missing_directory_publishes_empty();
    _test_dotted_name_is_owned();
    return 0;
}
