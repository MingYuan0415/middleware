#include "event_bus.h"
#include "host_event_bus.h"
#include "imu_service.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define HOST_EVENT_SUBTYPE_COUNT 4U

typedef struct host_event_record
{
    uint32_t attempts;
    uint32_t count;
    uint32_t flags;
    imu_service_snapshot_t snapshot;
    imu_service_sample_t interrupt;
} host_event_record_t;

typedef struct host_event_bus_state
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    esp_err_t result;
    host_event_record_t records[HOST_EVENT_SUBTYPE_COUNT];
} host_event_bus_state_t;

static host_event_bus_state_t s_event_bus =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
};

static bool _valid_subtype(uint32_t subtype)
{
    return subtype >= IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE &&
           subtype <= IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT;
}

static struct timespec _deadline_after_ms(uint32_t timeout_ms)
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

static bool _wait_for_counter(uint32_t subtype, uint32_t count,
                              uint32_t timeout_ms, bool attempts)
{
    if (!_valid_subtype(subtype))
    {
        return false;
    }
    const struct timespec deadline = _deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_event_bus.lock);
    int wait_result = 0;
    for (;;)
    {
        const uint32_t current = attempts ?
                                 s_event_bus.records[subtype].attempts :
                                 s_event_bus.records[subtype].count;
        if (current >= count || wait_result == ETIMEDOUT)
        {
            break;
        }
        wait_result = pthread_cond_timedwait(&s_event_bus.changed,
                                             &s_event_bus.lock, &deadline);
    }
    const uint32_t current = attempts ?
                             s_event_bus.records[subtype].attempts :
                             s_event_bus.records[subtype].count;
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return current >= count;
}

void host_event_bus_reset(void)
{
    (void)pthread_mutex_lock(&s_event_bus.lock);
    s_event_bus.result = ESP_OK;
    memset(s_event_bus.records, 0, sizeof(s_event_bus.records));
    (void)pthread_mutex_unlock(&s_event_bus.lock);
}

void host_event_bus_set_result(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_event_bus.lock);
    s_event_bus.result = result;
    (void)pthread_mutex_unlock(&s_event_bus.lock);
}

bool host_event_bus_wait_for_count(uint32_t subtype, uint32_t count,
                                   uint32_t timeout_ms)
{
    return _wait_for_counter(subtype, count, timeout_ms, false);
}

bool host_event_bus_wait_for_attempts(uint32_t subtype, uint32_t count,
                                      uint32_t timeout_ms)
{
    return _wait_for_counter(subtype, count, timeout_ms, true);
}

uint32_t host_event_bus_count(uint32_t subtype)
{
    if (!_valid_subtype(subtype))
    {
        return 0U;
    }
    (void)pthread_mutex_lock(&s_event_bus.lock);
    const uint32_t count = s_event_bus.records[subtype].count;
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return count;
}

uint32_t host_event_bus_flags(uint32_t subtype)
{
    if (!_valid_subtype(subtype))
    {
        return UINT32_MAX;
    }
    (void)pthread_mutex_lock(&s_event_bus.lock);
    const uint32_t flags = s_event_bus.records[subtype].flags;
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return flags;
}

bool host_event_bus_get_snapshot(uint32_t subtype,
                                 imu_service_snapshot_t *snapshot)
{
    if ((subtype != IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE &&
            subtype != IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED) ||
            snapshot == NULL)
    {
        return false;
    }
    (void)pthread_mutex_lock(&s_event_bus.lock);
    const bool present = s_event_bus.records[subtype].count != 0U;
    if (present)
    {
        *snapshot = s_event_bus.records[subtype].snapshot;
    }
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return present;
}

bool host_event_bus_get_interrupt(imu_service_sample_t *sample)
{
    if (sample == NULL)
    {
        return false;
    }
    (void)pthread_mutex_lock(&s_event_bus.lock);
    const bool present =
        s_event_bus.records[IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT].count != 0U;
    if (present)
    {
        *sample = s_event_bus.records[
                      IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT].interrupt;
    }
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return present;
}

esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags)
{
    if (msg_id != IMU_SERVICE_MSG || !_valid_subtype(sub_type) ||
            payload == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const bool interrupt =
        sub_type == IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT;
    const size_t expected_size = interrupt ? sizeof(imu_service_sample_t) :
                                 sizeof(imu_service_snapshot_t);
    if (payload_size != expected_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    (void)pthread_mutex_lock(&s_event_bus.lock);
    host_event_record_t *record = &s_event_bus.records[sub_type];
    ++record->attempts;
    const esp_err_t result = s_event_bus.result;
    if (result == ESP_OK)
    {
        ++record->count;
        record->flags = flags;
        if (interrupt)
        {
            memcpy(&record->interrupt, payload, payload_size);
        }
        else
        {
            memcpy(&record->snapshot, payload, payload_size);
        }
    }
    (void)pthread_cond_broadcast(&s_event_bus.changed);
    (void)pthread_mutex_unlock(&s_event_bus.lock);
    return result;
}
