#define DBG_TAG "ble_smp_io"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

#include "ble_nimble_smp_io.h"

#include <string.h>

static bool _smp_same(const ble_nimble_smp_io_tuple_t *a,
                      const ble_nimble_smp_io_tuple_t *b)
{
    return a->generation == b->generation && a->conn_handle == b->conn_handle &&
           a->token == b->token;
}

static void _smp_lock(ble_nimble_smp_io_t *io)
{
    (void)xSemaphoreTake(io->lock, portMAX_DELAY);
}

static void _smp_unlock(ble_nimble_smp_io_t *io)
{
    (void)xSemaphoreGive(io->lock);
}

static bool _smp_work(const ble_nimble_smp_io_t *io)
{
    return io->cancel_pending || io->reply.phase == BLE_NIMBLE_SMP_REPLY_QUEUED ||
           io->reply.phase == BLE_NIMBLE_SMP_REPLY_CANCELLED ||
           io->reply.phase == BLE_NIMBLE_SMP_REPLY_EXECUTING;
}

static bool _smp_current(ble_nimble_smp_io_t *io,
                         const ble_nimble_smp_io_tuple_t *tuple)
{
    uint32_t generation = 0U;
    uint16_t handle = UINT16_MAX;
    return io->ops.host_ready(io->ops.arg) &&
           io->ops.current_connection(&generation, &handle, io->ops.arg) == ESP_OK &&
           generation == tuple->generation && handle == tuple->conn_handle;
}

/* The caller holds the helper lock; no SDK or service callback here. */
static void _smp_invalidate_reply(ble_nimble_smp_io_t *io,
                                  const ble_nimble_smp_io_tuple_t *tuple)
{
    if (!_smp_same(&io->reply.tuple, tuple))
    {
        return;
    }
    io->reply.restore_allowed = false;
    if (io->reply.phase == BLE_NIMBLE_SMP_REPLY_QUEUED)
    {
        io->reply.phase = BLE_NIMBLE_SMP_REPLY_CANCELLED;
        io->reply.result = ESP_ERR_INVALID_STATE;
    }
}

esp_err_t ble_nimble_smp_io_init(ble_nimble_smp_io_t *io,
                                 const ble_nimble_smp_io_ops_t *ops)
{
    if (io == NULL || ops == NULL || ops->is_host_current == NULL ||
            ops->wake_owner == NULL || ops->now_us == NULL ||
            ops->current_connection == NULL || ops->host_ready == NULL ||
            ops->inject == NULL || ops->publish_confirmation == NULL ||
            ops->retire_confirmation == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (io->initialized)
    {
        return ESP_OK;
    }
    if (io->lock == NULL)
    {
        io->lock = xSemaphoreCreateMutexStatic(&io->lock_storage);
    }
    if (io->ack == NULL)
    {
        io->ack = xSemaphoreCreateBinaryStatic(&io->ack_storage);
    }
    if (io->lock == NULL || io->ack == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    io->ops = *ops;
    io->sync_ready = true;
    io->draining = true;
    io->initialized = true;
    return ESP_OK;
}

void ble_nimble_smp_io_open(ble_nimble_smp_io_t *io)
{
    if (io == NULL || !io->initialized || !io->ops.host_ready(io->ops.arg))
    {
        return;
    }
    _smp_lock(io);
    io->draining = false;
    if (io->event_live || _smp_work(io))
    {
        io->open_requested = true;
    }
    else
    {
        io->accepting = true;
        io->open_requested = false;
    }
    _smp_unlock(io);
}

esp_err_t ble_nimble_smp_io_offer(ble_nimble_smp_io_t *io,
                                  uint32_t generation, uint16_t conn_handle,
                                  uint64_t token, uint32_t passkey)
{
    if (generation == 0U || conn_handle == UINT16_MAX || token == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (io == NULL || !io->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    _smp_lock(io);
    /* One prompt owns the single reply slot until its terminal state is
     * dispatched and consumed; never replace a queued or live tuple. */
    if (!io->accepting || io->offer_token_active || io->offer_pending ||
            io->reply.phase != BLE_NIMBLE_SMP_REPLY_IDLE ||
            io->cancel_pending)
    {
        _smp_unlock(io);
        return ESP_ERR_INVALID_STATE;
    }
    io->offer = (ble_nimble_smp_io_tuple_t)
    {
        generation, conn_handle, token
    };
    io->offer_pending = true;
    io->offer_token_active = true;
    io->offer_passkey = passkey;
    /* Publication may wake another task, which must wait for this commit lock.
     * Its service callback cannot enter the helper synchronously. */
    const esp_err_t result = io->ops.publish_confirmation(passkey, token,
                             io->ops.arg);
    if (result != ESP_OK)
    {
        io->offer_pending = false;
        io->offer_token_active = false;
    }
    _smp_unlock(io);
    (void)xSemaphoreGive(io->ack);
    if (result != ESP_OK)
    {
        io->ops.retire_confirmation(token, false, io->ops.arg);
    }
    return result;
}

static void _smp_finish_reply(ble_nimble_smp_io_t *io, esp_err_t result)
{
    io->reply.result = result;
    io->reply.phase = io->reply.waiter_attached ?
                      BLE_NIMBLE_SMP_REPLY_DONE : BLE_NIMBLE_SMP_REPLY_IDLE;
}

static void _smp_run_host(ble_nimble_smp_io_t *io, bool dispatched)
{
    if (io == NULL || !io->initialized || !io->ops.is_host_current(io->ops.arg))
    {
        return;
    }
    ble_nimble_smp_io_tuple_t cancel;
    _smp_lock(io);
    const bool have_cancel = io->cancel_pending;
    cancel = io->cancel;
    io->cancel_pending = false;
    _smp_unlock(io);
    if (have_cancel && _smp_current(io, &cancel))
    {
        _smp_lock(io);
        const bool reject = io->accepting && _smp_same(&io->offer, &cancel) &&
                            !io->offer_pending;
        _smp_unlock(io);
        if (reject)
        {
            (void)io->ops.inject(cancel.conn_handle, false, io->ops.arg);
        }
    }

    uint64_t retired_token = 0U;
    _smp_lock(io);
    if (io->reply.phase == BLE_NIMBLE_SMP_REPLY_CANCELLED)
    {
        if (io->offer_token_active &&
                _smp_same(&io->offer, &io->reply.tuple))
        {
            retired_token = io->offer.token;
            io->offer_token_active = false;
            io->offer_pending = false;
        }
        _smp_finish_reply(io, io->reply.result);
    }
    const bool queued = io->reply.phase == BLE_NIMBLE_SMP_REPLY_QUEUED;
    const ble_nimble_smp_io_tuple_t tuple = io->reply.tuple;
    const uint64_t seq = io->reply.seq;
    _smp_unlock(io);
    if (retired_token != 0U)
    {
        io->ops.retire_confirmation(retired_token, false, io->ops.arg);
        retired_token = 0U;
    }
    const bool current = queued && _smp_current(io, &tuple);
    bool execute = false;
    bool accept = false;
    _smp_lock(io);
    if (queued && io->reply.seq == seq &&
            io->reply.phase == BLE_NIMBLE_SMP_REPLY_QUEUED)
    {
        if (!current || !io->accepting || !io->offer_pending ||
                !_smp_same(&io->offer, &tuple))
        {
            if (io->offer_token_active &&
                    _smp_same(&io->offer, &tuple))
            {
                retired_token = io->offer.token;
                io->offer_token_active = false;
                io->offer_pending = false;
            }
            _smp_finish_reply(io, ESP_ERR_INVALID_STATE);
        }
        else if (io->ops.now_us(io->ops.arg) >= io->reply.deadline_us)
        {
            retired_token = io->offer_token_active ? io->offer.token : 0U;
            io->offer_token_active = false;
            io->offer_pending = false;
            _smp_finish_reply(io, ESP_ERR_TIMEOUT);
        }
        else
        {
            io->reply.phase = BLE_NIMBLE_SMP_REPLY_EXECUTING;
            io->offer_pending = false;
            accept = io->reply.accept;
            execute = true;
        }
    }
    _smp_unlock(io);
    if (retired_token != 0U)
    {
        io->ops.retire_confirmation(retired_token, false, io->ops.arg);
    }
    if (execute)
    {
        /* GAP and SMP are on this host context: no ACL replacement can occur
         * between final validation and this injection. No mutex crosses SDK. */
        const int injected = io->ops.inject(tuple.conn_handle, accept, io->ops.arg);
        const bool still_current = _smp_current(io, &tuple);
        bool token_active = false;
        bool restored = false;
        _smp_lock(io);
        token_active = io->offer_token_active;
        if (_smp_same(&io->offer, &tuple))
        {
            if (injected != 0 && still_current && io->accepting &&
                    io->reply.restore_allowed && io->offer_token_active)
            {
                io->offer_pending = true;
                restored = true;
            }
            else
            {
                io->offer_token_active = false;
            }
        }
        _smp_unlock(io);
        if (token_active && (injected == 0 || !restored))
        {
            io->ops.retire_confirmation(tuple.token, injected == 0 ? accept : false,
                                        io->ops.arg);
        }
        _smp_lock(io);
        _smp_finish_reply(io, injected == 0 ? ESP_OK : ESP_FAIL);
        _smp_unlock(io);
    }

    /* DONE is visible before ACK; no delayed ACK may hold the state lock. */
    (void)xSemaphoreGive(io->ack);
    const bool ready = io->ops.host_ready(io->ops.arg);
    _smp_lock(io);
    if (dispatched)
    {
        io->event_live = false;
    }
    if (io->open_requested && ready && !io->draining && !io->event_live &&
            !_smp_work(io))
    {
        io->accepting = true;
        io->open_requested = false;
    }
    const bool more = _smp_work(io);
    _smp_unlock(io);
    if (more)
    {
        io->ops.wake_owner(io->ops.arg);
    }
}

void ble_nimble_smp_io_run_host(ble_nimble_smp_io_t *io)
{
    _smp_run_host(io, true);
}

static TickType_t _smp_remaining_ticks(uint64_t deadline, uint64_t now)
{
    if (now >= deadline)
    {
        return 0U;
    }
    const uint64_t ms = (deadline - now + 999U) / 1000U;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks == 0U ? 1U : ticks;
}

esp_err_t ble_nimble_smp_io_reply(ble_nimble_smp_io_t *io, uint64_t token,
                                  bool accept, uint32_t timeout_ms)
{
    if (token == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (io == NULL || !io->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const uint64_t deadline = io->ops.now_us(io->ops.arg) +
                              (uint64_t)timeout_ms * 1000U;
    const bool on_host = io->ops.is_host_current(io->ops.arg);
    if (xSemaphoreTake(io->lock, _smp_remaining_ticks(
                           deadline, io->ops.now_us(io->ops.arg))) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    if (!io->accepting || !io->offer_pending || io->offer.token != token ||
            io->reply.phase != BLE_NIMBLE_SMP_REPLY_IDLE || io->next_seq == UINT64_MAX)
    {
        _smp_unlock(io);
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreTake(io->ack, 0U);
    io->reply = (ble_nimble_smp_io_reply_t)
    {
        .seq = ++io->next_seq,
        .tuple = io->offer,
        .accept = accept,
        .deadline_us = deadline,
        .phase = BLE_NIMBLE_SMP_REPLY_QUEUED,
        .result = ESP_ERR_NOT_FINISHED,
        .waiter_attached = true,
        .restore_allowed = true,
    };
    const uint64_t seq = io->reply.seq;
    _smp_unlock(io);
    if (on_host)
    {
        _smp_run_host(io, false);
    }
    else
    {
        io->ops.wake_owner(io->ops.arg);
    }
    for (;;)
    {
        _smp_lock(io);
        if (io->reply.seq != seq)
        {
            _smp_unlock(io);
            return ESP_ERR_INVALID_STATE;
        }
        if (io->reply.phase == BLE_NIMBLE_SMP_REPLY_DONE ||
                io->reply.phase == BLE_NIMBLE_SMP_REPLY_CANCELLED)
        {
            const esp_err_t result = io->reply.result;
            io->reply.waiter_attached = false;
            if (io->reply.phase == BLE_NIMBLE_SMP_REPLY_DONE)
            {
                io->reply.phase = BLE_NIMBLE_SMP_REPLY_IDLE;
            }
            _smp_unlock(io);
            return result;
        }
        const TickType_t remaining = _smp_remaining_ticks(
                                         deadline, io->ops.now_us(io->ops.arg));
        if (remaining == 0U)
        {
            const bool executing = io->reply.phase == BLE_NIMBLE_SMP_REPLY_EXECUTING;
            if (!executing)
            {
                io->reply.phase = BLE_NIMBLE_SMP_REPLY_CANCELLED;
                io->reply.result = ESP_ERR_TIMEOUT;
            }
            io->reply.waiter_attached = false;
            _smp_unlock(io);
            io->ops.wake_owner(io->ops.arg);
            return executing ? ESP_ERR_NOT_FINISHED : ESP_ERR_TIMEOUT;
        }
        _smp_unlock(io);
        if (xSemaphoreTake(io->ack, remaining) != pdTRUE)
        {
            /* RTOS budget may expire while the injected clock is stationary. */
            _smp_lock(io);
            if (io->reply.seq == seq &&
                    io->reply.phase != BLE_NIMBLE_SMP_REPLY_DONE &&
                    io->reply.phase != BLE_NIMBLE_SMP_REPLY_CANCELLED)
            {
                const bool executing = io->reply.phase == BLE_NIMBLE_SMP_REPLY_EXECUTING;
                if (!executing)
                {
                    io->reply.phase = BLE_NIMBLE_SMP_REPLY_CANCELLED;
                    io->reply.result = ESP_ERR_TIMEOUT;
                }
                io->reply.waiter_attached = false;
                _smp_unlock(io);
                io->ops.wake_owner(io->ops.arg);
                return executing ? ESP_ERR_NOT_FINISHED : ESP_ERR_TIMEOUT;
            }
            _smp_unlock(io);
        }
    }
}

void ble_nimble_smp_io_cancel(ble_nimble_smp_io_t *io)
{
    if (io == NULL || !io->initialized)
    {
        return;
    }
    _smp_lock(io);
    const ble_nimble_smp_io_tuple_t tuple = io->offer;
    const bool retire = io->offer_token_active;
    const bool executing = io->reply.phase == BLE_NIMBLE_SMP_REPLY_EXECUTING &&
                           _smp_same(&io->reply.tuple, &tuple);
    const bool reject = io->accepting && io->offer_pending && !executing;
    io->offer_pending = false;
    io->offer_token_active = false;
    _smp_invalidate_reply(io, &tuple);
    if (reject)
    {
        io->cancel = tuple;
        io->cancel_pending = true;
    }
    _smp_unlock(io);
    (void)xSemaphoreGive(io->ack);
    if (retire)
    {
        io->ops.retire_confirmation(tuple.token, false, io->ops.arg);
    }
    if (io->ops.is_host_current(io->ops.arg) && !executing)
    {
        _smp_run_host(io, false);
    }
    else if (reject)
    {
        io->ops.wake_owner(io->ops.arg);
    }
}

void ble_nimble_smp_io_forget_connection(ble_nimble_smp_io_t *io,
        uint32_t generation,
        uint16_t conn_handle)
{
    if (io == NULL || !io->initialized)
    {
        return;
    }
    _smp_lock(io);
    const ble_nimble_smp_io_tuple_t tuple = io->offer;
    const bool matches = tuple.generation == generation && tuple.conn_handle == conn_handle;
    const bool retire = matches && io->offer_token_active;
    if (matches)
    {
        io->offer_pending = false;
        io->offer_token_active = false;
        _smp_invalidate_reply(io, &tuple);
    }
    if (io->cancel.generation == generation && io->cancel.conn_handle == conn_handle)
    {
        io->cancel_pending = false;
    }
    _smp_unlock(io);
    (void)xSemaphoreGive(io->ack);
    if (retire)
    {
        io->ops.retire_confirmation(tuple.token, false, io->ops.arg);
    }
}

void ble_nimble_smp_io_close(ble_nimble_smp_io_t *io)
{
    if (io == NULL || !io->initialized)
    {
        return;
    }
    _smp_lock(io);
    io->accepting = false;
    io->draining = true;
    io->open_requested = false;
    io->cancel_pending = false;
    const uint64_t token = io->offer.token;
    const bool retire = io->offer_token_active;
    io->offer_pending = false;
    io->offer_token_active = false;
    _smp_invalidate_reply(io, &io->offer);
    _smp_unlock(io);
    (void)xSemaphoreGive(io->ack);
    if (retire)
    {
        io->ops.retire_confirmation(token, false, io->ops.arg);
    }
    io->ops.wake_owner(io->ops.arg);
}

bool ble_nimble_smp_io_claim_event(ble_nimble_smp_io_t *io)
{
    if (io == NULL || !io->initialized)
    {
        return false;
    }
    _smp_lock(io);
    const bool claim = !io->event_live && _smp_work(io);
    if (claim)
    {
        io->event_live = true;
    }
    _smp_unlock(io);
    return claim;
}

bool ble_nimble_smp_io_pending(ble_nimble_smp_io_t *io)
{
    if (io == NULL || !io->initialized)
    {
        return false;
    }
    _smp_lock(io);
    const bool pending = io->event_live || _smp_work(io);
    _smp_unlock(io);
    return pending;
}

void ble_nimble_smp_io_host_exited(ble_nimble_smp_io_t *io)
{
    if (io == NULL || !io->initialized)
    {
        return;
    }
    ble_nimble_smp_io_close(io);
    _smp_lock(io);
    io->event_live = false;
    if (io->reply.phase == BLE_NIMBLE_SMP_REPLY_QUEUED ||
            io->reply.phase == BLE_NIMBLE_SMP_REPLY_CANCELLED)
    {
        _smp_finish_reply(io, ESP_ERR_INVALID_STATE);
    }
    _smp_unlock(io);
    (void)xSemaphoreGive(io->ack);
}
