#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "audio_service.h"
#include "audio_service_fakes.h"

typedef struct audio_service_io_thread
{
    uint8_t data[32];
    size_t transferred;
    esp_err_t result;
    bool read;
} audio_service_io_thread_t;

typedef struct audio_service_suspend_thread
{
    uint32_t timeout_ms;
    esp_err_t result;
    bool resume_required;
} audio_service_suspend_thread_t;

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
    if (thread->read)
    {
        thread->result = audio_service_read(thread->data,
                                            sizeof(thread->data),
                                            &thread->transferred, 1000U);
    }
    else
    {
        thread->result = audio_service_write(thread->data,
                                             sizeof(thread->data),
                                             &thread->transferred, 1000U);
    }
    return NULL;
}

static void *_run_suspend(void *context)
{
    audio_service_suspend_thread_t *thread = context;
    thread->result = audio_service_suspend(thread->timeout_ms,
                                           &thread->resume_required);
    return NULL;
}

static void _wait_for_state(audio_service_state_t expected)
{
    for (uint32_t attempt = 0U; attempt < 100000U; ++attempt)
    {
        if (audio_service_get_state() == expected)
        {
            return;
        }
        (void)sched_yield();
    }
    assert(false);
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

    assert(audio_service_start() == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_RUNNING);
    assert(fake->start_count == 1U);
    assert(audio_service_configure(&alternate) == ESP_ERR_INVALID_STATE);

    uint8_t output[32] = {0};
    size_t transferred = 0U;
    assert(audio_service_write(output, sizeof(output), &transferred, 250U) == ESP_OK);
    assert(transferred == sizeof(output));
    assert(fake->write_count == 1U);
    assert(fake->last_timeout_ms == 250U);
    assert(fake->state_during_io == AUDIO_SERVICE_STATE_RUNNING);

    uint8_t input[32] = {0};
    transferred = 0U;
    assert(audio_service_read(input, sizeof(input), &transferred, 500U) == ESP_OK);
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

    assert(audio_service_stop() == ESP_OK);
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
    assert(audio_service_start() == ESP_OK);
    fake->stop_result = ESP_FAIL;
    assert(audio_service_stop() == ESP_FAIL);
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
    assert(audio_service_stop() == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_READY);
    assert(!fake->started);
    assert(fake->stop_count == 2U);

    assert(audio_service_start() == ESP_OK);
    fake->stop_result = ESP_FAIL;
    assert(audio_service_deinit() == ESP_FAIL);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
    assert(audio_service_is_available());
    assert(fake->started);
    assert(fake->stop_count == 3U);

    fake->stop_result = ESP_OK;
    assert(audio_service_deinit() == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_UNINITIALIZED);
    assert(!fake->started);
    assert(fake->stop_count == 4U);
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

    assert(audio_service_start() == ESP_OK);
    fake->stop_result = ESP_FAIL;
    assert(audio_service_stop() == ESP_FAIL);
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
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_failed_running_suspend_can_resume(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(audio_service_start() == ESP_OK);

    fake->stop_result = ESP_FAIL;
    bool resume_required = false;
    assert(audio_service_suspend(10U, &resume_required) == ESP_FAIL);
    assert(resume_required);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
    uint8_t data = 0U;
    assert(audio_service_write(&data, sizeof(data), NULL, 0U) ==
           ESP_ERR_INVALID_STATE);

    fake->stop_result = ESP_OK;
    fake->start_result = ESP_FAIL;
    assert(audio_service_resume(10U) == ESP_FAIL);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_ERROR);
    assert(audio_service_write(&data, sizeof(data), NULL, 0U) ==
           ESP_ERR_INVALID_STATE);

    fake->start_result = ESP_OK;
    assert(audio_service_resume(10U) == ESP_OK);
    assert(audio_service_get_state() == AUDIO_SERVICE_STATE_RUNNING);
    assert(audio_service_write(&data, sizeof(data), NULL, 0U) == ESP_OK);
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_blocked_io_suspend_timeout(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(audio_service_start() == ESP_OK);

    audio_service_fakes_block_io(true);
    audio_service_io_thread_t io = {0};
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
    assert(audio_service_deinit() == ESP_OK);
}

static void _test_blocked_io_suspend_drain(void)
{
    audio_service_fakes_reset();
    audio_service_fake_state_t *fake = audio_service_fakes_state();
    fake->expose_ops = true;
    assert(audio_service_init(&s_init_config) == ESP_OK);
    assert(audio_service_start() == ESP_OK);

    audio_service_fakes_block_io(true);
    audio_service_io_thread_t io = {.read = true};
    pthread_t io_thread;
    assert(pthread_create(&io_thread, NULL, _run_io, &io) == 0);
    assert(audio_service_fakes_wait_for_io(1U, 1000U));

    audio_service_suspend_thread_t suspend = {.timeout_ms = 1000U};
    pthread_t suspend_thread;
    assert(pthread_create(&suspend_thread, NULL, _run_suspend, &suspend) == 0);
    _wait_for_state(AUDIO_SERVICE_STATE_SUSPENDING);

    uint8_t data = 0U;
    assert(audio_service_write(&data, sizeof(data), NULL, 0U) ==
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
    assert(audio_service_deinit() == ESP_OK);
}

int main(void)
{
    _test_missing_hal();
    _test_full_duplex_lifecycle();
    _test_stop_and_deinit_retry();
    _test_deinit_accepts_already_unavailable_bsp();
    _test_partial_init_cleanup_retry();
    _test_suspend_ready_and_error_semantics();
    _test_failed_running_suspend_can_resume();
    _test_blocked_io_suspend_timeout();
    _test_blocked_io_suspend_drain();
    puts("audio service host tests passed");
    return 0;
}
