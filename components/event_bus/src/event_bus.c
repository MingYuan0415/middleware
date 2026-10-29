/**
 * @brief Fixed-capacity event bus implementation.
 */

#if defined(ESP_PLATFORM)
    /* The TRACE gate below runs before mt_log.h pulls in sdkconfig.h. */
    #include "sdkconfig.h"
#endif

#if CONFIG_EVENT_BUS_TRACE
    /* Raise this file's compile-time ceiling; the tag still has to be enabled
    * for ESP_LOG_DEBUG at runtime. */
    #define DBG_LVL DBG_DEBUG
    #define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif

#define DBG_TAG "event_bus"
#include "mt_log.h"

#include "event_bus.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#if defined(ESP_PLATFORM) && !defined(HOST_TEST)
    #include <esp_attr.h>
#endif

#include "event_bus_core.h"

#if defined(ESP_PLATFORM) && !defined(HOST_TEST)
    #define _EVENT_BUS_EXT_BSS EXT_RAM_BSS_ATTR
    #define _EVENT_BUS_ASSERT_TASK() configASSERT(!xPortInIsrContext())
#else
    #define _EVENT_BUS_EXT_BSS
    #define _EVENT_BUS_ASSERT_TASK() ((void)0)
#endif

#if CONFIG_EVENT_BUS_STATS
    #define _EVENT_BUS_STAT(core, field) (void)(++(core)->stats.field)
#else
    #define _EVENT_BUS_STAT(core, field) ((void)0)
#endif

static event_bus_core_t s_core;
static _EVENT_BUS_EXT_BSS event_bus_payload_t
s_payloads[EVENT_BUS_MAX_PENDING_UI_BATCHES];
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_admission_mutex;
static SemaphoreHandle_t s_sync_signal;
static event_bus_ui_dispatch_fn s_ui_dispatch;
static event_bus_wake_request_fn s_wake_request;
static uint32_t s_ui_depth;
static atomic_bool s_initialized;

static void _event_bus_dispatch_batch(void *arg);

static bool _event_bus_is_initialized(void)
{
    return atomic_load_explicit(&s_initialized, memory_order_acquire);
}

static void _event_bus_lock(void)
{
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void _event_bus_unlock(void)
{
    (void)xSemaphoreGive(s_mutex);
}

static void _event_bus_signal_sync_idle(void)
{
    (void)xSemaphoreGive(s_sync_signal);
}

#define _EVENT_BUS_LOG_INTERVAL_MS 5000U

static bool _event_bus_timeout_elapsed(TickType_t start, uint32_t timeout_ms)
{
    const uint32_t elapsed_ticks = (uint32_t)(xTaskGetTickCount() - start);
    const uint64_t elapsed_ms = (uint64_t)elapsed_ticks * 1000U /
                                (uint64_t)configTICK_RATE_HZ;
    return elapsed_ms >= timeout_ms;
}

static TickType_t _event_bus_wait_ticks(void)
{
    const TickType_t ticks = pdMS_TO_TICKS(1);
    return ticks > 0 ? ticks : 1;
}

static atomic_bool s_drop_log_started;
static atomic_uint s_last_drop_log_tick;
static atomic_uint s_suppressed_drop_logs;

static void _event_bus_log_ui_drop(event_bus_msg_id_t msg_id,
                                   uint32_t sub_type, size_t payload_size,
                                   esp_err_t result)
{
    const TickType_t now = xTaskGetTickCount();
    const TickType_t last = atomic_load_explicit(&s_last_drop_log_tick,
        memory_order_relaxed);
    if (atomic_load_explicit(&s_drop_log_started, memory_order_relaxed)
            && (TickType_t)(now - last) <
            pdMS_TO_TICKS(_EVENT_BUS_LOG_INTERVAL_MS))
    {
        (void)atomic_fetch_add_explicit(&s_suppressed_drop_logs, 1U,
                                        memory_order_relaxed);
        return;
    }

    const unsigned suppressed = atomic_exchange_explicit(
                                    &s_suppressed_drop_logs, 0U,
                                    memory_order_relaxed);
    atomic_store_explicit(&s_drop_log_started, true, memory_order_relaxed);
    atomic_store_explicit(&s_last_drop_log_tick, now, memory_order_relaxed);
    LOG_W("UI drop %s/%u size=%u: %s (%u suppressed)", msg_id->name,
          (unsigned)sub_type, (unsigned)payload_size,
          esp_err_to_name(result), suppressed);

    /* Logging may be compiled out by a lower DBG_LVL. */
    (void)msg_id;
    (void)sub_type;
    (void)payload_size;
    (void)result;
    (void)suppressed;
}

static atomic_bool s_wake_log_started;
static atomic_uint s_last_wake_log_tick;
static atomic_uint s_suppressed_wake_logs;

static void _event_bus_log_wake_failure(esp_err_t result)
{
    const TickType_t now = xTaskGetTickCount();
    const TickType_t last = atomic_load_explicit(&s_last_wake_log_tick,
        memory_order_relaxed);
    if (atomic_load_explicit(&s_wake_log_started, memory_order_relaxed)
            && (TickType_t)(now - last) <
            pdMS_TO_TICKS(_EVENT_BUS_LOG_INTERVAL_MS))
    {
        (void)atomic_fetch_add_explicit(&s_suppressed_wake_logs, 1U,
                                        memory_order_relaxed);
        return;
    }

    const unsigned suppressed = atomic_exchange_explicit(
                                    &s_suppressed_wake_logs, 0U,
                                    memory_order_relaxed);
    atomic_store_explicit(&s_wake_log_started, true, memory_order_relaxed);
    atomic_store_explicit(&s_last_wake_log_tick, now, memory_order_relaxed);
    LOG_W("wake request rejected: %s (%u suppressed)", esp_err_to_name(result),
          suppressed);

    /* Logging may be compiled out by a lower DBG_LVL. */
    (void)result;
    (void)suppressed;
}

/**
 * @brief Copy the currently active subscription slots.
 *
 * The snapshot only narrows the scan; the publication sequence still decides
 * whether a slot was subscribed before the publication.
 */
static size_t _event_bus_snapshot_slots(uint8_t *slots, size_t capacity)
{
    size_t count = 0;

    _event_bus_lock();
    uint64_t mask = s_core.active_slots;
    _event_bus_unlock();
    for (size_t slot = 0; mask != 0 && count < capacity; ++slot, mask >>= 1)
    {
        if ((mask & UINT64_C(1)) != 0)
        {
            slots[count++] = (uint8_t)slot;
        }
    }
    return count;
}

static void _event_bus_request_wake(uint32_t flags,
                                    event_bus_wake_request_fn wake_request)
{
    if ((flags & EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST) != 0
            && wake_request != NULL)
    {
        const esp_err_t result = wake_request();
        if (result != ESP_OK)
        {
            _event_bus_log_wake_failure(result);
        }
    }
}

static esp_err_t _event_bus_validate_publish(event_bus_msg_id_t msg_id,
        const void *payload, size_t payload_size, uint32_t flags)
{
    if (msg_id == NULL || (payload == NULL && payload_size != 0)
            || (flags & ~(EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST
                          | EVENT_BUS_PUBLISH_FLAG_UI_LATEST
                          | EVENT_BUS_PUBLISH_FLAG_ATOMIC)) != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void _event_bus_dispatch_batch(void *arg)
{
    const int envelope_index = (int)(uintptr_t)arg - 1;
    if (envelope_index < 0
            || envelope_index >= EVENT_BUS_MAX_PENDING_UI_BATCHES)
    {
        return;
    }

    _event_bus_lock();
    event_bus_envelope_t *envelope = &s_core.envelopes[envelope_index];
    if (envelope->state != EVENT_BUS_ENV_DISPATCHING
            && envelope->state != EVENT_BUS_ENV_ADMITTED)
    {
        _event_bus_unlock();
        return;
    }
    envelope->state = EVENT_BUS_ENV_EXECUTING;
    const event_bus_msg_id_t msg_id = envelope->msg_id;
    const uint32_t sub_type = envelope->sub_type;
    const uint64_t publish_seq = envelope->publish_seq;
    const size_t payload_size = envelope->payload_size;
    _event_bus_unlock();

    uint8_t slots[EVENT_BUS_MAX_SUBSCRIBERS];
    const size_t slot_count = _event_bus_snapshot_slots(slots,
        EVENT_BUS_MAX_SUBSCRIBERS);
    for (size_t index = 0; index < slot_count; ++index)
    {
        const size_t slot = slots[index];
        _event_bus_lock();
        const bool matched = s_core.subscriptions[slot].context
                             == EVENT_BUS_DISPATCH_UI
                             && event_bus_core_matches(&s_core.subscriptions[slot],
                                 msg_id, sub_type);
        const event_bus_target_t target = event_bus_core_acquire(
                                              &s_core, slot, msg_id, sub_type,
                                              EVENT_BUS_DISPATCH_UI, publish_seq);
        if (matched && !target.live)
        {
            _EVENT_BUS_STAT(&s_core, late_subscriber_skips);
        }
        const void *payload = target.live && payload_size > 0 ?
                              s_payloads[envelope_index].bytes : NULL;
        _event_bus_unlock();

        if (target.live)
        {
            target.cb(msg_id, sub_type, payload, payload_size,
                      target.user_data);
        }
    }

    _event_bus_lock();
    event_bus_core_free_batch(&s_core, envelope_index);
    _event_bus_unlock();
}

static void _event_bus_deliver_sync(event_bus_msg_id_t msg_id,
                                    uint32_t sub_type, const void *payload,
                                    size_t payload_size, uint64_t publish_seq,
                                    uint16_t *delivered)
{
    uint16_t count = 0;
    uint8_t slots[EVENT_BUS_MAX_SUBSCRIBERS];
    const size_t slot_count = _event_bus_snapshot_slots(slots,
        EVENT_BUS_MAX_SUBSCRIBERS);

    for (size_t index = 0; index < slot_count; ++index)
    {
        const size_t slot = slots[index];
        _event_bus_lock();
        const bool matched =
            s_core.subscriptions[slot].context == EVENT_BUS_DISPATCH_PUBLISHER
            && event_bus_core_matches(&s_core.subscriptions[slot], msg_id,
                                      sub_type);
        const event_bus_target_t target = event_bus_core_acquire(
                                              &s_core, slot, msg_id, sub_type,
                                              EVENT_BUS_DISPATCH_PUBLISHER,
                                              publish_seq);
        if (matched && !target.live)
        {
            _EVENT_BUS_STAT(&s_core, late_subscriber_skips);
        }
        _event_bus_unlock();

        if (!target.live)
        {
            continue;
        }
        target.cb(msg_id, sub_type, payload, payload_size, target.user_data);
        ++count;

        _event_bus_lock();
        event_bus_core_release(&s_core, slot);
        if (event_bus_core_slot_in_flight(&s_core, slot) == 0
                || event_bus_core_in_flight(&s_core) == 0)
        {
            _event_bus_signal_sync_idle();
        }
#if CONFIG_EVENT_BUS_STATS
        ++s_core.stats.sync_delivered;
#endif
        _event_bus_unlock();
    }
    *delivered = count;
}

esp_err_t event_bus_init(void)
{
    _EVENT_BUS_ASSERT_TASK();
    if (_event_bus_is_initialized())
    {
        return ESP_OK;
    }

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t admission_mutex = xSemaphoreCreateMutex();
    if (admission_mutex == NULL)
    {
        vSemaphoreDelete(mutex);
        return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t sync_signal = xSemaphoreCreateBinary();
    if (sync_signal == NULL)
    {
        vSemaphoreDelete(admission_mutex);
        vSemaphoreDelete(mutex);
        return ESP_ERR_NO_MEM;
    }

    if (s_core.payloads == NULL)
    {
        event_bus_core_init(&s_core, s_payloads);
    }
    else
    {
        event_bus_core_reset(&s_core);
    }
    s_ui_dispatch = NULL;
    s_wake_request = NULL;
    s_ui_depth = 0;
    s_mutex = mutex;
    s_admission_mutex = admission_mutex;
    s_sync_signal = sync_signal;
    atomic_store_explicit(&s_drop_log_started, false, memory_order_relaxed);
    atomic_store_explicit(&s_last_drop_log_tick, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_suppressed_drop_logs, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_wake_log_started, false, memory_order_relaxed);
    atomic_store_explicit(&s_last_wake_log_tick, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_suppressed_wake_logs, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_initialized, true, memory_order_release);

    LOG_I("initialized (subscriptions=%u, batches=%u x %u)",
          (unsigned)EVENT_BUS_MAX_SUBSCRIBERS,
          (unsigned)EVENT_BUS_MAX_PENDING_UI_BATCHES,
          (unsigned)EVENT_BUS_MAX_UI_PAYLOAD_SIZE);
    return ESP_OK;
}

void event_bus_deinit(void)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return;
    }

    atomic_store_explicit(&s_initialized, false, memory_order_release);
    vSemaphoreDelete(s_sync_signal);
    vSemaphoreDelete(s_admission_mutex);
    vSemaphoreDelete(s_mutex);
    s_sync_signal = NULL;
    s_admission_mutex = NULL;
    s_mutex = NULL;
    s_ui_dispatch = NULL;
    s_wake_request = NULL;
    s_ui_depth = 0;
    event_bus_core_reset(&s_core);
}

esp_err_t event_bus_subscribe(event_bus_msg_id_t msg_id, uint32_t sub_type,
                              event_bus_cb_t cb, void *user_data,
                              event_bus_dispatch_context_t context,
                              event_bus_sub_handle_t *out_handle)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (msg_id == NULL || cb == NULL || out_handle == NULL
            || (context != EVENT_BUS_DISPATCH_PUBLISHER
                && context != EVENT_BUS_DISPATCH_UI))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    _event_bus_lock();
    if (context == EVENT_BUS_DISPATCH_UI && s_ui_dispatch == NULL)
    {
        _event_bus_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const int slot = event_bus_core_subscribe(&s_core, msg_id, sub_type, cb,
        user_data, context, out_handle);
    _event_bus_unlock();

    return slot < 0 ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t event_bus_unsubscribe(event_bus_sub_handle_t handle)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    _event_bus_lock();
    const esp_err_t result = event_bus_core_unsubscribe(&s_core, handle);
    _event_bus_unlock();
    return result;
}

esp_err_t event_bus_unsubscribe_sync(event_bus_sub_handle_t handle,
                                     uint32_t timeout_ms)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    size_t slot = 0;
    uint64_t seq = 0;
    if (!event_bus_core_decode_handle(handle, &slot, &seq))
    {
        return ESP_ERR_INVALID_ARG;
    }

    _event_bus_lock();
    const esp_err_t result = event_bus_core_unsubscribe(&s_core, handle);
    const bool owned = event_bus_core_slot_owned(&s_core, slot, seq);
    _event_bus_unlock();
    if (result != ESP_OK
            && !(result == ESP_ERR_NOT_FOUND && owned))
    {
        return result;
    }

    const TickType_t start = xTaskGetTickCount();
    for (;;)
    {
        _event_bus_lock();
        /* Only this subscription generation keeps the caller waiting. A slot
         * which was drained and reused by a newer subscriber is not ours. */
        const bool busy = event_bus_core_draining(&s_core, slot, seq);
        _event_bus_unlock();
        if (!busy)
        {
            return ESP_OK;
        }
        if (_event_bus_timeout_elapsed(start, timeout_ms))
        {
            return ESP_ERR_TIMEOUT;
        }
        (void)xSemaphoreTake(s_sync_signal, _event_bus_wait_ticks());
    }
}

esp_err_t event_bus_flush_sync(uint32_t timeout_ms)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t start = xTaskGetTickCount();
    for (;;)
    {
        _event_bus_lock();
        const bool busy = event_bus_core_in_flight(&s_core) > 0;
        _event_bus_unlock();
        if (!busy)
        {
            return ESP_OK;
        }
        if (_event_bus_timeout_elapsed(start, timeout_ms))
        {
            return ESP_ERR_TIMEOUT;
        }
        (void)xSemaphoreTake(s_sync_signal, _event_bus_wait_ticks());
    }
}

esp_err_t event_bus_publish_ex(event_bus_msg_id_t msg_id, uint32_t sub_type,
                               const void *payload, size_t payload_size,
                               uint32_t flags,
                               event_bus_publish_report_t *out_report)
{
    _EVENT_BUS_ASSERT_TASK();
    if (out_report != NULL)
    {
        *out_report = (event_bus_publish_report_t)
        {
            0
        };
    }
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = _event_bus_validate_publish(msg_id, payload,
        payload_size, flags);
    if (result != ESP_OK)
    {
        return result;
    }

    event_bus_publish_report_t report =
    {
        .sync_result = ESP_OK,
        .ui_result = ESP_OK,
    };
    event_bus_ui_dispatch_fn dispatch = NULL;
    event_bus_wake_request_fn wake_request = NULL;
    int envelope_index = EVENT_BUS_SLOT_NONE;
    int coalesce_index = EVENT_BUS_SLOT_NONE;
    bool coalesced = false;
    size_t ui_count = 0;
    size_t sync_count = 0;

    (void)xSemaphoreTake(s_admission_mutex, portMAX_DELAY);
    _event_bus_lock();
    const uint64_t publish_seq = event_bus_core_next_seq(&s_core);
    event_bus_core_count(&s_core, msg_id, sub_type, publish_seq, &ui_count,
                         &sync_count);
    report.matched = (uint16_t)(ui_count + sync_count);
    _EVENT_BUS_STAT(&s_core, publishes);

    if (ui_count > 0)
    {
        if (payload_size > EVENT_BUS_MAX_UI_PAYLOAD_SIZE)
        {
            report.ui_result = ESP_ERR_INVALID_SIZE;
            _EVENT_BUS_STAT(&s_core, drop_oversize);
        }
        else if (s_ui_dispatch == NULL)
        {
            report.ui_result = ESP_ERR_INVALID_STATE;
            _EVENT_BUS_STAT(&s_core, drop_no_dispatcher);
        }
        else
        {
            if ((flags & EVENT_BUS_PUBLISH_FLAG_UI_LATEST) != 0)
            {
                coalesce_index = event_bus_core_find_admitted(
                                     &s_core, msg_id, sub_type);
            }
            if (coalesce_index != EVENT_BUS_SLOT_NONE)
            {
                coalesced = event_bus_core_replace(&s_core, coalesce_index,
                                                   payload, payload_size,
                                                   publish_seq);
            }
            if (!coalesced)
            {
                if (s_core.pending_batches >= s_ui_depth)
                {
                    report.ui_result = ESP_ERR_NO_MEM;
                    _EVENT_BUS_STAT(&s_core, drop_no_mem);
                }
                else
                {
                    envelope_index = event_bus_core_begin_batch(
                                         &s_core, msg_id, sub_type, payload,
                                         payload_size,
                                         (flags & EVENT_BUS_PUBLISH_FLAG_UI_LATEST) != 0,
                                         publish_seq);
                    if (envelope_index == EVENT_BUS_SLOT_NONE)
                    {
                        report.ui_result = ESP_ERR_NO_MEM;
                        _EVENT_BUS_STAT(&s_core, drop_no_mem);
                    }
                }
            }
        }
        dispatch = s_ui_dispatch;
    }
    wake_request = s_wake_request;
    _event_bus_unlock();

    if (envelope_index != EVENT_BUS_SLOT_NONE && !coalesced)
    {
        report.ui_result = dispatch(_event_bus_dispatch_batch,
                                    (void *)(uintptr_t)(envelope_index + 1));
        _event_bus_lock();
        event_bus_core_commit_batch(&s_core, envelope_index,
                                    report.ui_result);
        _event_bus_unlock();
        if (report.ui_result == ESP_OK)
        {
            report.ui_queued = true;
            report.ui_targets = (uint16_t)ui_count;
            _EVENT_BUS_STAT(&s_core, ui_batches);
        }
    }
    else if (coalesced)
    {
        report.ui_coalesced = true;
        report.ui_targets = (uint16_t)ui_count;
    }
    (void)xSemaphoreGive(s_admission_mutex);

#if CONFIG_EVENT_BUS_TRACE
    LOG_D("publish %s/%u size=%u ui=%u sync=%u",
          msg_id->name, (unsigned)sub_type, (unsigned)payload_size,
          (unsigned)ui_count, (unsigned)sync_count);
#endif

    _event_bus_request_wake(flags, wake_request);
    if ((flags & EVENT_BUS_PUBLISH_FLAG_ATOMIC) != 0
            && report.ui_result != ESP_OK)
    {
        report.sync_result = report.ui_result;
        report.sync_delivered = 0;
    }
    else
    {
        _event_bus_deliver_sync(msg_id, sub_type, payload, payload_size,
                                publish_seq, &report.sync_delivered);
    }

    if (report.ui_result != ESP_OK)
    {
        _event_bus_log_ui_drop(msg_id, sub_type, payload_size,
                               report.ui_result);
    }

    if (out_report != NULL)
    {
        *out_report = report;
    }
    return report.sync_result != ESP_OK ? report.sync_result :
           report.ui_result;
}

esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags)
{
    return event_bus_publish_ex(msg_id, sub_type, payload, payload_size, flags,
                                NULL);
}

esp_err_t event_bus_register_ui_dispatch(event_bus_ui_dispatch_fn dispatch,
        uint32_t queue_depth)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (dispatch == NULL || queue_depth == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    _event_bus_lock();
    s_ui_dispatch = dispatch;
    s_ui_depth = queue_depth;
    _event_bus_unlock();
    return ESP_OK;
}

esp_err_t event_bus_unregister_ui_dispatch(
    event_bus_ui_dispatch_fn expected_dispatch)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_dispatch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    _event_bus_lock();
    esp_err_t result = ESP_OK;
    if (s_ui_dispatch != expected_dispatch)
    {
        result = ESP_ERR_NOT_FOUND;
    }
    else
    {
        s_ui_dispatch = NULL;
        s_ui_depth = 0;
    }
    _event_bus_unlock();
    return result;
}

esp_err_t event_bus_abandon_pending_ui(void)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    (void)xSemaphoreTake(s_admission_mutex, portMAX_DELAY);
    _event_bus_lock();
    const size_t released = event_bus_core_abandon(&s_core);
    _event_bus_unlock();
    (void)xSemaphoreGive(s_admission_mutex);

    if (released > 0)
    {
        LOG_I("released %u pending UI batches", (unsigned)released);
    }
    return ESP_OK;
}

esp_err_t event_bus_register_wake_requester(event_bus_wake_request_fn request_wake)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (request_wake == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    _event_bus_lock();
    s_wake_request = request_wake;
    _event_bus_unlock();
    return ESP_OK;
}

esp_err_t event_bus_unregister_wake_requester(
    event_bus_wake_request_fn expected_request_wake)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_request_wake == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    _event_bus_lock();
    esp_err_t result = ESP_OK;
    if (s_wake_request != expected_request_wake)
    {
        result = ESP_ERR_NOT_FOUND;
    }
    else
    {
        s_wake_request = NULL;
    }
    _event_bus_unlock();
    return result;
}

esp_err_t event_bus_get_stats(event_bus_stats_t *out_stats)
{
    _EVENT_BUS_ASSERT_TASK();
    if (!_event_bus_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_stats == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    _event_bus_lock();
    *out_stats = s_core.stats;
    _event_bus_unlock();
    return ESP_OK;
}
