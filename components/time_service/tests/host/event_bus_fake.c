#include "event_bus.h"
#include "host_event_bus.h"
#include "time_service.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

typedef struct host_event_bus_state
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    esp_err_t result;
    uint32_t attempts;
    uint32_t count;
    uint32_t last_flags;
    time_service_alarm_event_t last_event;
} host_event_bus_state_t;

static host_event_bus_state_t s_event_bus =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
};

static struct timespec _host_event_bus_deadline(uint32_t timeout_ms)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static bool _host_event_bus_wait_for(uint32_t *value, uint32_t count,
                                     uint32_t timeout_ms)
{
    const struct timespec deadline = _host_event_bus_deadline(timeout_ms);
    (void)pthread_mutex_lock(&s_event_bus.lock);
    int wait_result = 0;
    while (*value < count && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(
                          &s_event_bus.changed, &s_event_bus.lock, &deadline);
    }
    const bool reached = *value >= count;
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return reached;
}

void host_event_bus_reset(void)
{
    (void)pthread_mutex_lock(&s_event_bus.lock);
    s_event_bus.result = ESP_OK;
    s_event_bus.attempts = 0U;
    s_event_bus.count = 0U;
    s_event_bus.last_flags = 0U;
    memset(&s_event_bus.last_event, 0, sizeof(s_event_bus.last_event));
    (void)pthread_mutex_unlock(&s_event_bus.lock);
}

void host_event_bus_set_result(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_event_bus.lock);
    s_event_bus.result = result;
    (void)pthread_mutex_unlock(&s_event_bus.lock);
}

bool host_event_bus_wait_for_attempts(uint32_t count, uint32_t timeout_ms)
{
    return _host_event_bus_wait_for(
               &s_event_bus.attempts, count, timeout_ms);
}

bool host_event_bus_wait_for_count(uint32_t count, uint32_t timeout_ms)
{
    return _host_event_bus_wait_for(&s_event_bus.count, count, timeout_ms);
}

uint32_t host_event_bus_count(void)
{
    (void)pthread_mutex_lock(&s_event_bus.lock);
    const uint32_t count = s_event_bus.count;
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return count;
}

uint32_t host_event_bus_last_flags(void)
{
    (void)pthread_mutex_lock(&s_event_bus.lock);
    const uint32_t flags = s_event_bus.last_flags;
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return flags;
}

uint32_t host_event_bus_last_sequence(void)
{
    (void)pthread_mutex_lock(&s_event_bus.lock);
    const uint32_t sequence = s_event_bus.last_event.sequence;
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return sequence;
}

esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags)
{
    if (msg_id != TIME_SERVICE_MSG ||
            sub_type != TIME_SERVICE_MSG_SUB_TYPE_RTC_ALARM ||
            payload == NULL ||
            payload_size != sizeof(time_service_alarm_event_t))
    {
        return ESP_ERR_INVALID_ARG;
    }

    (void)pthread_mutex_lock(&s_event_bus.lock);
    ++s_event_bus.attempts;
    const esp_err_t result = s_event_bus.result;
    if (result == ESP_OK)
    {
        ++s_event_bus.count;
        s_event_bus.last_flags = flags;
        memcpy(&s_event_bus.last_event, payload, payload_size);
    }
    (void)pthread_cond_broadcast(&s_event_bus.changed);
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return result;
}
