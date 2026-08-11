#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"

#include "factory_reset_service.h"
#include "nv_storage.h"

#define FACTORY_RESET_STORAGE_KEY "factory.reset"
#define FACTORY_RESET_MARKER_BYTES 16U
#define REQUEST_THREAD_COUNT 8U

static pthread_mutex_t s_restart_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_restart_condition = PTHREAD_COND_INITIALIZER;
static bool s_block_restart;
static bool s_restart_blocked;

typedef struct restart_probe
{
    atomic_uint calls;
    atomic_bool durable_before_restart;
} restart_probe_t;

typedef struct request_thread_result
{
    esp_err_t result;
} request_thread_result_t;

typedef struct api_acquire_barrier
{
    atomic_bool entered;
    atomic_bool release;
} api_acquire_barrier_t;

static void _restart(void *context)
{
    restart_probe_t *probe = context;

    (void)pthread_mutex_lock(&s_restart_mutex);
    if (s_block_restart)
    {
        s_restart_blocked = true;
        (void)pthread_cond_broadcast(&s_restart_condition);
        while (s_block_restart)
        {
            (void)pthread_cond_wait(&s_restart_condition, &s_restart_mutex);
        }
        s_restart_blocked = false;
    }
    (void)pthread_mutex_unlock(&s_restart_mutex);
    atomic_fetch_add_explicit(&probe->calls, 1U, memory_order_relaxed);
    atomic_store_explicit(
        &probe->durable_before_restart,
        nv_storage_fake_committed_blob_len() == FACTORY_RESET_MARKER_BYTES,
        memory_order_release);
}

static factory_reset_service_config_t _config(restart_probe_t *probe)
{
    const factory_reset_service_config_t config =
    {
        .restart = _restart,
        .restart_context = probe,
    };
    return config;
}

static void _reset(restart_probe_t *probe)
{
    assert(factory_reset_service_deinit() == ESP_OK);
    nv_storage_fake_reset();
    (void)pthread_mutex_lock(&s_restart_mutex);
    s_block_restart = false;
    s_restart_blocked = false;
    (void)pthread_cond_broadcast(&s_restart_condition);
    (void)pthread_mutex_unlock(&s_restart_mutex);
    atomic_init(&probe->calls, 0U);
    atomic_init(&probe->durable_before_restart, false);
}

static void test_lifecycle(void)
{
    restart_probe_t probe;
    bool pending = true;

    _reset(&probe);
    factory_reset_service_config_t config = _config(&probe);
    factory_reset_service_config_t invalid = config;
    invalid.restart = NULL;

    assert(factory_reset_service_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(factory_reset_service_init(&invalid) == ESP_ERR_INVALID_ARG);
    assert(factory_reset_service_request() == ESP_ERR_INVALID_STATE);
    assert(factory_reset_service_recovery_pending(&pending) ==
           ESP_ERR_INVALID_STATE);
    assert(!pending);
    assert(factory_reset_service_complete_recovery() ==
           ESP_ERR_INVALID_STATE);
    assert(factory_reset_service_init(&config) == ESP_OK);
    assert(factory_reset_service_init(&config) == ESP_ERR_INVALID_STATE);
    assert(factory_reset_service_recovery_pending(NULL) == ESP_ERR_INVALID_ARG);
    assert(factory_reset_service_deinit() == ESP_OK);
    assert(factory_reset_service_deinit() == ESP_OK);
}

static void test_marker_is_durable_before_restart(void)
{
    restart_probe_t probe;
    bool pending = false;

    _reset(&probe);
    const factory_reset_service_config_t config = _config(&probe);

    assert(factory_reset_service_init(&config) == ESP_OK);
    assert(factory_reset_service_request() == ESP_OK);
    assert(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 1U);
    assert(atomic_load_explicit(&probe.durable_before_restart,
                                memory_order_acquire));
    assert(factory_reset_service_recovery_pending(&pending) == ESP_OK);
    assert(pending);
    assert(factory_reset_service_request() == ESP_ERR_INVALID_STATE);
    assert(factory_reset_service_complete_recovery() == ESP_OK);
    assert(factory_reset_service_request() == ESP_OK);
    assert(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 2U);
    assert(factory_reset_service_deinit() == ESP_OK);
}

static void test_write_failure_does_not_restart_and_can_retry(void)
{
    restart_probe_t probe;

    _reset(&probe);
    const factory_reset_service_config_t config = _config(&probe);

    assert(factory_reset_service_init(&config) == ESP_OK);
    nv_storage_fake_fail_next_set(ESP_FAIL);
    assert(factory_reset_service_request() == ESP_FAIL);
    assert(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 0U);
    assert(nv_storage_fake_committed_blob_len() == 0U);
    assert(factory_reset_service_request() == ESP_OK);
    assert(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 1U);
    assert(factory_reset_service_complete_recovery() == ESP_OK);

    nv_storage_fake_fail_next_commit(ESP_FAIL);
    assert(factory_reset_service_request() == ESP_FAIL);
    assert(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 1U);
    assert(nv_storage_fake_committed_blob_len() == 0U);
    assert(nv_storage_fake_commit_pending());
    nv_storage_fake_power_cycle();
    assert(!nv_storage_fake_commit_pending());
    assert(factory_reset_service_request() == ESP_OK);
    assert(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 2U);
    assert(factory_reset_service_deinit() == ESP_OK);
}

static void test_corrupt_and_truncated_markers_fail_closed(void)
{
    restart_probe_t probe;
    bool pending = true;
    uint8_t truncated = 0x54U;
    uint8_t corrupt[FACTORY_RESET_MARKER_BYTES];

    _reset(&probe);
    const factory_reset_service_config_t config = _config(&probe);
    assert(factory_reset_service_init(&config) == ESP_OK);

    assert(nv_storage_set_blob(FACTORY_RESET_STORAGE_KEY,
                               &truncated, sizeof(truncated)) == ESP_OK);
    assert(factory_reset_service_recovery_pending(&pending) ==
           ESP_ERR_INVALID_RESPONSE);
    assert(!pending);
    assert(factory_reset_service_complete_recovery() == ESP_OK);

    memset(corrupt, 0, sizeof(corrupt));
    assert(nv_storage_set_blob(FACTORY_RESET_STORAGE_KEY,
                               corrupt, sizeof(corrupt)) == ESP_OK);
    pending = true;
    assert(factory_reset_service_recovery_pending(&pending) ==
           ESP_ERR_INVALID_RESPONSE);
    assert(!pending);

    nv_storage_fake_fail_next_get(ESP_FAIL);
    pending = true;
    assert(factory_reset_service_recovery_pending(&pending) == ESP_FAIL);
    assert(!pending);
    assert(factory_reset_service_deinit() == ESP_OK);
}

static void test_complete_is_idempotent_and_erase_failure_stays_pending(void)
{
    restart_probe_t probe;
    bool pending = false;

    _reset(&probe);
    const factory_reset_service_config_t config = _config(&probe);
    assert(factory_reset_service_init(&config) == ESP_OK);
    assert(factory_reset_service_complete_recovery() == ESP_OK);
    assert(factory_reset_service_complete_recovery() == ESP_OK);

    assert(factory_reset_service_request() == ESP_OK);
    nv_storage_fake_fail_next_erase(ESP_FAIL);
    assert(factory_reset_service_complete_recovery() == ESP_FAIL);
    assert(factory_reset_service_recovery_pending(&pending) == ESP_OK);
    assert(pending);

    nv_storage_fake_fail_next_commit(ESP_FAIL);
    assert(factory_reset_service_complete_recovery() == ESP_FAIL);
    assert(nv_storage_fake_committed_blob_len() ==
           FACTORY_RESET_MARKER_BYTES);
    assert(nv_storage_fake_commit_pending());
    nv_storage_fake_power_cycle();
    assert(factory_reset_service_recovery_pending(&pending) == ESP_OK);
    assert(pending);
    assert(factory_reset_service_complete_recovery() == ESP_OK);
    assert(factory_reset_service_complete_recovery() == ESP_OK);
    assert(factory_reset_service_recovery_pending(&pending) == ESP_OK);
    assert(!pending);
    assert(factory_reset_service_deinit() == ESP_OK);
}

static void test_power_cycle_recovers_durable_marker(void)
{
    restart_probe_t first_probe;
    restart_probe_t second_probe;
    bool pending = false;

    _reset(&first_probe);
    const factory_reset_service_config_t first_config = _config(&first_probe);
    assert(factory_reset_service_init(&first_config) == ESP_OK);
    assert(factory_reset_service_request() == ESP_OK);
    assert(factory_reset_service_deinit() == ESP_OK);

    nv_storage_fake_power_cycle();
    atomic_init(&second_probe.calls, 0U);
    atomic_init(&second_probe.durable_before_restart, false);
    const factory_reset_service_config_t second_config = _config(&second_probe);

    assert(factory_reset_service_init(&second_config) == ESP_OK);
    assert(factory_reset_service_recovery_pending(&pending) == ESP_OK);
    assert(pending);
    assert(factory_reset_service_complete_recovery() == ESP_OK);
    assert(factory_reset_service_recovery_pending(&pending) == ESP_OK);
    assert(!pending);
    assert(atomic_load_explicit(&second_probe.calls, memory_order_relaxed) == 0U);
    assert(factory_reset_service_deinit() == ESP_OK);
}

static void *_request_thread(void *context)
{
    request_thread_result_t *result = context;

    result->result = factory_reset_service_request();
    return NULL;
}

static void _api_acquire_barrier(void *context)
{
    api_acquire_barrier_t *barrier = context;

    atomic_store_explicit(&barrier->entered, true, memory_order_release);
    while (!atomic_load_explicit(&barrier->release, memory_order_acquire))
    {
        (void)sched_yield();
    }
}

static void test_api_admission_rejects_retired_instance(void)
{
    restart_probe_t old_probe;
    restart_probe_t new_probe;
    request_thread_result_t request = {0};
    api_acquire_barrier_t barrier =
    {
        .entered = ATOMIC_VAR_INIT(false),
        .release = ATOMIC_VAR_INIT(false),
    };
    pthread_t thread;

    _reset(&old_probe);
    atomic_init(&new_probe.calls, 0U);
    atomic_init(&new_probe.durable_before_restart, false);
    const factory_reset_service_config_t old_config = _config(&old_probe);
    const factory_reset_service_config_t new_config = _config(&new_probe);

    assert(factory_reset_service_init(&old_config) == ESP_OK);
    factory_reset_service_test_set_api_acquire_hook(
        _api_acquire_barrier, &barrier);
    assert(pthread_create(&thread, NULL, _request_thread, &request) == 0);
    while (!atomic_load_explicit(&barrier.entered, memory_order_acquire))
    {
        (void)sched_yield();
    }
    assert(factory_reset_service_deinit() == ESP_OK);
    assert(factory_reset_service_init(&new_config) == ESP_OK);
    atomic_store_explicit(&barrier.release, true, memory_order_release);
    assert(pthread_join(thread, NULL) == 0);

    assert(request.result == ESP_ERR_INVALID_STATE);
    assert(atomic_load_explicit(&old_probe.calls,
                                memory_order_relaxed) == 0U);
    assert(atomic_load_explicit(&new_probe.calls,
                                memory_order_relaxed) == 0U);
    assert(nv_storage_fake_committed_blob_len() == 0U);
    factory_reset_service_test_set_api_acquire_hook(NULL, NULL);

    assert(factory_reset_service_request() == ESP_OK);
    assert(atomic_load_explicit(&new_probe.calls,
                                memory_order_relaxed) == 1U);
    assert(factory_reset_service_complete_recovery() == ESP_OK);
    assert(factory_reset_service_deinit() == ESP_OK);
}

static void test_concurrent_request_admits_one_restart(void)
{
    restart_probe_t probe;
    pthread_t threads[REQUEST_THREAD_COUNT];
    request_thread_result_t results[REQUEST_THREAD_COUNT];
    unsigned int successful = 0U;

    _reset(&probe);
    const factory_reset_service_config_t config = _config(&probe);
    assert(factory_reset_service_init(&config) == ESP_OK);
    memset(results, 0, sizeof(results));

    for (size_t i = 0U; i < REQUEST_THREAD_COUNT; ++i)
    {
        assert(pthread_create(&threads[i], NULL, _request_thread,
                              &results[i]) == 0);
    }
    for (size_t i = 0U; i < REQUEST_THREAD_COUNT; ++i)
    {
        assert(pthread_join(threads[i], NULL) == 0);
        successful += results[i].result == ESP_OK ? 1U : 0U;
        assert(results[i].result == ESP_OK ||
               results[i].result == ESP_ERR_INVALID_STATE);
    }
    assert(successful == 1U);
    assert(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 1U);
    assert(factory_reset_service_deinit() == ESP_OK);
}

static void test_deinit_refuses_active_request(void)
{
    restart_probe_t probe;
    pthread_t thread;
    request_thread_result_t request = {0};

    _reset(&probe);
    const factory_reset_service_config_t config = _config(&probe);
    assert(factory_reset_service_init(&config) == ESP_OK);
    nv_storage_fake_block_next_set();
    assert(pthread_create(&thread, NULL, _request_thread, &request) == 0);
    nv_storage_fake_wait_set_blocked();

    assert(factory_reset_service_deinit() == ESP_ERR_INVALID_STATE);
    bool pending = true;

    assert(factory_reset_service_recovery_pending(&pending) ==
           ESP_ERR_INVALID_STATE);
    assert(!pending);
    assert(factory_reset_service_complete_recovery() ==
           ESP_ERR_INVALID_STATE);
    nv_storage_fake_release_blocked_set();
    assert(pthread_join(thread, NULL) == 0);
    assert(request.result == ESP_OK);
    assert(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 1U);
    assert(factory_reset_service_deinit() == ESP_OK);
}

static void test_deinit_refuses_active_restart_callback(void)
{
    restart_probe_t probe;
    pthread_t thread;
    request_thread_result_t request = {0};

    _reset(&probe);
    const factory_reset_service_config_t config = _config(&probe);
    assert(factory_reset_service_init(&config) == ESP_OK);
    (void)pthread_mutex_lock(&s_restart_mutex);
    s_block_restart = true;
    s_restart_blocked = false;
    (void)pthread_mutex_unlock(&s_restart_mutex);
    assert(pthread_create(&thread, NULL, _request_thread, &request) == 0);
    (void)pthread_mutex_lock(&s_restart_mutex);
    while (!s_restart_blocked)
    {
        (void)pthread_cond_wait(&s_restart_condition, &s_restart_mutex);
    }
    (void)pthread_mutex_unlock(&s_restart_mutex);

    assert(factory_reset_service_deinit() == ESP_ERR_INVALID_STATE);
    bool pending = true;

    assert(nv_storage_fake_committed_blob_len() ==
           FACTORY_RESET_MARKER_BYTES);
    assert(factory_reset_service_recovery_pending(&pending) ==
           ESP_ERR_INVALID_STATE);
    assert(!pending);
    assert(factory_reset_service_complete_recovery() ==
           ESP_ERR_INVALID_STATE);
    assert(nv_storage_fake_committed_blob_len() ==
           FACTORY_RESET_MARKER_BYTES);
    (void)pthread_mutex_lock(&s_restart_mutex);
    s_block_restart = false;
    (void)pthread_cond_broadcast(&s_restart_condition);
    (void)pthread_mutex_unlock(&s_restart_mutex);
    assert(pthread_join(thread, NULL) == 0);
    assert(request.result == ESP_OK);
    assert(atomic_load_explicit(&probe.calls, memory_order_relaxed) == 1U);
    assert(factory_reset_service_deinit() == ESP_OK);
}

int main(void)
{
    test_lifecycle();
    test_marker_is_durable_before_restart();
    test_write_failure_does_not_restart_and_can_retry();
    test_corrupt_and_truncated_markers_fail_closed();
    test_complete_is_idempotent_and_erase_failure_stays_pending();
    test_power_cycle_recovers_durable_marker();
    test_api_admission_rejects_retired_instance();
    test_concurrent_request_admits_one_restart();
    test_deinit_refuses_active_request();
    test_deinit_refuses_active_restart_callback();
    puts("factory_reset_service host tests passed");
    return 0;
}
