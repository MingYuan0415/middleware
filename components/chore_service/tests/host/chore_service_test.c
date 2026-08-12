#define DBG_TAG "chore_service_test"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "chore_service.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "host_freertos.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHORE_TEST_TIMEOUT_MS  5000U
#define CHORE_TEST_POLL_US     1000U

static atomic_llong s_clock_offset_us;

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t real_us = (int64_t)now.tv_sec * 1000000 +
                            (int64_t)now.tv_nsec / 1000;
    return real_us + atomic_load_explicit(&s_clock_offset_us,
                                          memory_order_relaxed);
}

static int64_t _chore_test_real_ms(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static void _chore_test_clock_advance_ms(int64_t ms)
{
    atomic_fetch_add_explicit(&s_clock_offset_us, ms * 1000,
                              memory_order_relaxed);
}

static bool _chore_test_wait_int(atomic_int *value, int expected,
                                 uint32_t timeout_ms)
{
    const int64_t deadline = _chore_test_real_ms() + (int64_t)timeout_ms;
    for (;;)
    {
        if (atomic_load_explicit(value, memory_order_acquire) == expected)
        {
            return true;
        }
        if (_chore_test_real_ms() >= deadline)
        {
            return false;
        }
        usleep(CHORE_TEST_POLL_US);
    }
}

static bool _chore_test_wait_int_at_least(atomic_int *value, int minimum,
        uint32_t timeout_ms)
{
    const int64_t deadline = _chore_test_real_ms() + (int64_t)timeout_ms;
    for (;;)
    {
        if (atomic_load_explicit(value, memory_order_acquire) >= minimum)
        {
            return true;
        }
        if (_chore_test_real_ms() >= deadline)
        {
            return false;
        }
        usleep(CHORE_TEST_POLL_US);
    }
}

typedef struct chore_test_arg
{
    atomic_int runs;
    atomic_int releases;
    bool slow;
    bool slow_gated;
    atomic_bool slow_gate;
    bool poll_cancel;
    bool self_call;
    chore_service_handle_t handle;
    atomic_bool handle_ready;
    atomic_int self_rejected;
} chore_test_arg_t;

typedef struct chore_test_gated_arg
{
    atomic_int runs;
    atomic_int releases;
    atomic_int release_entered;
    atomic_bool release_gate;
} chore_test_gated_arg_t;

static void _chore_test_job_run(const chore_service_cancel_token_t *cancel,
                                void *context)
{
    chore_test_arg_t *arg = context;
    (void)cancel;
    atomic_fetch_add_explicit(&arg->runs, 1, memory_order_relaxed);
    if (arg->self_call)
    {
        while (!atomic_load_explicit(&arg->handle_ready,
                                     memory_order_acquire))
        {
            usleep(CHORE_TEST_POLL_US);
        }
        unsigned rejected = 0U;
        if (chore_service_cancel(&arg->handle, 100U) ==
                ESP_ERR_INVALID_STATE)
        {
            ++rejected;
        }
        if (chore_service_suspend(100U) == ESP_ERR_INVALID_STATE)
        {
            ++rejected;
        }
        if (chore_service_resume(100U) == ESP_ERR_INVALID_STATE)
        {
            ++rejected;
        }
        if (chore_service_deinit(100U) == ESP_ERR_INVALID_STATE)
        {
            ++rejected;
        }
        atomic_store_explicit(&arg->self_rejected, (int)rejected,
                              memory_order_release);
        return;
    }
    if (arg->poll_cancel)
    {
        while (!chore_service_cancel_pending(cancel))
        {
            usleep(CHORE_TEST_POLL_US);
        }
        return;
    }
    if (arg->slow)
    {
        if (arg->slow_gated)
        {
            while (!atomic_load_explicit(&arg->slow_gate,
                                         memory_order_acquire))
            {
                usleep(CHORE_TEST_POLL_US);
            }
        }
        usleep(120000);
    }
}

static void _chore_test_job_release(void *context)
{
    chore_test_arg_t *arg = context;
    atomic_fetch_add_explicit(&arg->releases, 1, memory_order_relaxed);
}

static void _chore_test_gated_job_run(const chore_service_cancel_token_t *cancel,
                                      void *context)
{
    chore_test_gated_arg_t *arg = context;
    atomic_fetch_add_explicit(&arg->runs, 1, memory_order_relaxed);
    while (!chore_service_cancel_pending(cancel))
    {
        usleep(CHORE_TEST_POLL_US);
    }
}

static void _chore_test_gated_job_release(void *context)
{
    chore_test_gated_arg_t *arg = context;
    atomic_fetch_add_explicit(&arg->release_entered, 1,
                              memory_order_relaxed);
    while (!atomic_load_explicit(&arg->release_gate, memory_order_acquire))
    {
        usleep(CHORE_TEST_POLL_US);
    }
    atomic_fetch_add_explicit(&arg->releases, 1, memory_order_relaxed);
}

static void _chore_test_arg_init(chore_test_arg_t *arg)
{
    memset(arg, 0, sizeof(*arg));
    atomic_init(&arg->runs, 0);
    atomic_init(&arg->releases, 0);
    atomic_init(&arg->handle_ready, false);
    atomic_init(&arg->self_rejected, 0);
    atomic_init(&arg->slow_gate, false);
}

static unsigned s_failures;

#define TEST_CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            ++s_failures; \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        } \
    } \
    while (0)

static bool _test_init_validation(void)
{
    const chore_service_config_t bad_priority =
    {
        .task_priority = 0U,
        .warning_duration_ms = 500U,
    };
    const chore_service_config_t high_priority =
    {
        .task_priority = configMAX_PRIORITIES,
        .warning_duration_ms = 500U,
    };
    const chore_service_config_t no_warning =
    {
        .task_priority = 4U,
        .warning_duration_ms = 0U,
    };
    TEST_CHECK(chore_service_init(NULL) == ESP_ERR_INVALID_ARG);
    TEST_CHECK(chore_service_init(&bad_priority) == ESP_ERR_INVALID_ARG);
    TEST_CHECK(chore_service_init(&high_priority) == ESP_ERR_INVALID_ARG);
    TEST_CHECK(chore_service_init(&no_warning) == ESP_ERR_INVALID_ARG);
    return true;
}

static bool _test_lifecycle_and_one_shot(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
    };
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_submit(&job, &handle) ==
               ESP_ERR_INVALID_STATE);
    TEST_CHECK(chore_service_is_available() == false);
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_is_available() == true);
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_suspend(100U) == ESP_OK);
    TEST_CHECK(chore_service_resume(100U) == ESP_OK);
    TEST_CHECK(chore_service_submit(NULL, &handle) == ESP_ERR_INVALID_ARG);
    const chore_service_job_t no_run =
    {
        .release = _chore_test_job_release,
        .arg = &arg,
    };
    TEST_CHECK(chore_service_submit(&no_run, &handle) ==
               ESP_ERR_INVALID_ARG);
    TEST_CHECK(chore_service_submit(&job, NULL) == ESP_ERR_INVALID_ARG);
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    TEST_CHECK(handle.slot < CONFIG_CHORE_SERVICE_JOB_CAPACITY);
    TEST_CHECK(handle.generation != 0U);
    TEST_CHECK(_chore_test_wait_int(&arg.runs, 1, CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(_chore_test_wait_int(&arg.releases, 1,
                                    CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_cancel(&handle, 100U) == ESP_OK);
    chore_service_status_t status;
    TEST_CHECK(chore_service_get_status(&status) == ESP_OK);
    TEST_CHECK(status.queued_count == 0U);
    TEST_CHECK(status.completed_count >= 1U);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    TEST_CHECK(chore_service_is_available() == false);
    TEST_CHECK(chore_service_get_status(&status) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(chore_service_deinit(100U) == ESP_OK);
    return true;
}

static bool _test_delayed_job(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
        .delay_ms = 120U,
    };
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    usleep(30000);
    TEST_CHECK(atomic_load_explicit(&arg.runs, memory_order_acquire) == 0);
    TEST_CHECK(_chore_test_wait_int(&arg.runs, 1, CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(_chore_test_wait_int(&arg.releases, 1,
                                    CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_periodic_and_cancel(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
        .period_ms = 30U,
    };
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int_at_least(&arg.runs, 3,
               CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_cancel(&handle, CHORE_TEST_TIMEOUT_MS) ==
               ESP_OK);
    /* Cancellation quiescence includes the release callback: it must have
       run before cancel returns. */
    TEST_CHECK(atomic_load_explicit(&arg.releases, memory_order_acquire) ==
               1);
    const int runs_after_cancel =
        atomic_load_explicit(&arg.runs, memory_order_acquire);
    usleep(80000);
    TEST_CHECK(atomic_load_explicit(&arg.runs, memory_order_acquire) ==
               runs_after_cancel);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_pool_full(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t args[CONFIG_CHORE_SERVICE_JOB_CAPACITY];
    chore_service_handle_t handles[CONFIG_CHORE_SERVICE_JOB_CAPACITY];
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    for (unsigned index = 0; index < CONFIG_CHORE_SERVICE_JOB_CAPACITY;
            ++index)
    {
        _chore_test_arg_init(&args[index]);
        args[index].slow = true;
        args[index].slow_gated = true;
        const chore_service_job_t job =
        {
            .run = _chore_test_job_run,
            .release = _chore_test_job_release,
            .arg = &args[index],
        };
        TEST_CHECK(chore_service_submit(&job, &handles[index]) == ESP_OK);
    }
    chore_test_arg_t extra;
    _chore_test_arg_init(&extra);
    const chore_service_job_t extra_job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &extra,
    };
    chore_service_handle_t extra_handle = {0};
    TEST_CHECK(chore_service_submit(&extra_job, &extra_handle) ==
               ESP_ERR_NO_MEM);
    for (unsigned index = 0; index < CONFIG_CHORE_SERVICE_JOB_CAPACITY;
            ++index)
    {
        atomic_store_explicit(&args[index].slow_gate, true,
                              memory_order_release);
    }
    for (unsigned index = 0; index < CONFIG_CHORE_SERVICE_JOB_CAPACITY;
            ++index)
    {
        TEST_CHECK(_chore_test_wait_int(&args[index].releases, 1,
                                        CHORE_TEST_TIMEOUT_MS));
    }
    TEST_CHECK(atomic_load_explicit(&extra.releases,
                                    memory_order_acquire) == 0);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_cancel_queued(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t first;
    chore_test_arg_t second;
    _chore_test_arg_init(&first);
    _chore_test_arg_init(&second);
    first.slow = true;
    const chore_service_job_t first_job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &first,
    };
    const chore_service_job_t second_job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &second,
    };
    chore_service_handle_t first_handle = {0};
    chore_service_handle_t second_handle = {0};
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&first_job, &first_handle) == ESP_OK);
    TEST_CHECK(chore_service_submit(&second_job, &second_handle) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&first.runs, 1, CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(atomic_load_explicit(&second.runs, memory_order_acquire) ==
               0);
    TEST_CHECK(chore_service_cancel(&second_handle, CHORE_TEST_TIMEOUT_MS) ==
               ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&second.releases, 1,
                                    CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(atomic_load_explicit(&second.runs, memory_order_acquire) ==
               0);
    TEST_CHECK(chore_service_cancel(&first_handle, CHORE_TEST_TIMEOUT_MS) ==
               ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&first.releases, 1,
                                    CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_cancel_running(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    arg.poll_cancel = true;
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
    };
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&arg.runs, 1, CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_cancel(&handle, CHORE_TEST_TIMEOUT_MS) ==
               ESP_OK);
    /* Cancellation quiescence includes the release callback. */
    TEST_CHECK(atomic_load_explicit(&arg.releases, memory_order_acquire) ==
               1);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_deinit_timeout_retry(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    arg.slow = true;
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
    };
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&job, NULL) == ESP_ERR_INVALID_ARG);
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&arg.runs, 1, CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_deinit(20U) == ESP_ERR_TIMEOUT);
    TEST_CHECK(_chore_test_wait_int(&arg.releases, 1,
                                    CHORE_TEST_TIMEOUT_MS));
    /* Retry after the worker finished: the retry must tolerate both the
       still-alive worker and the already-exited worker handle. */
    esp_err_t result = ESP_ERR_TIMEOUT;
    for (unsigned attempt = 0U; attempt < 50U && result != ESP_OK; ++attempt)
    {
        result = chore_service_deinit(10U);
    }
    TEST_CHECK(result == ESP_OK);
    TEST_CHECK(chore_service_is_available() == false);
    return true;
}

static bool _test_deinit_while_suspended(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    enum
    {
        CHORE_TEST_JOB_COUNT = 2,
    };
    chore_test_arg_t args[CHORE_TEST_JOB_COUNT];
    chore_service_handle_t handles[CHORE_TEST_JOB_COUNT];
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_suspend(CHORE_TEST_TIMEOUT_MS) == ESP_OK);
    for (unsigned index = 0; index < CHORE_TEST_JOB_COUNT; ++index)
    {
        _chore_test_arg_init(&args[index]);
        const chore_service_job_t job =
        {
            .run = _chore_test_job_run,
            .release = _chore_test_job_release,
            .arg = &args[index],
            .delay_ms = 60000U,
        };
        TEST_CHECK(chore_service_submit(&job, &handles[index]) == ESP_OK);
    }
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    for (unsigned index = 0; index < CHORE_TEST_JOB_COUNT; ++index)
    {
        TEST_CHECK(atomic_load_explicit(&args[index].runs,
                                        memory_order_acquire) == 0);
        TEST_CHECK(atomic_load_explicit(&args[index].releases,
                                        memory_order_acquire) == 1);
    }
    return true;
}

typedef struct chore_test_suspend_thread
{
    esp_err_t result;
} chore_test_suspend_thread_t;

static void *_chore_test_suspend_worker(void *context)
{
    chore_test_suspend_thread_t *request = context;
    request->result = chore_service_suspend(CHORE_SERVICE_WAIT_FOREVER);
    return NULL;
}

static bool _test_deinit_racing_suspend(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    arg.slow = true;
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
    };
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&arg.runs, 1, CHORE_TEST_TIMEOUT_MS));
    /* A WAIT_FOREVER suspend admitted while the worker is busy must not
       deadlock the teardown: deinit stops the worker, the STOPPED bit
       wakes the suspend waiter, and the drain completes. */
    chore_test_suspend_thread_t request =
    {
        .result = ESP_FAIL,
    };
    pthread_t suspend_thread;
    TEST_CHECK(pthread_create(&suspend_thread, NULL,
                              _chore_test_suspend_worker, &request) == 0);
    usleep(50000);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    TEST_CHECK(pthread_join(suspend_thread, NULL) == 0);
    TEST_CHECK(request.result == ESP_ERR_INVALID_STATE);
    return true;
}

static bool _test_repeated_init_policy(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    const chore_service_config_t different_priority_config =
    {
        .task_priority = 5U,
        .warning_duration_ms = 500U,
    };
    const chore_service_config_t different_warning_config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 300U,
    };
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_init(&different_priority_config) ==
               ESP_ERR_INVALID_STATE);
    TEST_CHECK(chore_service_init(&different_warning_config) ==
               ESP_ERR_INVALID_STATE);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    TEST_CHECK(chore_service_init(&different_priority_config) == ESP_OK);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_cancel_timeout(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    arg.slow = true;
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
    };
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&arg.runs, 1, CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_cancel(&handle, 20U) == ESP_ERR_TIMEOUT);
    TEST_CHECK(_chore_test_wait_int(&arg.releases, 1,
                                    CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_cancel(&handle, CHORE_TEST_TIMEOUT_MS) ==
               ESP_OK);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_suspend_no_catch_up(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
        .period_ms = 30U,
    };
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int_at_least(&arg.runs, 2,
               CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_suspend(CHORE_TEST_TIMEOUT_MS) == ESP_OK);
    const int runs_suspended =
        atomic_load_explicit(&arg.runs, memory_order_acquire);
    usleep(80000);
    TEST_CHECK(atomic_load_explicit(&arg.runs, memory_order_acquire) ==
               runs_suspended);
    _chore_test_clock_advance_ms(10000);
    TEST_CHECK(chore_service_resume(CHORE_TEST_TIMEOUT_MS) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int_at_least(&arg.runs, runs_suspended + 1,
               CHORE_TEST_TIMEOUT_MS));
    usleep(80000);
    const int runs_resumed =
        atomic_load_explicit(&arg.runs, memory_order_acquire);
    TEST_CHECK(runs_resumed < runs_suspended + 10);
    TEST_CHECK(chore_service_cancel(&handle, CHORE_TEST_TIMEOUT_MS) ==
               ESP_OK);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_suspend_timeout_rollback(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    arg.slow = true;
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
    };
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&arg.runs, 1, CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_suspend(20U) == ESP_ERR_TIMEOUT);
    TEST_CHECK(chore_service_resume(CHORE_TEST_TIMEOUT_MS) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&arg.releases, 1,
                                    CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_suspend(CHORE_TEST_TIMEOUT_MS) == ESP_OK);
    TEST_CHECK(chore_service_resume(CHORE_TEST_TIMEOUT_MS) == ESP_OK);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_deinit_releases_queued(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    enum
    {
        CHORE_TEST_JOB_COUNT = 4,
    };
    chore_test_arg_t args[CHORE_TEST_JOB_COUNT];
    chore_service_handle_t handles[CHORE_TEST_JOB_COUNT];
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    for (unsigned index = 0; index < CHORE_TEST_JOB_COUNT; ++index)
    {
        _chore_test_arg_init(&args[index]);
        const chore_service_job_t job =
        {
            .run = _chore_test_job_run,
            .release = _chore_test_job_release,
            .arg = &args[index],
            .delay_ms = 60000U,
        };
        TEST_CHECK(chore_service_submit(&job, &handles[index]) == ESP_OK);
    }
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    for (unsigned index = 0; index < CHORE_TEST_JOB_COUNT; ++index)
    {
        TEST_CHECK(atomic_load_explicit(&args[index].runs,
                                        memory_order_acquire) == 0);
        TEST_CHECK(atomic_load_explicit(&args[index].releases,
                                        memory_order_acquire) == 1);
    }
    return true;
}

static bool _test_self_call_rejected(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    arg.self_call = true;
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
    };
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    arg.handle = handle;
    atomic_store_explicit(&arg.handle_ready, true, memory_order_release);
    TEST_CHECK(_chore_test_wait_int(&arg.self_rejected, 4,
                                    CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(_chore_test_wait_int(&arg.releases, 1,
                                    CHORE_TEST_TIMEOUT_MS));
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_task_affinity_and_caps(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    const size_t baseline_tasks = host_dynamic_task_count();
    const size_t baseline_caps = host_caps_task_count();
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(host_dynamic_task_count() == baseline_tasks + 1U);
    TEST_CHECK(host_caps_task_count() == baseline_caps + 1U);
    TEST_CHECK(host_last_task_stack_caps() ==
               (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    const int64_t deadline = _chore_test_real_ms() + 2000;
    while (host_dynamic_task_count() != baseline_tasks &&
            _chore_test_real_ms() < deadline)
    {
        usleep(CHORE_TEST_POLL_US);
    }
    TEST_CHECK(host_dynamic_task_count() == baseline_tasks);
    return true;
}

typedef struct chore_test_cancel_thread
{
    chore_service_handle_t handle;
    esp_err_t result;
    atomic_int finished;
} chore_test_cancel_thread_t;

static void *_chore_test_cancel_worker(void *context)
{
    chore_test_cancel_thread_t *request = context;
    request->result = chore_service_cancel(&request->handle,
                                           CHORE_SERVICE_WAIT_FOREVER);
    atomic_store_explicit(&request->finished, 1, memory_order_release);
    return NULL;
}

static bool _test_cancel_waits_for_release(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_gated_arg_t arg;
    memset(&arg, 0, sizeof(arg));
    atomic_init(&arg.runs, 0);
    atomic_init(&arg.releases, 0);
    atomic_init(&arg.release_entered, 0);
    atomic_init(&arg.release_gate, false);
    const chore_service_job_t job =
    {
        .run = _chore_test_gated_job_run,
        .release = _chore_test_gated_job_release,
        .arg = &arg,
    };
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&arg.runs, 1, CHORE_TEST_TIMEOUT_MS));
    chore_test_cancel_thread_t request =
    {
        .handle = handle,
        .result = ESP_FAIL,
    };
    atomic_init(&request.finished, 0);
    pthread_t cancel_thread;
    TEST_CHECK(pthread_create(&cancel_thread, NULL, _chore_test_cancel_worker,
                              &request) == 0);
    /* The release callback blocks on the gate; cancellation must not
       complete until the release callback has run to completion. */
    TEST_CHECK(_chore_test_wait_int(&arg.release_entered, 1,
                                    CHORE_TEST_TIMEOUT_MS));
    usleep(50000);
    TEST_CHECK(atomic_load_explicit(&request.finished, memory_order_acquire)
               == 0);
    atomic_store_explicit(&arg.release_gate, true, memory_order_release);
    TEST_CHECK(pthread_join(cancel_thread, NULL) == 0);
    TEST_CHECK(request.result == ESP_OK);
    TEST_CHECK(atomic_load_explicit(&arg.releases, memory_order_acquire) ==
               1);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

static bool _test_concurrent_cancel(void)
{
    const chore_service_config_t config =
    {
        .task_priority = 4U,
        .warning_duration_ms = 500U,
    };
    chore_test_arg_t arg;
    _chore_test_arg_init(&arg);
    arg.poll_cancel = true;
    const chore_service_job_t job =
    {
        .run = _chore_test_job_run,
        .release = _chore_test_job_release,
        .arg = &arg,
    };
    chore_service_handle_t handle = {0};
    TEST_CHECK(chore_service_init(&config) == ESP_OK);
    TEST_CHECK(chore_service_submit(&job, &handle) == ESP_OK);
    TEST_CHECK(_chore_test_wait_int(&arg.runs, 1, CHORE_TEST_TIMEOUT_MS));
    chore_test_cancel_thread_t first_request =
    {
        .handle = handle,
        .result = ESP_FAIL,
    };
    chore_test_cancel_thread_t second_request =
    {
        .handle = handle,
        .result = ESP_FAIL,
    };
    atomic_init(&first_request.finished, 0);
    atomic_init(&second_request.finished, 0);
    pthread_t first_thread;
    pthread_t second_thread;
    TEST_CHECK(pthread_create(&first_thread, NULL,
                              _chore_test_cancel_worker,
                              &first_request) == 0);
    TEST_CHECK(pthread_create(&second_thread, NULL,
                              _chore_test_cancel_worker,
                              &second_request) == 0);
    TEST_CHECK(pthread_join(first_thread, NULL) == 0);
    TEST_CHECK(pthread_join(second_thread, NULL) == 0);
    TEST_CHECK(first_request.result == ESP_OK);
    TEST_CHECK(second_request.result == ESP_OK);
    TEST_CHECK(atomic_load_explicit(&arg.releases, memory_order_acquire) ==
               1);
    TEST_CHECK(chore_service_deinit(CHORE_SERVICE_WAIT_FOREVER) == ESP_OK);
    return true;
}

int main(void)
{
    static const struct
    {
        const char *name;
        bool (*run)(void);
    } tests[] =
    {
        { "init_validation", _test_init_validation },
        { "lifecycle_and_one_shot", _test_lifecycle_and_one_shot },
        { "delayed_job", _test_delayed_job },
        { "periodic_and_cancel", _test_periodic_and_cancel },
        { "pool_full", _test_pool_full },
        { "cancel_queued", _test_cancel_queued },
        { "cancel_running", _test_cancel_running },
        { "cancel_timeout", _test_cancel_timeout },
        { "cancel_waits_for_release", _test_cancel_waits_for_release },
        { "deinit_timeout_retry", _test_deinit_timeout_retry },
        { "deinit_while_suspended", _test_deinit_while_suspended },
        { "deinit_racing_suspend", _test_deinit_racing_suspend },
        { "repeated_init_policy", _test_repeated_init_policy },
        { "suspend_no_catch_up", _test_suspend_no_catch_up },
        { "suspend_timeout_rollback", _test_suspend_timeout_rollback },
        { "deinit_releases_queued", _test_deinit_releases_queued },
        { "self_call_rejected", _test_self_call_rejected },
        { "task_affinity_and_caps", _test_task_affinity_and_caps },
        { "concurrent_cancel", _test_concurrent_cancel },
    };
    unsigned failures = 0U;
    for (unsigned index = 0; index < sizeof(tests) / sizeof(tests[0]);
            ++index)
    {
        const unsigned failures_before = s_failures;
        atomic_store_explicit(&s_clock_offset_us, 0, memory_order_relaxed);
        (void)tests[index].run();
        if (s_failures == failures_before)
        {
            printf("PASS %s\n", tests[index].name);
        }
        else
        {
            printf("FAIL %s\n", tests[index].name);
            failures += s_failures - failures_before;
        }
    }
    printf("%s: %u failure(s)\n",
           failures == 0U ? "chore_service host tests" : "chore_service host tests FAILED",
           failures);
    return failures == 0U ? 0 : 1;
}
