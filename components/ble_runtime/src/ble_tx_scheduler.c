#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#ifndef UNIT_TEST_HOST
    #include "esp_heap_caps.h"
#endif

#include "ble_tx_scheduler.h"

#define DBG_TAG "ble_tx_scheduler"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

typedef struct ble_tx_scheduler_frame
{
    ble_tx_scheduler_kind_t kind;
    ble_link_operation_identity_t identity;
    uint16_t value_handle;
    size_t len;
    bool is_last;
    uint8_t data[1];
} ble_tx_scheduler_frame_t;

typedef struct ble_tx_scheduler_pending
{
    ble_tx_scheduler_kind_t kind;
    ble_link_operation_identity_t identity;
    uint16_t value_handle;
    esp_err_t status;
    bool is_last;
} ble_tx_scheduler_pending_t;

typedef struct ble_tx_scheduler
{
    const ble_tx_scheduler_config_t *config;
    ble_tx_scheduler_frame_t **frames;
    size_t head;
    size_t count;
    uint32_t token_counter;
    ble_link_operation_identity_t in_flight_identity;
    bool in_flight_is_last;
    bool in_flight;
    bool port_call_active;
    ble_tx_scheduler_kind_t in_flight_kind;
    uint16_t in_flight_attr;
    bool deinitializing;
    uint8_t *in_flight_data;
    unsigned int depth;
    bool draining;
    ble_tx_scheduler_pending_t *pending;
    size_t pending_count;
    size_t pending_capacity;
} ble_tx_scheduler_t;

static ble_tx_scheduler_t s_scheduler;

static ble_link_operation_kind_t _ble_tx_scheduler_operation_kind(
    ble_tx_scheduler_kind_t kind)
{
    return kind == BLE_TX_SCHEDULER_KIND_INDICATE ?
           BLE_LINK_OPERATION_TX_INDICATE : BLE_LINK_OPERATION_TX_NOTIFY;
}

static void *_ble_tx_scheduler_payload_alloc(size_t size)
{
#ifdef UNIT_TEST_HOST
    return malloc(size);
#else
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

static void _ble_tx_scheduler_lock(void)
{
    if (s_scheduler.config != NULL && s_scheduler.config->lock != NULL)
    {
        s_scheduler.config->lock(s_scheduler.config->lock_arg);
    }
}

static void _ble_tx_scheduler_unlock(void)
{
    if (s_scheduler.config != NULL && s_scheduler.config->unlock != NULL)
    {
        s_scheduler.config->unlock(s_scheduler.config->lock_arg);
    }
}

/**
 * @brief Queue one completion for later delivery; caller holds the lock.
 *
 * Deliveries are deferred while the scheduler is re-entered (a synchronous
 * NOTIFY_TX event or a completion callback submitting new frames) and drained
 * at the outermost entry, so the completion callback never runs inside the
 * port ops call and its stack usage is bounded by one frame chain. The
 * The fixed buffer is one entry larger than the frame queue. Admission treats
 * queued, in-flight, and pending-completion entries as one shared credit pool,
 * so this helper cannot overflow after a successful submission.
 */
static void _ble_tx_scheduler_queue_completion(
    ble_tx_scheduler_kind_t kind,
    const ble_link_operation_identity_t *identity, uint16_t value_handle,
    esp_err_t status, bool is_last)
{
    if (s_scheduler.pending == NULL)
    {
        LOG_E("pending completions not allocated");
        return;
    }
    if (s_scheduler.pending_count >= s_scheduler.pending_capacity)
    {
        LOG_E("pending completion credit invariant violated");
        return;
    }
    ble_tx_scheduler_pending_t *entry =
        &s_scheduler.pending[s_scheduler.pending_count];

    entry->kind = kind;
    entry->identity = *identity;
    entry->value_handle = value_handle;
    entry->status = status;
    entry->is_last = is_last;
    s_scheduler.pending_count++;
}

/**
 * @brief Complete and discard every frame still waiting in the ring.
 *
 * Caller holds the scheduler lock and has already entered a deferred
 * completion scope. A terminal scheduler failure must not silently lose
 * submitted frame identities.
 */
static void _ble_tx_scheduler_drop_queued_locked(esp_err_t status)
{
    while (s_scheduler.count > 0U)
    {
        ble_tx_scheduler_frame_t *frame = s_scheduler.frames[s_scheduler.head];

        if (frame != NULL)
        {
            _ble_tx_scheduler_queue_completion(
                frame->kind, &frame->identity, frame->value_handle,
                status, frame->is_last);
        }
        s_scheduler.head = (s_scheduler.head + 1U) %
                           s_scheduler.config->queue_depth;
        s_scheduler.count--;
    }
    s_scheduler.head = 0U;
}

/**
 * @brief Complete queued frames owned by one failed service flow.
 *
 * Frames from other flows and untracked best-effort notifications retain
 * their order. A zero flow id has no transaction ownership and therefore
 * retires only the in-flight frame.
 */
static void _ble_tx_scheduler_drop_flow_locked(
    uint32_t flow_id, esp_err_t status)
{
    if (flow_id == 0U || s_scheduler.count == 0U)
    {
        return;
    }
    const size_t original_count = s_scheduler.count;
    size_t kept = 0U;

    for (size_t i = 0U; i < original_count; ++i)
    {
        const size_t source = (s_scheduler.head + i) %
                              s_scheduler.config->queue_depth;
        ble_tx_scheduler_frame_t *frame = s_scheduler.frames[source];

        if (frame != NULL && frame->identity.flow_id == flow_id)
        {
            _ble_tx_scheduler_queue_completion(
                frame->kind, &frame->identity, frame->value_handle,
                status, frame->is_last);
            continue;
        }
        const size_t destination = (s_scheduler.head + kept) %
                                   s_scheduler.config->queue_depth;

        if (destination != source)
        {
            ble_tx_scheduler_frame_t *swap = s_scheduler.frames[destination];

            s_scheduler.frames[destination] = frame;
            s_scheduler.frames[source] = swap;
        }
        kept++;
    }
    s_scheduler.count = kept;
    if (kept == 0U)
    {
        s_scheduler.head = 0U;
    }
}

/**
 * @brief Deliver queued completions; caller does not hold the lock.
 *
 * Runs only at the outermost level (depth == 0) and only once at a time
 * (draining flag), so the completion callback never runs inside the port ops
 * call and concurrent drainers cannot reorder deliveries. Each delivery runs
 * the callback outside the lock. The draining flag is cleared in the same
 * lock hold as the empty check, so a producer that enqueued while the flag
 * was set either finds it cleared and drains, or has its entry picked up by
 * this loop.
 *
 * The completion callback must not submit frames synchronously; the session
 * layer defers submissions to its worker (matching the refactor architecture
 * where session handlers never run inside host callbacks). Under that
 * contract the pending buffer (queue_depth + 1, pre-allocated at init) is a
 * strict bound: one submit chain can complete at most that many frames.
 */
static void _ble_tx_scheduler_drain_completions(void)
{
    _ble_tx_scheduler_lock();
    if (s_scheduler.config == NULL || s_scheduler.depth != 0U ||
            s_scheduler.draining)
    {
        _ble_tx_scheduler_unlock();
        return;
    }
    s_scheduler.draining = true;
    _ble_tx_scheduler_unlock();

    for (;;)
    {
        ble_tx_scheduler_pending_t entry;
        ble_tx_scheduler_completion_cb_t completed = NULL;
        void *completed_arg = NULL;
        bool have_entry = false;

        _ble_tx_scheduler_lock();
        if (s_scheduler.config == NULL)
        {
            s_scheduler.draining = false;
            _ble_tx_scheduler_unlock();
            return;
        }
        if (s_scheduler.pending_count > 0U)
        {
            entry = s_scheduler.pending[0];
            completed = s_scheduler.config->completed;
            completed_arg = s_scheduler.config->completed_arg;
            memmove(&s_scheduler.pending[0], &s_scheduler.pending[1],
                    (s_scheduler.pending_count - 1U) *
                    sizeof(s_scheduler.pending[0]));
            s_scheduler.pending_count--;
            have_entry = true;
        }
        if (!have_entry)
        {
            s_scheduler.draining = false;
            _ble_tx_scheduler_unlock();
            return;
        }
        _ble_tx_scheduler_unlock();
        if (completed != NULL)
        {
            ble_tx_scheduler_result_t result;

            memset(&result, 0, sizeof(result));
            result.identity = entry.identity;
            result.kind = entry.kind;
            result.conn_handle = entry.identity.conn_handle;
            result.value_handle = entry.value_handle;
            result.flow_id = entry.identity.flow_id;
            result.token = entry.identity.token;
            result.status = entry.status;
            result.is_last = entry.is_last;
            completed(&result, completed_arg);
        }
    }
}

/**
 * @brief Send queued frames; caller holds the lock.
 *
 * Each frame is copied into dedicated in-flight storage and its metadata
 * into locals before the lock is released, so the ring slot is immediately
 * reusable and a concurrent submit can never overwrite the transmission. The
 * port fires NOTIFY_TX synchronously from inside the ops call: for
 * notifications the event completes the frame (and the re-entrant handler
 * advances the queue), for indications it reports SENT only and CONFIRMED
 * arrives later. The frame token distinguishes this frame from any frame a
 * re-entrant path may have started afterwards.
 *
 * When the ops call fails or succeeds without a synchronous event (host-test
 * fakes) and the in-flight frame is still ours, it is completed locally and
 * the loop continues; the recursion depth is bounded by the queue depth.
 */
#ifdef UNIT_TEST_HOST
void ble_tx_scheduler_test_set_token(uint32_t value)
{
    _ble_tx_scheduler_lock();
    s_scheduler.token_counter = value;
    _ble_tx_scheduler_unlock();
}
#endif

static esp_err_t _ble_tx_scheduler_send_head(void)
{
    ble_tx_scheduler_frame_t *frame;
    ble_tx_scheduler_kind_t kind;
    ble_link_operation_identity_t identity;
    uint16_t conn;
    uint16_t attr;
    uint32_t flow_id;
    size_t len;
    bool is_last;
    esp_err_t first_error = ESP_OK;

    for (;;)
    {
        if (s_scheduler.count == 0U || s_scheduler.in_flight ||
                s_scheduler.port_call_active)
        {
            return first_error;
        }
        frame = s_scheduler.frames[s_scheduler.head];
        kind = frame->kind;
        identity = frame->identity;
        conn = identity.conn_handle;
        attr = frame->value_handle;
        flow_id = identity.flow_id;
        len = frame->len;
        is_last = frame->is_last;
        memcpy(s_scheduler.in_flight_data, frame->data, len);
        s_scheduler.in_flight_kind = kind;
        s_scheduler.in_flight_identity = identity;
        s_scheduler.in_flight_attr = attr;
        s_scheduler.in_flight_is_last = is_last;
        if (identity.token == 0U)
        {
            /* Submission rejects token exhaustion, so this is an internal
             * identity invariant failure. Complete the affected frame and
             * continue with independent work. */
            s_scheduler.head = (s_scheduler.head + 1U) %
                               s_scheduler.config->queue_depth;
            s_scheduler.count--;
            _ble_tx_scheduler_queue_completion(
                kind, &identity, attr, ESP_ERR_INVALID_STATE, is_last);
            if (kind == BLE_TX_SCHEDULER_KIND_INDICATE)
            {
                _ble_tx_scheduler_drop_flow_locked(
                    flow_id, ESP_ERR_INVALID_STATE);
            }
            memset(&s_scheduler.in_flight_identity, 0,
                   sizeof(s_scheduler.in_flight_identity));
            if (first_error == ESP_OK)
            {
                first_error = ESP_ERR_INVALID_STATE;
            }
            continue;
        }
        s_scheduler.in_flight = true;
        s_scheduler.port_call_active = true;
        s_scheduler.head = (s_scheduler.head + 1U) %
                           s_scheduler.config->queue_depth;
        s_scheduler.count--;

        _ble_tx_scheduler_unlock();
        if (kind == BLE_TX_SCHEDULER_KIND_INDICATE)
        {
            const esp_err_t result = s_scheduler.config->ops->indicate(
                                         conn, attr,
                                         s_scheduler.in_flight_data, len);

            _ble_tx_scheduler_lock();
            s_scheduler.port_call_active = false;
            const bool ours = s_scheduler.in_flight &&
                              ble_link_operation_identity_equal(
                                  &s_scheduler.in_flight_identity,
                                  &identity);

            if (result != ESP_OK && ours)
            {
                s_scheduler.in_flight = false;
                memset(&s_scheduler.in_flight_identity, 0,
                       sizeof(s_scheduler.in_flight_identity));
                _ble_tx_scheduler_queue_completion(
                    kind, &identity, attr, result, is_last);
                _ble_tx_scheduler_drop_flow_locked(flow_id, result);
                if (first_error == ESP_OK)
                {
                    first_error = result;
                }
                continue;
            }
            if (result != ESP_OK && first_error == ESP_OK)
            {
                /* A synchronous terminal event already completed this token;
                 * preserve exactly-once completion and only surface the
                 * contradictory port return to the initiating caller. */
                first_error = result;
            }
            continue;
        }
        const esp_err_t result = s_scheduler.config->ops->notify(
                                     conn, attr, s_scheduler.in_flight_data,
                                     len);
        _ble_tx_scheduler_lock();
        s_scheduler.port_call_active = false;
        const bool ours = s_scheduler.in_flight &&
                          ble_link_operation_identity_equal(
                              &s_scheduler.in_flight_identity, &identity);

        if (ours)
        {
            s_scheduler.in_flight = false;
            memset(&s_scheduler.in_flight_identity, 0,
                   sizeof(s_scheduler.in_flight_identity));
            _ble_tx_scheduler_queue_completion(
                kind, &identity, attr, result, is_last);
        }
        if (result != ESP_OK && first_error == ESP_OK)
        {
            first_error = result;
        }
    }
}

esp_err_t ble_tx_scheduler_init(const ble_tx_scheduler_config_t *config)
{
    if (config == NULL || config->queue_depth == 0U ||
            config->max_frame_bytes == 0U || config->ops == NULL ||
            config->ops->notify == NULL || config->ops->indicate == NULL ||
            config->queue_depth == SIZE_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_scheduler, 0, sizeof(s_scheduler));
    s_scheduler.config = config;
    s_scheduler.pending_capacity = config->queue_depth + 1U;
    s_scheduler.pending = calloc(s_scheduler.pending_capacity,
                                 sizeof(s_scheduler.pending[0]));
    if (s_scheduler.pending == NULL)
    {
        s_scheduler.config = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ble_tx_scheduler_deinit(void)
{
    void (*unlock_cb)(void *) = s_scheduler.config != NULL
                                ? s_scheduler.config->unlock
                                : NULL;
    void *unlock_arg = s_scheduler.config != NULL
                       ? s_scheduler.config->lock_arg
                       : NULL;

    _ble_tx_scheduler_lock();
    if (s_scheduler.config == NULL)
    {
        _ble_tx_scheduler_unlock();
        return;
    }
    /* Refuse new submissions, then complete every outstanding frame and
     * every already-generated pending completion before any buffer is
     * freed: a submitted frame identity must never vanish silently. The
     * completion callback runs in the drain below, outside the lock. */
    s_scheduler.deinitializing = true;
    if (s_scheduler.in_flight)
    {
        s_scheduler.depth++;
        _ble_tx_scheduler_queue_completion(
            s_scheduler.in_flight_kind, &s_scheduler.in_flight_identity,
            s_scheduler.in_flight_attr, ESP_ERR_INVALID_STATE,
            s_scheduler.in_flight_is_last);
        s_scheduler.depth--;
        s_scheduler.in_flight = false;
        memset(&s_scheduler.in_flight_identity, 0,
               sizeof(s_scheduler.in_flight_identity));
    }
    s_scheduler.depth++;
    _ble_tx_scheduler_drop_queued_locked(ESP_ERR_INVALID_STATE);
    s_scheduler.depth--;
    _ble_tx_scheduler_unlock();
    _ble_tx_scheduler_drain_completions();
    _ble_tx_scheduler_lock();
    if (s_scheduler.config != NULL && s_scheduler.frames != NULL)
    {
        for (size_t i = 0U; i < s_scheduler.config->queue_depth; ++i)
        {
            free(s_scheduler.frames[i]);
            s_scheduler.frames[i] = NULL;
        }
        free(s_scheduler.frames);
        s_scheduler.frames = NULL;
    }
    free(s_scheduler.in_flight_data);
    s_scheduler.in_flight_data = NULL;
    free(s_scheduler.pending);
    s_scheduler.pending = NULL;
    s_scheduler.config = NULL;
    s_scheduler.count = 0U;
    s_scheduler.head = 0U;
    s_scheduler.pending_count = 0U;
    s_scheduler.pending_capacity = 0U;
    s_scheduler.in_flight = false;
    s_scheduler.depth = 0U;
    s_scheduler.deinitializing = false;
    _ble_tx_scheduler_unlock();
    if (unlock_cb != NULL)
    {
        unlock_cb(unlock_arg);
    }
}

esp_err_t ble_tx_scheduler_submit(
    ble_tx_scheduler_kind_t kind,
    const ble_link_operation_identity_t *identity,
    uint16_t value_handle, const uint8_t *data, size_t len,
    bool is_last)
{
    ble_tx_scheduler_frame_t *frame;
    size_t slot;
    esp_err_t result = ESP_OK;

    if ((kind != BLE_TX_SCHEDULER_KIND_NOTIFY &&
            kind != BLE_TX_SCHEDULER_KIND_INDICATE) || identity == NULL ||
            identity->generation == 0U || identity->token != 0U ||
            identity->kind != _ble_tx_scheduler_operation_kind(kind))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (data == NULL || len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_tx_scheduler_lock();
    if (s_scheduler.config == NULL)
    {
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scheduler.deinitializing)
    {
        /* Teardown accepted the outstanding queue already; new work is
         * refused until the next init. */
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (len > s_scheduler.config->max_frame_bytes)
    {
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    const size_t credits_in_use = s_scheduler.pending_count +
                                  s_scheduler.count +
                                  (s_scheduler.in_flight ? 1U : 0U);

    if (credits_in_use >= s_scheduler.pending_capacity ||
            s_scheduler.count >= s_scheduler.config->queue_depth)
    {
        _ble_tx_scheduler_unlock();
        return ESP_ERR_NO_MEM;
    }
    if (s_scheduler.token_counter >= UINT32_MAX - 1U ||
            s_scheduler.count >=
            (size_t)(UINT32_MAX - 1U - s_scheduler.token_counter))
    {
        /* The token allocator never wraps: submissions that cannot be
         * assigned a fresh token are rejected (fail closed). */
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scheduler.frames == NULL)
    {
        s_scheduler.frames = calloc(s_scheduler.config->queue_depth,
                                    sizeof(s_scheduler.frames[0]));
        if (s_scheduler.frames == NULL)
        {
            _ble_tx_scheduler_unlock();
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_scheduler.in_flight_data == NULL)
    {
        s_scheduler.in_flight_data = _ble_tx_scheduler_payload_alloc(
                                         s_scheduler.config->max_frame_bytes);
        if (s_scheduler.in_flight_data == NULL)
        {
            _ble_tx_scheduler_unlock();
            return ESP_ERR_NO_MEM;
        }
    }
    slot = (s_scheduler.head + s_scheduler.count) %
           s_scheduler.config->queue_depth;
    frame = s_scheduler.frames[slot];
    if (frame == NULL)
    {
        frame = _ble_tx_scheduler_payload_alloc(
                    sizeof(ble_tx_scheduler_frame_t) +
                    s_scheduler.config->max_frame_bytes - 1U);
        if (frame == NULL)
        {
            _ble_tx_scheduler_unlock();
            return ESP_ERR_NO_MEM;
        }
        s_scheduler.frames[slot] = frame;
    }
    s_scheduler.token_counter++;
    frame->kind = kind;
    frame->identity = *identity;
    frame->identity.token = s_scheduler.token_counter;
    frame->value_handle = value_handle;
    frame->len = len;
    frame->is_last = is_last;
    memcpy(frame->data, data, len);
    s_scheduler.count++;
    s_scheduler.depth++;
    result = _ble_tx_scheduler_send_head();
    s_scheduler.depth--;
    _ble_tx_scheduler_unlock();
    _ble_tx_scheduler_drain_completions();
    return result;
}

esp_err_t ble_tx_scheduler_handle_notify_tx(const ble_port_event_t *event)
{
    esp_err_t status;
    ble_tx_scheduler_kind_t done_kind;
    ble_link_operation_identity_t done_identity;
    uint16_t done_attr;
    bool done_is_last;

    if (event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->type != BLE_PORT_EVENT_NOTIFY_TX)
    {
        return ESP_OK;
    }
    _ble_tx_scheduler_lock();
    if (s_scheduler.config == NULL)
    {
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_scheduler.in_flight ||
            !ble_link_operation_identity_equal(
                &s_scheduler.in_flight_identity, &event->identity) ||
            event->conn_handle != event->identity.conn_handle ||
            s_scheduler.in_flight_attr != event->attr_handle ||
            (s_scheduler.in_flight_kind == BLE_TX_SCHEDULER_KIND_INDICATE) !=
            event->indication)
    {
        /* The event does not belong to the in-flight frame. */
        _ble_tx_scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (s_scheduler.in_flight_kind == BLE_TX_SCHEDULER_KIND_INDICATE &&
            event->tx_result == BLE_PORT_TX_SENT)
    {
        /* Indications report SENT synchronously and CONFIRMED later. */
        _ble_tx_scheduler_unlock();
        return ESP_OK;
    }
    if (event->tx_result == BLE_PORT_TX_SENT ||
            event->tx_result == BLE_PORT_TX_CONFIRMED)
    {
        status = ESP_OK;
    }
    else if (event->tx_result == BLE_PORT_TX_TIMEOUT)
    {
        status = ESP_ERR_TIMEOUT;
    }
    else
    {
        status = ESP_FAIL;
    }
    done_kind = s_scheduler.in_flight_kind;
    done_identity = s_scheduler.in_flight_identity;
    done_attr = s_scheduler.in_flight_attr;
    done_is_last = s_scheduler.in_flight_is_last;
    s_scheduler.in_flight = false;
    memset(&s_scheduler.in_flight_identity, 0,
           sizeof(s_scheduler.in_flight_identity));
    s_scheduler.depth++;
    _ble_tx_scheduler_queue_completion(
        done_kind, &done_identity, done_attr, status,
        done_is_last);
    if (status != ESP_OK && done_kind == BLE_TX_SCHEDULER_KIND_INDICATE)
    {
        /* An indication failure retires only its response flow. Independent
         * notifications remain best effort and other epochs may re-handshake
         * on the same ACL. */
        _ble_tx_scheduler_drop_flow_locked(done_identity.flow_id, status);
    }
    (void)_ble_tx_scheduler_send_head();
    s_scheduler.depth--;
    _ble_tx_scheduler_unlock();
    _ble_tx_scheduler_drain_completions();
    return ESP_OK;
}

void ble_tx_scheduler_reset(void)
{
    _ble_tx_scheduler_lock();
    if (s_scheduler.config == NULL)
    {
        _ble_tx_scheduler_unlock();
        return;
    }
    /* Complete the in-flight frame AND every queued frame with the
     * teardown status, so N submitted frames always produce N
     * completions. */
    if (s_scheduler.in_flight)
    {
        s_scheduler.depth++;
        _ble_tx_scheduler_queue_completion(
            s_scheduler.in_flight_kind, &s_scheduler.in_flight_identity,
            s_scheduler.in_flight_attr, ESP_ERR_INVALID_STATE,
            s_scheduler.in_flight_is_last);
        s_scheduler.depth--;
        s_scheduler.in_flight = false;
        memset(&s_scheduler.in_flight_identity, 0,
               sizeof(s_scheduler.in_flight_identity));
    }
    s_scheduler.depth++;
    _ble_tx_scheduler_drop_queued_locked(ESP_ERR_INVALID_STATE);
    s_scheduler.depth--;
    _ble_tx_scheduler_unlock();
    _ble_tx_scheduler_drain_completions();
}

static esp_err_t _ble_tx_scheduler_fail_in_flight(esp_err_t status)
{
    ble_tx_scheduler_kind_t kind = BLE_TX_SCHEDULER_KIND_NOTIFY;
    ble_link_operation_identity_t identity;
    uint16_t attr = 0U;
    bool is_last = false;

    memset(&identity, 0, sizeof(identity));

    if (s_scheduler.in_flight)
    {
        kind = s_scheduler.in_flight_kind;
        identity = s_scheduler.in_flight_identity;
        attr = s_scheduler.in_flight_attr;
        is_last = s_scheduler.in_flight_is_last;
        s_scheduler.in_flight = false;
        memset(&s_scheduler.in_flight_identity, 0,
               sizeof(s_scheduler.in_flight_identity));
        s_scheduler.depth++;
        _ble_tx_scheduler_queue_completion(
            kind, &identity, attr, status, is_last);
        s_scheduler.depth--;
    }
    if (kind == BLE_TX_SCHEDULER_KIND_INDICATE)
    {
        /* The framing contract ends this response flow, not the scheduler or
         * unrelated best-effort notifications. */
        s_scheduler.depth++;
        _ble_tx_scheduler_drop_flow_locked(identity.flow_id, status);
        s_scheduler.depth--;
    }
    s_scheduler.depth++;
    (void)_ble_tx_scheduler_send_head();
    s_scheduler.depth--;
    return ESP_OK;
}

esp_err_t ble_tx_scheduler_handle_indication_timeout(uint32_t token)
{
    esp_err_t result;

    _ble_tx_scheduler_lock();
    if (s_scheduler.config == NULL)
    {
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_scheduler.in_flight ||
            s_scheduler.in_flight_kind != BLE_TX_SCHEDULER_KIND_INDICATE)
    {
        /* Nothing in flight, or a notification: no timeout applies. */
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (token != 0U && s_scheduler.in_flight_identity.token != token)
    {
        /* A late timer from a previous indication is ignored. */
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    result = _ble_tx_scheduler_fail_in_flight(ESP_ERR_TIMEOUT);
    _ble_tx_scheduler_unlock();
    _ble_tx_scheduler_drain_completions();
    return result;
}

uint32_t ble_tx_scheduler_get_in_flight_token(void)
{
    uint32_t token;

    _ble_tx_scheduler_lock();
    token = s_scheduler.in_flight ? s_scheduler.in_flight_identity.token : 0U;
    _ble_tx_scheduler_unlock();
    return token;
}

esp_err_t ble_tx_scheduler_get_in_flight_identity(
    ble_link_operation_identity_t *identity)
{
    if (identity == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_tx_scheduler_lock();
    if (!s_scheduler.in_flight)
    {
        memset(identity, 0, sizeof(*identity));
        _ble_tx_scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    *identity = s_scheduler.in_flight_identity;
    _ble_tx_scheduler_unlock();
    return ESP_OK;
}

bool ble_tx_scheduler_is_busy(void)
{
    bool busy;

    _ble_tx_scheduler_lock();
    busy = s_scheduler.in_flight || s_scheduler.count > 0U;
    _ble_tx_scheduler_unlock();
    return busy;
}
