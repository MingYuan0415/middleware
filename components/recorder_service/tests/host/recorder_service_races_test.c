/* White-box race tests include the implementation so independent scan and
 * streaming-finalization publishers can be scheduled without production hooks. */
#include "recorder_service.h"
#include "sd_storage_service.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static void _test_unlock(volatile atomic_flag *flag, memory_order order);
#undef atomic_flag_clear_explicit
#define atomic_flag_clear_explicit(flag, order) _test_unlock(flag, order)

static DIR *_test_opendir(const char *path);
static struct dirent *_test_readdir(DIR *directory);
static int _test_fflush(FILE *file);
static FILE *_test_fopen(const char *path, const char *mode);
static esp_err_t _test_storage_snapshot(sd_storage_service_snapshot_t *snapshot);
static void *_test_calloc(size_t count, size_t size);
static bool s_fail_scan_allocation;
#define calloc _test_calloc
#define opendir _test_opendir
#define readdir _test_readdir
#define fflush _test_fflush
#define fopen _test_fopen
#define sd_storage_service_get_snapshot _test_storage_snapshot
#include "../../src/recorder_service.c"
#undef opendir
#undef readdir
#undef fflush
#undef fopen
#undef sd_storage_service_get_snapshot
#undef calloc

static void *_test_calloc(size_t count, size_t size)
{
    /* The scan allocates one bundle per refresh; fail exactly that request. */
    if (s_fail_scan_allocation && count == 1U &&
            size == sizeof(recorder_scan_t))
    {
        return NULL;
    }
    return calloc(count, size);
}

extern void fake_sd_set_mount_path(const char *path);
extern void fake_sd_mutate(bool remount);
extern unsigned fake_sd_leases(sd_storage_service_usage_kind_t kind);
extern esp_err_t s_fake_audio_stop_error;
extern audio_service_lease_t fake_audio_active_lease(void);
static int s_open_error;
static int s_read_error;
static bool s_fail_after_wav;
static bool s_returned_wav;
static int s_flush_error;
static pthread_mutex_t s_test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_test_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_old_thread;
static bool s_pause_old;
static bool s_old_paused;
static unsigned s_old_snapshots;
static bool s_replace_on_unlock;
static unsigned s_file_opens;

static FILE *_test_fopen(const char *path, const char *mode)
{
    ++s_file_opens;
    return fopen(path, mode);
}

static void _test_unlock(volatile atomic_flag *flag, memory_order order)
{
    (void)order;
    if (s_replace_on_unlock)
    {
        /* Replace the cache at the first recorder unlock after SD metadata was
         * captured. A split freshness/copy path would copy this other index. */
        s_replace_on_unlock = false;
        ++s_recorder.index_revision;
        s_index_files[0].target.revision = s_recorder.index_revision;
        (void)snprintf(s_index_files[0].target.name,
                       sizeof(s_index_files[0].target.name), "wrong-publication");
    }
    atomic_flag_clear(flag);
}

static DIR *_test_opendir(const char *path)
{
    if (s_open_error != 0)
    {
        errno = s_open_error;
        return NULL;
    }
    return opendir(path);
}

static struct dirent *_test_readdir(DIR *directory)
{
    if (s_read_error != 0 && (!s_fail_after_wav || s_returned_wav))
    {
        errno = s_read_error;
        return NULL;
    }
    struct dirent *entry = readdir(directory);
    if (entry != NULL && strstr(entry->d_name, ".wav") != NULL)
    {
        s_returned_wav = true;
    }
    return entry;
}

static int _test_fflush(FILE *file)
{
    if (s_flush_error != 0)
    {
        errno = s_flush_error;
        return EOF;
    }
    return fflush(file);
}

static esp_err_t _test_storage_snapshot(sd_storage_service_snapshot_t *snapshot)
{
    const esp_err_t result = sd_storage_service_get_snapshot(snapshot);
    pthread_mutex_lock(&s_test_mutex);
    if (s_pause_old && pthread_equal(s_old_thread, pthread_self()) &&
            ++s_old_snapshots == 2U)
    {
        /* Pause after copying the final old epoch/revision, before publishing. */
        s_old_paused = true;
        pthread_cond_broadcast(&s_test_cond);
        while (s_pause_old)
        {
            pthread_cond_wait(&s_test_cond, &s_test_mutex);
        }
    }
    pthread_mutex_unlock(&s_test_mutex);
    return result;
}

static void *_old_scan(void *argument)
{
    (void)argument;
    pthread_mutex_lock(&s_test_mutex);
    s_old_thread = pthread_self();
    pthread_mutex_unlock(&s_test_mutex);
    _recorder_refresh_index(NULL);
    return NULL;
}

static void _assert_mounted(bool mounted)
{
    sd_storage_service_snapshot_t snapshot;
    assert(sd_storage_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.mounted == mounted);
}

int main(void)
{
    char volume[] = "/tmp/mt-recorder-races-XXXXXX";
    assert(mkdtemp(volume) != NULL);
    fake_sd_set_mount_path(volume);
    memset(&s_recorder, 0, sizeof(s_recorder));
    atomic_init(&s_recorder.task_running, false);
    s_recorder.config = (recorder_service_config_t)
    {
        .directory = "Recordings",
        .max_duration_seconds = 60U,
        .minimum_free_bytes = 1U,
    };
    s_initialized = true;
    assert(_recorder_start_now() == ESP_OK);
    _recorder_finish(false, ESP_OK, false);
    assert(s_recorder.file_count == 1U);
    /* A valid same-path replacement must not even be opened for an old row. */
    const recorder_service_target_t stale = s_index_files[0].target;
    assert(remove(stale.name) == 0);
    FILE *replacement = fopen(stale.name, "wb+");
    assert(replacement != NULL);
    assert(_recorder_header_write(replacement, 0U) == ESP_OK);
    assert(fclose(replacement) == 0);
    fake_sd_mutate(false);
    const unsigned opens = s_file_opens;
    assert(_recorder_play_now(&stale) == ESP_ERR_INVALID_STATE);
    assert(s_file_opens == opens && s_recorder.file == NULL);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    _recorder_refresh_index(NULL);

    /* Async refresh must wake generation-driven consumers, even when only the
     * external SD revision changed. An identical refresh is not a notification. */
    uint32_t generation = s_recorder.snapshot.generation;
    fake_sd_mutate(false);
    _recorder_refresh_index(NULL);
    assert(s_recorder.snapshot.generation > generation);
    generation = s_recorder.snapshot.generation;
    _recorder_refresh_index(NULL);
    assert(s_recorder.snapshot.generation == generation);

    /* Directory failures stay retryable, without faulting a healthy volume. */
    s_open_error = EACCES;
    _recorder_refresh_index(NULL);
    assert(!s_recorder.index_valid);
    _assert_mounted(true);
    s_open_error = 0;
    _recorder_refresh_index(NULL);
    assert(s_recorder.index_valid && s_recorder.file_count == 1U);
    s_fail_scan_allocation = true;
    _recorder_refresh_index(NULL);
    assert(!s_recorder.index_valid && s_recorder.file_count == 0U);
    assert(s_recorder.snapshot.space_error == ESP_ERR_NO_MEM);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_ACCESS) == 0U);
    _assert_mounted(true);
    s_fail_scan_allocation = false;
    _recorder_refresh_index(NULL);
    assert(s_recorder.index_valid && s_recorder.file_count == 1U);
    s_read_error = EACCES;
    s_fail_after_wav = true;
    s_returned_wav = false;
    _recorder_refresh_index(NULL);
    assert(!s_recorder.index_valid);
    _assert_mounted(true);
    s_read_error = 0;
    assert(s_returned_wav);
    s_fail_after_wav = false;
    _recorder_refresh_index(NULL);
    assert(s_recorder.index_valid && s_recorder.file_count == 1U);

    /* An old scan passed its final SD check, then a newer scan publishes. The
     * old completion must neither overwrite nor invalidate that newer index. */
    pthread_mutex_lock(&s_test_mutex);
    s_pause_old = true;
    pthread_mutex_unlock(&s_test_mutex);
    pthread_t old;
    assert(pthread_create(&old, NULL, _old_scan, NULL) == 0);
    pthread_mutex_lock(&s_test_mutex);
    while (!s_old_paused)
    {
        pthread_cond_wait(&s_test_cond, &s_test_mutex);
    }
    pthread_mutex_unlock(&s_test_mutex);
    fake_sd_mutate(false);
    _recorder_refresh_index(NULL);
    const uint32_t newest_revision = sd_storage_service_content_revision();
    generation = s_recorder.snapshot.generation;
    pthread_mutex_lock(&s_test_mutex);
    s_pause_old = false;
    pthread_cond_broadcast(&s_test_cond);
    pthread_mutex_unlock(&s_test_mutex);
    assert(pthread_join(old, NULL) == 0);
    assert(s_recorder.index_valid && s_recorder.index_revision == newest_revision);
    assert(s_recorder.snapshot.generation == generation);
    recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES];
    size_t count;
    assert(recorder_service_list(files, RECORDER_SERVICE_MAX_FILES, &count) == ESP_OK);
    assert(count == 1U);
    const uint32_t expected_generation = files[0].target.generation;
    const uint32_t expected_revision = files[0].target.revision;
    char expected_name[RECORDER_SERVICE_PATH_BYTES];
    (void)snprintf(expected_name, sizeof(expected_name), "%s", files[0].target.name);
    s_replace_on_unlock = true;
    assert(recorder_service_list(files, RECORDER_SERVICE_MAX_FILES, &count) == ESP_OK);
    assert(count == 1U && strcmp(files[0].target.name, expected_name) == 0);
    assert(files[0].target.generation == expected_generation);
    assert(files[0].target.revision == expected_revision);
    _recorder_refresh_index(NULL);
    /* A different consumer keeps its generation while startup rolls back. */
    audio_service_lease_t background = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &background) == ESP_OK);
    const size_t before_busy = s_recorder.file_count;
    assert(_recorder_start_now() == ESP_ERR_INVALID_STATE);
    assert(s_recorder.file == NULL && !s_recorder.audio_leased);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    _recorder_refresh_index(NULL);
    assert(s_recorder.file_count == before_busy);
    assert(fake_audio_active_lease().generation == background.generation);
    assert(_recorder_play_now(&s_index_files[0].target) == ESP_ERR_INVALID_STATE);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    assert(fake_audio_active_lease().generation == background.generation);
    assert(audio_service_release(background) == ESP_OK);

    /* Task allocation, audio shutdown, and ENOSPC are operation errors, not
     * proof of broken media. Every path still closes and releases SESSION. */
    assert(_recorder_start_now() == ESP_OK);
    _recorder_finish(true, ESP_ERR_NO_MEM, false);
    _assert_mounted(true);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    assert(_recorder_start_now() == ESP_OK);
    s_fake_audio_stop_error = ESP_FAIL;
    assert(_recorder_stop_now() == ESP_FAIL);
    const audio_service_lease_t retained = s_recorder.audio_lease;
    assert(retained.token != 0U && s_recorder.audio_leased);
    const unsigned failed_opens = s_file_opens;
    const size_t failed_count = s_recorder.file_count;
    assert(_recorder_start_now() == ESP_FAIL);
    assert(_recorder_play_now(&s_index_files[0].target) == ESP_FAIL);
    assert(s_file_opens == failed_opens && s_recorder.file_count == failed_count);
    assert(_recorder_stop_now() == ESP_FAIL);
    assert(_recorder_stop_playback_now() == ESP_FAIL);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    assert(s_recorder.audio_lease.token == retained.token &&
           s_recorder.audio_lease.generation == retained.generation);
    assert(fake_audio_active_lease().generation == retained.generation);
    /* INVALID_STATE may mean a service transition, never a released lease. */
    s_fake_audio_stop_error = ESP_ERR_INVALID_STATE;
    assert(_recorder_audio_release() == ESP_ERR_INVALID_STATE);
    assert(s_recorder.audio_leased);
    s_fake_audio_stop_error = ESP_OK;
    _assert_mounted(true);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    assert(_recorder_start_now() == ESP_OK);
    assert(s_recorder.audio_lease.generation != retained.generation);
    assert(audio_service_release(retained) == ESP_ERR_INVALID_STATE);
    s_flush_error = ENOSPC;
    _recorder_finish(false, ESP_OK, false);
    s_flush_error = 0;
    _assert_mounted(true);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);

    /* A confirmed filesystem EIO, unlike those failures, invalidates media. */
    assert(_recorder_start_now() == ESP_OK);
    s_flush_error = EIO;
    _recorder_finish(false, ESP_OK, false);
    s_flush_error = 0;
    _assert_mounted(false);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    fake_sd_set_mount_path(volume);
    s_read_error = EIO;
    _recorder_refresh_index(NULL);
    s_read_error = 0;
    _assert_mounted(false);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_ACCESS) == 0U);
    s_initialized = false;
    char cleanup[256];
    assert(snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", volume) > 0);
    assert(system(cleanup) == 0);
    return 0;
}
