#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host_freertos.h"

#include <errno.h>
#include <sched.h>
#include <time.h>

static pthread_mutex_t s_take_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_take_gate_changed = PTHREAD_COND_INITIALIZER;
static bool s_block_next_take;
static bool s_blocked_take_entered;
static bool s_blocked_take_released;
static bool s_block_after_next_take;
static bool s_blocked_after_take_entered;
static bool s_blocked_after_take_released;
static unsigned s_pending_take_count;

static struct timespec _host_freertos_deadline(uint32_t timeout_ms)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

void host_freertos_block_next_semaphore_take(void)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    s_block_next_take = true;
    s_blocked_take_entered = false;
    s_blocked_take_released = false;
    (void)pthread_mutex_unlock(&s_take_gate_lock);
}

bool host_freertos_wait_for_blocked_semaphore_take(uint32_t timeout_ms)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    const struct timespec deadline = _host_freertos_deadline(timeout_ms);
    int wait_result = 0;
    while (!s_blocked_take_entered && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_take_gate_changed,
                                             &s_take_gate_lock, &deadline);
    }
    bool entered = s_blocked_take_entered;
    (void)pthread_mutex_unlock(&s_take_gate_lock);
    return entered;
}

void host_freertos_release_blocked_semaphore_take(void)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    s_blocked_take_released = true;
    (void)pthread_cond_broadcast(&s_take_gate_changed);
    (void)pthread_mutex_unlock(&s_take_gate_lock);
}

void host_freertos_block_after_next_semaphore_take(void)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    s_block_after_next_take = true;
    s_blocked_after_take_entered = false;
    s_blocked_after_take_released = false;
    (void)pthread_mutex_unlock(&s_take_gate_lock);
}

bool host_freertos_wait_for_blocked_after_semaphore_take(uint32_t timeout_ms)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    const struct timespec deadline = _host_freertos_deadline(timeout_ms);
    int wait_result = 0;
    while (!s_blocked_after_take_entered && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_take_gate_changed,
                                             &s_take_gate_lock, &deadline);
    }
    const bool entered = s_blocked_after_take_entered;
    (void)pthread_mutex_unlock(&s_take_gate_lock);
    return entered;
}

void host_freertos_release_blocked_after_semaphore_take(void)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    s_blocked_after_take_released = true;
    (void)pthread_cond_broadcast(&s_take_gate_changed);
    (void)pthread_mutex_unlock(&s_take_gate_lock);
}

bool host_freertos_wait_for_pending_semaphore_take(uint32_t timeout_ms)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    const struct timespec deadline = _host_freertos_deadline(timeout_ms);
    int wait_result = 0;
    while (s_pending_take_count == 0U && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_take_gate_changed,
                                             &s_take_gate_lock, &deadline);
    }
    const bool pending = s_pending_take_count > 0U;
    (void)pthread_mutex_unlock(&s_take_gate_lock);
    return pending;
}

static void _host_freertos_apply_take_gate(void)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    if (s_block_next_take)
    {
        s_block_next_take = false;
        s_blocked_take_entered = true;
        (void)pthread_cond_broadcast(&s_take_gate_changed);
        while (!s_blocked_take_released)
        {
            (void)pthread_cond_wait(&s_take_gate_changed, &s_take_gate_lock);
        }
        s_blocked_take_entered = false;
        s_blocked_take_released = false;
    }
    (void)pthread_mutex_unlock(&s_take_gate_lock);
}

static void _host_freertos_apply_after_take_gate(void)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    if (s_block_after_next_take)
    {
        s_block_after_next_take = false;
        s_blocked_after_take_entered = true;
        (void)pthread_cond_broadcast(&s_take_gate_changed);
        while (!s_blocked_after_take_released)
        {
            (void)pthread_cond_wait(&s_take_gate_changed, &s_take_gate_lock);
        }
        s_blocked_after_take_entered = false;
        s_blocked_after_take_released = false;
    }
    (void)pthread_mutex_unlock(&s_take_gate_lock);
}

static void _host_freertos_pending_take_add(void)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    ++s_pending_take_count;
    (void)pthread_cond_broadcast(&s_take_gate_changed);
    (void)pthread_mutex_unlock(&s_take_gate_lock);
}

static void _host_freertos_pending_take_remove(void)
{
    (void)pthread_mutex_lock(&s_take_gate_lock);
    --s_pending_take_count;
    (void)pthread_cond_broadcast(&s_take_gate_changed);
    (void)pthread_mutex_unlock(&s_take_gate_lock);
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    SemaphoreHandle_t semaphore = NULL;
    if (storage != NULL && !storage->initialized &&
            pthread_mutex_init(&storage->mutex, NULL) == 0)
    {
        storage->initialized = 1;
        semaphore = storage;
    }
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t timeout_ticks)
{
    (void)timeout_ticks;
    _host_freertos_apply_take_gate();
    if (semaphore == NULL || !semaphore->initialized)
    {
        return pdFALSE;
    }

    _host_freertos_pending_take_add();
    const int lock_result = pthread_mutex_lock(&semaphore->mutex);
    _host_freertos_pending_take_remove();
    if (lock_result != 0)
    {
        return pdFALSE;
    }
    _host_freertos_apply_after_take_gate();
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return semaphore != NULL && semaphore->initialized &&
           pthread_mutex_unlock(&semaphore->mutex) == 0 ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (semaphore != NULL && semaphore->initialized)
    {
        (void)pthread_mutex_destroy(&semaphore->mutex);
        semaphore->initialized = 0;
    }
}

void vTaskDelay(TickType_t ticks)
{
    if (ticks == 0)
    {
        (void)sched_yield();
    }
    else
    {
        const struct timespec delay =
        {
            .tv_sec = (time_t)(ticks / 1000U),
            .tv_nsec = (long)(ticks % 1000U) * 1000000L,
        };
        (void)nanosleep(&delay, NULL);
    }
}
