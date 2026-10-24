/* Execute the real ESP_PLATFORM stream and command owner with controlled I/O. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "host_freertos.h"
#include "../../src/recorder_service.c"

extern void fake_sd_set_mount_path(const char *path);
extern unsigned fake_sd_leases(sd_storage_service_usage_kind_t kind);

static pthread_mutex_t s_io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_io_changed = PTHREAD_COND_INITIALIZER;
static unsigned s_entered;
static unsigned s_credits;
static bool s_write_through;
static bool s_read_failure;
static bool s_write_failure;
static atomic_bool s_fail_create;
static atomic_bool s_complete_before_create_return;
static atomic_bool s_join_done;
static char s_root[] = "/tmp/mt_recorder_stream_XXXXXX";
static size_t s_base_tasks;
static size_t s_base_caps;
static size_t s_base_wrong;
static size_t s_base_self;

static struct timespec _deadline(void)
{
    struct timespec value;
    assert(clock_gettime(CLOCK_REALTIME, &value) == 0);
    value.tv_sec += 2;
    return value;
}

#define WAIT_UNTIL(condition) do { \
    const int64_t deadline_us = esp_timer_get_time() + 2000000LL; \
    while (!(condition)) { \
        assert(esp_timer_get_time() < deadline_us); \
        usleep(1000U); \
    } \
} while (0)

static bool _running(void)
{
    return atomic_load_explicit(&s_recorder.task_running, memory_order_acquire);
}

BaseType_t __real_xTaskCreatePinnedToCoreWithCaps(
    void (*entry)(void *), const char *name, uint32_t depth,
    void *context, UBaseType_t priority, TaskHandle_t *task,
    BaseType_t core, UBaseType_t caps);
BaseType_t __wrap_xTaskCreatePinnedToCoreWithCaps(
    void (*entry)(void *), const char *name, uint32_t depth,
    void *context, UBaseType_t priority, TaskHandle_t *task,
    BaseType_t core, UBaseType_t caps)
{
    if (atomic_exchange(&s_fail_create, false)) return pdFAIL;
    const BaseType_t result = __real_xTaskCreatePinnedToCoreWithCaps(
                                  entry, name, depth, context, priority, task, core, caps);
    if (result == pdPASS && atomic_exchange(&s_complete_before_create_return, false))
    {
        /* Exercise completion before the owner has published the returned handle. */
        WAIT_UNTIL(!_running());
    }
    return result;
}

static esp_err_t _audio_gate(bool write)
{
    assert(pthread_mutex_lock(&s_io_lock) == 0);
    ++s_entered;
    assert(pthread_cond_broadcast(&s_io_changed) == 0);
    const struct timespec deadline = _deadline();
    while (s_credits == 0U && !(write && s_write_through))
    {
        assert(pthread_cond_timedwait(&s_io_changed, &s_io_lock, &deadline) == 0);
    }
    if (s_credits != 0U) --s_credits;
    const bool failed = write ? s_write_failure : s_read_failure;
    assert(pthread_mutex_unlock(&s_io_lock) == 0);
    return failed ? ESP_FAIL : ESP_OK;
}

esp_err_t audio_service_read(void *data, size_t bytes, size_t *read_bytes,
                             uint32_t timeout_ms)
{
    (void)timeout_ms;
    const esp_err_t result = _audio_gate(false);
    if (result == ESP_OK)
    {
        memset(data, 0x5a, bytes);
        *read_bytes = bytes;
    }
    return result;
}

esp_err_t audio_service_write(void *data, size_t bytes, size_t *written,
                              uint32_t timeout_ms)
{
    (void)timeout_ms;
    const unsigned char *pcm = data;
    for (size_t i = 0U; i < bytes; ++i) assert(pcm[i] == 0x5a);
    const esp_err_t result = _audio_gate(true);
    if (written != NULL) *written = result == ESP_OK ? bytes : 0U;
    return result;
}

static void _io_reset(bool write_through, bool read_failure, bool write_failure)
{
    assert(pthread_mutex_lock(&s_io_lock) == 0);
    s_entered = 0U;
    s_credits = 0U;
    s_write_through = write_through;
    s_read_failure = read_failure;
    s_write_failure = write_failure;
    assert(pthread_mutex_unlock(&s_io_lock) == 0);
}

static void _io_entered(unsigned count)
{
    assert(pthread_mutex_lock(&s_io_lock) == 0);
    const struct timespec deadline = _deadline();
    while (s_entered < count)
    {
        assert(pthread_cond_timedwait(&s_io_changed, &s_io_lock, &deadline) == 0);
    }
    assert(pthread_mutex_unlock(&s_io_lock) == 0);
}

static void _io_release(void)
{
    assert(pthread_mutex_lock(&s_io_lock) == 0);
    ++s_credits;
    assert(pthread_cond_broadcast(&s_io_changed) == 0);
    assert(pthread_mutex_unlock(&s_io_lock) == 0);
}

static recorder_service_snapshot_t _snapshot(void)
{
    recorder_service_snapshot_t snapshot;
    assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
    return snapshot;
}

static recorder_service_snapshot_t _command(void)
{
    WAIT_UNTIL(!_snapshot().operation_pending);
    return _snapshot();
}

static void _reaped(size_t owner_count)
{
    WAIT_UNTIL(host_caps_task_count() == s_base_caps);
    assert(host_caps_task_owner_delete_count() == owner_count + 1U);
    assert(host_dynamic_task_count() == s_base_tasks + 1U);
    assert(host_caps_task_wrong_delete_count() == s_base_wrong);
    assert(host_caps_task_self_delete_count() == s_base_self);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    _recorder_lock();
    assert(s_recorder.task == NULL && s_recorder.file == NULL);
    _recorder_unlock();
}

static unsigned _open_files(void)
{
    DIR *directory = opendir("/proc/self/fd");
    assert(directory != NULL);
    unsigned count = 0U;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (entry->d_name[0] != '.') ++count;
    }
    assert(closedir(directory) == 0);
    return count;
}

static void _start_record(void)
{
    _io_reset(false, false, false);
    assert(recorder_service_start() == ESP_OK);
    assert(_command().last_error == ESP_OK);
    _io_entered(1U);
    assert(_snapshot().state == RECORDER_SERVICE_RECORDING);
    assert(host_caps_task_count() == s_base_caps + 1U);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 1U);
    sd_storage_service_usage_t denied = {0};
    assert(sd_storage_service_usage_acquire(SD_STORAGE_SERVICE_USAGE_MAINTENANCE,
                                            &denied) == ESP_ERR_INVALID_STATE);
}

static bool _stop_requested(void)
{
    _recorder_lock();
    const bool stop = s_recorder.stop_requested;
    _recorder_unlock();
    return stop;
}

static void *_stop_thread(void *argument)
{
    (void)argument;
    assert(recorder_service_stop() == ESP_OK);
    assert(_command().last_error == ESP_OK);
    atomic_store(&s_join_done, true);
    return NULL;
}

static void *_deinit_thread(void *argument)
{
    (void)argument;
    assert(recorder_service_deinit() == ESP_OK);
    atomic_store(&s_join_done, true);
    return NULL;
}

static void _stop_blocked(bool deinit)
{
    pthread_t thread;
    atomic_store(&s_join_done, false);
    assert(pthread_create(&thread, NULL, deinit ? _deinit_thread : _stop_thread, NULL) == 0);
    WAIT_UNTIL(_stop_requested());
    _io_release();
    WAIT_UNTIL(atomic_load(&s_join_done));
    assert(pthread_join(thread, NULL) == 0);
}

static recorder_service_target_t _wav(void)
{
    const recorder_service_snapshot_t snapshot = _snapshot();
    FILE *file = fopen(snapshot.name, "rb");
    assert(file != NULL);
    recorder_wav_header_t header;
    assert(fread(&header, 1U, sizeof(header), file) == sizeof(header));
    assert(memcmp(header.riff, "RIFF", 4U) == 0);
    assert(memcmp(header.wave, "WAVE", 4U) == 0);
    assert(header.channels == 2U && header.sample_rate == 16000U);
    assert(header.bits_per_sample == 16U && header.data_size == 4096U);
    assert(header.file_size == header.data_size + 36U);
    assert(fseek(file, 0, SEEK_END) == 0);
    assert(ftell(file) == (long)(sizeof(header) + header.data_size));
    assert(fclose(file) == 0);
    recorder_service_target_t target =
    {
        .generation = snapshot.storage_generation,
        .revision = snapshot.content_revision,
    };
    snprintf(target.name, sizeof(target.name), "%s", snapshot.name);
    return target;
}

static recorder_service_target_t _record_stop(void)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _start_record();
    _stop_blocked(false);
    assert(_snapshot().state == RECORDER_SERVICE_IDLE);
    _reaped(owners);
    return _wav();
}

static void _play_eof(const recorder_service_target_t *target, bool early_completion)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _io_reset(true, false, false);
    atomic_store(&s_complete_before_create_return, early_completion);
    assert(recorder_service_play(target) == ESP_OK);
    assert(_command().last_error == ESP_OK);
    WAIT_UNTIL(!_running());
    _reaped(owners); /* No stop or other user command drives this reap. */
    assert(_snapshot().state == RECORDER_SERVICE_IDLE);
}

static void _quick_restart(const recorder_service_target_t *target, bool next_play)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _io_reset(false, false, false);
    assert(recorder_service_play(target) == ESP_OK);
    assert(_command().last_error == ESP_OK);
    _io_entered(1U);
    _io_release();
    WAIT_UNTIL(!_running());
    /* Do not wait for owner reap before submitting the replacement. */
    _io_reset(false, false, false);
    assert((next_play ? recorder_service_play(target) : recorder_service_start()) == ESP_OK);
    assert(_command().last_error == ESP_OK);
    _io_entered(1U);
    assert(host_caps_task_owner_delete_count() == owners + 1U);
    assert(host_caps_task_count() == s_base_caps + 1U);
    _stop_blocked(false);
    _reaped(owners + 1U);
}

static void _creation_failures(const recorder_service_target_t *target)
{
    const unsigned descriptors = _open_files();
    const size_t owners = host_caps_task_owner_delete_count();
    const bool saved = atomic_exchange(&s_fail_create, true);
    assert(recorder_service_play(target) == ESP_OK);
    assert(_command().last_error == ESP_ERR_NO_MEM);
    assert(_snapshot().state == RECORDER_SERVICE_ERROR);
    assert(! _running());
    assert(_open_files() == descriptors);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    atomic_store(&s_fail_create, true);
    assert(recorder_service_start() == ESP_OK);
    assert(_command().last_error == ESP_ERR_NO_MEM);
    assert(_snapshot().state == RECORDER_SERVICE_ERROR);
    assert(!_running());
    assert(host_caps_task_count() == s_base_caps);
    assert(host_caps_task_owner_delete_count() == owners);
    assert(_open_files() == descriptors);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    _recorder_lock();
    assert(s_recorder.task == NULL && s_recorder.file == NULL);
    _recorder_unlock();
    atomic_store(&s_fail_create, saved);
}

static void _io_error(bool playback, const recorder_service_target_t *target)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _io_reset(false, !playback, playback);
    assert((playback ? recorder_service_play(target) : recorder_service_start()) == ESP_OK);
    assert(_command().last_error == ESP_OK);
    _io_entered(1U);
    _io_release();
    WAIT_UNTIL(!_running());
    _reaped(owners);
    assert(_snapshot().state == RECORDER_SERVICE_ERROR);
    assert(_snapshot().last_error == ESP_ERR_INVALID_RESPONSE);
}

static void _maximum_duration(void)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _start_record();
    const int64_t expired_start = esp_timer_get_time() - 11000000LL;
    _recorder_lock();
    s_recorder.started_us = expired_start;
    _recorder_unlock();
    _io_release();
    WAIT_UNTIL(!_running());
    _reaped(owners);
    assert(_snapshot().state == RECORDER_SERVICE_IDLE);
    (void)_wav();
}

int main(void)
{
    assert(mkdtemp(s_root) != NULL);
    fake_sd_set_mount_path(s_root);
    s_base_tasks = host_dynamic_task_count();
    s_base_caps = host_caps_task_count();
    s_base_wrong = host_caps_task_wrong_delete_count();
    s_base_self = host_caps_task_self_delete_count();
    const recorder_service_config_t config =
    {
        .directory = "Recordings", .max_duration_seconds = 10U,
        .minimum_free_bytes = 0U,
    };
    assert(recorder_service_init(&config) == ESP_OK);
    recorder_service_target_t target = _record_stop();
    const unsigned descriptors = _open_files();
    _play_eof(&target, false);
    _creation_failures(&target);
    target = _record_stop();
    _io_error(true, &target);
    target = _record_stop();
    _io_error(false, NULL);
    target = _record_stop();
    _maximum_duration();
    target = _record_stop();
    _play_eof(&target, true);
    _quick_restart(&target, true);
    _quick_restart(&target, false);
    /* A completed-before-create-return handle must be reaped before next start. */
    for (unsigned i = 0U; i < 20U; ++i)
    {
        target = _record_stop();
        _play_eof(&target, true);
        assert(_open_files() == descriptors);
    }
    const size_t owners = host_caps_task_owner_delete_count();
    _start_record();
    _stop_blocked(true);
    WAIT_UNTIL(host_dynamic_task_count() == s_base_tasks);
    assert(host_caps_task_count() == s_base_caps);
    assert(host_caps_task_owner_delete_count() == owners + 1U);
    assert(host_caps_task_wrong_delete_count() == s_base_wrong);
    assert(host_caps_task_self_delete_count() == s_base_self);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    assert(_open_files() == descriptors);
    char command[256];
    snprintf(command, sizeof(command), "rm -rf '%s'", s_root);
    assert(system(command) == 0);
    puts("device stream WAV, EOF, error, failure and owner lifecycle regressions passed");
    return 0;
}
