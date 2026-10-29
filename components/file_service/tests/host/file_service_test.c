#include "file_service.h"
#include "sd_storage_service.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "host_freertos.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char s_root[] = "/tmp/mt_file_worker_XXXXXX";
static pthread_mutex_t s_sd_lock = PTHREAD_MUTEX_INITIALIZER;
static sd_storage_service_snapshot_t s_sd = {.generation = 1U, .revision = 1U};
static uint32_t s_leases;
static uint32_t s_token;
static atomic_uint s_faults;
static atomic_bool s_fail_queue;
static atomic_int s_read_error;
static atomic_int s_stat_error;
static atomic_int s_rmdir_error;
static atomic_bool s_fail_mount;
static atomic_int s_space_error;
static atomic_int s_space_errno;
static atomic_uint s_mount_calls;
static pthread_mutex_t s_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_gate_changed = PTHREAD_COND_INITIALIZER;
static bool s_block;
static bool s_entered;
static bool s_mount_gate;
static atomic_uint s_allocations;
static unsigned s_init_allocations;
static atomic_bool s_fail_task;
static size_t s_task_baseline;
static size_t s_caps_baseline;
static size_t s_owner_baseline;
static size_t s_wrong_baseline;
static size_t s_self_baseline;

BaseType_t __real_xTaskCreatePinnedToCoreWithCaps(
    void (*entry)(void *), const char *name, uint32_t stack_depth,
    void *context, UBaseType_t priority, TaskHandle_t *task,
    BaseType_t core, UBaseType_t caps);
BaseType_t __wrap_xTaskCreatePinnedToCoreWithCaps(
    void (*entry)(void *), const char *name, uint32_t stack_depth,
    void *context, UBaseType_t priority, TaskHandle_t *task,
    BaseType_t core, UBaseType_t caps)
{
    if (atomic_exchange(&s_fail_task, false)) return pdFAIL;
    return __real_xTaskCreatePinnedToCoreWithCaps(entry, name, stack_depth,
            context, priority, task, core, caps);
}

static void _task_baseline(void)
{
    s_task_baseline = host_dynamic_task_count();
    s_caps_baseline = host_caps_task_count();
    s_owner_baseline = host_caps_task_owner_delete_count();
    s_wrong_baseline = host_caps_task_wrong_delete_count();
    s_self_baseline = host_caps_task_self_delete_count();
}

static void _task_reclaimed(void)
{
    assert(host_dynamic_task_count() == s_task_baseline);
    assert(host_caps_task_count() == s_caps_baseline);
    assert(host_caps_task_owner_delete_count() == s_owner_baseline + 1U);
    assert(host_caps_task_wrong_delete_count() == s_wrong_baseline);
    assert(host_caps_task_self_delete_count() == s_self_baseline);
}

void *file_service_test_allocate(size_t size, unsigned caps)
{
    (void)caps;
    atomic_fetch_add(&s_allocations, 1U);
    return malloc(size);
}

void file_service_test_release(void *pointer)
{
    free(pointer);
}

static void _gate(void)
{
    pthread_mutex_lock(&s_gate_lock);
    if (s_block)
    {
        s_entered = true;
        pthread_cond_broadcast(&s_gate_changed);
        while (s_block) pthread_cond_wait(&s_gate_changed, &s_gate_lock);
    }
    pthread_mutex_unlock(&s_gate_lock);
}

static void _block(bool mount)
{
    pthread_mutex_lock(&s_gate_lock);
    s_block = true;
    s_entered = false;
    s_mount_gate = mount;
    pthread_mutex_unlock(&s_gate_lock);
}

static void _entered(void)
{
    pthread_mutex_lock(&s_gate_lock);
    while (!s_entered) pthread_cond_wait(&s_gate_changed, &s_gate_lock);
    pthread_mutex_unlock(&s_gate_lock);
}

static void _release(void)
{
    pthread_mutex_lock(&s_gate_lock);
    s_block = false;
    pthread_cond_broadcast(&s_gate_changed);
    pthread_mutex_unlock(&s_gate_lock);
}

DIR *__real_opendir(const char *path);
DIR *__wrap_opendir(const char *path)
{
    _gate();
    return __real_opendir(path);
}
struct dirent *__real_readdir(DIR *directory);
struct dirent *__wrap_readdir(DIR *directory)
{
    int error = atomic_exchange(&s_read_error, 0);
    if (error != 0)
    {
        errno = error;
        return NULL;
    }
    return __real_readdir(directory);
}
int __real_lstat(const char *path, struct stat *info);
int __wrap_lstat(const char *path, struct stat *info)
{
    int error = atomic_exchange(&s_stat_error, 0);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    return __real_lstat(path, info);
}
int __real_rmdir(const char *path);
int __wrap_rmdir(const char *path)
{
    _gate();
    int error = atomic_exchange(&s_rmdir_error, 0);
    if (error != 0)
    {
        errno = error;
        return -1;
    }
    return __real_rmdir(path);
}
int __real_unlink(const char *path);
int __wrap_unlink(const char *path)
{
    _gate();
    return __real_unlink(path);
}
BaseType_t __real_xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
BaseType_t __wrap_xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    if (atomic_exchange(&s_fail_queue, false)) return pdFALSE;
    return __real_xQueueSend(queue, item, timeout);
}

esp_err_t sd_storage_service_get_snapshot(sd_storage_service_snapshot_t *snapshot)
{
    pthread_mutex_lock(&s_sd_lock);
    *snapshot = s_sd;
    pthread_mutex_unlock(&s_sd_lock);
    return ESP_OK;
}
uint32_t sd_storage_service_content_revision(void)
{
    sd_storage_service_snapshot_t snapshot;
    sd_storage_service_get_snapshot(&snapshot);
    return snapshot.revision;
}
esp_err_t sd_storage_service_usage_acquire(sd_storage_service_usage_kind_t kind,
        sd_storage_service_usage_t *usage)
{
    pthread_mutex_lock(&s_sd_lock);
    if (!s_sd.mounted || s_sd.faulted || s_sd.maintenance ||
            (kind == SD_STORAGE_SERVICE_USAGE_MAINTENANCE && s_leases != 0U))
    {
        pthread_mutex_unlock(&s_sd_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ++s_leases;
    usage->token = ++s_token;
    usage->generation = s_sd.generation;
    pthread_mutex_unlock(&s_sd_lock);
    return ESP_OK;
}
esp_err_t sd_storage_service_usage_release(const sd_storage_service_usage_t *usage)
{
    assert(usage->token != 0U);
    pthread_mutex_lock(&s_sd_lock);
    assert(s_leases != 0U);
    --s_leases;
    pthread_mutex_unlock(&s_sd_lock);
    return ESP_OK;
}
esp_err_t sd_storage_service_note_content_change(const sd_storage_service_usage_t *usage)
{
    assert(usage->token != 0U);
    pthread_mutex_lock(&s_sd_lock);
    assert(s_leases != 0U);
    ++s_sd.revision;
    pthread_mutex_unlock(&s_sd_lock);
    return ESP_OK;
}
esp_err_t sd_storage_service_usage_report_success(
    const sd_storage_service_usage_t *usage)
{
    assert(usage->token != 0U);
    return ESP_OK;
}
esp_err_t sd_storage_service_usage_report_fault(
    const sd_storage_service_usage_t *usage, esp_err_t error)
{
    assert(usage->token != 0U);
    atomic_fetch_add(&s_faults, 1U);
    pthread_mutex_lock(&s_sd_lock);
    assert(usage->generation == s_sd.generation);
    s_sd.faulted = true;
    s_sd.mounted = false;
    s_sd.last_error = error == ESP_OK ? ESP_FAIL : error;
    ++s_sd.generation;
    pthread_mutex_unlock(&s_sd_lock);
    return ESP_OK;
}
esp_err_t sd_storage_service_get_space(const sd_storage_service_usage_t *usage,
                                       uint64_t *total, uint64_t *free_bytes)
{
    assert(usage->token != 0U);
    const esp_err_t error = atomic_load(&s_space_error);
    if (error != ESP_OK)
    {
        errno = atomic_load(&s_space_errno);
        return error;
    }
    *total = 8192U;
    *free_bytes = 4096U;
    return ESP_OK;
}
esp_err_t sd_storage_service_init(const sd_storage_service_config_t *config)
{
    (void)config;
    atomic_fetch_add(&s_mount_calls, 1U);
    pthread_mutex_lock(&s_gate_lock);
    bool gate = s_mount_gate;
    pthread_mutex_unlock(&s_gate_lock);
    if (gate) _gate();
    pthread_mutex_lock(&s_sd_lock);
    assert(s_leases == 0U);
    s_sd.mounted = !atomic_load(&s_fail_mount);
    s_sd.faulted = false;
    s_sd.last_error = s_sd.mounted ? ESP_OK : ESP_FAIL;
    ++s_sd.generation;
    ++s_sd.revision;
    pthread_mutex_unlock(&s_sd_lock);
    return atomic_load(&s_fail_mount) ? ESP_FAIL : ESP_OK;
}
esp_err_t sd_storage_service_recover_and_mount(const sd_storage_service_config_t *config,
        uint32_t expected_generation)
{
    assert(expected_generation == s_sd.generation);
    return sd_storage_service_init(config);
}
esp_err_t sd_storage_service_unmount(uint32_t expected_generation)
{
    pthread_mutex_lock(&s_sd_lock);
    assert(expected_generation == s_sd.generation && s_leases == 0U);
    s_sd.mounted = false;
    ++s_sd.generation;
    ++s_sd.revision;
    pthread_mutex_unlock(&s_sd_lock);
    return ESP_OK;
}
esp_err_t sd_storage_service_format(const sd_storage_service_config_t *config,
                                    uint32_t expected_generation)
{
    assert(expected_generation == s_sd.generation);
    return sd_storage_service_init(config);
}

static file_service_snapshot_t _snapshot(void)
{
    file_service_snapshot_t snapshot;
    assert(file_service_get_snapshot(&snapshot) == ESP_OK);
    return snapshot;
}
static void _idle(void)
{
    for (unsigned n = 0; n < 2000U; ++n)
    {
        if (!_snapshot().busy) return;
        usleep(1000);
    }
    assert(!"worker did not drain");
}
static file_service_result_t _result(uint32_t session)
{
    file_service_result_t result;
    for (unsigned n = 0; n < 2000U; ++n)
    {
        assert(file_service_session_get_result(session, &result) == ESP_OK);
        if (result.state >= FILE_SERVICE_RESULT_DONE) return result;
        usleep(1000);
    }
    assert(!"worker did not finish");
    return result;
}
static void _start(void)
{
    _task_baseline();
    const file_service_config_t config = {.mount_path = s_root};
    assert(file_service_init(&config) == ESP_OK);
    assert(host_dynamic_task_count() == s_task_baseline + 1U);
    assert(host_caps_task_count() == s_caps_baseline + 1U);
    s_init_allocations = atomic_load(&s_allocations);
    _idle();
    assert(_snapshot().storage.operation_id != 0U);
}
static uint32_t _open(void)
{
    uint32_t session;
    assert(file_service_session_open(&session) == ESP_OK);
    return session;
}
static file_service_target_t *_target(const char *path)
{
    static file_service_target_t target;
    file_service_snapshot_t snapshot = _snapshot();
    memset(&target, 0, sizeof(target));
    strcpy(target.relative_path, path);
    target.generation = snapshot.generation;
    target.revision = snapshot.revision;
    return &target;
}
static void _file(const char *name)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", s_root, name);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fputs("abc", file) >= 0);
    assert(fclose(file) == 0);
}
static void _stop(void)
{
    assert(file_service_deinit() == ESP_OK);
    _task_reclaimed();
    assert(atomic_load(&s_allocations) == s_init_allocations);
    pthread_mutex_lock(&s_sd_lock);
    assert(s_leases == 0U);
    pthread_mutex_unlock(&s_sd_lock);
}

static void _ownership_and_copy(void)
{
    _start();
    _file("中文.txt");
    uint32_t old = _open();
    _block(false);
    assert(file_service_session_list(old, "", NULL, NULL) == ESP_OK);
    _entered();
    assert(_snapshot().pending_requests == 1U);
    assert(file_service_session_close(old) == ESP_OK);
    uint32_t others[3];
    for (unsigned n = 0; n < 3U; ++n) others[n] = _open();
    uint32_t denied;
    assert(file_service_session_open(&denied) == ESP_ERR_NO_MEM);
    assert(file_service_session_list(others[0], "", NULL, NULL) == ESP_OK);
    assert(file_service_session_cancel(others[0]) == ESP_OK);
    assert(file_service_suspend(0U) == ESP_ERR_TIMEOUT);
    assert(file_service_resume() == ESP_OK);
    _release();
    _idle();
    assert(_result(others[0]).reason == FILE_SERVICE_ERROR_CANCELLED);
    uint32_t fresh = _open();
    assert(fresh != old);
    assert(file_service_session_list(fresh, "", NULL, NULL) == ESP_OK);
    assert(_result(fresh).state == FILE_SERVICE_RESULT_DONE);
    file_service_batch_t *copy = malloc(sizeof(*copy));
    assert(copy != NULL);
    assert(file_service_session_get_batch(fresh, copy) == ESP_OK);
    size_t count = copy->count;
    assert(count != 0U);
    _block(false);
    assert(file_service_session_list(fresh, "", NULL, NULL) == ESP_OK);
    _entered();
    assert(copy->count == count);
    assert(file_service_session_get_batch(fresh, copy) == ESP_OK);
    assert(copy->count == count);
    _release();
    _idle();
    free(copy);
    _stop();
}

static void _queue_and_epochs(void)
{
    _start();
    uint32_t session = _open();
    file_service_result_t result;
    atomic_store(&s_fail_queue, true);
    assert(file_service_session_list(session, "", NULL, &result) != ESP_OK);
    assert(result.reason == FILE_SERVICE_ERROR_BUSY);
    assert(_snapshot().pending_requests == 0U);
    atomic_store(&s_fail_queue, true);
    assert(file_service_request_storage(FILE_SERVICE_STORAGE_REFRESH_SPACE,
                                        _snapshot().generation, &result) != ESP_OK);
    assert(result.reason == FILE_SERVICE_ERROR_BUSY);
    assert(_snapshot().pending_requests == 0U);
    _file("stale.txt");
    file_service_target_t *target = _target("stale.txt");
    pthread_mutex_lock(&s_sd_lock);
    ++s_sd.revision;
    pthread_mutex_unlock(&s_sd_lock);
    esp_err_t error = file_service_session_delete(session, target, &result);
    if (error == ESP_OK) result = _result(session);
    assert(result.reason == FILE_SERVICE_ERROR_STALE);
    char path[1200];
    snprintf(path, sizeof(path), "%s/stale.txt", s_root);
    assert(access(path, F_OK) == 0);
    uint32_t generation = _snapshot().generation;
    assert(file_service_request_storage(FILE_SERVICE_STORAGE_UNMOUNT,
                                        generation - 1U, &result) != ESP_OK);
    assert(result.reason == FILE_SERVICE_ERROR_STALE);
    uint32_t blocker = _open();
    _block(false);
    assert(file_service_session_list(blocker, "", NULL, NULL) == ESP_OK);
    _entered();
    assert(file_service_session_delete(session, _target("stale.txt"), NULL) == ESP_OK);
    assert(file_service_request_storage(FILE_SERVICE_STORAGE_RECOVER_FORMAT,
                                        generation, NULL) == ESP_OK);
    pthread_mutex_lock(&s_sd_lock);
    ++s_sd.revision;
    ++s_sd.generation;
    pthread_mutex_unlock(&s_sd_lock);
    _release();
    _idle();
    assert(_result(session).reason == FILE_SERVICE_ERROR_STALE);
    assert(_snapshot().storage.reason == FILE_SERVICE_ERROR_STALE);
    assert(access(path, F_OK) == 0);
    assert(file_service_session_close(blocker) == ESP_OK);
    assert(file_service_session_detail(session, _target("stale.txt"), NULL) == ESP_OK);
    assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
    file_service_detail_t *detail = malloc(sizeof(*detail));
    assert(detail != NULL);
    assert(file_service_session_get_detail(session, detail) == ESP_OK);
    assert(strcmp(detail->name, "stale.txt") == 0);
    free(detail);
    assert(file_service_session_delete(session, _target("stale.txt"), NULL) == ESP_OK);
    assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
    assert(access(path, F_OK) != 0);
    assert(file_service_suspend(20U) == ESP_OK);
    assert(file_service_session_list(session, "", NULL, &result) != ESP_OK);
    assert(result.reason == FILE_SERVICE_ERROR_BUSY);
    assert(file_service_resume() == ESP_OK);
    _stop();
}

static void _errors_and_symlinks(void)
{
    _start();
    uint32_t session = _open();
    unsigned faults = atomic_load(&s_faults);
    const int errors[] = {ENOENT, EACCES, ENAMETOOLONG, ENOSPC, ENOMEM, EIO};
    const file_service_error_reason_t reasons[] = {FILE_SERVICE_ERROR_NOT_FOUND,
                                                   FILE_SERVICE_ERROR_PERMISSION, FILE_SERVICE_ERROR_PATH_TOO_LONG,
                                                   FILE_SERVICE_ERROR_NO_SPACE, FILE_SERVICE_ERROR_NO_MEMORY, FILE_SERVICE_ERROR_IO
                                                  };
    for (size_t n = 0; n < sizeof(errors) / sizeof(errors[0]); ++n)
    {
        atomic_store(&s_read_error, errors[n]);
        assert(file_service_session_list(session, "", NULL, NULL) == ESP_OK);
        file_service_result_t result = _result(session);
        assert(result.state == FILE_SERVICE_RESULT_FAILED);
        assert(result.reason == reasons[n] && result.system_errno == errors[n]);
    }
    assert(atomic_load(&s_faults) == faults + 1U);
    assert(file_service_request_storage(FILE_SERVICE_STORAGE_MOUNT,
                                        _snapshot().generation, NULL) == ESP_OK);
    _idle();
    assert(!_snapshot().faulted && _snapshot().storage.state ==
           FILE_SERVICE_RESULT_DONE);
    char directory[1200], link[1200];
    snprintf(directory, sizeof(directory), "%s/full", s_root);
    assert(mkdir(directory, 0700) == 0);
    _file("full/child");
    atomic_store(&s_rmdir_error, EACCES);
    assert(file_service_session_delete(session, _target("full"), NULL) == ESP_OK);
    file_service_result_t result = _result(session);
    assert(result.reason == FILE_SERVICE_ERROR_NOT_EMPTY && result.system_errno == EACCES);
    snprintf(link, sizeof(link), "%s/link", s_root);
    assert(symlink(directory, link) == 0);
    assert(file_service_session_delete(session, _target("link/child"), NULL) == ESP_OK);
    assert(_result(session).reason == FILE_SERVICE_ERROR_INVALID);
    assert(file_service_session_detail(session, _target("link"), NULL) == ESP_OK);
    assert(_result(session).reason == FILE_SERVICE_ERROR_INVALID);
    assert(file_service_session_list(session, "", NULL, NULL) == ESP_OK);
    result = _result(session);
    assert(result.reason == FILE_SERVICE_ERROR_INVALID);
    assert(atomic_load(&s_faults) == faults + 1U);
    pthread_mutex_lock(&s_sd_lock);
    s_sd.faulted = true;
    s_sd.mounted = false;
    s_sd.last_error = ESP_FAIL;
    pthread_mutex_unlock(&s_sd_lock);
    file_service_snapshot_t snapshot = _snapshot();
    assert(snapshot.faulted && snapshot.last_error == ESP_FAIL);
    assert(snapshot.volume == FILE_SERVICE_VOLUME_UNAVAILABLE);
    pthread_mutex_lock(&s_sd_lock);
    s_sd.faulted = false;
    pthread_mutex_unlock(&s_sd_lock);
    assert(unlink(link) == 0);
    _stop();
}

static void _worker_create_failure(void)
{
    const bool saved = atomic_exchange(&s_fail_task, true);
    _task_baseline();
    const file_service_config_t config = {.mount_path = s_root};
    assert(file_service_init(&config) == ESP_ERR_NO_MEM);
    assert(host_dynamic_task_count() == s_task_baseline);
    assert(host_caps_task_count() == s_caps_baseline);
    assert(host_caps_task_owner_delete_count() == s_owner_baseline);
    pthread_mutex_lock(&s_sd_lock);
    assert(s_leases == 0U);
    pthread_mutex_unlock(&s_sd_lock);
    atomic_store(&s_fail_task, saved);
    _start();
    const uint32_t session = _open();
    assert(file_service_session_list(session, "", NULL, NULL) == ESP_OK);
    assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
    _stop();
}

static void _initial_mount_and_stop(void)
{
    _task_baseline();
    _block(true);
    const file_service_config_t config = {.mount_path = s_root};
    assert(file_service_init(&config) == ESP_OK);
    assert(host_caps_task_count() == s_caps_baseline + 1U);
    _entered();
    file_service_snapshot_t snapshot = _snapshot();
    assert(snapshot.busy && snapshot.pending_requests == 1U);
    assert(snapshot.storage.state == FILE_SERVICE_RESULT_RUNNING);
    assert(file_service_suspend(0U) == ESP_ERR_TIMEOUT);
    assert(file_service_deinit() == ESP_ERR_TIMEOUT);
    assert(host_dynamic_task_count() == s_task_baseline + 1U);
    assert(host_caps_task_owner_delete_count() == s_owner_baseline);
    assert(_snapshot().suspended);
    assert(file_service_resume() == ESP_ERR_INVALID_STATE);
    _release();
    assert(file_service_deinit() == ESP_OK);
    _task_reclaimed();
    pthread_mutex_lock(&s_gate_lock);
    s_mount_gate = false;
    pthread_mutex_unlock(&s_gate_lock);
    atomic_store(&s_fail_mount, true);
    _start();
    assert(_snapshot().storage.state == FILE_SERVICE_RESULT_FAILED);
    atomic_store(&s_fail_mount, false);
    assert(file_service_request_storage(FILE_SERVICE_STORAGE_MOUNT,
                                        _snapshot().generation, NULL) == ESP_OK);
    _idle();
    assert(_snapshot().storage.state == FILE_SERVICE_RESULT_DONE);
    _stop();

    _start();
    const uint32_t session = _open();
    _block(false);
    assert(file_service_session_list(session, "", NULL, NULL) == ESP_OK);
    _entered();
    pthread_mutex_lock(&s_sd_lock);
    const uint32_t leases = s_leases;
    assert(leases != 0U);
    pthread_mutex_unlock(&s_sd_lock);
    assert(file_service_deinit() == ESP_ERR_TIMEOUT);
    assert(host_dynamic_task_count() == s_task_baseline + 1U);
    assert(host_caps_task_count() == s_caps_baseline + 1U);
    assert(host_caps_task_owner_delete_count() == s_owner_baseline);
    pthread_mutex_lock(&s_sd_lock);
    assert(s_leases == leases);
    pthread_mutex_unlock(&s_sd_lock);
    _release();
    _stop();
}

static void _pagination_and_paths(void)
{
    _start();
    for (unsigned n = 0U; n < 40U; ++n)
    {
        char name[32];
        snprintf(name, sizeof(name), "page_%02u.txt", n);
        _file(name);
    }
    for (unsigned n = 0U; n < 1100U; ++n)
    {
        char path[1200];
        snprintf(path, sizeof(path), "%s/many_%04u", s_root, n);
        assert(mkdir(path, 0700) == 0);
    }
    char edge_path[1200], edge_name[48];
    snprintf(edge_path, sizeof(edge_path), "%s/edge_files", s_root);
    assert(mkdir(edge_path, 0700) == 0);
    snprintf(edge_path, sizeof(edge_path), "%s/edge_dirs", s_root);
    assert(mkdir(edge_path, 0700) == 0);
    snprintf(edge_path, sizeof(edge_path), "%s/edge_mixed", s_root);
    assert(mkdir(edge_path, 0700) == 0);
    for (unsigned n = 0U; n < FILE_SERVICE_BATCH_ENTRIES; ++n)
    {
        snprintf(edge_name, sizeof(edge_name), "edge_files/file_%02u.txt", n);
        _file(edge_name);
        const char *parents[] = {"edge_dirs", "edge_mixed"};
        for (size_t parent = 0U; parent < sizeof(parents) / sizeof(parents[0]);
                ++parent)
        {
            snprintf(edge_path, sizeof(edge_path), "%s/%s/dir_%02u", s_root,
                     parents[parent], n);
            assert(mkdir(edge_path, 0700) == 0);
        }
    }
    _file("edge_mixed/first.txt");
    _file("edge_mixed/second.txt");
    uint32_t session = _open();
    file_service_batch_t *batch = malloc(sizeof(*batch));
    assert(batch != NULL);
    file_service_cursor_t cursor = {0};
    assert(file_service_session_list(session, "edge_files", NULL, NULL) == ESP_OK);
    assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
    assert(file_service_session_get_batch(session, batch) == ESP_OK);
    assert(batch->count == FILE_SERVICE_BATCH_ENTRIES && !batch->has_more);
    for (size_t n = 0U; n < batch->count; ++n)
        assert(batch->entries[n].kind == FILE_SERVICE_ENTRY_FILE);

    assert(file_service_session_list(session, "edge_dirs", NULL, NULL) == ESP_OK);
    assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
    assert(file_service_session_get_batch(session, batch) == ESP_OK);
    assert(batch->count == FILE_SERVICE_BATCH_ENTRIES && !batch->has_more);
    for (size_t n = 0U; n < batch->count; ++n)
        assert(batch->entries[n].kind == FILE_SERVICE_ENTRY_DIRECTORY);

    assert(file_service_session_list(session, "edge_mixed", NULL, NULL) == ESP_OK);
    assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
    assert(file_service_session_get_batch(session, batch) == ESP_OK);
    assert(batch->count == FILE_SERVICE_BATCH_ENTRIES && batch->has_more);
    for (size_t n = 0U; n < batch->count; ++n)
        assert(batch->entries[n].kind == FILE_SERVICE_ENTRY_DIRECTORY);
    assert(file_service_session_get_cursor(session, &cursor) == ESP_OK);
    assert(file_service_session_list(session, "edge_mixed", &cursor, NULL) == ESP_OK);
    assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
    assert(file_service_session_get_batch(session, batch) == ESP_OK);
    assert(batch->count == 2U && !batch->has_more);
    assert(batch->entries[0].kind == FILE_SERVICE_ENTRY_FILE);
    assert(batch->entries[1].kind == FILE_SERVICE_ENTRY_FILE);
    const bool saw_first = strcmp(batch->entries[0].name, "first.txt") == 0 ||
                           strcmp(batch->entries[1].name, "first.txt") == 0;
    const bool saw_second = strcmp(batch->entries[0].name, "second.txt") == 0 ||
                            strcmp(batch->entries[1].name, "second.txt") == 0;
    assert(saw_first && saw_second);
    cursor = (file_service_cursor_t)
    {
        0
    };
    unsigned seen = 0U;
    unsigned directories = 0U;
    bool files_started = false;
    for (unsigned n = 0U; n < 100U; ++n)
    {
        assert(file_service_session_list(session, "", &cursor, NULL) == ESP_OK);
        assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
        assert(file_service_session_get_batch(session, batch) == ESP_OK);
        assert(batch->count <= FILE_SERVICE_BATCH_ENTRIES);
        for (size_t i = 0U; i < batch->count; ++i)
        {
            const file_service_entry_t *entry = &batch->entries[i];
            if (entry->kind == FILE_SERVICE_ENTRY_DIRECTORY)
            {
                assert(!files_started);
                if (strncmp(entry->name, "many_", 5U) == 0) ++directories;
            }
            else files_started = true;
            if (strncmp(entry->name, "page_", 5U) == 0) ++seen;
        }
        assert(atomic_load(&s_allocations) == s_init_allocations);
        assert(file_service_session_get_cursor(session, &cursor) == ESP_OK);
        if (!batch->has_more) break;
    }
    assert(seen == 40U);
    assert(directories == 1100U);
    file_service_result_t result;
    const char *invalid[] = {"../escape", "/absolute", "a//b", "a/./b", "a\\b"};
    for (size_t n = 0U; n < sizeof(invalid) / sizeof(invalid[0]); ++n)
    {
        assert(file_service_session_list(session, invalid[n], NULL, &result) != ESP_OK);
        assert(result.reason == FILE_SERVICE_ERROR_INVALID);
    }
    char path[FILE_SERVICE_PATH_BYTES];
    for (size_t n = 0U; n < sizeof(path) - 1U; ++n) path[n] = n % 2U == 0U ? 'a' : '/';
    path[sizeof(path) - 1U] = '\0';
    assert(file_service_session_list(session, path, NULL, NULL) == ESP_OK);
    assert(_result(session).reason == FILE_SERVICE_ERROR_PATH_TOO_LONG);
    pthread_mutex_lock(&s_sd_lock);
    ++s_sd.revision;
    pthread_mutex_unlock(&s_sd_lock);
    assert(file_service_session_list(session, "", &cursor, &result) != ESP_OK);
    assert(result.reason == FILE_SERVICE_ERROR_STALE);
    free(batch);
    _stop();
}

static void _chinese_roundtrip(void)
{
    _start();
    char path[1200];
    snprintf(path, sizeof(path), "%s/中文测试", s_root);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/中文测试/相册", s_root);
    assert(mkdir(path, 0700) == 0);
    _file("中文测试/中文文件.txt");
    _file("中文测试/相册/照片一.jpg");
    uint32_t session = _open();
    assert(file_service_session_list(session, "中文测试", NULL, NULL) == ESP_OK);
    assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
    file_service_batch_t *batch = malloc(sizeof(*batch));
    file_service_detail_t *detail = malloc(sizeof(*detail));
    assert(batch != NULL && detail != NULL);
    assert(file_service_session_get_batch(session, batch) == ESP_OK);
    assert(batch->count == 2U);
    assert(batch->entries[0].kind == FILE_SERVICE_ENTRY_DIRECTORY);
    assert(strcmp(batch->entries[0].name, "相册") == 0);
    assert(batch->entries[1].kind == FILE_SERVICE_ENTRY_FILE);
    assert(strcmp(batch->entries[1].name, "中文文件.txt") == 0);
    assert(batch->entries[1].size_bytes == 3U);
    assert(file_service_session_detail(session, _target("中文测试/中文文件.txt"), NULL) == ESP_OK);
    assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
    assert(file_service_session_get_detail(session, detail) == ESP_OK);
    assert(strcmp(detail->name, "中文文件.txt") == 0);
    assert(strcmp(detail->relative_path, "中文测试/中文文件.txt") == 0);
    assert(detail->size_bytes == 3U);
    assert(file_service_session_detail(session, _target("中文测试/相册/照片一.jpg"), NULL) == ESP_OK);
    assert(_result(session).state == FILE_SERVICE_RESULT_DONE);
    assert(file_service_session_get_detail(session, detail) == ESP_OK);
    assert(strcmp(detail->name, "照片一.jpg") == 0);
    free(detail);
    free(batch);
    _stop();
}

static void _mutation_outcomes(void)
{
    _start();
    for (unsigned directory = 0U; directory < 2U; ++directory)
    {
        for (unsigned close = 0U; close < 2U; ++close)
        {
            char name[32], path[1200];
            snprintf(name, sizeof(name), "mutation_%u_%u", directory, close);
            snprintf(path, sizeof(path), "%s/%s", s_root, name);
            if (directory != 0U) assert(mkdir(path, 0700) == 0);
            else _file(name);
            uint32_t session = _open();
            uint32_t revision = _snapshot().revision;
            _block(false);
            assert(file_service_session_delete(session, _target(name), NULL) == ESP_OK);
            _entered();
            if (close != 0U) assert(file_service_session_close(session) == ESP_OK);
            else assert(file_service_session_cancel(session) == ESP_OK);
            _release();
            _idle();
            assert(access(path, F_OK) != 0);
            assert(_snapshot().revision == revision + 1U);
            file_service_result_t result;
            if (close != 0U)
            {
                assert(file_service_session_get_result(session, &result) == ESP_ERR_INVALID_ARG);
                uint32_t replacement = _open();
                assert(replacement != session);
                assert(file_service_session_get_result(replacement, &result) == ESP_OK);
                assert(result.state == FILE_SERVICE_RESULT_NONE);
                assert(file_service_session_close(replacement) == ESP_OK);
            }
            else
            {
                result = _result(session);
                assert(result.state == FILE_SERVICE_RESULT_DONE);
                assert(result.reason == FILE_SERVICE_ERROR_NONE);
                assert(file_service_session_close(session) == ESP_OK);
            }
        }
    }
    char path[1200];
    snprintf(path, sizeof(path), "%s/denied_mutation", s_root);
    assert(mkdir(path, 0700) == 0);
    uint32_t session = _open();
    atomic_store(&s_rmdir_error, EACCES);
    _block(false);
    assert(file_service_session_delete(session, _target("denied_mutation"), NULL) == ESP_OK);
    _entered();
    assert(file_service_session_cancel(session) == ESP_OK);
    _release();
    file_service_result_t result = _result(session);
    assert(result.state == FILE_SERVICE_RESULT_FAILED);
    assert(result.reason == FILE_SERVICE_ERROR_PERMISSION && result.system_errno == EACCES);
    assert(access(path, F_OK) == 0);
    _stop();
}

static void _capacity_failures(void)
{
    _start();
    const file_service_storage_action_t actions[] =
    {
        FILE_SERVICE_STORAGE_MOUNT,
        FILE_SERVICE_STORAGE_RECOVER_FORMAT,
        FILE_SERVICE_STORAGE_FORMAT,
    };
    atomic_store(&s_space_error, ESP_FAIL);
    atomic_store(&s_space_errno, 0);
    const unsigned faults = atomic_load(&s_faults);
    for (size_t n = 0U; n < sizeof(actions) / sizeof(actions[0]); ++n)
    {
        const uint32_t generation = _snapshot().generation;
        assert(file_service_request_storage(actions[n], generation, NULL) == ESP_OK);
        _idle();
        file_service_snapshot_t snapshot = _snapshot();
        assert(snapshot.storage.state == FILE_SERVICE_RESULT_FAILED);
        assert(snapshot.storage.error == ESP_FAIL &&
               snapshot.storage.reason == FILE_SERVICE_ERROR_IO);
        assert(snapshot.storage.system_errno == 0);
        assert(!snapshot.space.valid && snapshot.space.last_error == ESP_FAIL);
        assert(!snapshot.faulted);
        assert(snapshot.pending_requests == 0U);
    }
    assert(atomic_load(&s_faults) == faults);

    atomic_store(&s_space_errno, ENODEV);
    const uint32_t ordinary_generation = _snapshot().generation;
    const unsigned ordinary_mounts = atomic_load(&s_mount_calls);
    assert(file_service_request_storage(FILE_SERVICE_STORAGE_REFRESH_SPACE,
                                        ordinary_generation, NULL) == ESP_OK);
    _idle();
    file_service_snapshot_t snapshot = _snapshot();
    assert(snapshot.storage.state == FILE_SERVICE_RESULT_FAILED);
    assert(snapshot.storage.system_errno == ENODEV);
    assert(!snapshot.faulted && snapshot.generation == ordinary_generation);
    assert(!snapshot.space.valid && snapshot.space.last_error == ESP_FAIL);
    assert(atomic_load(&s_mount_calls) == ordinary_mounts);
    assert(atomic_load(&s_faults) == faults);

    atomic_store(&s_space_errno, EIO);
    const uint32_t fault_generation = snapshot.generation;
    assert(file_service_request_storage(FILE_SERVICE_STORAGE_REFRESH_SPACE,
                                        fault_generation, NULL) == ESP_OK);
    _idle();
    snapshot = _snapshot();
    assert(snapshot.storage.state == FILE_SERVICE_RESULT_FAILED);
    assert(snapshot.storage.system_errno == EIO);
    assert(snapshot.faulted);
    assert(snapshot.generation == fault_generation + 1U);
    assert(!snapshot.space.valid);
    assert(atomic_load(&s_faults) == faults + 1U);

    atomic_store(&s_space_error, ESP_OK);
    const unsigned remounts = atomic_load(&s_mount_calls);
    assert(file_service_request_storage(FILE_SERVICE_STORAGE_MOUNT,
                                        snapshot.generation, NULL) == ESP_OK);
    _idle();
    snapshot = _snapshot();
    assert(snapshot.storage.state == FILE_SERVICE_RESULT_DONE);
    assert(snapshot.space.valid && !snapshot.faulted);
    assert(snapshot.generation == fault_generation + 2U);
    assert(atomic_load(&s_mount_calls) == remounts + 1U);
    _stop();
}

static void _media_state_errors(void)
{
    _start();
    const uint32_t session = _open();
    const file_service_error_reason_t reasons[] =
    {
        FILE_SERVICE_ERROR_UNAVAILABLE, FILE_SERVICE_ERROR_IO,
        FILE_SERVICE_ERROR_BUSY,
    };
    for (size_t n = 0U; n < sizeof(reasons) / sizeof(reasons[0]); ++n)
    {
        pthread_mutex_lock(&s_sd_lock);
        s_sd.mounted = n == 2U;
        s_sd.faulted = n == 1U;
        s_sd.maintenance = n == 2U;
        pthread_mutex_unlock(&s_sd_lock);
        assert(file_service_session_list(session, "", NULL, NULL) == ESP_OK);
        assert(_result(session).reason == reasons[n]);
        assert(file_service_session_delete(session, _target("中文.txt"), NULL) == ESP_OK);
        assert(_result(session).reason == reasons[n]);
        assert(file_service_request_storage(FILE_SERVICE_STORAGE_REFRESH_SPACE,
                                            _snapshot().generation, NULL) == ESP_OK);
        _idle();
        assert(_snapshot().storage.reason == reasons[n]);
    }
    pthread_mutex_lock(&s_sd_lock);
    s_sd.mounted = true;
    s_sd.faulted = false;
    s_sd.maintenance = false;
    pthread_mutex_unlock(&s_sd_lock);
    _stop();
}

int main(void)
{
    assert(mkdtemp(s_root) != NULL);
    _worker_create_failure();
    _ownership_and_copy();
    _queue_and_epochs();
    _chinese_roundtrip();
    _mutation_outcomes();
    _capacity_failures();
    _media_state_errors();
    _errors_and_symlinks();
    _initial_mount_and_stop();
    _pagination_and_paths();
    char command[1400];
    snprintf(command, sizeof(command), "rm -rf '%s'", s_root);
    assert(system(command) == 0);
    puts("file worker ownership regressions passed");
    return 0;
}
