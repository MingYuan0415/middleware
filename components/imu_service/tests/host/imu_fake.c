#include "host_imu.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define HOST_IMU_INTERRUPT_LEVEL_CAPACITY 16U
#define HOST_IMU_STATUS_INT_CAPACITY      16U

typedef struct host_imu_state
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    imu_service_sample_t sample;
    esp_err_t read_result;
    esp_err_t configure_result;
    esp_err_t poll_result;
    esp_err_t enable_result;
    esp_err_t disable_result;
    uint32_t read_count;
    uint32_t configure_count;
    uint32_t configured_sample_rate_hz;
    uint32_t enable_count;
    uint32_t disable_count;
    size_t interrupt_count;
    size_t interrupt_index;
    size_t status_int_count;
    size_t status_int_index;
    uint32_t blocked_read_count;
    bool interrupt_levels[HOST_IMU_INTERRUPT_LEVEL_CAPACITY];
    uint8_t status_int_values[HOST_IMU_STATUS_INT_CAPACITY];
    bool available;
    bool reads_blocked;
} host_imu_state_t;

static host_imu_state_t s_imu =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
};

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

static bool _is_available(void)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    const bool available = s_imu.available;
    (void)pthread_mutex_unlock(&s_imu.lock);
    return available;
}

static esp_err_t _configure(uint32_t sample_rate_hz)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    ++s_imu.configure_count;
    s_imu.configured_sample_rate_hz = sample_rate_hz;
    const esp_err_t result = s_imu.configure_result;
    (void)pthread_mutex_unlock(&s_imu.lock);
    return result;
}

static esp_err_t _read(imu_service_sample_t *sample)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    ++s_imu.read_count;
    if (s_imu.reads_blocked)
    {
        ++s_imu.blocked_read_count;
        (void)pthread_cond_broadcast(&s_imu.changed);
        while (s_imu.reads_blocked)
        {
            (void)pthread_cond_wait(&s_imu.changed, &s_imu.lock);
        }
        --s_imu.blocked_read_count;
    }
    const esp_err_t result = s_imu.read_result;
    if (result == ESP_OK)
    {
        *sample = s_imu.sample;
        if (s_imu.status_int_index < s_imu.status_int_count)
        {
            sample->status_int =
                s_imu.status_int_values[s_imu.status_int_index++];
        }
    }
    (void)pthread_cond_broadcast(&s_imu.changed);
    (void)pthread_mutex_unlock(&s_imu.lock);
    return result;
}

static esp_err_t _set_enabled(bool enabled)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    esp_err_t result;
    if (enabled)
    {
        ++s_imu.enable_count;
        result = s_imu.enable_result;
    }
    else
    {
        ++s_imu.disable_count;
        result = s_imu.disable_result;
    }
    (void)pthread_mutex_unlock(&s_imu.lock);
    return result;
}

static esp_err_t _poll_interrupt(bool *active)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    const esp_err_t result = s_imu.poll_result;
    if (result == ESP_OK)
    {
        bool level = false;
        if (s_imu.interrupt_count != 0U)
        {
            const size_t index = s_imu.interrupt_index <
                                 s_imu.interrupt_count ?
                                 s_imu.interrupt_index :
                                 s_imu.interrupt_count - 1U;
            level = s_imu.interrupt_levels[index];
            if (s_imu.interrupt_index < s_imu.interrupt_count)
            {
                ++s_imu.interrupt_index;
            }
        }
        *active = level;
    }
    (void)pthread_mutex_unlock(&s_imu.lock);
    return result;
}

static const imu_service_imu_ops_t s_ops =
{
    .is_available = _is_available,
    .configure = _configure,
    .read = _read,
    .set_enabled = _set_enabled,
    .poll_interrupt = _poll_interrupt,
};

void host_imu_reset(void)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    memset(&s_imu.sample, 0, sizeof(s_imu.sample));
    s_imu.read_result = ESP_OK;
    s_imu.configure_result = ESP_OK;
    s_imu.poll_result = ESP_OK;
    s_imu.enable_result = ESP_OK;
    s_imu.disable_result = ESP_OK;
    s_imu.read_count = 0U;
    s_imu.configure_count = 0U;
    s_imu.configured_sample_rate_hz = 0U;
    s_imu.enable_count = 0U;
    s_imu.disable_count = 0U;
    s_imu.interrupt_count = 0U;
    s_imu.interrupt_index = 0U;
    s_imu.status_int_count = 0U;
    s_imu.status_int_index = 0U;
    s_imu.blocked_read_count = 0U;
    memset(s_imu.interrupt_levels, 0, sizeof(s_imu.interrupt_levels));
    memset(s_imu.status_int_values, 0, sizeof(s_imu.status_int_values));
    s_imu.available = true;
    s_imu.reads_blocked = false;
    (void)pthread_mutex_unlock(&s_imu.lock);
}

const imu_service_imu_ops_t *host_imu_ops(void)
{
    return &s_ops;
}

void host_imu_set_available(bool available)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    s_imu.available = available;
    (void)pthread_mutex_unlock(&s_imu.lock);
}

void host_imu_set_read_result(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    s_imu.read_result = result;
    (void)pthread_mutex_unlock(&s_imu.lock);
}

void host_imu_set_configure_result(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    s_imu.configure_result = result;
    (void)pthread_mutex_unlock(&s_imu.lock);
}

void host_imu_set_sample(const imu_service_sample_t *sample)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    s_imu.sample = *sample;
    (void)pthread_mutex_unlock(&s_imu.lock);
}

void host_imu_set_poll_result(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    s_imu.poll_result = result;
    (void)pthread_mutex_unlock(&s_imu.lock);
}

void host_imu_set_interrupt_levels(const bool *levels, size_t count)
{
    if (count > HOST_IMU_INTERRUPT_LEVEL_CAPACITY)
    {
        count = HOST_IMU_INTERRUPT_LEVEL_CAPACITY;
    }
    (void)pthread_mutex_lock(&s_imu.lock);
    s_imu.interrupt_count = count;
    s_imu.interrupt_index = 0U;
    if (count != 0U)
    {
        memcpy(s_imu.interrupt_levels, levels, count * sizeof(*levels));
    }
    (void)pthread_mutex_unlock(&s_imu.lock);
}

void host_imu_set_status_int_values(const uint8_t *values, size_t count)
{
    if (count > HOST_IMU_STATUS_INT_CAPACITY)
    {
        count = HOST_IMU_STATUS_INT_CAPACITY;
    }
    (void)pthread_mutex_lock(&s_imu.lock);
    s_imu.status_int_count = count;
    s_imu.status_int_index = 0U;
    if (count != 0U)
    {
        memcpy(s_imu.status_int_values, values, count * sizeof(*values));
    }
    (void)pthread_mutex_unlock(&s_imu.lock);
}

void host_imu_set_enable_result(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    s_imu.enable_result = result;
    (void)pthread_mutex_unlock(&s_imu.lock);
}

void host_imu_set_disable_result(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    s_imu.disable_result = result;
    (void)pthread_mutex_unlock(&s_imu.lock);
}

void host_imu_block_reads(bool blocked)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    s_imu.reads_blocked = blocked;
    if (!blocked)
    {
        (void)pthread_cond_broadcast(&s_imu.changed);
    }
    (void)pthread_mutex_unlock(&s_imu.lock);
}

bool host_imu_wait_for_blocked_read(uint32_t timeout_ms)
{
    const struct timespec deadline = _deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_imu.lock);
    int wait_result = 0;
    while (s_imu.blocked_read_count == 0U && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_imu.changed, &s_imu.lock,
                                             &deadline);
    }
    const bool blocked = s_imu.blocked_read_count != 0U;
    (void)pthread_mutex_unlock(&s_imu.lock);
    return blocked;
}

bool host_imu_wait_for_reads(uint32_t count, uint32_t timeout_ms)
{
    const struct timespec deadline = _deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_imu.lock);
    int wait_result = 0;
    while (s_imu.read_count < count && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_imu.changed, &s_imu.lock,
                                             &deadline);
    }
    const bool reached = s_imu.read_count >= count;
    (void)pthread_mutex_unlock(&s_imu.lock);
    return reached;
}

uint32_t host_imu_read_count(void)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    const uint32_t count = s_imu.read_count;
    (void)pthread_mutex_unlock(&s_imu.lock);
    return count;
}

uint32_t host_imu_configure_count(void)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    const uint32_t count = s_imu.configure_count;
    (void)pthread_mutex_unlock(&s_imu.lock);
    return count;
}

uint32_t host_imu_configured_sample_rate_hz(void)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    const uint32_t sample_rate_hz = s_imu.configured_sample_rate_hz;
    (void)pthread_mutex_unlock(&s_imu.lock);
    return sample_rate_hz;
}

uint32_t host_imu_enable_count(void)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    const uint32_t count = s_imu.enable_count;
    (void)pthread_mutex_unlock(&s_imu.lock);
    return count;
}

uint32_t host_imu_disable_count(void)
{
    (void)pthread_mutex_lock(&s_imu.lock);
    const uint32_t count = s_imu.disable_count;
    (void)pthread_mutex_unlock(&s_imu.lock);
    return count;
}
