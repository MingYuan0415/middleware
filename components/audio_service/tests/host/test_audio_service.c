#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "audio_service.h"
#include "audio_service_fakes.h"
#include "freertos/semphr.h"

typedef enum test_thread_role
{
    TEST_THREAD_NONE,
    TEST_THREAD_IO,
    TEST_THREAD_CONTROL,
    TEST_THREAD_ACQUIRE,
} test_thread_role_t;

typedef enum test_event
{
    TEST_EVENT_IO_UNLOCK = 1U << 0,
    TEST_EVENT_DRAIN_WAIT = 1U << 1,
    TEST_EVENT_CONTROL_RETURN = 1U << 2,
    TEST_EVENT_CONTROL_UNLOCK = 1U << 3,
    TEST_EVENT_ACQUIRE_TAKE = 1U << 4,
    TEST_EVENT_ACQUIRE_RETURN = 1U << 5,
} test_event_t;

static pthread_mutex_t s_schedule_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_schedule_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local test_thread_role_t s_thread_role;
static SemaphoreHandle_t s_service_mutex;
static SemaphoreHandle_t s_drained;
static unsigned int s_events;
static bool s_gate_io_unlock;
static bool s_gate_control_unlock;
static bool s_acquire_barrier;
static unsigned int s_acquire_arrivals;

SemaphoreHandle_t __real_xSemaphoreCreateMutex(void);
SemaphoreHandle_t __real_xSemaphoreCreateBinary(void);
BaseType_t __real_xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t __real_xSemaphoreGive(SemaphoreHandle_t semaphore);

SemaphoreHandle_t __wrap_xSemaphoreCreateMutex(void);
SemaphoreHandle_t __wrap_xSemaphoreCreateBinary(void);
BaseType_t __wrap_xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t __wrap_xSemaphoreGive(SemaphoreHandle_t semaphore);

static struct timespec _test_deadline(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    return deadline;
}

static void _schedule_event(unsigned int event)
{
    assert(pthread_mutex_lock(&s_schedule_lock) == 0);
    s_events |= event;
    assert(pthread_cond_broadcast(&s_schedule_changed) == 0);
    assert(pthread_mutex_unlock(&s_schedule_lock) == 0);
}

static bool _schedule_wait(unsigned int events)
{
    const struct timespec deadline = _test_deadline();
    assert(pthread_mutex_lock(&s_schedule_lock) == 0);
    int result = 0;
    while ((s_events & events) == 0U && result == 0)
    {
        result = pthread_cond_timedwait(&s_schedule_changed, &s_schedule_lock,
                                        &deadline);
    }
    assert(result == 0 || result == ETIMEDOUT);
    const bool reached = (s_events & events) != 0U;
    assert(pthread_mutex_unlock(&s_schedule_lock) == 0);
    return reached;
}

static void _schedule_reset(void)
{
    assert(pthread_mutex_lock(&s_schedule_lock) == 0);
    s_events = 0U;
    s_gate_io_unlock = false;
    s_gate_control_unlock = false;
    s_acquire_barrier = false;
    s_acquire_arrivals = 0U;
    assert(pthread_mutex_unlock(&s_schedule_lock) == 0);
}

static void _schedule_gate(bool io, bool blocked)
{
    assert(pthread_mutex_lock(&s_schedule_lock) == 0);
    if (io)
    {
        s_gate_io_unlock = blocked;
    }
    else
    {
        s_gate_control_unlock = blocked;
    }
    assert(pthread_cond_broadcast(&s_schedule_changed) == 0);
    assert(pthread_mutex_unlock(&s_schedule_lock) == 0);
}

SemaphoreHandle_t __wrap_xSemaphoreCreateMutex(void)
{
    s_service_mutex = __real_xSemaphoreCreateMutex();
    return s_service_mutex;
}

SemaphoreHandle_t __wrap_xSemaphoreCreateBinary(void)
{
    s_drained = __real_xSemaphoreCreateBinary();
    return s_drained;
}

BaseType_t __wrap_xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    if (s_thread_role == TEST_THREAD_CONTROL && semaphore == s_drained &&
            timeout != 0U)
    {
        /* 在真实排空等待之前发布，而不是靠 SUSPENDING 状态猜测。 */
        _schedule_event(TEST_EVENT_DRAIN_WAIT);
    }
    if (s_thread_role == TEST_THREAD_ACQUIRE && semaphore == s_service_mutex)
    {
        _schedule_event(TEST_EVENT_ACQUIRE_TAKE);
    }
    return __real_xSemaphoreTake(semaphore, timeout);
}

BaseType_t __wrap_xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    const BaseType_t result = __real_xSemaphoreGive(semaphore);
    if (semaphore != s_service_mutex)
    {
        return result;
    }
    assert(pthread_mutex_lock(&s_schedule_lock) == 0);
    if (s_thread_role == TEST_THREAD_IO && s_gate_io_unlock)
    {
        s_events |= TEST_EVENT_IO_UNLOCK;
        assert(pthread_cond_broadcast(&s_schedule_changed) == 0);
        while (s_gate_io_unlock)
        {
            assert(pthread_cond_wait(&s_schedule_changed, &s_schedule_lock) == 0);
        }
    }
    if (s_thread_role == TEST_THREAD_CONTROL && s_gate_control_unlock)
    {
        s_events |= TEST_EVENT_CONTROL_UNLOCK;
        assert(pthread_cond_broadcast(&s_schedule_changed) == 0);
        while (s_gate_control_unlock)
        {
            assert(pthread_cond_wait(&s_schedule_changed, &s_schedule_lock) == 0);
        }
    }
    assert(pthread_mutex_unlock(&s_schedule_lock) == 0);
    return result;
}

typedef struct audio_service_io_thread
{
    uint8_t data[32];
    size_t transferred;
    esp_err_t result;
    bool read;
    audio_service_lease_t lease;
} audio_service_io_thread_t;

typedef struct audio_service_suspend_thread
{
    uint32_t timeout_ms;
    esp_err_t result;
    bool resume_required;
} audio_service_suspend_thread_t;

typedef struct audio_service_control_thread
{
    audio_service_lease_t lease;
    esp_err_t result;
    bool deinit;
} audio_service_control_thread_t;

typedef struct audio_service_acquire_thread
{
    audio_service_lease_t lease;
    audio_service_session_kind_t kind;
    esp_err_t result;
} audio_service_acquire_thread_t;

static esp_err_t _test_write(void *data, size_t bytes, size_t *written,
                             uint32_t timeout_ms);
static esp_err_t _test_read(void *data, size_t bytes, size_t *read,
                            uint32_t timeout_ms);

static const audio_service_init_config_t s_init_config =
{
    .stream = {
        .sample_rate_hz = 16000U,
        .bits_per_sample = 16U,
        .channels = 2U,
        .mclk_multiple = 384U,
    },
    .volume_percent = 60U,
    .muted = false,
    .pa_enabled = true,
};

static void *_run_io(void *context)
{
    audio_service_io_thread_t *thread = context;
    s_thread_role = TEST_THREAD_IO;
    if (thread->read)
    {
        thread->result = audio_service_read_leased(thread->lease, thread->data,
            sizeof(thread->data), &thread->transferred, 1000U);
    }
    else
    {
        thread->result = audio_service_write_leased(thread->lease, thread->data,
            sizeof(thread->data), &thread->transferred, 1000U);
    }
    return NULL;
}

static void *_run_suspend(void *context)
{
    audio_service_suspend_thread_t *thread = context;
    s_thread_role = TEST_THREAD_CONTROL;
    thread->result = audio_service_suspend(thread->timeout_ms,
                                           &thread->resume_required);
    _schedule_event(TEST_EVENT_CONTROL_RETURN);
    return NULL;
}

static void *_run_control(void *context)
{
    audio_service_control_thread_t *thread = context;
    s_thread_role = TEST_THREAD_CONTROL;
    thread->result = thread->deinit ? audio_service_deinit() :
                     audio_service_release(thread->lease);
    _schedule_event(TEST_EVENT_CONTROL_RETURN);
    return NULL;
}

static void *_run_acquire(void *context)
{
    audio_service_acquire_thread_t *thread = context;
    s_thread_role = TEST_THREAD_ACQUIRE;
    assert(pthread_mutex_lock(&s_schedule_lock) == 0);
    ++s_acquire_arrivals;
    assert(pthread_cond_broadcast(&s_schedule_changed) == 0);
    while (s_acquire_barrier)
    {
        assert(pthread_cond_wait(&s_schedule_changed, &s_schedule_lock) == 0);
    }
    assert(pthread_mutex_unlock(&s_schedule_lock) == 0);
    thread->result = audio_service_acquire(thread->kind, &thread->lease);
    _schedule_event(TEST_EVENT_ACQUIRE_RETURN);
    return NULL;
}

static audio_service_lease_t s_lease;

static esp_err_t _test_acquire(audio_service_session_kind_t kind)
{
    return audio_service_acquire(kind, &s_lease);
}

static esp_err_t _test_release(void)
{
    return audio_service_release(s_lease);
}

static esp_err_t _test_write(void *data, size_t bytes, size_t *written,
                             uint32_t timeout_ms)
{
    return audio_service_write_leased(s_lease, data, bytes, written,
                                      timeout_ms);
}

static esp_err_t _test_read(void *data, size_t bytes, size_t *read,
                            uint32_t timeout_ms)
{
    return audio_service_read_leased(s_lease, data, bytes, read, timeout_ms);
}

static void _test_missing_hal(void)
{
    audio_service_fakes_reset();
    assert(audio_service_init(NULL) == ESP_ERR_INVALID_ARG);
    audio_service_init_config_t invalid = s_init_config;
    invalid.stream.mclk_multiple = 0U;
    assert(audio_service_init(&invalid) == ESP_ERR_INVALID_ARG);
    assert(audio_service_init(&s_init_config) == ESP_ERR_NOT_FOUND);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
    assert(!audio_service_is_available());
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_full_duplex_lifecycle(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;

    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(audio_service_init(&s_init_config) == ESP_OK);
    audio_service_init_config_t different = s_init_config;
    different.volume_percent++;
    assert(audio_service_init(&different) == ESP_ERR_INVALID_STATE);
    audio_service_config_t active = {0};
    assert(audio_service_get_config(&active) == ESP_OK);
    assert(active.sample_rate_hz == s_init_config.stream.sample_rate_hz);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_READY);
    assert(audio_service_is_available());
    assert(fake->configure_count == 1U);
    assert(fake->config.sample_rate_hz == 16000U);
    assert(fake->config.bits_per_sample == 16U);
    assert(fake->config.channels == 2U);
    assert(fake->config.mclk_multiple == 384U);
    assert(fake->pa_enabled);

    const audio_service_config_t alternate =
    {
        .sample_rate_hz = 48000U,
        .bits_per_sample = 24U,
        .channels = 2U,
        .mclk_multiple = 384U,
    };
    assert(audio_service_configure(&alternate) == ESP_OK);
    assert(fake->configure_count == 2U);
    assert(fake->config.sample_rate_hz == 48000U);

    assert(_test_acquire(AUDIO_SERVICE_SESSION_RECORD) == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_RUNNING);
    assert(fake->start_count == 1U);
    assert(audio_service_configure(&alternate) == ESP_ERR_INVALID_STATE);

    uint8_t output[32] = {0};
    size_t transferred = 0U;
    assert(_test_write(output, sizeof(output), &transferred, 250U) == ESP_OK);
    assert(transferred == sizeof(output));
    assert(fake->write_count == 1U);
    assert(fake->last_timeout_ms == 250U);
    assert(fake->state_during_io == AUDIO_SERVICE_STATE_RUNNING);

    uint8_t input[32] = {0};
    transferred = 0U;
    assert(_test_read(input, sizeof(input), &transferred, 500U) == ESP_OK);
    assert(transferred == sizeof(input));
    assert(input[0] == 0x5aU);
    assert(fake->read_count == 1U);
    assert(fake->last_timeout_ms == 500U);
    assert(fake->state_during_io == AUDIO_SERVICE_STATE_RUNNING);

    assert(audio_service_set_volume(73U) == ESP_OK);
    uint8_t volume = 0U;
    assert(audio_service_get_volume(&volume) == ESP_OK);
    assert(volume == 73U);
    assert(audio_service_set_mute(true) == ESP_OK);
    bool muted = false;
    assert(audio_service_get_mute(&muted) == ESP_OK);
    assert(muted);
    assert(audio_service_set_pa(false) == ESP_OK);
    bool pa_enabled = true;
    assert(audio_service_get_pa(&pa_enabled) == ESP_OK);
    assert(!pa_enabled);
    assert(audio_service_init(&s_init_config) == ESP_OK);

    assert(_test_release() == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_READY);
    assert(fake->stop_count == 1U);
    assert(audio_service_deinit() == ESP_OK);
    assert(fake->stop_count == 2U);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_UNINITIALIZED);
}

static void _test_stop_and_deinit_retry(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;

    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(_test_acquire(AUDIO_SERVICE_SESSION_RECORD) == ESP_OK);
    fake->stop_result = ESP_FAIL;
    assert(_test_release() == ESP_FAIL);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
    assert(fake->started);
    assert(fake->stop_count == 1U);

    const audio_service_config_t alternate =
    {
        .sample_rate_hz = 48000U,
        .bits_per_sample = 16U,
        .channels = 2U,
        .mclk_multiple = 384U,
    };
    assert(audio_service_configure(&alternate) == ESP_ERR_INVALID_STATE);
    assert(fake->configure_count == 1U);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);

    fake->stop_result = ESP_OK;
    assert(_test_release() == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_READY);
    assert(!fake->started);
    assert(fake->stop_count == 2U);

    assert(_test_acquire(AUDIO_SERVICE_SESSION_RECORD) == ESP_OK);
    assert(audio_service_deinit() == ESP_ERR_INVALID_STATE);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_RUNNING);
    assert(fake->started);
    assert(fake->stop_count == 2U);

    fake->stop_result = ESP_FAIL;
    assert(_test_release() == ESP_FAIL);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
    assert(fake->stop_count == 3U);

    fake->stop_result = ESP_OK;
    assert(_test_release() == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_READY);
    assert(audio_service_deinit() == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_UNINITIALIZED);
    assert(!fake->started);
    assert(fake->stop_count == 5U);
}

static void _test_deinit_accepts_already_unavailable_bsp(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;

    assert(audio_service_init(&s_init_config) == ESP_OK);
    fake->available = false;
    fake->stop_result = ESP_ERR_INVALID_STATE;
    assert(audio_service_deinit() == ESP_OK);
    assert(fake->stop_count == 1U);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_UNINITIALIZED);
}

static void _test_partial_init_cleanup_retry(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    fake->set_pa_result = ESP_FAIL;
    fake->stop_result = ESP_FAIL;

    assert(audio_service_init(&s_init_config) == ESP_FAIL);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
    assert(audio_service_init(&s_init_config) == ESP_ERR_INVALID_STATE);
    audio_service_lease_t partial = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &partial) ==
           ESP_ERR_INVALID_STATE);
    assert(partial.token == 0U && partial.generation == 0U);
    assert(audio_service_resume(0U) == ESP_ERR_INVALID_STATE);
    assert(fake->start_count == 0U);
    assert(audio_service_deinit() == ESP_FAIL);
    assert(fake->stop_count == 1U);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);

    fake->stop_result = ESP_OK;
    assert(audio_service_deinit() == ESP_OK);
    assert(fake->stop_count == 2U);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_UNINITIALIZED);

    audio_service_fakes_reset();
    fake = audio_service_fakes_state();
    fake->expose_ops = true;
    fake->configure_result = ESP_FAIL;
    assert(audio_service_init(&s_init_config) == ESP_FAIL);
    assert(audio_service_deinit() == ESP_OK);
    assert(fake->stop_count == 1U);
}

static void _test_suspend_ready_and_error_semantics(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);

    bool resume_required = true;
    assert(audio_service_suspend(0U, &resume_required) == ESP_OK);
    assert(!resume_required);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_READY);
    assert(fake->stop_count == 0U);

    assert(_test_acquire(AUDIO_SERVICE_SESSION_RECORD) == ESP_OK);
    fake->stop_result = ESP_FAIL;
    assert(_test_release() == ESP_FAIL);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);

    resume_required = true;
    assert(audio_service_suspend(10U, &resume_required) == ESP_FAIL);
    assert(!resume_required);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
    assert(fake->stop_count == 2U);

    fake->stop_result = ESP_OK;
    assert(audio_service_suspend(10U, &resume_required) == ESP_OK);
    assert(!resume_required);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_READY);
    assert(fake->stop_count == 3U);
    assert(_test_release() == ESP_OK);
    assert(fake->stop_count == 3U);
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_failed_running_suspend_can_resume(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(_test_acquire(AUDIO_SERVICE_SESSION_RECORD) == ESP_OK);

    fake->stop_result = ESP_FAIL;
    bool resume_required = false;
    assert(audio_service_suspend(10U, &resume_required) == ESP_FAIL);
    assert(resume_required);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
    uint8_t data = 0U;
    assert(_test_write(&data, sizeof(data), NULL, 0U) ==
           ESP_ERR_INVALID_STATE);

    fake->stop_result = ESP_OK;
    fake->start_result = ESP_FAIL;
    assert(audio_service_resume(10U) == ESP_FAIL);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
    assert(_test_write(&data, sizeof(data), NULL, 0U) ==
           ESP_ERR_INVALID_STATE);

    fake->start_result = ESP_OK;
    assert(audio_service_resume(10U) == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_RUNNING);
    assert(_test_write(&data, sizeof(data), NULL, 0U) == ESP_OK);
    assert(_test_release() == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_blocked_io_suspend_timeout(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(_test_acquire(AUDIO_SERVICE_SESSION_RECORD) == ESP_OK);

    audio_service_fakes_block_io(true);
    audio_service_io_thread_t io = {.lease = s_lease};
    pthread_t io_thread;
    assert(pthread_create(&io_thread, NULL, _run_io, &io) == 0);
    assert(audio_service_fakes_wait_for_io(1U, 1000U));

    bool resume_required = false;
    assert(audio_service_suspend(0U, &resume_required) == ESP_ERR_TIMEOUT);
    assert(resume_required);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_RUNNING);
    assert(fake->stop_count == 0U);

    audio_service_fakes_block_io(false);
    assert(pthread_join(io_thread, NULL) == 0);
    assert(io.result == ESP_OK);
    assert(io.transferred == sizeof(io.data));

    assert(audio_service_suspend(100U, &resume_required) == ESP_OK);
    assert(resume_required);
    assert(!fake->stop_during_io);
    assert(audio_service_resume(100U) == ESP_OK);
    assert(_test_release() == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_blocked_io_suspend_drain(void)
{
    _schedule_reset();
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(_test_acquire(AUDIO_SERVICE_SESSION_RECORD) == ESP_OK);

    audio_service_fakes_block_io(true);
    audio_service_io_thread_t io = {.read = true, .lease = s_lease};
    pthread_t io_thread;
    assert(pthread_create(&io_thread, NULL, _run_io, &io) == 0);
    assert(audio_service_fakes_wait_for_io(1U, 1000U));

    audio_service_suspend_thread_t suspend = {.timeout_ms = 1000U};
    pthread_t suspend_thread;
    assert(pthread_create(&suspend_thread, NULL, _run_suspend, &suspend) == 0);
    assert(_schedule_wait(TEST_EVENT_DRAIN_WAIT | TEST_EVENT_CONTROL_RETURN));
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_SUSPENDING);

    uint8_t data = 0U;
    assert(_test_write(&data, sizeof(data), NULL, 0U) ==
           ESP_ERR_INVALID_STATE);
    audio_service_fakes_block_io(false);
    assert(pthread_join(io_thread, NULL) == 0);
    assert(pthread_join(suspend_thread, NULL) == 0);
    assert(io.result == ESP_OK);
    assert(suspend.result == ESP_OK);
    assert(suspend.resume_required);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_READY);
    assert(!fake->stop_during_io);

    assert(audio_service_resume(100U) == ESP_OK);
    assert(_test_release() == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_lease_exclusivity_and_stale_handle(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);

    audio_service_lease_t first = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &first) ==
           ESP_OK);
    assert(audio_service_is_leased());
    assert(first.token != 0U);
    assert(first.generation != 0U);

    audio_service_lease_t second = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_RECORD, &second) ==
           ESP_ERR_INVALID_STATE);
    uint8_t data = 0U;
    size_t transferred = 0U;
    assert(audio_service_read_leased(first, &data, sizeof(data),
                                     &transferred, 0U) ==
           ESP_ERR_INVALID_STATE);
    assert(audio_service_write_leased(first, &data, sizeof(data),
                                      &transferred, 0U) == ESP_OK);
    assert(audio_service_get_lease(&second) == ESP_OK);
    assert(second.token == first.token);
    assert(second.generation == first.generation);

    assert(audio_service_release(first) == ESP_OK);
    assert(!audio_service_is_leased());
    assert(audio_service_release(first) == ESP_ERR_INVALID_STATE);

    audio_service_lease_t third = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_RECORD, &third) ==
           ESP_OK);
    assert(third.generation != first.generation);
    assert(audio_service_read_leased(third, &data, sizeof(data),
                                     &transferred, 0U) == ESP_OK);
    assert(audio_service_release(third) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_deinit_rejects_active_lease(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    audio_service_lease_t lease = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_MAINTENANCE, &lease) ==
           ESP_OK);
    assert(audio_service_deinit() == ESP_ERR_INVALID_STATE);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_RUNNING);
    assert(audio_service_release(lease) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
}

static void _assert_stale_io(audio_service_lease_t stale)
{
    uint8_t data = 0U;
    size_t transferred = 99U;
    assert(audio_service_write_leased(stale, &data, 1U, &transferred, 0U) ==
           ESP_ERR_INVALID_STATE);
    assert(transferred == 0U);
    transferred = 99U;
    assert(audio_service_read_leased(stale, &data, 1U, &transferred, 0U) ==
           ESP_ERR_INVALID_STATE);
    assert(transferred == 0U);
    assert(audio_service_release(stale) == ESP_ERR_INVALID_STATE);
}

static void _assert_playback_owner(audio_service_lease_t lease)
{
    audio_service_lease_t current = {0};
    assert(audio_service_get_lease(&current) == ESP_OK);
    assert(current.token == lease.token && current.generation == lease.generation);
    uint8_t data = 0U;
    size_t transferred = 99U;
    assert(audio_service_read_leased(lease, &data, 1U, &transferred, 0U) ==
           ESP_ERR_INVALID_STATE);
    assert(transferred == 0U);
    assert(audio_service_write_leased(lease, &data, 1U, &transferred, 0U) == ESP_OK);
    assert(transferred == 1U);
}

static void _test_io_admission_and_release_reservation(bool read)
{
    _schedule_reset();
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    audio_service_lease_t first = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_RECORD, &first) == ESP_OK);

    /* 停在 I/O 线程真实第一次解锁之后，旧实现此时尚未 active_io++。 */
    _schedule_gate(true, true);
    audio_service_io_thread_t io = {.read = read, .lease = first};
    pthread_t io_thread;
    assert(pthread_create(&io_thread, NULL, _run_io, &io) == 0);
    assert(_schedule_wait(TEST_EVENT_IO_UNLOCK));
    audio_service_control_thread_t release = {.lease = first};
    pthread_t control_thread;
    assert(pthread_create(&control_thread, NULL, _run_control, &release) == 0);
    assert(_schedule_wait(TEST_EVENT_DRAIN_WAIT | TEST_EVENT_CONTROL_RETURN));
    assert(pthread_mutex_lock(&s_schedule_lock) == 0);
    const bool drained = (s_events & TEST_EVENT_DRAIN_WAIT) != 0U;
    assert(pthread_mutex_unlock(&s_schedule_lock) == 0);

    audio_service_lease_t next = {0};
    if (drained)
    {
        assert(audio_service_get_state() == AUDIO_SERVICE_STATE_SUSPENDING);
        assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &next) ==
               ESP_ERR_INVALID_STATE);
        assert(next.token == 0U && next.generation == 0U);
        assert(audio_service_release(first) == ESP_ERR_INVALID_STATE);
        bool resume_required = true;
        assert(audio_service_suspend(0U, &resume_required) == ESP_ERR_INVALID_STATE);
        assert(!resume_required);
        assert(audio_service_deinit() == ESP_ERR_INVALID_STATE);
        assert(audio_service_resume(0U) == ESP_ERR_INVALID_STATE);
        assert(audio_service_configure(&s_init_config.stream) == ESP_ERR_INVALID_STATE);
        audio_service_lease_t current = {0};
        assert(audio_service_get_lease(&current) == ESP_OK);
        assert(current.token == first.token && current.generation == first.generation);
        _assert_stale_io((audio_service_lease_t)
        {
            0
        });
        uint8_t byte = 0U;
        size_t transferred = 99U;
        assert(audio_service_write_leased(first, &byte, 1U, &transferred, 0U) ==
               ESP_ERR_INVALID_STATE);
        assert(transferred == 0U);
        assert(fake->stop_count == 0U && fake->start_count == 1U);
    }
    else
    {
        /* 旧实现的确定性反例：先建立 B，再让 A 越过第二层准入。 */
        assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &next) == ESP_OK);
    }
    _schedule_gate(true, false);
    assert(pthread_join(io_thread, NULL) == 0);
    assert(pthread_join(control_thread, NULL) == 0);
    assert(release.result == ESP_OK);
    assert(io.result == ESP_OK && io.transferred == sizeof(io.data));
    if (drained)
    {
        assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &next) == ESP_OK);
    }
    assert(next.generation != first.generation);
    const uint32_t reads = fake->read_count;
    const uint32_t writes = fake->write_count;
    _assert_stale_io(first);
    audio_service_lease_t wrong_token = next;
    ++wrong_token.token;
    _assert_stale_io(wrong_token);
    audio_service_lease_t wrong_generation = next;
    ++wrong_generation.generation;
    _assert_stale_io(wrong_generation);
    assert(fake->read_count == reads && fake->write_count == writes);
    _assert_playback_owner(next);
    assert(audio_service_release(next) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
    assert(!fake->stop_during_io);
    assert(drained);
}

static void _test_two_simultaneous_acquires(void)
{
    _schedule_reset();
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(pthread_mutex_lock(&s_schedule_lock) == 0);
    s_acquire_barrier = true;
    assert(pthread_mutex_unlock(&s_schedule_lock) == 0);
    audio_service_acquire_thread_t first = {.kind = AUDIO_SERVICE_SESSION_RECORD};
    audio_service_acquire_thread_t second = {.kind = AUDIO_SERVICE_SESSION_PLAYBACK};
    pthread_t threads[2];
    assert(pthread_create(&threads[0], NULL, _run_acquire, &first) == 0);
    assert(pthread_create(&threads[1], NULL, _run_acquire, &second) == 0);
    const struct timespec deadline = _test_deadline();
    assert(pthread_mutex_lock(&s_schedule_lock) == 0);
    int result = 0;
    while (s_acquire_arrivals != 2U && result == 0)
    {
        result = pthread_cond_timedwait(&s_schedule_changed, &s_schedule_lock,
                                        &deadline);
    }
    const bool both_ready = s_acquire_arrivals == 2U;
    s_acquire_barrier = false;
    assert(pthread_cond_broadcast(&s_schedule_changed) == 0);
    assert(pthread_mutex_unlock(&s_schedule_lock) == 0);
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);
    assert(both_ready);
    assert((first.result == ESP_OK && second.result == ESP_ERR_INVALID_STATE) ||
           (second.result == ESP_OK && first.result == ESP_ERR_INVALID_STATE));
    const audio_service_acquire_thread_t *winner = first.result == ESP_OK ? &first : &second;
    const audio_service_acquire_thread_t *loser = first.result == ESP_OK ? &second : &first;
    assert(loser->lease.token == 0U && loser->lease.generation == 0U);
    assert(fake->start_count == 1U);
    assert(audio_service_release(winner->lease) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_zero_io_deinit_atomic_commit(void)
{
    _schedule_reset();
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    audio_service_fakes_block_stop(true);
    audio_service_control_thread_t deinit = {.deinit = true};
    pthread_t control_thread;
    assert(pthread_create(&control_thread, NULL, _run_control, &deinit) == 0);
    assert(audio_service_fakes_wait_for_stop(1000U));
    /* 只拦 stop 后的解锁，不能误拦旧实现 stop 前的临时 reservation。 */
    _schedule_gate(false, true);
    audio_service_acquire_thread_t acquire = {.kind = AUDIO_SERVICE_SESSION_PLAYBACK};
    pthread_t acquire_thread;
    assert(pthread_create(&acquire_thread, NULL, _run_acquire, &acquire) == 0);
    assert(_schedule_wait(TEST_EVENT_ACQUIRE_TAKE));
    audio_service_fakes_block_stop(false);
    assert(_schedule_wait(TEST_EVENT_CONTROL_UNLOCK));
    assert(_schedule_wait(TEST_EVENT_ACQUIRE_RETURN));
    assert(pthread_join(acquire_thread, NULL) == 0);
    _schedule_gate(false, false);
    assert(pthread_join(control_thread, NULL) == 0);
    assert(deinit.result == ESP_OK);
    assert(acquire.result == ESP_ERR_INVALID_STATE);
    assert(acquire.lease.token == 0U && acquire.lease.generation == 0U);
    assert(fake->start_count == 0U && fake->stop_count == 1U);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_UNINITIALIZED);
}

static void _test_acquire_start_compensation(void)
{
    for (unsigned int compensation_fails = 0U; compensation_fails != 2U;
            ++compensation_fails)
    {
        audio_service_fakes_reset();
        audio_service_fake_state_t *fake = audio_service_fakes_state();
        fake->expose_ops = true;
        assert(audio_service_init(&s_init_config) == ESP_OK);
        audio_service_lease_t previous = {0};
        assert(audio_service_acquire(AUDIO_SERVICE_SESSION_RECORD, &previous) == ESP_OK);
        assert(audio_service_release(previous) == ESP_OK);
        fake->start_result = ESP_ERR_TIMEOUT;
        fake->start_enables_before_failure = true;
        fake->stop_result = compensation_fails ? ESP_FAIL : ESP_OK;
        audio_service_lease_t failed = {.token = 42U, .generation = 42U};
        assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &failed) ==
               ESP_ERR_TIMEOUT);
        assert(failed.token == 0U && failed.generation == 0U);
        assert(!audio_service_is_leased());
        assert(fake->stop_count == 2U);
        assert(audio_service_get_state() == (compensation_fails ?
                                             AUDIO_SERVICE_STATE_ERROR : AUDIO_SERVICE_STATE_READY));
        assert(fake->started == (compensation_fails != 0U));
        if (compensation_fails)
        {
            assert(audio_service_resume(0U) == ESP_ERR_INVALID_STATE);
            fake->start_result = ESP_OK;
            assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &failed) == ESP_FAIL);
            assert(failed.token == 0U && failed.generation == 0U);
            assert(fake->start_count == 2U && fake->stop_count == 3U);
            assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
        }
        fake->start_result = ESP_OK;
        fake->stop_result = ESP_OK;
        audio_service_lease_t recovered = {0};
        assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &recovered) == ESP_OK);
        assert(recovered.generation == previous.generation + 1U);
        assert(fake->last_start_stop_count == (compensation_fails ? 4U : 2U));
        _assert_playback_owner(recovered);
        assert(audio_service_release(recovered) == ESP_OK);
        assert(audio_service_deinit() == ESP_OK);
    }
}

static void _test_io_failure_transferred_zero(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    audio_service_lease_t lease = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_RECORD, &lease) == ESP_OK);
    uint8_t data = 0U;
    size_t transferred = 99U;
    assert(audio_service_write_leased(lease, NULL, 1U, &transferred, 0U) ==
           ESP_ERR_INVALID_ARG);
    assert(transferred == 0U);
    transferred = 99U;
    assert(audio_service_read_leased(lease, NULL, 1U, &transferred, 0U) ==
           ESP_ERR_INVALID_ARG);
    assert(transferred == 0U);
    transferred = 99U;
    assert(audio_service_write_leased(lease, &data, 0U, &transferred, 0U) ==
           ESP_ERR_INVALID_ARG);
    assert(transferred == 0U);
    transferred = 99U;
    assert(audio_service_read_leased(lease, &data, 0U, &transferred, 0U) ==
           ESP_ERR_INVALID_ARG);
    assert(transferred == 0U);
    bool resume_required = false;
    assert(audio_service_suspend(100U, &resume_required) == ESP_OK);
    transferred = 99U;
    assert(audio_service_read_leased(lease, &data, 1U, &transferred, 0U) ==
           ESP_ERR_INVALID_STATE);
    assert(transferred == 0U);
    transferred = 99U;
    assert(audio_service_write_leased(lease, &data, 1U, &transferred, 0U) ==
           ESP_ERR_INVALID_STATE);
    assert(transferred == 0U);
    assert(fake->read_count == 0U && fake->write_count == 0U);
    assert(audio_service_release(lease) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
    _assert_stale_io(lease);
}

static void _test_ready_release_atomic_commit(void)
{
    _schedule_reset();
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    audio_service_lease_t first = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_RECORD, &first) == ESP_OK);
    bool resume_required = false;
    assert(audio_service_suspend(100U, &resume_required) == ESP_OK);
    assert(resume_required);
    _schedule_gate(false, true);
    audio_service_control_thread_t release = {.lease = first};
    pthread_t control_thread;
    assert(pthread_create(&control_thread, NULL, _run_control, &release) == 0);
    assert(_schedule_wait(TEST_EVENT_CONTROL_UNLOCK));
    audio_service_lease_t next = {0};
    const esp_err_t acquire_result = audio_service_acquire(
                                         AUDIO_SERVICE_SESSION_PLAYBACK, &next);
    _schedule_gate(false, false);
    assert(pthread_join(control_thread, NULL) == 0);
    assert(release.result == ESP_OK);
    assert(acquire_result == ESP_OK);
    assert(fake->stop_count == 1U);
    assert(audio_service_resume(0U) == ESP_OK);
    assert(fake->start_count == 2U && fake->stop_count == 1U);
    _assert_stale_io(first);
    _assert_playback_owner(next);
    assert(audio_service_release(next) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_suspended_release_and_resume(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(audio_service_resume(0U) == ESP_OK);
    assert(fake->start_count == 0U);
    audio_service_lease_t first = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_RECORD, &first) == ESP_OK);
    bool resume_required = false;
    assert(audio_service_suspend(100U, &resume_required) == ESP_OK);
    assert(resume_required);
    assert(audio_service_configure(&s_init_config.stream) == ESP_ERR_INVALID_STATE);
    audio_service_lease_t next = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &next) ==
           ESP_ERR_INVALID_STATE);
    assert(audio_service_suspend(0U, &resume_required) == ESP_OK);
    assert(!resume_required);
    assert(audio_service_release(first) == ESP_OK);
    assert(fake->stop_count == 1U);
    assert(audio_service_resume(0U) == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_READY);
    assert(fake->start_count == 1U && fake->stop_count == 1U);
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &next) == ESP_OK);
    assert(audio_service_resume(0U) == ESP_OK);
    assert(fake->start_count == 2U && fake->stop_count == 1U);
    _assert_stale_io(first);
    _assert_playback_owner(next);
    assert(audio_service_release(next) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
    assert(audio_service_resume(0U) == ESP_ERR_INVALID_STATE);
    assert(audio_service_init(&s_init_config) == ESP_OK);
    audio_service_lease_t after_reinit = {0};
    assert(audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK, &after_reinit) == ESP_OK);
    assert(after_reinit.generation == next.generation + 1U);
    _assert_stale_io(first);
    _assert_stale_io(next);
    _assert_playback_owner(after_reinit);
    assert(audio_service_release(after_reinit) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
}

/* The plain setter is used by drag previews and benchmarks, so it must never
 * touch flash. */
static void _test_volume_set_does_not_persist(void)
{
    audio_service_fakes_reset();
    audio_service_fakes_state()->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);

    assert(audio_service_set_volume(35U) == ESP_OK);
    assert(audio_service_fakes_state()->volume == 35U);
    assert(audio_service_fakes_nv_set_count() == 0U);
    assert(!audio_service_fakes_nv_present());

    assert(audio_service_deinit() == ESP_OK);
}

/* A settled choice is applied and stored exactly once. */
static void _test_volume_persisted_applies_and_stores(void)
{
    audio_service_fakes_reset();
    audio_service_fakes_state()->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);

    assert(audio_service_set_volume_persisted(40U) == ESP_OK);
    assert(audio_service_fakes_state()->volume == 40U);
    assert(audio_service_fakes_nv_present());
    assert(audio_service_fakes_nv_value() == 40U);
    assert(audio_service_fakes_nv_set_count() == 1U);
    assert(audio_service_fakes_chore_submit_count() == 1U);

    /* Out of range is refused before anything is applied or stored. */
    assert(audio_service_set_volume_persisted(101U) == ESP_ERR_INVALID_ARG);
    assert(audio_service_fakes_state()->volume == 40U);
    assert(audio_service_fakes_nv_set_count() == 1U);
    assert(audio_service_fakes_chore_submit_count() == 1U);

    assert(audio_service_deinit() == ESP_OK);
}

/* The queued commit runs after the call returns on the firmware; a storage
 * fault is logged and must not revert the level. */
static void _test_volume_persist_failure_keeps_level(void)
{
    audio_service_fakes_reset();
    audio_service_fakes_state()->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);

    audio_service_fakes_nv_fail_set();
    assert(audio_service_set_volume_persisted(55U) == ESP_OK);
    assert(audio_service_fakes_state()->volume == 55U);
    assert(!audio_service_fakes_nv_present());
    assert(audio_service_fakes_nv_set_count() == 1U);
    assert(audio_service_fakes_chore_submit_count() == 1U);

    assert(audio_service_deinit() == ESP_OK);
}

/* A full or unavailable chore queue still applies the level but reports that
 * the commit was not accepted. */
static void _test_volume_persist_queue_failure_keeps_level(void)
{
    audio_service_fakes_reset();
    audio_service_fakes_state()->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);

    audio_service_fakes_chore_fail(ESP_ERR_NO_MEM);
    assert(audio_service_set_volume_persisted(45U) == ESP_ERR_NO_MEM);
    assert(audio_service_fakes_state()->volume == 45U);
    assert(!audio_service_fakes_nv_present());
    assert(audio_service_fakes_nv_set_count() == 0U);
    assert(audio_service_fakes_chore_submit_count() == 1U);

    assert(audio_service_deinit() == ESP_OK);
}

/* A refused audio request must not reach storage at all. */
static void _test_volume_persist_skipped_without_audio_path(void)
{
    audio_service_fakes_reset();
    audio_service_fakes_state()->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);

    /* Deinitialized: the audio path rejects the level. */
    assert(audio_service_set_volume_persisted(70U) == ESP_ERR_INVALID_STATE);
    assert(audio_service_fakes_nv_set_count() == 0U);
    assert(!audio_service_fakes_nv_present());
}

static void _test_volume_restore(void)
{
    audio_service_fakes_reset();
    audio_service_fakes_state()->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);

    /* No record keeps the configured default and is not an error. */
    assert(audio_service_restore_volume() == ESP_OK);
    assert(audio_service_fakes_state()->volume == 60U);

    /* A stored level overrides the default. */
    audio_service_fakes_reset();
    audio_service_fakes_state()->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(audio_service_set_volume_persisted(25U) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);

    audio_service_fakes_state()->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(audio_service_fakes_state()->volume == 60U);
    assert(audio_service_restore_volume() == ESP_OK);
    assert(audio_service_fakes_state()->volume == 25U);

    /* A storage read fault is reported, not swallowed. */
    audio_service_fakes_nv_fail_get();
    assert(audio_service_restore_volume() == ESP_FAIL);
    assert(audio_service_fakes_state()->volume == 25U);

    /* A corrupt record is refused so the default stands instead of muting or
     * overdriving the speaker. */
    audio_service_fakes_reset();
    audio_service_fakes_state()->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    audio_service_fakes_nv_seed(200U);
    assert(audio_service_restore_volume() == ESP_ERR_INVALID_STATE);
    assert(audio_service_fakes_state()->volume == 60U);

    assert(audio_service_deinit() == ESP_OK);
}

static void _test_volume_reset_persisted_state(void)
{
    audio_service_fakes_reset();
    audio_service_fakes_state()->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);

    /* Absent key counts as success. */
    assert(audio_service_reset_persisted_state() == ESP_OK);
    assert(audio_service_fakes_nv_erase_count() == 1U);

    /* A stored key is erased. */
    assert(audio_service_set_volume_persisted(80U) == ESP_OK);
    assert(audio_service_fakes_nv_present());
    assert(audio_service_reset_persisted_state() == ESP_OK);
    assert(!audio_service_fakes_nv_present());

    /* A real erase failure still surfaces. */
    audio_service_fakes_nv_fail_erase();
    assert(audio_service_reset_persisted_state() == ESP_FAIL);

    assert(audio_service_deinit() == ESP_OK);
}

int main(void)
{
    _test_missing_hal();
    _test_full_duplex_lifecycle();
    _test_lease_exclusivity_and_stale_handle();
    _test_deinit_rejects_active_lease();
    _test_stop_and_deinit_retry();
    _test_deinit_accepts_already_unavailable_bsp();
    _test_partial_init_cleanup_retry();
    _test_suspend_ready_and_error_semantics();
    _test_failed_running_suspend_can_resume();
    _test_blocked_io_suspend_timeout();
    _test_blocked_io_suspend_drain();
    _test_io_admission_and_release_reservation(false);
    _test_io_admission_and_release_reservation(true);
    _test_two_simultaneous_acquires();
    _test_zero_io_deinit_atomic_commit();
    _test_ready_release_atomic_commit();
    _test_io_failure_transferred_zero();
    _test_acquire_start_compensation();
    _test_suspended_release_and_resume();
    _test_volume_set_does_not_persist();
    _test_volume_persisted_applies_and_stores();
    _test_volume_persist_failure_keeps_level();
    _test_volume_persist_queue_failure_keeps_level();
    _test_volume_persist_skipped_without_audio_path();
    _test_volume_restore();
    _test_volume_reset_persisted_state();
    puts("audio service host tests passed");
    return 0;
}
