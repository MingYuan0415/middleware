#include "recorder_service.h"
#include "sd_storage_service.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern void fake_sd_set_mount_path(const char *path);
extern void fake_sd_space(esp_err_t error, uint64_t bytes);
extern void fake_sd_block_scan(bool block);
extern void fake_sd_wait_scan(void);
extern void fake_sd_mutate(bool remount);
extern void fake_sd_expect_finalize(bool expect);
extern void fake_sd_expect_discard(void);
extern unsigned fake_sd_leases(sd_storage_service_usage_kind_t kind);
extern void fake_sd_set_ui_thread(void);
extern void fake_sd_block_usage(sd_storage_service_usage_kind_t kind, bool block);
extern void fake_sd_wait_usage(void);
extern unsigned s_fake_audio_starts;

static void _delay(void)
{
    const struct timespec delay = {.tv_nsec = 1000000L};
    nanosleep(&delay, NULL);
}

static recorder_service_snapshot_t _settled(void)
{
    recorder_service_snapshot_t snapshot;
    for (unsigned i = 0U; i < 5000U; ++i)
    {
        assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
        if (!snapshot.operation_pending)
        {
            return snapshot;
        }
        _delay();
    }
    assert(!"command did not settle");
    return snapshot;
}

static void _command(esp_err_t accepted, esp_err_t result)
{
    assert(accepted == ESP_OK);
    assert(_settled().last_error == result);
}

static size_t _list(recorder_service_file_t *files)
{
    size_t count = 0U;
    assert(recorder_service_list(files, RECORDER_SERVICE_MAX_FILES, &count) == ESP_OK);
    return count;
}

static size_t _wait_count(recorder_service_file_t *files, size_t expected)
{
    for (unsigned i = 0U; i < 5000U; ++i)
    {
        const size_t count = _list(files);
        if (count == expected)
        {
            return count;
        }
        _delay();
    }
    assert(!"index did not converge");
    return 0U;
}

/* Hold admission at the worker lease boundary, then recreate the selected path.
 * Neither changing the caller's copy nor refreshing the index may retarget it. */
static void _stale_replacement(recorder_service_file_t *files, bool play,
                               bool remount)
{
    const recorder_service_target_t old = files[0].target;
    recorder_service_target_t submitted = old;
    const sd_storage_service_usage_kind_t kind = play ?
        SD_STORAGE_SERVICE_USAGE_SESSION : SD_STORAGE_SERVICE_USAGE_MAINTENANCE;
    const unsigned starts = s_fake_audio_starts;
    fake_sd_block_usage(kind, true);
    assert((play ? recorder_service_play(&submitted) :
            recorder_service_delete(&submitted)) == ESP_OK);
    fake_sd_wait_usage();
    recorder_service_snapshot_t pending;
    assert(recorder_service_get_snapshot(&pending) == ESP_OK);
    assert(pending.operation_pending);
    assert(unlink(old.name) == 0);
    const char replacement[] = "same-path replacement";
    FILE *file = fopen(old.name, "wb");
    assert(file != NULL);
    assert(fwrite(replacement, 1U, sizeof(replacement), file) == sizeof(replacement));
    assert(fclose(file) == 0);
    fake_sd_mutate(remount);
    sd_storage_service_snapshot_t storage;
    assert(sd_storage_service_get_snapshot(&storage) == ESP_OK);
    submitted.generation = storage.generation;
    submitted.revision = storage.revision;
    fake_sd_block_usage(kind, false);
    const recorder_service_snapshot_t done = _settled();
    assert(done.operation_id == pending.operation_id);
    assert(done.last_error == ESP_ERR_INVALID_STATE);
    assert(s_fake_audio_starts == starts);
    assert(fake_sd_leases(kind) == 0U);
    file = fopen(old.name, "rb");
    char actual[sizeof(replacement)];
    assert(file != NULL && fread(actual, 1U, sizeof(actual), file) == sizeof(actual));
    assert(memcmp(actual, replacement, sizeof(actual)) == 0);
    assert(fgetc(file) == EOF && fclose(file) == 0);
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
    char volume[] = "/tmp/mt-recorder-leases-XXXXXX";
    char other[] = "/tmp/mt-recorder-remount-XXXXXX";
    assert(mkdtemp(volume) != NULL && mkdtemp(other) != NULL);
    fake_sd_set_ui_thread();
    fake_sd_set_mount_path(NULL);
    const recorder_service_config_t config =
    {
        .directory = "MicroTech/Recordings",
        .max_duration_seconds = 60U,
        .minimum_free_bytes = 1U,
    };
    assert(recorder_service_init(&config) == ESP_OK);
    _command(recorder_service_start(), ESP_ERR_INVALID_STATE);
    char recordings[256];
    assert(snprintf(recordings, sizeof(recordings), "%s/MicroTech/Recordings", volume) > 0);
    struct stat info;
    assert(stat(recordings, &info) != 0);

    fake_sd_set_mount_path(volume);
    fake_sd_space(ESP_FAIL, UINT64_MAX);
    _command(recorder_service_start(), ESP_FAIL);
    recorder_service_snapshot_t snapshot = _settled();
    assert(!snapshot.space_valid && snapshot.space_error == ESP_FAIL);
    fake_sd_space(ESP_OK, 0U);
    _command(recorder_service_start(), ESP_ERR_NO_MEM);
    assert(_settled().space_valid);
    fake_sd_space(ESP_OK, UINT64_MAX);

    _command(recorder_service_start(), ESP_OK);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 1U);
    _command(recorder_service_pause(), ESP_OK);
    assert(_settled().state == RECORDER_SERVICE_PAUSED);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 1U);
    sd_storage_service_usage_t browsing = {0};
    sd_storage_service_usage_t maintenance = {0};
    assert(sd_storage_service_usage_acquire(SD_STORAGE_SERVICE_USAGE_ACCESS, &browsing) == ESP_OK);
    assert(sd_storage_service_usage_acquire(SD_STORAGE_SERVICE_USAGE_MAINTENANCE, &maintenance) == ESP_ERR_INVALID_STATE);
    assert(sd_storage_service_usage_release(&browsing) == ESP_OK);
    _command(recorder_service_resume(), ESP_OK);
    fake_sd_expect_finalize(true);
    _command(recorder_service_stop(), ESP_OK);
    assert(_settled().state == RECORDER_SERVICE_IDLE);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    fake_sd_expect_finalize(false);

    recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES];
    _wait_count(files, 1U);
    assert(files[0].bytes == 44U);
    /* A calibrated clock names the file by local time; the boot counter is a
     * fallback that only appears before SNTP. */
    const char *leaf = strrchr(files[0].target.name, '/');
    assert(leaf != NULL);
    unsigned year = 0U;
    unsigned month = 0U;
    unsigned day = 0U;
    unsigned hour = 0U;
    unsigned minute = 0U;
    unsigned second = 0U;
    int consumed = 0;
    assert(sscanf(leaf + 1, "REC_%4u%2u%2u_%2u%2u%2u.wav%n", &year, &month,
                  &day, &hour, &minute, &second, &consumed) == 6);
    assert(consumed == (int)strlen(leaf + 1));
    assert(year >= 2020U && month >= 1U && month <= 12U && day >= 1U &&
           day <= 31U && hour <= 23U && minute <= 59U && second <= 59U);
    assert(files[0].has_mtime && files[0].modified_epoch_s > 0U);
    assert(_settled().max_duration_ms == 60000U);
    /* Retain the real WAV duration regression without synthesizing playback
     * completion on the UI thread. Extend the finalized PCM file to 3 seconds. */
    FILE *wav = fopen(files[0].target.name, "rb+");
    const uint32_t body_bytes = 192000U;
    const uint32_t riff_bytes = body_bytes + 36U;
    assert(wav != NULL && fseek(wav, 4L, SEEK_SET) == 0);
    assert(fwrite(&riff_bytes, sizeof(riff_bytes), 1U, wav) == 1U);
    assert(fseek(wav, 40L, SEEK_SET) == 0);
    assert(fwrite(&body_bytes, sizeof(body_bytes), 1U, wav) == 1U);
    assert(fseek(wav, (long)(44U + body_bytes - 1U), SEEK_SET) == 0);
    assert(fputc(0, wav) == 0 && fclose(wav) == 0);
    fake_sd_mutate(false);
    _wait_count(files, 1U);
    snapshot = _settled();
    assert(snapshot.index_valid);
    assert(snapshot.storage_generation == snapshot.index_generation);
    assert(snapshot.content_revision == snapshot.index_revision);
    assert(files[0].target.generation == snapshot.index_generation);
    assert(files[0].target.revision == snapshot.index_revision);
    _command(recorder_service_play(&files[0].target), ESP_OK);
    assert(_settled().state == RECORDER_SERVICE_PLAYING);
    assert(_settled().playback_duration_ms == 3000U);
    assert(_settled().playback_position_ms == 0U);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 1U);
    assert(sd_storage_service_usage_acquire(SD_STORAGE_SERVICE_USAGE_ACCESS, &browsing) == ESP_OK);
    assert(sd_storage_service_usage_acquire(SD_STORAGE_SERVICE_USAGE_MAINTENANCE, &maintenance) == ESP_ERR_INVALID_STATE);
    assert(sd_storage_service_usage_release(&browsing) == ESP_OK);
    fake_sd_block_scan(true);
    fake_sd_mutate(false);
    assert(_list(files) == 0U);
    fake_sd_wait_scan();
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_ACCESS) == 1U);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 1U);
    assert(sd_storage_service_usage_acquire(SD_STORAGE_SERVICE_USAGE_MAINTENANCE,
                                            &maintenance) == ESP_ERR_INVALID_STATE);
    fake_sd_block_scan(false);
    _wait_count(files, 1U);
    const uint32_t revision = sd_storage_service_content_revision();
    _command(recorder_service_stop_playback(), ESP_OK);
    assert(_settled().playback_position_ms == 3000U);
    assert(sd_storage_service_content_revision() == revision);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);

    /* Full paths must fit even when the leaf name uses its 63-byte limit. */
    {
        const char long_name[] =
            "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL.wav";
        char long_path[RECORDER_SERVICE_PATH_BYTES];
        const int path_length = snprintf(long_path, sizeof(long_path), "%s/%s",
                                         recordings, long_name);
        assert(path_length > 0 && (size_t)path_length < sizeof(long_path));
        _write_empty_wav(long_path);
        fake_sd_mutate(false);
        _wait_count(files, 2U);
        size_t selected = 0U;
        while (selected < 2U && strcmp(files[selected].target.name, long_path) != 0)
        {
            ++selected;
        }
        assert(selected < 2U);
        const recorder_service_target_t target = files[selected].target;
        _command(recorder_service_play(&target), ESP_OK);
        assert(_settled().state == RECORDER_SERVICE_PLAYING);
        assert(strcmp(_settled().name, long_path) == 0);
        _command(recorder_service_stop_playback(), ESP_OK);
        _command(recorder_service_delete(&target), ESP_OK);
        assert(access(long_path, F_OK) != 0);
        _wait_count(files, 1U);
    }

    assert(sd_storage_service_usage_acquire(SD_STORAGE_SERVICE_USAGE_MAINTENANCE,
                                            &maintenance) == ESP_OK);
    fake_sd_mutate(false);
    assert(_list(files) == 0U);
    /* This user command settles behind the rejected refresh, proving it never
     * borrowed filesystem access from the maintenance owner. */
    _command(recorder_service_start(), ESP_ERR_INVALID_STATE);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_ACCESS) == 0U);
    assert(sd_storage_service_usage_release(&maintenance) == ESP_OK);
    _wait_count(files, 1U);

    /* An ACCESS lease spans path resolution, capacity, directory and WAV reads.
     * UI getters remain nonblocking while that worker is deliberately stalled. */
    fake_sd_block_scan(true);
    fake_sd_mutate(false);
    (void)_list(files);
    fake_sd_wait_scan();
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_ACCESS) == 1U);
    assert(sd_storage_service_usage_acquire(SD_STORAGE_SERVICE_USAGE_MAINTENANCE, &maintenance) == ESP_ERR_INVALID_STATE);
    for (unsigned i = 0U; i < 20U; ++i)
    {
        assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
        assert(_list(files) == 0U);
    }
    /* A content revision changing mid-scan must not stamp old data fresh. */
    fake_sd_mutate(false);
    fake_sd_expect_discard();
    fake_sd_block_scan(false);
    _wait_count(files, 1U);

    /* A fault/remount changing the epoch mid-scan discards every old path. */
    fake_sd_block_scan(true);
    fake_sd_mutate(false);
    (void)_list(files);
    fake_sd_wait_scan();
    fake_sd_set_mount_path(other);
    fake_sd_expect_discard();
    fake_sd_block_scan(false);
    _wait_count(files, 0U);
    _command(recorder_service_start(), ESP_OK);
    _command(recorder_service_stop(), ESP_OK);
    _wait_count(files, 1U);
    assert(strncmp(files[0].target.name, other, strlen(other)) == 0);
    _stale_replacement(files, false, false);
    _wait_count(files, 0U);
    _command(recorder_service_start(), ESP_OK);
    _command(recorder_service_stop(), ESP_OK);
    _wait_count(files, 1U);
    _stale_replacement(files, true, false);
    _wait_count(files, 0U);
    _command(recorder_service_start(), ESP_OK);
    _command(recorder_service_stop(), ESP_OK);
    _wait_count(files, 1U);
    _stale_replacement(files, true, true);
    _wait_count(files, 0U);
    _command(recorder_service_start(), ESP_OK);
    _command(recorder_service_stop(), ESP_OK);
    _wait_count(files, 1U);
    _stale_replacement(files, false, true);
    _wait_count(files, 0U);
    _command(recorder_service_start(), ESP_OK);
    _command(recorder_service_stop(), ESP_OK);
    _wait_count(files, 1U);
    _command(recorder_service_delete(&files[0].target), ESP_OK);
    _wait_count(files, 0U);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_MAINTENANCE) == 0U);
    sd_storage_service_snapshot_t storage;
    assert(sd_storage_service_get_snapshot(&storage) == ESP_OK);
    const recorder_service_target_t unowned =
    {
        .name = "/tmp/not-owned.wav",
        .generation = storage.generation,
        .revision = storage.revision,
    };
    _command(recorder_service_play(&unowned), ESP_ERR_INVALID_ARG);
    _command(recorder_service_start(), ESP_OK);
    assert(recorder_service_deinit() == ESP_OK);

    /* New initialization retains the real worker and no-card behavior. */
    fake_sd_set_mount_path(NULL);
    assert(recorder_service_init(&config) == ESP_OK);
    assert(recorder_service_deinit() == ESP_OK);
    char cleanup[512];
    assert(snprintf(cleanup, sizeof(cleanup), "rm -rf '%s' '%s'", volume, other) > 0);
    assert(system(cleanup) == 0);
    return 0;
}
