/**
 * @brief Event-bus state machine.
 */

#include "event_bus_core.h"

#include <string.h>

_Static_assert(EVENT_BUS_MAX_SUBSCRIBERS <= 64,
               "subscriber indexes share a 64-bit mask");
_Static_assert(EVENT_BUS_MAX_PENDING_UI_BATCHES <= UINT8_MAX,
               "pending batch count is stored in uint8_t");

#define _EVENT_BUS_SLOT_BITS 8U
#define _EVENT_BUS_SLOT_MASK ((UINT64_C(1) << _EVENT_BUS_SLOT_BITS) - UINT64_C(1))
#define _EVENT_BUS_SEQ_BITS (64U - _EVENT_BUS_SLOT_BITS)
#define _EVENT_BUS_SEQ_MASK ((UINT64_C(1) << _EVENT_BUS_SEQ_BITS) - UINT64_C(1))

static void _event_bus_core_clear_slot(event_bus_sub_slot_t *slot)
{
    /* Keep the sequence and the in-flight count: a slot which still has
     * publisher callbacks must not be reused, and the sequence identifies the
     * handle generation until the slot is reused. */
    const uint64_t subscribe_seq = slot->subscribe_seq;
    const uint16_t in_flight = slot->in_flight;

    *slot = (event_bus_sub_slot_t)
    {
        .valid = false,
        .subscribe_seq = subscribe_seq,
        .in_flight = in_flight,
    };
}

void event_bus_core_init(event_bus_core_t *core, event_bus_payload_t *payloads)
{
    memset(core, 0, sizeof(*core));
    core->payloads = payloads;
    if (payloads != NULL)
    {
        memset(payloads, 0, sizeof(*payloads) *
               EVENT_BUS_MAX_PENDING_UI_BATCHES);
    }
}

void event_bus_core_reset(event_bus_core_t *core)
{
    event_bus_payload_t *payloads = core->payloads;
    const uint64_t seq = core->seq;

    memset(core, 0, sizeof(*core));
    core->payloads = payloads;
    /* Keep the sequence monotonic so handles from a previous session cannot
     * alias a subscription created after the reset. */
    core->seq = seq;
    if (payloads != NULL)
    {
        memset(payloads, 0, sizeof(*payloads) *
               EVENT_BUS_MAX_PENDING_UI_BATCHES);
    }
}

uint64_t event_bus_core_next_seq(event_bus_core_t *core)
{
    do
    {
        ++core->seq;
        if (core->seq == 0)
        {
            ++core->seq;
        }
    }
    while ((core->seq & _EVENT_BUS_SEQ_MASK) == 0);
    return core->seq;
}

bool event_bus_core_visible(uint64_t subscribe_seq, uint64_t publish_seq)
{
    /* The sequence is 64-bit, so plain ordering cannot wrap in practice. */
    return subscribe_seq != 0 && subscribe_seq < publish_seq;
}

bool event_bus_core_matches(const event_bus_sub_slot_t *slot,
                            event_bus_msg_id_t msg_id, uint32_t sub_type)
{
    return slot->valid && slot->msg_id == msg_id
           && (slot->sub_type == EVENT_BUS_SUB_TYPE_ANY
               || slot->sub_type == sub_type);
}

bool event_bus_core_decode_handle(event_bus_sub_handle_t handle,
                                  size_t *out_slot, uint64_t *out_seq)
{
    const uint64_t encoded_slot = handle & _EVENT_BUS_SLOT_MASK;
    const uint64_t decoded_seq = handle >> _EVENT_BUS_SLOT_BITS;

    if (encoded_slot == 0
            || encoded_slot > EVENT_BUS_MAX_SUBSCRIBERS
            || decoded_seq == 0)
    {
        return false;
    }
    *out_slot = (size_t)(encoded_slot - UINT64_C(1));
    *out_seq = decoded_seq;
    return true;
}

int event_bus_core_subscribe(event_bus_core_t *core, event_bus_msg_id_t msg_id,
                             uint32_t sub_type, event_bus_cb_t cb,
                             void *user_data,
                             event_bus_dispatch_context_t context,
                             event_bus_sub_handle_t *out_handle)
{
    int free_slot = EVENT_BUS_SLOT_NONE;

    for (size_t i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; ++i)
    {
        if (!core->subscriptions[i].valid
                && core->subscriptions[i].in_flight == 0)
        {
            free_slot = (int)i;
            break;
        }
    }
    if (free_slot < 0)
    {
        return EVENT_BUS_SLOT_NONE;
    }

    const uint64_t seq = event_bus_core_next_seq(core);
    event_bus_sub_slot_t *slot = &core->subscriptions[free_slot];
    slot->valid = true;
    slot->context = (uint8_t)context;
    slot->in_flight = 0;
    slot->msg_id = msg_id;
    slot->sub_type = sub_type;
    slot->cb = cb;
    slot->user_data = user_data;
    slot->subscribe_seq = seq;
    core->active_slots |= UINT64_C(1) << (unsigned)free_slot;
    *out_handle = ((seq & _EVENT_BUS_SEQ_MASK) << _EVENT_BUS_SLOT_BITS)
                  | ((uint64_t)free_slot + UINT64_C(1));
    return free_slot;
}

esp_err_t event_bus_core_unsubscribe(event_bus_core_t *core,
                                     event_bus_sub_handle_t handle)
{
    size_t slot_index = 0;
    uint64_t seq = 0;

    if (!event_bus_core_decode_handle(handle, &slot_index, &seq))
    {
        return ESP_ERR_INVALID_ARG;
    }
    event_bus_sub_slot_t *slot = &core->subscriptions[slot_index];
    if (!slot->valid || (slot->subscribe_seq & _EVENT_BUS_SEQ_MASK) != seq)
    {
        return ESP_ERR_NOT_FOUND;
    }

    _event_bus_core_clear_slot(slot);
    core->active_slots &= ~(UINT64_C(1) << (unsigned)slot_index);
    return ESP_OK;
}

bool event_bus_core_slot_owned(const event_bus_core_t *core, size_t slot_index,
                               uint64_t seq)
{
    if (slot_index >= EVENT_BUS_MAX_SUBSCRIBERS)
    {
        return false;
    }
    const event_bus_sub_slot_t *slot = &core->subscriptions[slot_index];
    return !slot->valid
           && (slot->subscribe_seq & _EVENT_BUS_SEQ_MASK) == seq;
}

bool event_bus_core_draining(const event_bus_core_t *core, size_t slot_index,
                             uint64_t seq)
{
    return event_bus_core_slot_owned(core, slot_index, seq)
           && core->subscriptions[slot_index].in_flight > 0;
}

void event_bus_core_count(const event_bus_core_t *core,
                          event_bus_msg_id_t msg_id, uint32_t sub_type,
                          uint64_t publish_seq, size_t *ui_count,
                          size_t *sync_count)
{
    size_t ui = 0;
    size_t sync = 0;
    uint64_t mask = core->active_slots;

    for (size_t i = 0; mask != 0; ++i, mask >>= 1)
    {
        if ((mask & UINT64_C(1)) == 0)
        {
            continue;
        }
        const event_bus_sub_slot_t *slot = &core->subscriptions[i];
        if (!event_bus_core_matches(slot, msg_id, sub_type)
                || !event_bus_core_visible(slot->subscribe_seq, publish_seq))
        {
            continue;
        }
        if (slot->context == EVENT_BUS_DISPATCH_UI)
        {
            ++ui;
        }
        else
        {
            ++sync;
        }
    }
    *ui_count = ui;
    *sync_count = sync;
}

event_bus_target_t event_bus_core_acquire(event_bus_core_t *core, size_t slot_index,
        event_bus_msg_id_t msg_id, uint32_t sub_type,
        event_bus_dispatch_context_t context, uint64_t publish_seq)
{
    event_bus_target_t target =
    {
        .live = false,
    };
    if (slot_index >= EVENT_BUS_MAX_SUBSCRIBERS)
    {
        return target;
    }
    event_bus_sub_slot_t *slot = &core->subscriptions[slot_index];
    if (slot->context != (uint8_t)context
            || !event_bus_core_matches(slot, msg_id, sub_type)
            || !event_bus_core_visible(slot->subscribe_seq, publish_seq))
    {
        return target;
    }

    target.live = true;
    target.cb = slot->cb;
    target.user_data = slot->user_data;
    if (context == EVENT_BUS_DISPATCH_PUBLISHER)
    {
        ++slot->in_flight;
        ++core->in_flight;
    }
    return target;
}

void event_bus_core_release(event_bus_core_t *core, size_t slot_index)
{
    if (slot_index >= EVENT_BUS_MAX_SUBSCRIBERS)
    {
        return;
    }
    event_bus_sub_slot_t *slot = &core->subscriptions[slot_index];
    if (slot->in_flight > 0)
    {
        --slot->in_flight;
    }
    if (core->in_flight > 0)
    {
        --core->in_flight;
    }
}

size_t event_bus_core_slot_in_flight(const event_bus_core_t *core,
                                     size_t slot_index)
{
    if (slot_index >= EVENT_BUS_MAX_SUBSCRIBERS)
    {
        return 0;
    }
    return core->subscriptions[slot_index].in_flight;
}

int event_bus_core_find_admitted(const event_bus_core_t *core,
                                 event_bus_msg_id_t msg_id, uint32_t sub_type)
{
    for (size_t i = 0; i < EVENT_BUS_MAX_PENDING_UI_BATCHES; ++i)
    {
        const event_bus_envelope_t *envelope = &core->envelopes[i];
        if (envelope->state == EVENT_BUS_ENV_ADMITTED
                && envelope->latest_only
                && envelope->msg_id == msg_id
                && envelope->sub_type == sub_type)
        {
            return (int)i;
        }
    }
    return EVENT_BUS_SLOT_NONE;
}

bool event_bus_core_replace(event_bus_core_t *core, int envelope_index,
                            const void *payload, size_t payload_size,
                            uint64_t publish_seq)
{
    if (envelope_index < 0
            || envelope_index >= EVENT_BUS_MAX_PENDING_UI_BATCHES)
    {
        return false;
    }
    event_bus_envelope_t *envelope = &core->envelopes[envelope_index];
    if (envelope->state != EVENT_BUS_ENV_ADMITTED || !envelope->latest_only
            || payload_size > EVENT_BUS_MAX_UI_PAYLOAD_SIZE)
    {
        return false;
    }

    if (payload_size < envelope->payload_size)
    {
        memset(core->payloads[envelope_index].bytes + payload_size, 0,
               envelope->payload_size - payload_size);
    }
    envelope->payload_size = payload_size;
    if (payload_size > 0)
    {
        memcpy(core->payloads[envelope_index].bytes, payload, payload_size);
    }
    envelope->publish_seq = publish_seq;
#if CONFIG_EVENT_BUS_STATS
    ++core->stats.coalesced;
#endif
    return true;
}

int event_bus_core_begin_batch(event_bus_core_t *core,
                               event_bus_msg_id_t msg_id, uint32_t sub_type,
                               const void *payload, size_t payload_size,
                               bool latest_only, uint64_t publish_seq)
{
    if (core->pending_batches >= EVENT_BUS_MAX_PENDING_UI_BATCHES
            || payload_size > EVENT_BUS_MAX_UI_PAYLOAD_SIZE)
    {
        return EVENT_BUS_SLOT_NONE;
    }

    int free_slot = EVENT_BUS_SLOT_NONE;
    for (size_t i = 0; i < EVENT_BUS_MAX_PENDING_UI_BATCHES; ++i)
    {
        if (core->envelopes[i].state == EVENT_BUS_ENV_FREE)
        {
            free_slot = (int)i;
            break;
        }
    }
    if (free_slot < 0)
    {
        return EVENT_BUS_SLOT_NONE;
    }

    event_bus_envelope_t *envelope = &core->envelopes[free_slot];
    envelope->state = EVENT_BUS_ENV_DISPATCHING;
    envelope->latest_only = latest_only;
    envelope->msg_id = msg_id;
    envelope->sub_type = sub_type;
    envelope->publish_seq = publish_seq;
    envelope->payload_size = payload_size;
    if (payload_size > 0)
    {
        memcpy(core->payloads[free_slot].bytes, payload, payload_size);
    }
    ++core->pending_batches;
#if CONFIG_EVENT_BUS_STATS
    if (core->pending_batches > core->stats.envelope_peak)
    {
        core->stats.envelope_peak = core->pending_batches;
    }
#endif
    return free_slot;
}

void event_bus_core_commit_batch(event_bus_core_t *core, int envelope_index,
                                 esp_err_t dispatch_result)
{
    if (envelope_index < 0
            || envelope_index >= EVENT_BUS_MAX_PENDING_UI_BATCHES)
    {
        return;
    }
    event_bus_envelope_t *envelope = &core->envelopes[envelope_index];
    if (envelope->state != EVENT_BUS_ENV_DISPATCHING)
    {
        return;
    }
    if (dispatch_result == ESP_OK)
    {
        envelope->state = EVENT_BUS_ENV_ADMITTED;
    }
    else
    {
        event_bus_core_free_batch(core, envelope_index);
    }
}

void event_bus_core_free_batch(event_bus_core_t *core, int envelope_index)
{
    if (envelope_index < 0
            || envelope_index >= EVENT_BUS_MAX_PENDING_UI_BATCHES)
    {
        return;
    }
    event_bus_envelope_t *envelope = &core->envelopes[envelope_index];
    if (envelope->state == EVENT_BUS_ENV_FREE)
    {
        return;
    }
    if (envelope->payload_size > 0)
    {
        memset(core->payloads[envelope_index].bytes, 0,
               envelope->payload_size);
    }
    *envelope = (event_bus_envelope_t)
    {
        .state = EVENT_BUS_ENV_FREE,
    };
    if (core->pending_batches > 0)
    {
        --core->pending_batches;
    }
}

size_t event_bus_core_abandon(event_bus_core_t *core)
{
    size_t released = 0;

    for (size_t i = 0; i < EVENT_BUS_MAX_PENDING_UI_BATCHES; ++i)
    {
        if (core->envelopes[i].state == EVENT_BUS_ENV_FREE
                || core->envelopes[i].state == EVENT_BUS_ENV_EXECUTING)
        {
            continue;
        }
        event_bus_core_free_batch(core, (int)i);
        ++released;
    }
    return released;
}

size_t event_bus_core_in_flight(const event_bus_core_t *core)
{
    return core->in_flight;
}
