#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host_freertos.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

struct host_task
{
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint32_t notification;
    void (*entry)(void *);
    void *context;
};

struct host_semaphore
{
    pthread_mutex_t lock;
};

struct host_event_group
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    EventBits_t bits;
};

static _Thread_local TaskHandle_t s_current_task;
static atomic_uint s_notification_count;
static pthread_mutex_t s_task_count_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_task_count_changed = PTHREAD_COND_INITIALIZER;
static unsigned s_task_count;

static struct timespec _deadline_after_ticks(TickType_t ticks)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    const uint64_t nanoseconds =
        ((uint64_t)ticks * UINT64_C(1000000000)) / configTICK_RATE_HZ;
    deadline.tv_sec += (time_t)(nanoseconds / UINT64_C(1000000000));
    deadline.tv_nsec += (long)(nanoseconds % UINT64_C(1000000000));
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static void _task_count_add(void)
{
    (void)pthread_mutex_lock(&s_task_count_lock);
    ++s_task_count;
    (void)pthread_mutex_unlock(&s_task_count_lock);
}

static void _task_count_remove(void)
{
    (void)pthread_mutex_lock(&s_task_count_lock);
    --s_task_count;
    (void)pthread_cond_broadcast(&s_task_count_changed);
    (void)pthread_mutex_unlock(&s_task_count_lock);
}

static void *_task_trampoline(void *context)
{
    TaskHandle_t task = context;
    s_current_task = task;
    task->entry(task->context);
    s_current_task = NULL;
    (void)pthread_mutex_destroy(&task->lock);
    (void)pthread_cond_destroy(&task->changed);
    free(task);
    _task_count_remove();
    return NULL;
}

BaseType_t xTaskCreate(void (*entry)(void *), const char *name,
                       uint32_t stack_depth, void *context,
                       UBaseType_t priority, TaskHandle_t *out_task)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    BaseType_t result = pdFAIL;
    TaskHandle_t task = NULL;
    bool lock_ready = false;
    bool changed_ready = false;
    if (entry == NULL || out_task == NULL)
    {
        return pdFAIL;
    }

    task = calloc(1, sizeof(*task));
    if (task == NULL)
    {
        return pdFAIL;
    }
    if (pthread_mutex_init(&task->lock, NULL) != 0)
    {
        goto exit;
    }
    lock_ready = true;
    if (pthread_cond_init(&task->changed, NULL) != 0)
    {
        goto exit;
    }
    changed_ready = true;
    task->entry = entry;
    task->context = context;
    _task_count_add();
    if (pthread_create(&task->thread, NULL, _task_trampoline, task) != 0)
    {
        _task_count_remove();
        goto exit;
    }
    (void)pthread_detach(task->thread);
    *out_task = task;
    task = NULL;
    result = pdPASS;

exit:
    if (task != NULL)
    {
        if (changed_ready)
        {
            (void)pthread_cond_destroy(&task->changed);
        }
        if (lock_ready)
        {
            (void)pthread_mutex_destroy(&task->lock);
        }
        free(task);
    }
    return result;
}

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value,
                       eNotifyAction action)
{
    if (task == NULL || action != eSetBits)
    {
        return pdFAIL;
    }
    (void)pthread_mutex_lock(&task->lock);
    task->notification |= value;
    (void)pthread_cond_signal(&task->changed);
    (void)pthread_mutex_unlock(&task->lock);
    atomic_fetch_add(&s_notification_count, 1U);
    return pdPASS;
}

BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                           uint32_t *value, TickType_t timeout_ticks)
{
    BaseType_t result = pdFALSE;
    TaskHandle_t task = s_current_task;
    if (task == NULL)
    {
        return pdFALSE;
    }

    (void)pthread_mutex_lock(&task->lock);
    task->notification &= ~clear_on_entry;
    int wait_result = 0;
    struct timespec deadline = {0};
    if (timeout_ticks != portMAX_DELAY)
    {
        deadline = _deadline_after_ticks(timeout_ticks);
    }
    while (task->notification == 0 && wait_result != ETIMEDOUT)
    {
        if (timeout_ticks == 0)
        {
            wait_result = ETIMEDOUT;
        }
        else if (timeout_ticks == portMAX_DELAY)
        {
            wait_result = pthread_cond_wait(&task->changed, &task->lock);
        }
        else
        {
            wait_result = pthread_cond_timedwait(&task->changed, &task->lock,
                                                 &deadline);
        }
    }
    if (task->notification != 0)
    {
        if (value != NULL)
        {
            *value = task->notification;
        }
        task->notification &= ~clear_on_exit;
        result = pdTRUE;
    }
    (void)pthread_mutex_unlock(&task->lock);
    return result;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return s_current_task;
}

void vTaskDelay(TickType_t ticks)
{
    const uint64_t nanoseconds =
        ((uint64_t)ticks * UINT64_C(1000000000)) / configTICK_RATE_HZ;
    struct timespec delay =
    {
        .tv_sec = (time_t)(nanoseconds / UINT64_C(1000000000)),
        .tv_nsec = (long)(nanoseconds % UINT64_C(1000000000)),
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
    {
    }
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t semaphore = malloc(sizeof(*semaphore));
    if (semaphore != NULL && pthread_mutex_init(&semaphore->lock, NULL) != 0)
    {
        free(semaphore);
        semaphore = NULL;
    }
    return semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t timeout_ticks)
{
    (void)timeout_ticks;
    return semaphore != NULL && pthread_mutex_lock(&semaphore->lock) == 0 ?
           pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return semaphore != NULL && pthread_mutex_unlock(&semaphore->lock) == 0 ?
           pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (semaphore != NULL)
    {
        (void)pthread_mutex_destroy(&semaphore->lock);
        free(semaphore);
    }
}

EventGroupHandle_t xEventGroupCreate(void)
{
    EventGroupHandle_t group = calloc(1, sizeof(*group));
    if (group == NULL)
    {
        return NULL;
    }
    if (pthread_mutex_init(&group->lock, NULL) != 0)
    {
        free(group);
        return NULL;
    }
    if (pthread_cond_init(&group->changed, NULL) != 0)
    {
        (void)pthread_mutex_destroy(&group->lock);
        free(group);
        group = NULL;
    }
    return group;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    (void)pthread_mutex_lock(&group->lock);
    group->bits |= bits;
    const EventBits_t result = group->bits;
    (void)pthread_cond_broadcast(&group->changed);
    (void)pthread_mutex_unlock(&group->lock);
    return result;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    (void)pthread_mutex_lock(&group->lock);
    const EventBits_t result = group->bits;
    group->bits &= ~bits;
    (void)pthread_mutex_unlock(&group->lock);
    return result;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group,
                                EventBits_t bits_to_wait_for,
                                BaseType_t clear_on_exit,
                                BaseType_t wait_for_all,
                                TickType_t timeout_ticks)
{
    (void)pthread_mutex_lock(&group->lock);
    int wait_result = 0;
    struct timespec deadline = {0};
    if (timeout_ticks != portMAX_DELAY)
    {
        deadline = _deadline_after_ticks(timeout_ticks);
    }
    bool ready = false;
    while (!ready && wait_result != ETIMEDOUT)
    {
        const EventBits_t matching = group->bits & bits_to_wait_for;
        ready = wait_for_all == pdTRUE ? matching == bits_to_wait_for :
                matching != 0;
        if (ready || timeout_ticks == 0)
        {
            break;
        }
        if (timeout_ticks == portMAX_DELAY)
        {
            wait_result = pthread_cond_wait(&group->changed, &group->lock);
        }
        else
        {
            wait_result = pthread_cond_timedwait(&group->changed, &group->lock,
                                                 &deadline);
        }
    }
    const EventBits_t result = group->bits;
    if (ready && clear_on_exit == pdTRUE)
    {
        group->bits &= ~bits_to_wait_for;
    }
    (void)pthread_mutex_unlock(&group->lock);
    return result;
}

void vEventGroupDelete(EventGroupHandle_t group)
{
    if (group != NULL)
    {
        (void)pthread_mutex_destroy(&group->lock);
        (void)pthread_cond_destroy(&group->changed);
        free(group);
    }
}

uint32_t host_freertos_notification_count(void)
{
    return atomic_load(&s_notification_count);
}

bool host_freertos_wait_for_tasks(uint32_t timeout_ms)
{
    (void)pthread_mutex_lock(&s_task_count_lock);
    const struct timespec deadline = _deadline_after_ticks(timeout_ms);
    int wait_result = 0;
    while (s_task_count != 0 && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_task_count_changed,
                                             &s_task_count_lock, &deadline);
    }
    const bool stopped = s_task_count == 0;
    (void)pthread_mutex_unlock(&s_task_count_lock);
    return stopped;
}
