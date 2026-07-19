#include "host_event_bus.h"
#include "host_freertos.h"
#include "host_time_port.h"
#include "time_service.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct alarm_fake_state
{
    pthread_mutex_t lock;
    time_service_alarm_config_t config;
    bool enabled;
    bool pending;
    bool interrupt_active;
    uint32_t clear_count;
} alarm_fake_state_t;

static alarm_fake_state_t s_alarm =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static bool _alarm_available(void)
{
    return true;
}

static esp_err_t _alarm_read(struct tm *utc_time)
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

static esp_err_t _alarm_write(const struct tm *utc_time)
{
    assert(utc_time != NULL);
    return ESP_OK;
}

static esp_err_t _alarm_configure(
    const time_service_alarm_config_t *config)
{
    assert(config != NULL);
    (void)pthread_mutex_lock(&s_alarm.lock);
    s_alarm.config = *config;
    s_alarm.enabled = true;
    s_alarm.pending = false;
    s_alarm.interrupt_active = false;
    (void)pthread_mutex_unlock(&s_alarm.lock);
    return ESP_OK;
}

static esp_err_t _alarm_disable(void)
{
    (void)pthread_mutex_lock(&s_alarm.lock);
    s_alarm.enabled = false;
    s_alarm.pending = false;
    s_alarm.interrupt_active = false;
    (void)pthread_mutex_unlock(&s_alarm.lock);
    return ESP_OK;
}

static esp_err_t _alarm_get_status(time_service_alarm_status_t *status)
{
    assert(status != NULL);
    (void)pthread_mutex_lock(&s_alarm.lock);
    *status = (time_service_alarm_status_t)
    {
        .enabled = s_alarm.enabled,
        .pending = s_alarm.pending,
        .interrupt_active = false,
    };
    (void)pthread_mutex_unlock(&s_alarm.lock);
    return ESP_OK;
}

static esp_err_t _alarm_clear(void)
{
    (void)pthread_mutex_lock(&s_alarm.lock);
    s_alarm.pending = false;
    s_alarm.interrupt_active = false;
    ++s_alarm.clear_count;
    (void)pthread_mutex_unlock(&s_alarm.lock);
    return ESP_OK;
}

static esp_err_t _alarm_poll_interrupt(bool *active)
{
    assert(active != NULL);
    (void)pthread_mutex_lock(&s_alarm.lock);
    *active = s_alarm.interrupt_active;
    (void)pthread_mutex_unlock(&s_alarm.lock);
    return ESP_OK;
}

static void _alarm_trigger(bool assert_interrupt)
{
    (void)pthread_mutex_lock(&s_alarm.lock);
    assert(s_alarm.enabled);
    s_alarm.pending = true;
    s_alarm.interrupt_active = assert_interrupt;
    (void)pthread_mutex_unlock(&s_alarm.lock);
}

static uint32_t _alarm_clear_count(void)
{
    (void)pthread_mutex_lock(&s_alarm.lock);
    const uint32_t count = s_alarm.clear_count;
    (void)pthread_mutex_unlock(&s_alarm.lock);
    return count;
}

static bool _wait_for_clear_count(uint32_t expected, uint32_t timeout_ms)
{
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
    for (uint32_t elapsed = 0U; elapsed < timeout_ms; ++elapsed)
    {
        if (_alarm_clear_count() >= expected)
        {
            return true;
        }
        (void)nanosleep(&delay, NULL);
    }
    return _alarm_clear_count() >= expected;
}

int main(void)
{
    host_time_port_reset();
    host_event_bus_reset();
    (void)pthread_mutex_lock(&s_alarm.lock);
    memset(&s_alarm.config, 0, sizeof(s_alarm.config));
    s_alarm.enabled = false;
    s_alarm.pending = false;
    s_alarm.interrupt_active = false;
    s_alarm.clear_count = 0U;
    (void)pthread_mutex_unlock(&s_alarm.lock);

    const time_service_rtc_ops_t ops =
    {
        .is_available = _alarm_available,
        .read = _alarm_read,
        .write = _alarm_write,
        .alarm_configure = _alarm_configure,
        .alarm_disable = _alarm_disable,
        .alarm_get_status = _alarm_get_status,
        .alarm_clear = _alarm_clear,
        .alarm_poll_interrupt = _alarm_poll_interrupt,
    };
    assert(time_service_register_rtc_ops(&ops) == ESP_OK);
    assert(time_service_init() == ESP_OK);

    const time_service_alarm_config_t invalid = {0};
    assert(time_service_alarm_configure(&invalid) == ESP_ERR_INVALID_ARG);
    const time_service_alarm_config_t config =
    {
        .match_minute = true,
        .minute = 42U,
    };
    assert(time_service_alarm_configure(&config) == ESP_OK);

    time_service_alarm_status_t status;
    assert(time_service_alarm_get_status(&status) == ESP_OK);
    assert(status.enabled && !status.pending && !status.interrupt_active);

    host_event_bus_set_result(ESP_ERR_NO_MEM);
    _alarm_trigger(true);
    assert(_wait_for_clear_count(1U, 1000U));
    assert(host_event_bus_wait_for_attempts(1U, 1000U));
    assert(host_event_bus_count() == 0U);

    host_event_bus_set_result(ESP_OK);
    assert(host_event_bus_wait_for_count(1U, 1000U));
    assert(host_event_bus_last_flags() == 0U);
    assert(host_event_bus_last_sequence() == 1U);
    struct timespec settle = {.tv_sec = 0, .tv_nsec = 250000000L};
    (void)nanosleep(&settle, NULL);
    assert(host_event_bus_count() == 1U);
    assert(_alarm_clear_count() == 1U);

    _alarm_trigger(true);
    assert(host_event_bus_wait_for_count(2U, 1000U));
    assert(_wait_for_clear_count(2U, 1000U));
    assert(host_event_bus_last_sequence() == 2U);

    _alarm_trigger(false);
    assert(time_service_alarm_get_status(&status) == ESP_OK);
    assert(status.pending && !status.interrupt_active);
    assert(time_service_alarm_clear() == ESP_OK);
    assert(time_service_alarm_disable() == ESP_OK);
    assert(time_service_alarm_get_status(&status) == ESP_OK);
    assert(!status.enabled && !status.pending && !status.interrupt_active);

    assert(time_service_deinit() == ESP_OK);
    assert(host_freertos_wait_for_tasks(1000U));
    puts("time_service RTC alarm regression passed");
    return 0;
}
