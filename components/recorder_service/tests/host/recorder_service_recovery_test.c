/* Real command owner, with deterministic queue/clock seams only in this TU. */
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "esp_timer.h"
#include "../../src/recorder_host_worker.h"

static int _receive(QueueHandle_t queue, void *item, uint32_t timeout);
static int _send(QueueHandle_t queue, const void *item, uint32_t timeout);
static void _delay(uint32_t milliseconds);
static int64_t _time(void);
#define xQueueReceive _receive
#define xQueueSend _send
#define vTaskDelay _delay
#define esp_timer_get_time _time
#include "../../src/recorder_service.c"
#undef xQueueReceive
#undef xQueueSend
#undef vTaskDelay
#undef esp_timer_get_time

extern void fake_sd_set_mount_path(const char *path);
extern unsigned fake_sd_leases(sd_storage_service_usage_kind_t kind);
extern esp_err_t s_fake_audio_stop_error;
extern audio_service_lease_t fake_audio_active_lease(void);

static pthread_mutex_t s_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_gate_cond = PTHREAD_COND_INITIALIZER;
static bool s_hold_shutdown;
static bool s_shutdown_received;
static bool s_retry_waiting;
static bool s_timeout;
static unsigned s_sends;
static unsigned s_fail_send;
static unsigned s_receives;
static _Thread_local int64_t s_clock_offset;

static int64_t _time(void)
{
    return esp_timer_get_time() + s_clock_offset;
}

static struct timespec _deadline(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 2;
    return deadline;
}

static int _send(QueueHandle_t queue, const void *item, uint32_t timeout)
{
    ++s_sends;
    if (s_sends == s_fail_send) return 0;
    assert(queue != NULL);
    return xQueueSend(queue, item, timeout);
}

static int _receive(QueueHandle_t queue, void *item, uint32_t timeout)
{
    pthread_mutex_lock(&s_gate_mutex);
    ++s_receives;
    pthread_cond_broadcast(&s_gate_cond);
    pthread_mutex_unlock(&s_gate_mutex);
    const int result = xQueueReceive(queue, item, timeout);
    const recorder_command_t *command = item;
    if (result == pdTRUE && command->type == RECORDER_COMMAND_SHUTDOWN)
    {
        pthread_mutex_lock(&s_gate_mutex);
        s_shutdown_received = true;
        pthread_cond_broadcast(&s_gate_cond);
        const struct timespec deadline = _deadline();
        while (s_hold_shutdown)
        {
            assert(pthread_cond_timedwait(&s_gate_cond, &s_gate_mutex, &deadline) == 0);
        }
        pthread_mutex_unlock(&s_gate_mutex);
    }
    return result;
}

static void _delay(uint32_t milliseconds)
{
    pthread_mutex_lock(&s_gate_mutex);
    if (s_timeout)
    {
        const struct timespec deadline = _deadline();
        while (!s_shutdown_received)
        {
            assert(pthread_cond_timedwait(&s_gate_cond, &s_gate_mutex, &deadline) == 0);
        }
        s_clock_offset = 6000000LL;
    }
    else if (s_hold_shutdown)
    {
        s_retry_waiting = true;
        pthread_cond_broadcast(&s_gate_cond);
        const struct timespec deadline = _deadline();
        while (s_hold_shutdown)
        {
            assert(pthread_cond_timedwait(&s_gate_cond, &s_gate_mutex, &deadline) == 0);
        }
    }
    pthread_mutex_unlock(&s_gate_mutex);
    vTaskDelay(milliseconds);
}

static void *_retry(void *argument)
{
    (void)argument;
    assert(recorder_service_deinit() == ESP_OK);
    return NULL;
}

static void _init_ready(const recorder_service_config_t *config)
{
    pthread_mutex_lock(&s_gate_mutex);
    const unsigned ready = s_receives + 2U;
    pthread_mutex_unlock(&s_gate_mutex);
    assert(recorder_service_init(config) == ESP_OK);
    /* The second receive begins only after the initial refresh has completed.
     * Private start/finish calls must not race that asynchronous publisher. */
    pthread_mutex_lock(&s_gate_mutex);
    const struct timespec deadline = _deadline();
    while (s_receives < ready)
    {
        assert(pthread_cond_timedwait(&s_gate_cond, &s_gate_mutex, &deadline) == 0);
    }
    pthread_mutex_unlock(&s_gate_mutex);
}

int main(void)
{
    char root[] = "/tmp/mt-recorder-recovery-XXXXXX";
    assert(mkdtemp(root) != NULL);
    fake_sd_set_mount_path(root);
    const recorder_service_config_t config =
    {
        .directory = "Recordings", .max_duration_seconds = 60U,
        .minimum_free_bytes = 1U,
    };
    _init_ready(&config);
    assert(_recorder_start_now() == ESP_OK);
    const audio_service_lease_t retained = fake_audio_active_lease();
    s_fake_audio_stop_error = ESP_FAIL;
    _recorder_finish(false, ESP_OK, false);
    assert(recorder_service_deinit() == ESP_FAIL);
    assert(s_initialized && s_recorder.stopping);
    assert(s_recorder.command_queue == NULL && s_recorder.command_task == NULL);
    assert(fake_audio_active_lease().generation == retained.generation);
    const unsigned sends = s_sends;
    assert(recorder_service_deinit() == ESP_FAIL);
    assert(s_sends == sends);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    s_fake_audio_stop_error = ESP_OK;
    assert(recorder_service_deinit() == ESP_OK);
    assert(fake_audio_active_lease().token == 0U);

    /* Reinitialize, record, and play without resetting recorder/fake state. */
    _init_ready(&config);
    assert(_recorder_start_now() == ESP_OK);
    assert(fake_audio_active_lease().generation != retained.generation);
    _recorder_finish(false, ESP_OK, false);
    assert(s_recorder.file_count > 0U);
    assert(_recorder_play_now(&s_index_files[0].target) == ESP_OK);
    _recorder_finish(false, ESP_OK, false);

    /* Failed enqueue leaves the queue live and retryable at either boundary. */
    s_fail_send = s_sends + 1U;
    assert(recorder_service_deinit() == ESP_ERR_TIMEOUT);
    assert(!s_recorder.command_shutdown_sent && s_recorder.command_queue != NULL);
    s_fail_send = s_sends + 2U;
    assert(recorder_service_deinit() == ESP_ERR_TIMEOUT);
    assert(!s_recorder.command_shutdown_sent && s_recorder.command_queue != NULL);
    s_fail_send = 0U;
    pthread_mutex_lock(&s_gate_mutex);
    s_hold_shutdown = true;
    s_shutdown_received = false;
    s_timeout = true;
    pthread_mutex_unlock(&s_gate_mutex);
    assert(recorder_service_deinit() == ESP_ERR_TIMEOUT);
    s_clock_offset = 0;
    assert(s_recorder.command_shutdown_sent && s_recorder.command_queue != NULL);
    const unsigned shutdown_sends = s_sends;
    pthread_mutex_lock(&s_gate_mutex);
    s_timeout = false;
    pthread_mutex_unlock(&s_gate_mutex);
    pthread_t retry;
    assert(pthread_create(&retry, NULL, _retry, NULL) == 0);
    pthread_mutex_lock(&s_gate_mutex);
    const struct timespec deadline = _deadline();
    while (!s_retry_waiting)
    {
        assert(pthread_cond_timedwait(&s_gate_cond, &s_gate_mutex, &deadline) == 0);
    }
    assert(s_sends == shutdown_sends);
    s_hold_shutdown = false;
    pthread_cond_broadcast(&s_gate_cond);
    pthread_mutex_unlock(&s_gate_mutex);
    assert(pthread_join(retry, NULL) == 0);
    assert(!s_initialized && s_recorder.command_queue == NULL);
    _init_ready(&config);
    assert(_recorder_start_now() == ESP_OK);
    assert(recorder_service_deinit() == ESP_OK);
    char cleanup[256];
    assert(snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", root) > 0);
    assert(system(cleanup) == 0);
    return 0;
}
