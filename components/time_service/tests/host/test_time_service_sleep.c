#include "host_freertos.h"
#include "host_time_port.h"
#include "time_service.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef struct sleep_rtc_fake
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool block_poll;
    bool poll_entered;
    bool block_status;
    bool status_entered;
    bool block_write;
    bool write_entered;
    uint32_t poll_count;
    uint32_t write_count;
} sleep_rtc_fake_t;

static sleep_rtc_fake_t s_rtc =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
};

static struct timespec _deadline_after_ms(uint32_t timeout_ms)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    const uint64_t nanoseconds = (uint64_t)timeout_ms * UINT64_C(1000000);
    deadline.tv_sec += (time_t)(nanoseconds / UINT64_C(1000000000));
    deadline.tv_nsec += (long)(nanoseconds % UINT64_C(1000000000));
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static bool _wait_for_flag(const bool *flag, uint32_t timeout_ms)
{
    const struct timespec deadline = _deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_rtc.lock);
    int wait_result = 0;
    while (!*flag && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_rtc.changed, &s_rtc.lock,
                                             &deadline);
    }
    const bool reached = *flag;
    (void)pthread_mutex_unlock(&s_rtc.lock);
    return reached;
}

static bool _rtc_available(void)
{
    return true;
}

static esp_err_t _rtc_read(struct tm *utc_time)
{
    assert(utc_time != NULL);
    *utc_time = (struct tm)
    {
        .tm_year = 124,
        .tm_mon = 0,
        .tm_mday = 1,
        .tm_wday = 1,
        .tm_yday = 0,
        .tm_isdst = 0,
    };
    return ESP_OK;
}

static esp_err_t _rtc_write(const struct tm *utc_time)
{
    assert(utc_time != NULL);
    (void)pthread_mutex_lock(&s_rtc.lock);
    ++s_rtc.write_count;
    if (s_rtc.block_write)
    {
        s_rtc.write_entered = true;
        (void)pthread_cond_broadcast(&s_rtc.changed);
        while (s_rtc.block_write)
        {
            (void)pthread_cond_wait(&s_rtc.changed, &s_rtc.lock);
        }
    }
    (void)pthread_cond_broadcast(&s_rtc.changed);
    (void)pthread_mutex_unlock(&s_rtc.lock);
    return ESP_OK;
}

static esp_err_t _alarm_configure(
    const time_service_alarm_config_t *config)
{
    assert(config != NULL);
    return ESP_OK;
}

static esp_err_t _alarm_disable(void)
{
    return ESP_OK;
}

static esp_err_t _alarm_get_status(time_service_alarm_status_t *status)
{
    assert(status != NULL);
    (void)pthread_mutex_lock(&s_rtc.lock);
    if (s_rtc.block_status)
    {
        s_rtc.status_entered = true;
        (void)pthread_cond_broadcast(&s_rtc.changed);
        while (s_rtc.block_status)
        {
            (void)pthread_cond_wait(&s_rtc.changed, &s_rtc.lock);
        }
    }
    *status = (time_service_alarm_status_t)
    {
        .enabled = true,
        .pending = false,
        .interrupt_active = false,
    };
    (void)pthread_mutex_unlock(&s_rtc.lock);
    return ESP_OK;
}

static esp_err_t _alarm_clear(void)
{
    return ESP_OK;
}

static esp_err_t _alarm_poll_interrupt(bool *active)
{
    assert(active != NULL);
    (void)pthread_mutex_lock(&s_rtc.lock);
    ++s_rtc.poll_count;
    if (s_rtc.block_poll)
    {
        s_rtc.poll_entered = true;
        (void)pthread_cond_broadcast(&s_rtc.changed);
        while (s_rtc.block_poll)
        {
            (void)pthread_cond_wait(&s_rtc.changed, &s_rtc.lock);
        }
    }
    *active = false;
    (void)pthread_mutex_unlock(&s_rtc.lock);
    return ESP_OK;
}

static void _set_block_poll(bool blocked)
{
    (void)pthread_mutex_lock(&s_rtc.lock);
    s_rtc.block_poll = blocked;
    s_rtc.poll_entered = false;
    (void)pthread_cond_broadcast(&s_rtc.changed);
    (void)pthread_mutex_unlock(&s_rtc.lock);
}

static void _set_block_status(bool blocked)
{
    (void)pthread_mutex_lock(&s_rtc.lock);
    s_rtc.block_status = blocked;
    s_rtc.status_entered = false;
    (void)pthread_cond_broadcast(&s_rtc.changed);
    (void)pthread_mutex_unlock(&s_rtc.lock);
}

static void _set_block_write(bool blocked)
{
    (void)pthread_mutex_lock(&s_rtc.lock);
    s_rtc.block_write = blocked;
    s_rtc.write_entered = false;
    (void)pthread_cond_broadcast(&s_rtc.changed);
    (void)pthread_mutex_unlock(&s_rtc.lock);
}

static uint32_t _poll_count(void)
{
    (void)pthread_mutex_lock(&s_rtc.lock);
    const uint32_t count = s_rtc.poll_count;
    (void)pthread_mutex_unlock(&s_rtc.lock);
    return count;
}

static uint32_t _write_count(void)
{
    (void)pthread_mutex_lock(&s_rtc.lock);
    const uint32_t count = s_rtc.write_count;
    (void)pthread_mutex_unlock(&s_rtc.lock);
    return count;
}

static void *_read_alarm_status(void *context)
{
    esp_err_t *result = context;
    time_service_alarm_status_t status;
    *result = time_service_alarm_get_status(&status);
    return NULL;
}

static void _delay_ms(uint32_t delay_ms)
{
    struct timespec delay =
    {
        .tv_sec = (time_t)(delay_ms / 1000U),
        .tv_nsec = (long)(delay_ms % 1000U) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
    {
    }
}

int main(void)
{
    host_time_port_reset();
    _set_block_poll(true);
    const time_service_rtc_ops_t ops =
    {
        .is_available = _rtc_available,
        .read = _rtc_read,
        .write = _rtc_write,
        .alarm_configure = _alarm_configure,
        .alarm_disable = _alarm_disable,
        .alarm_get_status = _alarm_get_status,
        .alarm_clear = _alarm_clear,
        .alarm_poll_interrupt = _alarm_poll_interrupt,
    };
    assert(time_service_register_rtc_ops(&ops) == ESP_OK);
    assert(time_service_init() == ESP_OK);

    assert(_wait_for_flag(&s_rtc.poll_entered, 1000U));
    assert(time_service_suspend(20U) == ESP_ERR_TIMEOUT);
    assert(time_service_resume(0U) == ESP_ERR_TIMEOUT);
    _set_block_poll(false);
    assert(time_service_resume(500U) == ESP_OK);

    _set_block_write(true);
    assert(time_service_request_sync() == ESP_OK);
    assert(host_time_port_complete(INT64_C(1704067200)));
    assert(_wait_for_flag(&s_rtc.write_entered, 1000U));
    assert(time_service_suspend(20U) == ESP_ERR_TIMEOUT);
    _set_block_write(false);
    assert(time_service_resume(500U) == ESP_OK);
    assert(time_service_wait_sync(1000U) == ESP_OK);

    _set_block_status(true);
    esp_err_t status_result = ESP_FAIL;
    pthread_t status_thread;
    assert(pthread_create(&status_thread, NULL, _read_alarm_status,
                          &status_result) == 0);
    assert(_wait_for_flag(&s_rtc.status_entered, 1000U));
    assert(time_service_suspend(20U) == ESP_ERR_TIMEOUT);
    _set_block_status(false);
    assert(pthread_join(status_thread, NULL) == 0);
    assert(status_result == ESP_OK);
    assert(time_service_resume(500U) == ESP_OK);

    assert(time_service_request_sync() == ESP_OK);
    assert(time_service_suspend(500U) == ESP_OK);
    const uint32_t suspended_polls = _poll_count();
    const uint32_t suspended_writes = _write_count();
    assert(host_time_port_complete(INT64_C(1704067201)));
    _delay_ms(250U);
    assert(_poll_count() == suspended_polls);
    assert(_write_count() == suspended_writes);
    time_service_alarm_status_t status;
    assert(time_service_alarm_get_status(&status) == ESP_ERR_INVALID_STATE);
    assert(time_service_request_sync() == ESP_ERR_INVALID_STATE);
    assert(time_service_wait_sync(0U) == ESP_ERR_INVALID_STATE);

    assert(time_service_resume(500U) == ESP_OK);
    assert(time_service_wait_sync(1000U) == ESP_OK);
    assert(_write_count() == suspended_writes + 1U);
    assert(time_service_deinit() == ESP_OK);
    assert(host_freertos_wait_for_tasks(1000U));
    puts("time_service sleep barrier regression passed");
    return 0;
}
