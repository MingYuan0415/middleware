#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_tx_scheduler.h"

#define DBG_TAG "ble_tx_scheduler"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

typedef struct ble_tx_scheduler_frame
{
    ble_tx_scheduler_kind_t kind;
    uint16_t conn_handle;
    uint16_t value_handle;
    size_t len;
    bool is_last;
    uint8_t data[1];
} ble_tx_scheduler_frame_t;

typedef struct ble_tx_scheduler_pending
{
    ble_tx_scheduler_kind_t kind;
    uint16_t conn_handle;
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
    uint32_t in_flight_token;
    bool in_flight_is_last;
    bool in_flight;
    bool sync_event_seen;
    ble_tx_scheduler_kind_t in_flight_kind;
    uint16_t in_flight_conn;
    uint16_t in_flight_attr;
    esp_err_t fatal_error;
    bool transaction_aborted;
    size_t in_flight_len;
    uint8_t *in_flight_data;
    unsigned int depth;
    bool draining;
    ble_tx_scheduler_pending_t *pending;
    size_t pending_count;
    size_t pending_capacity;
} ble_tx_scheduler_t;

static ble_tx_scheduler_t s_scheduler;

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
 * pending buffer is pre-allocated for the worst case (one chain of
 * queue_depth + 1 frames), so a full buffer is an internal invariant
 * violation, not an allocation failure.
 */
static void _ble_tx_scheduler_queue_completion(
    ble_tx_scheduler_kind_t kind, uint16_t conn_handle,
    uint16_t value_handle, esp_err_t status)
{
    if (s_scheduler.depth == 0U && !s_scheduler.draining &&
            s_scheduler.pending_count == 0U &&
            s_scheduler.config->completed != NULL)
    {
        ble_tx_scheduler_result_t result;

        result.kind = kind;
        result.conn_handle = conn_handle;
        result.value_handle = value_handle;
        result.status = status;
        result.is_last = s_scheduler.in_flight_is_last;
        s_scheduler.config->completed(&result, s_scheduler.config->completed_arg);
        return;
    }
    if (s_scheduler.pending == NULL)
    {
        LOG_E("pending completions not allocated");
        return;
    }
    if (s_scheduler.pending_count >= s_scheduler.pending_capacity)
    {
        LOG_E("pending completion overflow");
        return;
    }
    ble_tx_scheduler_pending_t *entry =
        &s_scheduler.pending[s_scheduler.pending_count];

    entry->kind = kind;
    entry->conn_handle = conn_handle;
    entry->value_handle = value_handle;
    entry->status = status;
    entry->is_last = s_scheduler.in_flight_is_last;
    s_scheduler.pending_count++;
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
        if (s_scheduler.config->completed != NULL)
        {
            ble_tx_scheduler_result_t result;

            result.kind = entry.kind;
            result.conn_handle = entry.conn_handle;
            result.value_handle = entry.value_handle;
            result.status = entry.status;
            result.is_last = entry.is_last;
            s_scheduler.config->completed(&result, s_scheduler.config->completed_arg);
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

static void _ble_tx_scheduler_send_head(void)
{
    ble_tx_scheduler_frame_t *frame;
    ble_tx_scheduler_kind_t kind;
    uint16_t conn;
    uint16_t attr;
    size_t len;
    uint32_t token;
    esp_err_t result;

    for (;;)
    {
        if (s_scheduler.count == 0U || s_scheduler.in_flight)
        {
            return;
        }
        frame = s_scheduler.frames[s_scheduler.head];
        kind = frame->kind;
        conn = frame->conn_handle;
        attr = frame->value_handle;
        len = frame->len;
        memcpy(s_scheduler.in_flight_data, frame->data, len);
        s_scheduler.in_flight_kind = kind;
        s_scheduler.in_flight_conn = conn;
        s_scheduler.in_flight_attr = attr;
        s_scheduler.in_flight_len = len;
        s_scheduler.in_flight_is_last = frame->is_last;
        if (s_scheduler.token_counter >= UINT32_MAX)
        {
            /* The token allocator never wraps: exhausted frames are
             * rejected (fail closed). */
            s_scheduler.head = (s_scheduler.head + 1U) %
                               s_scheduler.config->queue_depth;
            s_scheduler.count--;
            return;
        }
        s_scheduler.token_counter++;
        token = s_scheduler.token_counter;
        s_scheduler.in_flight_token = token;
        s_scheduler.in_flight = true;
        s_scheduler.sync_event_seen = false;
        s_scheduler.head = (s_scheduler.head + 1U) %
                           s_scheduler.config->queue_depth;
        s_scheduler.count--;

        _ble_tx_scheduler_unlock();
        if (kind == BLE_TX_SCHEDULER_KIND_INDICATE)
        {
            result = s_scheduler.config->ops->indicate(
                         conn, attr, s_scheduler.in_flight_data, len);
        }
        else
        {
            result = s_scheduler.config->ops->notify(
                         conn, attr, s_scheduler.in_flight_data, len);
        }
        _ble_tx_scheduler_lock();

        /* A synchronous event completed this frame (or left an indication
         * waiting for confirmation) and the re-entrant handler already
         * advanced the queue, so re-check the loop conditions. Only when no
         * event fired (host-test fakes) and the in-flight token still matches
         * ours is the frame completed locally. */
        if (s_scheduler.sync_event_seen)
        {
            continue;
        }
        if (s_scheduler.in_flight &&
                s_scheduler.in_flight_token == token)
        {
            s_scheduler.in_flight = false;
            s_scheduler.in_flight_token = 0U;
            _ble_tx_scheduler_queue_completion(kind, conn, attr, result);
            if (result != ESP_OK)
            {
                /* A failed transmission ends the transaction: nothing
                 * further is sent and later submissions fail closed. */
                s_scheduler.head = 0U;
                s_scheduler.count = 0U;
                s_scheduler.fatal_error = result;
                continue;
            }
        }
    }
}

esp_err_t ble_tx_scheduler_init(const ble_tx_scheduler_config_t *config)
{
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

    if (s_scheduler.config != NULL && s_scheduler.config->lock != NULL)
    {
        s_scheduler.config->lock(s_scheduler.config->lock_arg);
    }
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
    s_scheduler.in_flight = false;
    s_scheduler.depth = 0U;
    if (unlock_cb != NULL)
    {
        unlock_cb(unlock_arg);
    }
}

esp_err_t ble_tx_scheduler_submit(
    ble_tx_scheduler_kind_t kind, uint16_t conn_handle,
    uint16_t value_handle, const uint8_t *data, size_t len,
    bool is_last)
{
    ble_tx_scheduler_frame_t *frame;
    size_t slot;
    esp_err_t result = ESP_OK;

    if (kind != BLE_TX_SCHEDULER_KIND_NOTIFY &&
            kind != BLE_TX_SCHEDULER_KIND_INDICATE)
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
    if (s_scheduler.fatal_error != ESP_OK)
    {
        /* A previous transmission failed: the transaction is over and the
         * session must be closed by the caller. */
        const esp_err_t fatal = s_scheduler.fatal_error;

        _ble_tx_scheduler_unlock();
        return fatal;
    }
    if (s_scheduler.transaction_aborted)
    {
        /* The transaction was terminated by a timeout or error: further
         * submissions fail closed. */
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (len > s_scheduler.config->max_frame_bytes)
    {
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (s_scheduler.count >= s_scheduler.config->queue_depth)
    {
        _ble_tx_scheduler_unlock();
        return ESP_ERR_NO_MEM;
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
        s_scheduler.in_flight_data = malloc(s_scheduler.config->max_frame_bytes);
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
        frame = malloc(sizeof(ble_tx_scheduler_frame_t) +
                       s_scheduler.config->max_frame_bytes - 1U);
        if (frame == NULL)
        {
            _ble_tx_scheduler_unlock();
            return ESP_ERR_NO_MEM;
        }
        s_scheduler.frames[slot] = frame;
    }
    if (s_scheduler.token_counter >= UINT32_MAX ||
            s_scheduler.token_counter + s_scheduler.count + 1U >=
            UINT32_MAX)
    {
        /* The token allocator never wraps: submissions that cannot be
         * assigned a fresh token are rejected (fail closed). */
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    frame->kind = kind;
    frame->conn_handle = conn_handle;
    frame->value_handle = value_handle;
    frame->len = len;
    frame->is_last = is_last;
    memcpy(frame->data, data, len);
    s_scheduler.count++;
    s_scheduler.depth++;
    _ble_tx_scheduler_send_head();
    s_scheduler.depth--;
    /* A synchronous transmission failure is returned to the caller of the
     * submit that caused it, so a single-fragment transaction fails
     * immediately and the session is closed. */
    if (s_scheduler.fatal_error != ESP_OK)
    {
        result = s_scheduler.fatal_error;
    }
    _ble_tx_scheduler_unlock();
    _ble_tx_scheduler_drain_completions();
    return result;
}

esp_err_t ble_tx_scheduler_handle_notify_tx(const ble_port_event_t *event)
{
    esp_err_t status;
    ble_tx_scheduler_kind_t done_kind;
    uint16_t done_conn;
    uint16_t done_attr;

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
            s_scheduler.in_flight_conn != event->conn_handle ||
            s_scheduler.in_flight_attr != event->attr_handle ||
            (s_scheduler.in_flight_kind == BLE_TX_SCHEDULER_KIND_INDICATE) !=
            event->indication)
    {
        /* The event does not belong to the in-flight frame. */
        _ble_tx_scheduler_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    s_scheduler.sync_event_seen = true;
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
    done_conn = s_scheduler.in_flight_conn;
    done_attr = s_scheduler.in_flight_attr;
    s_scheduler.in_flight = false;
    s_scheduler.in_flight_token = 0U;
    s_scheduler.depth++;
    _ble_tx_scheduler_queue_completion(done_kind, done_conn, done_attr, status);
    if (status == ESP_ERR_TIMEOUT || status == ESP_FAIL)
    {
        /* A timeout or a failed transmission ends the transaction: the
         * queued fragments are dropped and nothing further is sent. */
        s_scheduler.head = 0U;
        s_scheduler.count = 0U;
        s_scheduler.transaction_aborted = true;
    }
    else
    {
        _ble_tx_scheduler_send_head();
    }
    s_scheduler.depth--;
    /* A synchronous failure of a queued fragment (sent from this event)
     * is surfaced to the caller so the session is closed. */
    if (status == ESP_OK && s_scheduler.fatal_error != ESP_OK)
    {
        const esp_err_t fatal = s_scheduler.fatal_error;

        _ble_tx_scheduler_unlock();
        _ble_tx_scheduler_drain_completions();
        return fatal;
    }
    _ble_tx_scheduler_unlock();
    _ble_tx_scheduler_drain_completions();
    return ESP_OK;
}

void ble_tx_scheduler_reset(void)
{
    ble_tx_scheduler_kind_t kind = BLE_TX_SCHEDULER_KIND_NOTIFY;
    uint16_t conn = 0U;
    uint16_t attr = 0U;
    bool complete_pending = false;

    _ble_tx_scheduler_lock();
    if (s_scheduler.config == NULL)
    {
        _ble_tx_scheduler_unlock();
        return;
    }
    if (s_scheduler.in_flight)
    {
        kind = s_scheduler.in_flight_kind;
        conn = s_scheduler.in_flight_conn;
        attr = s_scheduler.in_flight_attr;
        s_scheduler.in_flight = false;
        s_scheduler.in_flight_token = 0U;
        complete_pending = true;
    }
    s_scheduler.head = 0U;
    s_scheduler.count = 0U;
    s_scheduler.fatal_error = ESP_OK;
    s_scheduler.transaction_aborted = false;
    if (complete_pending)
    {
        s_scheduler.depth++;
        _ble_tx_scheduler_queue_completion(kind, conn, attr,
                                           ESP_ERR_INVALID_STATE);
        s_scheduler.depth--;
    }
    _ble_tx_scheduler_unlock();
    _ble_tx_scheduler_drain_completions();
}

static esp_err_t _ble_tx_scheduler_fail_in_flight(
    esp_err_t status, bool drop_queue)
{
    ble_tx_scheduler_kind_t kind = BLE_TX_SCHEDULER_KIND_NOTIFY;
    uint16_t conn = 0U;
    uint16_t attr = 0U;

    if (s_scheduler.in_flight)
    {
        kind = s_scheduler.in_flight_kind;
        conn = s_scheduler.in_flight_conn;
        attr = s_scheduler.in_flight_attr;
        s_scheduler.in_flight = false;
        s_scheduler.in_flight_token = 0U;
        s_scheduler.depth++;
        _ble_tx_scheduler_queue_completion(kind, conn, attr, status);
        s_scheduler.depth--;
    }
    if (drop_queue)
    {
        /* The framing contract ends the whole transaction: the queued
         * fragments are dropped and further submissions fail closed. */
        s_scheduler.head = 0U;
        s_scheduler.count = 0U;
        s_scheduler.transaction_aborted = true;
    }
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
    if (token != 0U && s_scheduler.in_flight_token != token)
    {
        /* A late timer from a previous indication is ignored. */
        _ble_tx_scheduler_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    result = _ble_tx_scheduler_fail_in_flight(ESP_ERR_TIMEOUT, true);
    _ble_tx_scheduler_unlock();
    _ble_tx_scheduler_drain_completions();
    return result;
}

uint32_t ble_tx_scheduler_get_in_flight_token(void)
{
    uint32_t token;

    _ble_tx_scheduler_lock();
    token = s_scheduler.in_flight ? s_scheduler.in_flight_token : 0U;
    _ble_tx_scheduler_unlock();
    return token;
}

bool ble_tx_scheduler_is_busy(void)
{
    bool busy;

    _ble_tx_scheduler_lock();
    busy = s_scheduler.in_flight || s_scheduler.count > 0U;
    _ble_tx_scheduler_unlock();
    return busy;
}
