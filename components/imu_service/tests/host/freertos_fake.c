#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host_freertos.h"

#include <errno.h>
#include <pthread.h>
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

typedef struct host_runtime
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint32_t mutex_create_attempt;
    uint32_t fail_mutex_create_attempt;
    uint32_t notification_count;
    uint32_t active_mutexes;
    uint32_t active_event_groups;
    uint32_t active_tasks;
    bool fail_event_group_create;
    bool fail_task_create;
    bool notifications_blocked;
} host_runtime_t;

static _Thread_local TaskHandle_t s_current_task;
static struct host_task s_external_task;
static host_runtime_t s_runtime =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
};

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

static void _wait_for_notification_delivery(void)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    while (s_runtime.notifications_blocked)
    {
        (void)pthread_cond_wait(&s_runtime.changed, &s_runtime.lock);
    }
    (void)pthread_mutex_unlock(&s_runtime.lock);
}

static bool _consume_event_group_failure(void)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    const bool fail = s_runtime.fail_event_group_create;
    s_runtime.fail_event_group_create = false;
    (void)pthread_mutex_unlock(&s_runtime.lock);
    return fail;
}

static bool _consume_task_failure(void)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    const bool fail = s_runtime.fail_task_create;
    s_runtime.fail_task_create = false;
    (void)pthread_mutex_unlock(&s_runtime.lock);
    return fail;
}

static void _active_task_add(void)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    ++s_runtime.active_tasks;
    (void)pthread_mutex_unlock(&s_runtime.lock);
}

static void _active_task_remove(void)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    --s_runtime.active_tasks;
    (void)pthread_cond_broadcast(&s_runtime.changed);
    (void)pthread_mutex_unlock(&s_runtime.lock);
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
    _active_task_remove();
    return NULL;
}

BaseType_t xTaskCreate(void (*entry)(void *), const char *name,
                       uint32_t stack_depth, void *context,
                       UBaseType_t priority, TaskHandle_t *out_task)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    if (entry == NULL || out_task == NULL || _consume_task_failure())
    {
        return pdFAIL;
    }

    BaseType_t result = pdFAIL;
    TaskHandle_t task = calloc(1, sizeof(*task));
    bool lock_ready = false;
    bool changed_ready = false;
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
    _active_task_add();
    if (pthread_create(&task->thread, NULL, _task_trampoline, task) != 0)
    {
        _active_task_remove();
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

    (void)pthread_mutex_lock(&s_runtime.lock);
    ++s_runtime.notification_count;
    (void)pthread_mutex_unlock(&s_runtime.lock);
    return pdPASS;
}

BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                           uint32_t *value, TickType_t timeout_ticks)
{
    TaskHandle_t task = s_current_task;
    if (task == NULL)
    {
        return pdFALSE;
    }

    BaseType_t result = pdFALSE;
    (void)pthread_mutex_lock(&task->lock);
    task->notification &= ~clear_on_entry;
    int wait_result = 0;
    struct timespec deadline = {0};
    if (timeout_ticks != portMAX_DELAY)
    {
        deadline = _deadline_after_ticks(timeout_ticks);
    }
    while (task->notification == 0U && wait_result != ETIMEDOUT)
    {
        if (timeout_ticks == 0U)
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
    if (task->notification != 0U)
    {
        (void)pthread_mutex_unlock(&task->lock);
        _wait_for_notification_delivery();
        (void)pthread_mutex_lock(&task->lock);
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
    return s_current_task != NULL ? s_current_task : &s_external_task;
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
    (void)pthread_mutex_lock(&s_runtime.lock);
    ++s_runtime.mutex_create_attempt;
    const bool fail = s_runtime.fail_mutex_create_attempt != 0U &&
                      s_runtime.mutex_create_attempt ==
                      s_runtime.fail_mutex_create_attempt;
    (void)pthread_mutex_unlock(&s_runtime.lock);
    if (fail)
    {
        return NULL;
    }

    SemaphoreHandle_t semaphore = malloc(sizeof(*semaphore));
    if (semaphore != NULL && pthread_mutex_init(&semaphore->lock, NULL) != 0)
    {
        free(semaphore);
        semaphore = NULL;
    }
    if (semaphore != NULL)
    {
        (void)pthread_mutex_lock(&s_runtime.lock);
        ++s_runtime.active_mutexes;
        (void)pthread_mutex_unlock(&s_runtime.lock);
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
    if (semaphore == NULL)
    {
        return;
    }
    (void)pthread_mutex_destroy(&semaphore->lock);
    free(semaphore);
    (void)pthread_mutex_lock(&s_runtime.lock);
    --s_runtime.active_mutexes;
    (void)pthread_mutex_unlock(&s_runtime.lock);
}

EventGroupHandle_t xEventGroupCreate(void)
{
    if (_consume_event_group_failure())
    {
        return NULL;
    }
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
        return NULL;
    }
    (void)pthread_mutex_lock(&s_runtime.lock);
    ++s_runtime.active_event_groups;
    (void)pthread_mutex_unlock(&s_runtime.lock);
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
                matching != 0U;
        if (ready || timeout_ticks == 0U)
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
    if (group == NULL)
    {
        return;
    }
    (void)pthread_mutex_destroy(&group->lock);
    (void)pthread_cond_destroy(&group->changed);
    free(group);
    (void)pthread_mutex_lock(&s_runtime.lock);
    --s_runtime.active_event_groups;
    (void)pthread_mutex_unlock(&s_runtime.lock);
}

bool host_freertos_reset(void)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    const bool idle = s_runtime.active_mutexes == 0U &&
                      s_runtime.active_event_groups == 0U &&
                      s_runtime.active_tasks == 0U;
    if (idle)
    {
        s_runtime.mutex_create_attempt = 0U;
        s_runtime.fail_mutex_create_attempt = 0U;
        s_runtime.notification_count = 0U;
        s_runtime.fail_event_group_create = false;
        s_runtime.fail_task_create = false;
        s_runtime.notifications_blocked = false;
        (void)pthread_cond_broadcast(&s_runtime.changed);
    }
    (void)pthread_mutex_unlock(&s_runtime.lock);
    return idle;
}

void host_freertos_fail_mutex_create_on(uint32_t attempt)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    s_runtime.fail_mutex_create_attempt = attempt;
    (void)pthread_mutex_unlock(&s_runtime.lock);
}

void host_freertos_fail_event_group_create(bool fail)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    s_runtime.fail_event_group_create = fail;
    (void)pthread_mutex_unlock(&s_runtime.lock);
}

void host_freertos_fail_task_create(bool fail)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    s_runtime.fail_task_create = fail;
    (void)pthread_mutex_unlock(&s_runtime.lock);
}

void host_freertos_block_notifications(bool blocked)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    s_runtime.notifications_blocked = blocked;
    if (!blocked)
    {
        (void)pthread_cond_broadcast(&s_runtime.changed);
    }
    (void)pthread_mutex_unlock(&s_runtime.lock);
}

uint32_t host_freertos_notification_count(void)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    const uint32_t count = s_runtime.notification_count;
    (void)pthread_mutex_unlock(&s_runtime.lock);
    return count;
}

uint32_t host_freertos_active_mutex_count(void)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    const uint32_t count = s_runtime.active_mutexes;
    (void)pthread_mutex_unlock(&s_runtime.lock);
    return count;
}

uint32_t host_freertos_active_event_group_count(void)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    const uint32_t count = s_runtime.active_event_groups;
    (void)pthread_mutex_unlock(&s_runtime.lock);
    return count;
}

uint32_t host_freertos_active_task_count(void)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    const uint32_t count = s_runtime.active_tasks;
    (void)pthread_mutex_unlock(&s_runtime.lock);
    return count;
}

bool host_freertos_wait_for_tasks(uint32_t timeout_ms)
{
    (void)pthread_mutex_lock(&s_runtime.lock);
    const struct timespec deadline = _deadline_after_ticks(timeout_ms);
    int wait_result = 0;
    while (s_runtime.active_tasks != 0U && wait_result != ETIMEDOUT)
    {
        wait_result = pthread_cond_timedwait(&s_runtime.changed,
                                             &s_runtime.lock, &deadline);
    }
    const bool stopped = s_runtime.active_tasks == 0U;
    (void)pthread_mutex_unlock(&s_runtime.lock);
    return stopped;
}
