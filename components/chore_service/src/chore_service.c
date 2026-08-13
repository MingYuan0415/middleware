#define DBG_TAG "chore_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "chore_service.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CHORE_CMD_WAKE    (UINT32_C(1) << 0)
#define CHORE_CMD_SUSPEND (UINT32_C(1) << 1)
#define CHORE_CMD_RESUME  (UINT32_C(1) << 2)
#define CHORE_CMD_STOP    (UINT32_C(1) << 3)

#define CHORE_SLOT_BIT(slot) \
    (UINT32_C(1) << (slot))
#define CHORE_EVENT_PAUSED_BIT \
    (UINT32_C(1) << CONFIG_CHORE_SERVICE_JOB_CAPACITY)
#define CHORE_EVENT_STOPPED_BIT \
    (UINT32_C(1) << (CONFIG_CHORE_SERVICE_JOB_CAPACITY + 1U))
#define CHORE_EVENT_RUNNING_BIT \
    (UINT32_C(1) << (CONFIG_CHORE_SERVICE_JOB_CAPACITY + 2U))

#define CHORE_WAIT_FOREVER_MS      (-1)
#define CHORE_WAIT_CEILING_MS      (60 * 1000)
#define CHORE_STACK_WARNING_BYTES  1024U

_Static_assert(CONFIG_CHORE_SERVICE_JOB_CAPACITY > 0U,
               "chore job capacity must be positive");
_Static_assert(CONFIG_CHORE_SERVICE_JOB_CAPACITY + 3U <= 24U,
               "chore job capacity exceeds FreeRTOS event-group bits");

typedef enum
{
    CHORE_STATE_STOPPED = 0,
    CHORE_STATE_RUNNING,
    CHORE_STATE_PAUSE_PENDING,
    CHORE_STATE_PAUSED,
    CHORE_STATE_RESUME_PENDING,
} chore_service_state_t;

typedef struct chore_slot
{
    void (*run)(const chore_service_cancel_token_t *cancel, void *arg);
    void (*release)(void *arg);
    void *arg;
    uint32_t period_ms;
    int64_t next_due_ms;
    uint32_t generation;
    atomic_bool cancel_requested;
    bool queued;
    bool running;
    bool releasing;
} chore_slot_t;

typedef struct chore_service_context
{
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t lifecycle_mutex;
    EventGroupHandle_t events;
    TaskHandle_t worker;
    chore_slot_t slots[CONFIG_CHORE_SERVICE_JOB_CAPACITY];
    uint32_t task_priority;
    uint32_t warning_duration_ms;
    atomic_uint minimum_stack_remaining;
    uint64_t completed_count;
    uint64_t cancelled_count;
    atomic_bool worker_tail_complete;
    chore_service_state_t state;
    atomic_bool initialized;
    bool stopping;
    int64_t last_duration_warning_ms;
} chore_service_context_t;

static chore_service_context_t s_chore;

/* Process-lifetime high-water mark of every slot generation ever issued
   (seeded at init and advanced at each release finalize). New instances
   seed strictly above it, so a handle from a previous instance can never
   match a job of a new instance; the 32-bit wrap after 2^32 total
   releases is the documented bound. Written by the worker under the
   service mutex and by init, which is caller-serialized with deinit. */
static uint32_t s_chore_generation_high_water;

/* The admission gate is a process-lifetime atomic that is never memset or
   reinitialized: deinit closes it with the closing bit and drains in-flight
   calls, while a successful init reopens it under a fresh epoch. The open
   preserves the in-flight count, so every admitted call balances its own
   increment exactly once and the count always equals the true number of
   in-flight calls; the deinit drain therefore always terminates. */
#define CHORE_API_CLOSING     (UINT32_C(1) << 31)
#define CHORE_API_EPOCH_SHIFT 16U
#define CHORE_API_EPOCH_VALUE (UINT32_C(0x7FFF))
#define CHORE_API_EPOCH_MASK  (CHORE_API_EPOCH_VALUE << CHORE_API_EPOCH_SHIFT)
#define CHORE_API_COUNT_MASK  (UINT32_C(0xFFFF))

static atomic_uint s_chore_admission = ATOMIC_VAR_INIT(0U);

static void _chore_api_leave(void)
{
    atomic_fetch_sub_explicit(&s_chore_admission, 1U, memory_order_acq_rel);
}

static esp_err_t _chore_api_enter(void)
{
    for (;;)
    {
        const uint32_t value = atomic_load_explicit(&s_chore_admission,
                               memory_order_relaxed);
        if ((value & CHORE_API_CLOSING) != 0U)
        {
            return ESP_ERR_INVALID_STATE;
        }
        if (atomic_compare_exchange_weak_explicit(
                    &s_chore_admission, &value, value + 1U,
                    memory_order_acq_rel, memory_order_relaxed))
        {
            /* The epoch is bumped only by init's _chore_admission_open,
               which preserves the count. A mismatch here means this call
               was admitted in a previous instance: balance the increment
               and refuse. The post-CAS read may be satisfied by this
               call's own CAS write on weakly-ordered hardware, in which
               case the epochs match and the call proceeds against the new
               instance — safe, because admission reopens only after the
               new context is complete. */
            const uint32_t current = atomic_load_explicit(
                                         &s_chore_admission,
                                         memory_order_acquire);
            if ((current & CHORE_API_EPOCH_MASK) ==
                    (value & CHORE_API_EPOCH_MASK))
            {
                return ESP_OK;
            }
            _chore_api_leave();
            return ESP_ERR_INVALID_STATE;
        }
    }
}

static void _chore_admission_close(void)
{
    uint32_t value = atomic_load_explicit(&s_chore_admission,
                                          memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(
                &s_chore_admission, &value, value | CHORE_API_CLOSING,
                memory_order_acq_rel, memory_order_relaxed))
    {
    }
}

static void _chore_admission_open(void)
{
    for (;;)
    {
        const uint32_t value = atomic_load_explicit(&s_chore_admission,
                               memory_order_relaxed);
        const uint32_t epoch = (value >> CHORE_API_EPOCH_SHIFT) &
                               CHORE_API_EPOCH_VALUE;
        const uint32_t next = (value & CHORE_API_COUNT_MASK) |
                              (((epoch + 1U) & CHORE_API_EPOCH_VALUE)
                               << CHORE_API_EPOCH_SHIFT);
        if (atomic_compare_exchange_weak_explicit(
                    &s_chore_admission, &value, next,
                    memory_order_acq_rel, memory_order_relaxed))
        {
            return;
        }
    }
}

static void _chore_wait_api_drained(void)
{
    while ((atomic_load_explicit(&s_chore_admission, memory_order_acquire) &
            CHORE_API_COUNT_MASK) != 0U)
    {
        vTaskDelay(1);
    }
}

static TickType_t _chore_timeout_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == CHORE_SERVICE_WAIT_FOREVER)
    {
        return portMAX_DELAY;
    }
    uint64_t ticks = ((uint64_t)timeout_ms * configTICK_RATE_HZ + 999U) /
                     1000U;
    if (timeout_ms > 0U && ticks == 0U)
    {
        ticks = 1U;
    }
    if (ticks >= portMAX_DELAY)
    {
        ticks = portMAX_DELAY - 1U;
    }
    return (TickType_t)ticks;
}

static int64_t _chore_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void _chore_record_stack(void)
{
#if defined(ESP_PLATFORM)
    const uint32_t remaining = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    uint32_t minimum = atomic_load_explicit(&s_chore.minimum_stack_remaining,
                                            memory_order_relaxed);
    while (remaining < minimum)
    {
        if (atomic_compare_exchange_weak_explicit(
                    &s_chore.minimum_stack_remaining, &minimum, remaining,
                    memory_order_relaxed, memory_order_relaxed))
        {
            if (remaining < CHORE_STACK_WARNING_BYTES)
            {
                LOG_W("chore worker stack low: %u bytes remaining",
                      (unsigned)remaining);
            }
            break;
        }
    }
#endif
}

static int64_t _chore_wait_ms_locked(int64_t now_ms)
{
    bool cancel_pending = false;
    int64_t earliest_due = INT64_MAX;
    for (uint32_t slot = 0; slot < CONFIG_CHORE_SERVICE_JOB_CAPACITY; ++slot)
    {
        chore_slot_t *current = &s_chore.slots[slot];
        if (!current->queued || current->releasing)
        {
            continue;
        }
        if (atomic_load_explicit(&current->cancel_requested,
                                 memory_order_acquire))
        {
            cancel_pending = true;
        }
        else if (current->next_due_ms < earliest_due)
        {
            earliest_due = current->next_due_ms;
        }
    }
    if (cancel_pending)
    {
        return 0;
    }
    if (earliest_due == INT64_MAX)
    {
        return CHORE_WAIT_FOREVER_MS;
    }
    int64_t wait_ms = earliest_due - now_ms;
    if (wait_ms <= 0)
    {
        return 1;
    }
    return wait_ms > CHORE_WAIT_CEILING_MS ? CHORE_WAIT_CEILING_MS : wait_ms;
}

static chore_slot_t *_chore_pick_locked(int64_t now_ms)
{
    for (uint32_t slot = 0; slot < CONFIG_CHORE_SERVICE_JOB_CAPACITY; ++slot)
    {
        chore_slot_t *current = &s_chore.slots[slot];
        if (current->queued && !current->releasing &&
                atomic_load_explicit(&current->cancel_requested,
                                     memory_order_acquire))
        {
            return current;
        }
    }
    chore_slot_t *earliest = NULL;
    for (uint32_t slot = 0; slot < CONFIG_CHORE_SERVICE_JOB_CAPACITY; ++slot)
    {
        chore_slot_t *current = &s_chore.slots[slot];
        if (!current->queued || current->releasing ||
                atomic_load_explicit(&current->cancel_requested,
                                     memory_order_acquire) ||
                current->next_due_ms > now_ms)
        {
            continue;
        }
        if (earliest == NULL || current->next_due_ms < earliest->next_due_ms)
        {
            earliest = current;
        }
    }
    return earliest;
}

/* Under the mutex: mark a finished or cancelled slot as releasing and
   snapshot its release callback. The generation and the acknowledgement
   bit stay untouched until the release callback has run, so cancelers
   keep waiting for quiescence until the argument is actually released. */
static void _chore_begin_release_locked(chore_slot_t *slot,
                                        void (**release_out)(void *),
                                        void **arg_out)
{
    slot->running = false;
    slot->releasing = true;
    *release_out = slot->release;
    *arg_out = slot->arg;
}

/* Under the mutex: make the slot reusable and acknowledge waiters. */
static void _chore_finalize_release_locked(chore_slot_t *slot,
        uint32_t slot_index)
{
    slot->releasing = false;
    ++slot->generation;
    if (slot->generation == 0U)
    {
        slot->generation = 1U;
    }
    if (slot->generation > s_chore_generation_high_water)
    {
        s_chore_generation_high_water = slot->generation;
    }
    slot->queued = false;
    slot->run = NULL;
    slot->release = NULL;
    slot->arg = NULL;
    slot->period_ms = 0U;
    slot->next_due_ms = 0;
    atomic_store_explicit(&slot->cancel_requested, false,
                          memory_order_release);
    xEventGroupSetBits(s_chore.events, CHORE_SLOT_BIT(slot_index));
}

static void _chore_dispatch_job(chore_slot_t *slot, uint32_t slot_index)
{
    const chore_service_cancel_token_t token =
    {
        .requested = &slot->cancel_requested,
    };
    const int64_t started_ms = _chore_now_ms();
    slot->run(&token, slot->arg);
    const int64_t finished_ms = _chore_now_ms();
    _chore_record_stack();
    const int64_t duration_ms = finished_ms - started_ms;
    if (duration_ms >= (int64_t)s_chore.warning_duration_ms)
    {
        const int64_t now_ms = _chore_now_ms();
        if (s_chore.last_duration_warning_ms == 0 ||
                now_ms - s_chore.last_duration_warning_ms >= 60000)
        {
            s_chore.last_duration_warning_ms = now_ms;
            LOG_W("job slot=%u took %lld ms", (unsigned)slot_index,
                  (long long)duration_ms);
        }
    }
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    const bool cancelled = atomic_load_explicit(&slot->cancel_requested,
                           memory_order_acquire);
    if (cancelled)
    {
        ++s_chore.cancelled_count;
    }
    else
    {
        ++s_chore.completed_count;
    }
    if (cancelled || slot->period_ms == 0U)
    {
        void (*release)(void *) = NULL;
        void *arg = NULL;
        _chore_begin_release_locked(slot, &release, &arg);
        xSemaphoreGive(s_chore.mutex);
        if (release != NULL)
        {
            release(arg);
        }
        xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
        _chore_finalize_release_locked(slot, slot_index);
        xSemaphoreGive(s_chore.mutex);
        return;
    }
    slot->running = false;
    slot->next_due_ms = _chore_now_ms() + (int64_t)slot->period_ms;
    xSemaphoreGive(s_chore.mutex);
}

static void _chore_run_due_jobs(void)
{
    for (;;)
    {
        const int64_t now_ms = _chore_now_ms();
        xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
        const bool stopping = s_chore.stopping;
        if (!stopping && s_chore.state != CHORE_STATE_RUNNING)
        {
            xSemaphoreGive(s_chore.mutex);
            return;
        }
        chore_slot_t *slot = _chore_pick_locked(now_ms);
        if (slot == NULL)
        {
            xSemaphoreGive(s_chore.mutex);
            return;
        }
        const uint32_t slot_index =
            (uint32_t)(slot - s_chore.slots);
        if (stopping ||
                atomic_load_explicit(&slot->cancel_requested,
                                     memory_order_acquire))
        {
            void (*release)(void *) = NULL;
            void *arg = NULL;
            ++s_chore.cancelled_count;
            _chore_begin_release_locked(slot, &release, &arg);
            xSemaphoreGive(s_chore.mutex);
            if (release != NULL)
            {
                release(arg);
            }
            xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
            _chore_finalize_release_locked(slot, slot_index);
            xSemaphoreGive(s_chore.mutex);
            continue;
        }
        slot->running = true;
        xSemaphoreGive(s_chore.mutex);
        _chore_dispatch_job(slot, slot_index);
    }
}

static bool _chore_worker_pause(uint32_t commands)
{
    bool stop = (commands & CHORE_CMD_STOP) != 0U;
    bool resume = (commands & CHORE_CMD_RESUME) != 0U;
    bool pause = !stop && !resume && (commands & CHORE_CMD_SUSPEND) != 0U;
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    if (resume)
    {
        s_chore.state = CHORE_STATE_RUNNING;
    }
    else if (pause)
    {
        s_chore.state = CHORE_STATE_PAUSED;
    }
    xSemaphoreGive(s_chore.mutex);
    if (resume)
    {
        xEventGroupClearBits(s_chore.events, CHORE_EVENT_PAUSED_BIT);
        xEventGroupSetBits(s_chore.events, CHORE_EVENT_RUNNING_BIT);
    }
    if (pause)
    {
        xEventGroupClearBits(s_chore.events, CHORE_EVENT_RUNNING_BIT);
        xEventGroupSetBits(s_chore.events, CHORE_EVENT_PAUSED_BIT);
        while (!stop)
        {
            commands = 0U;
            (void)xTaskNotifyWait(0U, UINT32_MAX, &commands, portMAX_DELAY);
            stop = (commands & CHORE_CMD_STOP) != 0U;
            if (!stop && (commands & CHORE_CMD_RESUME) != 0U)
            {
                xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
                s_chore.state = CHORE_STATE_RUNNING;
                xSemaphoreGive(s_chore.mutex);
                xEventGroupClearBits(s_chore.events,
                                     CHORE_EVENT_PAUSED_BIT);
                xEventGroupSetBits(s_chore.events,
                                   CHORE_EVENT_RUNNING_BIT);
                break;
            }
        }
    }
    return stop;
}

static void _chore_worker(void *context)
{
    (void)context;
    for (;;)
    {
        int64_t wait_ms;
        xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
        wait_ms = _chore_wait_ms_locked(_chore_now_ms());
        xSemaphoreGive(s_chore.mutex);
        const TickType_t ticks = wait_ms == CHORE_WAIT_FOREVER_MS ?
                                 portMAX_DELAY :
                                 _chore_timeout_ticks((uint32_t)wait_ms);
        uint32_t commands = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &commands, ticks);
        if ((commands & CHORE_CMD_STOP) != 0U)
        {
            _chore_run_due_jobs();
            break;
        }
        if (_chore_worker_pause(commands))
        {
            _chore_run_due_jobs();
            break;
        }
        _chore_run_due_jobs();
    }
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    s_chore.state = CHORE_STATE_STOPPED;
    s_chore.worker = NULL;
    xSemaphoreGive(s_chore.mutex);
    xEventGroupSetBits(s_chore.events, CHORE_EVENT_STOPPED_BIT);
    atomic_store_explicit(&s_chore.worker_tail_complete, true,
                          memory_order_release);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t chore_service_init(const chore_service_config_t *config)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (config == NULL || config->task_priority == 0U ||
            config->task_priority >= configMAX_PRIORITIES ||
            config->warning_duration_ms == 0U)
    {
        return result;
    }
    if (atomic_load_explicit(&s_chore.initialized, memory_order_acquire))
    {
        if (s_chore.stopping)
        {
            return ESP_ERR_INVALID_STATE;
        }
        return s_chore.task_priority == config->task_priority &&
               s_chore.warning_duration_ms == config->warning_duration_ms ?
               ESP_OK : ESP_ERR_INVALID_STATE;
    }
    /* Pre-init API calls only ever read s_chore.initialized, so the plain
       reset must not cover that flag: zeroing it with a concurrent atomic
       read would be a data race. Everything before it is only touched by
       init or by calls admitted after initialization, which observe this
       reset through the mutex. */
    memset(&s_chore, 0, offsetof(chore_service_context_t, initialized));
    s_chore.stopping = false;
    s_chore.last_duration_warning_ms = 0;
    s_chore.task_priority = config->task_priority;
    s_chore.warning_duration_ms = config->warning_duration_ms;
    s_chore.state = CHORE_STATE_RUNNING;
    atomic_init(&s_chore.worker_tail_complete, false);
    atomic_init(&s_chore.minimum_stack_remaining, UINT32_MAX);
    atomic_init(&s_chore.initialized, false);
    for (uint32_t slot = 0; slot < CONFIG_CHORE_SERVICE_JOB_CAPACITY; ++slot)
    {
        atomic_init(&s_chore.slots[slot].cancel_requested, false);
        uint32_t generation = s_chore_generation_high_water + 1U + slot;
        if (generation == 0U)
        {
            generation = 1U;
        }
        s_chore.slots[slot].generation = generation;
    }
    s_chore_generation_high_water += CONFIG_CHORE_SERVICE_JOB_CAPACITY + 1U;
    s_chore.mutex = xSemaphoreCreateMutex();
    s_chore.lifecycle_mutex = xSemaphoreCreateMutex();
    s_chore.events = xEventGroupCreate();
    if (s_chore.mutex == NULL || s_chore.lifecycle_mutex == NULL ||
            s_chore.events == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (xTaskCreatePinnedToCoreWithCaps(
                _chore_worker, "chore_worker",
                CONFIG_CHORE_SERVICE_TASK_STACK_SIZE, NULL,
                s_chore.task_priority, &s_chore.worker,
                CONFIG_MAIN_PROJECT_TASK_CORE_ID,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
#if CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0 || \
    CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1
    LOG_I("task affinity name=chore_worker core=%d",
          (int)xTaskGetCoreID(s_chore.worker));
#endif
    atomic_store_explicit(&s_chore.initialized, true, memory_order_release);
    /* The admission gate reopens only once the new context is complete, so
       calls admitted in a previous instance can never touch this one. */
    _chore_admission_open();
    LOG_I("worker started, priority=%u", (unsigned)s_chore.task_priority);
    return ESP_OK;

cleanup:
    if (s_chore.events != NULL)
    {
        vEventGroupDelete(s_chore.events);
    }
    if (s_chore.mutex != NULL)
    {
        vSemaphoreDelete(s_chore.mutex);
    }
    if (s_chore.lifecycle_mutex != NULL)
    {
        vSemaphoreDelete(s_chore.lifecycle_mutex);
    }
    /* Same discipline as the success path: never plain-reset the
       initialized flag that pre-init API calls read concurrently. */
    memset(&s_chore, 0, offsetof(chore_service_context_t, initialized));
    s_chore.stopping = false;
    s_chore.last_duration_warning_ms = 0;
    return result;
}

esp_err_t chore_service_deinit(uint32_t timeout_ms)
{
    if (!atomic_load_explicit(&s_chore.initialized, memory_order_acquire))
    {
        return ESP_OK;
    }
    /* A deinit issued from the worker context is rejected before admission
       closes, otherwise the service would be left permanently unadmitted.
       The handle is read under the mutex so it is ordered with the
       worker's exit-block write. */
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    const bool self_deinit = xTaskGetCurrentTaskHandle() == s_chore.worker;
    xSemaphoreGive(s_chore.mutex);
    if (self_deinit)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* Close admission before the drain begins: after this point every new
       public call is refused without touching any shared state. */
    _chore_admission_close();
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    s_chore.stopping = true;
    for (uint32_t slot = 0; slot < CONFIG_CHORE_SERVICE_JOB_CAPACITY; ++slot)
    {
        atomic_store_explicit(&s_chore.slots[slot].cancel_requested, true,
                              memory_order_release);
    }
    /* The worker clears its own handle under the same mutex before
       self-deletion, so a notify issued while holding the mutex can never
       target a task that has already been deleted. */
    TaskHandle_t worker = s_chore.worker;
    if (worker != NULL)
    {
        (void)xTaskNotify(worker, CHORE_CMD_STOP, eSetBits);
    }
    xSemaphoreGive(s_chore.mutex);
    EventBits_t bits = xEventGroupWaitBits(
                           s_chore.events, CHORE_EVENT_STOPPED_BIT,
                           pdFALSE, pdTRUE, _chore_timeout_ticks(timeout_ms));
    if ((bits & CHORE_EVENT_STOPPED_BIT) == 0U)
    {
        return ESP_ERR_TIMEOUT;
    }
    while (!atomic_load_explicit(&s_chore.worker_tail_complete,
                                 memory_order_acquire))
    {
        vTaskDelay(1);
    }
    /* Admitted public calls still in flight may hold the mutex or wait on
       the event group; the worker's STOPPED bit also wakes any lifecycle
       waiter, so the drain always terminates. */
    _chore_wait_api_drained();
    vEventGroupDelete(s_chore.events);
    vSemaphoreDelete(s_chore.mutex);
    vSemaphoreDelete(s_chore.lifecycle_mutex);
    memset(&s_chore, 0, sizeof(s_chore));
    atomic_init(&s_chore.initialized, false);
    atomic_init(&s_chore.worker_tail_complete, false);
    atomic_init(&s_chore.minimum_stack_remaining, UINT32_MAX);
    for (uint32_t slot = 0; slot < CONFIG_CHORE_SERVICE_JOB_CAPACITY; ++slot)
    {
        atomic_init(&s_chore.slots[slot].cancel_requested, false);
    }
    return ESP_OK;
}

esp_err_t chore_service_submit(const chore_service_job_t *job,
                               chore_service_handle_t *handle)
{
    if (job == NULL || job->run == NULL || handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _chore_api_enter();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!atomic_load_explicit(&s_chore.initialized, memory_order_acquire))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    if (s_chore.stopping)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit_locked;
    }
    chore_slot_t *slot = NULL;
    uint32_t slot_index = 0U;
    for (uint32_t index = 0; index < CONFIG_CHORE_SERVICE_JOB_CAPACITY;
            ++index)
    {
        if (!s_chore.slots[index].queued &&
                !s_chore.slots[index].running &&
                !s_chore.slots[index].releasing)
        {
            slot = &s_chore.slots[index];
            slot_index = index;
            break;
        }
    }
    if (slot == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit_locked;
    }
    slot->run = job->run;
    slot->release = job->release;
    slot->arg = job->arg;
    slot->period_ms = job->period_ms;
    slot->next_due_ms = _chore_now_ms() + (int64_t)job->delay_ms;
    slot->queued = true;
    atomic_store_explicit(&slot->cancel_requested, false,
                          memory_order_release);
    *handle = (chore_service_handle_t)
    {
        .slot = slot_index,
        .generation = slot->generation,
    };
    result = ESP_OK;
    /* Notifying under the mutex keeps the worker handle live: the worker
       clears it in its exit block which needs the same mutex. */
    if (s_chore.worker != NULL)
    {
        (void)xTaskNotify(s_chore.worker, CHORE_CMD_WAKE, eSetBits);
    }
exit_locked:
    xSemaphoreGive(s_chore.mutex);
exit:
    _chore_api_leave();
    return result;
}

esp_err_t chore_service_cancel(chore_service_handle_t *handle,
                               uint32_t timeout_ms)
{
    if (handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t slot_index = handle->slot;
    if (slot_index >= CONFIG_CHORE_SERVICE_JOB_CAPACITY)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _chore_api_enter();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!atomic_load_explicit(&s_chore.initialized, memory_order_acquire))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    bool wait_for_ack = false;
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    chore_slot_t *slot = &s_chore.slots[slot_index];
    /* The stale-handle check comes first so a completed job still reports
       ESP_OK even while the worker is already shutting down. A still
       pending job during shutdown is rejected per the public contract. */
    const bool pending = slot->generation == handle->generation &&
                         (slot->queued || slot->running ||
                          slot->releasing);
    if (!pending)
    {
        goto exit_locked;
    }
    if (s_chore.stopping || s_chore.worker == NULL ||
            xTaskGetCurrentTaskHandle() == s_chore.worker)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit_locked;
    }
    /* The acknowledgement bit is cleared only while the slot is still
       pending under the mutex: a stale canceler can never clear the
       acknowledgement of a slot another canceler is already waiting on. */
    xEventGroupClearBits(s_chore.events, CHORE_SLOT_BIT(slot_index));
    atomic_store_explicit(&slot->cancel_requested, true,
                          memory_order_release);
    wait_for_ack = true;
    if (s_chore.worker != NULL)
    {
        (void)xTaskNotify(s_chore.worker, CHORE_CMD_WAKE, eSetBits);
    }
exit_locked:
    xSemaphoreGive(s_chore.mutex);
    if (wait_for_ack)
    {
        /* The slot-freed bit is shared: concurrent cancelers may consume
           the same acknowledgement, so the bit is never cleared on exit
           and each canceler re-validates the slot after waking. */
        (void)xEventGroupWaitBits(s_chore.events, CHORE_SLOT_BIT(slot_index),
                                  pdFALSE, pdTRUE,
                                  _chore_timeout_ticks(timeout_ms));
        xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
        result = slot->generation == handle->generation &&
                 (slot->queued || slot->running || slot->releasing) ?
                 ESP_ERR_TIMEOUT : ESP_OK;
        xSemaphoreGive(s_chore.mutex);
    }
exit:
    _chore_api_leave();
    return result;
}

esp_err_t chore_service_suspend(uint32_t timeout_ms)
{
    esp_err_t result = _chore_api_enter();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!atomic_load_explicit(&s_chore.initialized, memory_order_acquire))
    {
        result = ESP_OK;
        goto exit;
    }
    /* The worker-context rejection must happen before the lifecycle mutex
       is taken: a job calling suspend while another task holds the
       lifecycle mutex would otherwise block forever before reaching the
       self-check. The handle is read under the service mutex. */
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    const bool self_suspend = xTaskGetCurrentTaskHandle() == s_chore.worker;
    xSemaphoreGive(s_chore.mutex);
    if (self_suspend)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    /* The lifecycle mutex serializes the whole suspend transaction against
       a concurrent resume, so opposite commands can never coalesce in the
       worker and opposite callers can never erase each other's
       acknowledgements. Deinit does not take this mutex: the STOPPED bit
       wakes the waiters. */
    xSemaphoreTake(s_chore.lifecycle_mutex, portMAX_DELAY);
    bool send_suspend = false;
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    if (s_chore.stopping || s_chore.worker == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit_locked;
    }
    if (s_chore.state == CHORE_STATE_PAUSED)
    {
        result = ESP_OK;
        goto exit_locked;
    }
    if (s_chore.state != CHORE_STATE_RUNNING)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit_locked;
    }
    s_chore.state = CHORE_STATE_PAUSE_PENDING;
    send_suspend = true;
    result = ESP_OK;
    /* The acknowledgement bit is cleared before the notification while the
       mutex is held, so the worker's acknowledgement can never be erased
       after it is set. */
    xEventGroupClearBits(s_chore.events, CHORE_EVENT_PAUSED_BIT);
    (void)xTaskNotify(s_chore.worker, CHORE_CMD_SUSPEND, eSetBits);
exit_locked:
    xSemaphoreGive(s_chore.mutex);
    if (!send_suspend)
    {
        goto exit_lifecycle;
    }
    /* The STOPPED bit wakes the wait when deinit shuts the worker down, so
       a WAIT_FOREVER suspend cannot deadlock the teardown drain. */
    EventBits_t bits = xEventGroupWaitBits(
                           s_chore.events, CHORE_EVENT_PAUSED_BIT |
                           CHORE_EVENT_STOPPED_BIT,
                           pdFALSE, pdFALSE, _chore_timeout_ticks(timeout_ms));
    if ((bits & CHORE_EVENT_PAUSED_BIT) != 0U)
    {
        result = ESP_OK;
        goto exit_lifecycle;
    }
    if ((bits & CHORE_EVENT_STOPPED_BIT) != 0U)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit_lifecycle;
    }
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    if (s_chore.state == CHORE_STATE_PAUSE_PENDING ||
            s_chore.state == CHORE_STATE_PAUSED)
    {
        s_chore.state = CHORE_STATE_RESUME_PENDING;
    }
    xEventGroupClearBits(s_chore.events, CHORE_EVENT_PAUSED_BIT |
                         CHORE_EVENT_RUNNING_BIT);
    if (s_chore.worker != NULL)
    {
        (void)xTaskNotify(s_chore.worker, CHORE_CMD_RESUME, eSetBits);
    }
    xSemaphoreGive(s_chore.mutex);
    EventBits_t rollback_bits = xEventGroupWaitBits(
                                    s_chore.events, CHORE_EVENT_RUNNING_BIT |
                                    CHORE_EVENT_STOPPED_BIT,
                                    pdFALSE, pdFALSE,
                                    _chore_timeout_ticks(timeout_ms));
    result = (rollback_bits & CHORE_EVENT_STOPPED_BIT) != 0U ?
             ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
exit_lifecycle:
    xSemaphoreGive(s_chore.lifecycle_mutex);
exit:
    _chore_api_leave();
    return result;
}

esp_err_t chore_service_resume(uint32_t timeout_ms)
{
    esp_err_t result = _chore_api_enter();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!atomic_load_explicit(&s_chore.initialized, memory_order_acquire))
    {
        result = ESP_OK;
        goto exit;
    }
    /* Worker-context rejection before the lifecycle mutex, mirroring
       suspend: a job calling resume while another task holds the
       lifecycle mutex must not block forever. */
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    const bool self_resume = xTaskGetCurrentTaskHandle() == s_chore.worker;
    xSemaphoreGive(s_chore.mutex);
    if (self_resume)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    xSemaphoreTake(s_chore.lifecycle_mutex, portMAX_DELAY);
    bool send_resume = false;
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    if (s_chore.stopping || s_chore.worker == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit_locked;
    }
    if (s_chore.state == CHORE_STATE_RUNNING)
    {
        result = ESP_OK;
        goto exit_locked;
    }
    if (s_chore.state != CHORE_STATE_PAUSED &&
            s_chore.state != CHORE_STATE_PAUSE_PENDING &&
            s_chore.state != CHORE_STATE_RESUME_PENDING)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit_locked;
    }
    s_chore.state = CHORE_STATE_RESUME_PENDING;
    send_resume = true;
    result = ESP_OK;
    /* Clear before notify, under the mutex: the worker's RUNNING
       acknowledgement can never be erased after it is set. */
    xEventGroupClearBits(s_chore.events, CHORE_EVENT_RUNNING_BIT);
    (void)xTaskNotify(s_chore.worker, CHORE_CMD_RESUME, eSetBits);
exit_locked:
    xSemaphoreGive(s_chore.mutex);
    if (!send_resume)
    {
        goto exit_lifecycle;
    }
    /* The STOPPED bit wakes the wait when deinit shuts the worker down. */
    EventBits_t bits = xEventGroupWaitBits(
                           s_chore.events, CHORE_EVENT_RUNNING_BIT |
                           CHORE_EVENT_STOPPED_BIT,
                           pdFALSE, pdFALSE, _chore_timeout_ticks(timeout_ms));
    if ((bits & CHORE_EVENT_RUNNING_BIT) != 0U)
    {
        result = ESP_OK;
        goto exit_lifecycle;
    }
    result = (bits & CHORE_EVENT_STOPPED_BIT) != 0U ?
             ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
exit_lifecycle:
    xSemaphoreGive(s_chore.lifecycle_mutex);
exit:
    _chore_api_leave();
    return result;
}

esp_err_t chore_service_get_status(chore_service_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _chore_api_enter();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!atomic_load_explicit(&s_chore.initialized, memory_order_acquire))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
    status->initialized = true;
    status->suspended = s_chore.state == CHORE_STATE_PAUSED ||
                        s_chore.state == CHORE_STATE_PAUSE_PENDING;
    status->stopping = s_chore.stopping;
    status->queued_count = 0U;
    status->running_count = 0U;
    for (uint32_t slot = 0; slot < CONFIG_CHORE_SERVICE_JOB_CAPACITY; ++slot)
    {
        if (s_chore.slots[slot].queued)
        {
            ++status->queued_count;
        }
        if (s_chore.slots[slot].running)
        {
            ++status->running_count;
        }
    }
    status->stack_high_water = atomic_load_explicit(
                                   &s_chore.minimum_stack_remaining,
                                   memory_order_relaxed);
    status->completed_count = s_chore.completed_count;
    status->cancelled_count = s_chore.cancelled_count;
    result = ESP_OK;
    xSemaphoreGive(s_chore.mutex);
exit:
    _chore_api_leave();
    return result;
}

bool chore_service_is_available(void)
{
    if (_chore_api_enter() != ESP_OK)
    {
        return false;
    }
    bool available = false;
    if (atomic_load_explicit(&s_chore.initialized, memory_order_acquire))
    {
        xSemaphoreTake(s_chore.mutex, portMAX_DELAY);
        available = !s_chore.stopping;
        xSemaphoreGive(s_chore.mutex);
    }
    _chore_api_leave();
    return available;
}

bool chore_service_cancel_pending(
    const chore_service_cancel_token_t *cancel)
{
    return cancel != NULL && cancel->requested != NULL &&
           atomic_load_explicit(cancel->requested, memory_order_acquire);
}
