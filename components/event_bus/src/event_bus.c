/**
 * @brief Fixed-capacity event bus implementation.
 */

#define DBG_TAG "event_bus"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "event_bus.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define HANDLE_SLOT_BITS 8U
#define HANDLE_SLOT_MASK ((UINT64_C(1) << HANDLE_SLOT_BITS) - UINT64_C(1))
#define HANDLE_GENERATION_MAX (UINT64_MAX >> HANDLE_SLOT_BITS)

_Static_assert(EVENT_BUS_MAX_PENDING_UI_CALLBACKS >=
               EVENT_BUS_MAX_SUBSCRIBERS,
               "callback pool must cover every subscriber");
_Static_assert(EVENT_BUS_MAX_SUBSCRIBERS <= UINT8_MAX,
               "subscriber indexes use uint8_t");
_Static_assert(EVENT_BUS_MAX_PENDING_UI_CALLBACKS <= UINT8_MAX,
               "callback indexes use uint8_t");
_Static_assert(EVENT_BUS_MAX_PENDING_UI_PAYLOADS <= UINT8_MAX,
               "payload indexes use uint8_t");

typedef struct event_bus_sub_slot
{
    bool valid;
    uint64_t generation;
    event_bus_msg_id_t msg_id;
    uint32_t sub_type;
    event_bus_cb_t cb;
    void *user_data;
    event_bus_dispatch_context_t context;
} event_bus_sub_slot_t;

typedef union event_bus_aligned_payload
{
    max_align_t alignment;
    uint8_t bytes[EVENT_BUS_MAX_UI_PAYLOAD_SIZE];
} event_bus_aligned_payload_t;

typedef enum
{
    EVENT_BUS_BATCH_DISPATCHING = 0,
    EVENT_BUS_BATCH_ADMITTED,
    EVENT_BUS_BATCH_EXECUTING,
} event_bus_batch_state_t;

struct event_bus_dispatch_item;

typedef struct event_bus_envelope
{
    bool in_use;
    bool latest_only;
    uint8_t ref_count;
    uint8_t subscriber_count;
    uint64_t allocation_generation;
    event_bus_batch_state_t state;
    event_bus_msg_id_t msg_id;
    uint32_t sub_type;
    size_t payload_size;
    event_bus_aligned_payload_t payload;
    struct event_bus_dispatch_item *head;
} event_bus_envelope_t;

typedef struct event_bus_dispatch_item
{
    bool in_use;
    uint8_t sub_slot;
    uint8_t envelope_slot;
    uint64_t sub_generation;
    event_bus_msg_id_t msg_id;
    uint32_t sub_type;
    event_bus_cb_t cb;
    void *user_data;
    struct event_bus_dispatch_item *next;
} event_bus_dispatch_item_t;

typedef struct event_bus_snapshot_entry
{
    uint8_t slot;
    uint64_t generation;
    event_bus_cb_t cb;
    void *user_data;
    event_bus_dispatch_context_t context;
} event_bus_snapshot_entry_t;

typedef struct event_bus_publish_snapshot
{
    event_bus_snapshot_entry_t entries[EVENT_BUS_MAX_SUBSCRIBERS];
    size_t count;
    size_t ui_count;
    event_bus_ui_dispatch_fn ui_dispatch;
    event_bus_wake_request_fn wake_request;
} event_bus_publish_snapshot_t;

typedef struct event_bus_ui_batch
{
    event_bus_dispatch_item_t *head;
    int envelope_index;
    uint64_t allocation_generation;
    bool coalesced;
} event_bus_ui_batch_t;

static event_bus_sub_slot_t s_subscriptions[EVENT_BUS_MAX_SUBSCRIBERS];
static event_bus_envelope_t s_envelopes[EVENT_BUS_MAX_PENDING_UI_PAYLOADS];
static event_bus_dispatch_item_t s_dispatch_items[EVENT_BUS_MAX_PENDING_UI_CALLBACKS];
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_ui_admission_mutex;
static event_bus_ui_dispatch_fn s_ui_dispatch;
static event_bus_wake_request_fn s_wake_request;
static bool s_initialized;

static void _event_bus_dispatch_batch(void *arg);

static uint64_t _event_bus_next_generation(uint64_t generation)
{
    generation = (generation % HANDLE_GENERATION_MAX) + UINT64_C(1);
    return generation;
}

static uint64_t _event_bus_next_allocation_generation(uint64_t generation)
{
    ++generation;
    return generation == 0 ? UINT64_C(1) : generation;
}

static event_bus_sub_handle_t _event_bus_make_handle(uint8_t slot, uint64_t generation)
{
    return (generation << HANDLE_SLOT_BITS) | ((uint64_t)slot + UINT64_C(1));
}

static bool _event_bus_decode_handle(event_bus_sub_handle_t handle,
                                     uint8_t *slot, uint64_t *generation)
{
    uint64_t encoded_slot = handle & HANDLE_SLOT_MASK;
    uint64_t encoded_generation = handle >> HANDLE_SLOT_BITS;

    if (encoded_slot == 0 || encoded_slot > EVENT_BUS_MAX_SUBSCRIBERS
            || encoded_generation == 0)
    {
        return false;
    }

    *slot = (uint8_t)(encoded_slot - UINT64_C(1));
    *generation = encoded_generation;
    return true;
}

/* All pool helpers below require s_mutex. */
static void _event_bus_clear_envelope(event_bus_envelope_t *envelope)
{
    uint64_t allocation_generation = envelope->allocation_generation;

    memset(envelope, 0, sizeof(*envelope));
    envelope->allocation_generation = allocation_generation;
}

static int _event_bus_alloc_envelope(void)
{
    int slot = -1;
    for (size_t i = 0; i < EVENT_BUS_MAX_PENDING_UI_PAYLOADS; ++i)
    {
        if (!s_envelopes[i].in_use)
        {
            s_envelopes[i].allocation_generation =
                _event_bus_next_allocation_generation(
                    s_envelopes[i].allocation_generation);
            s_envelopes[i].in_use = true;
            slot = (int)i;
            break;
        }
    }
    return slot;
}

static int _event_bus_alloc_dispatch_item(void)
{
    int slot = -1;
    for (size_t i = 0; i < EVENT_BUS_MAX_PENDING_UI_CALLBACKS; ++i)
    {
        if (!s_dispatch_items[i].in_use)
        {
            s_dispatch_items[i].in_use = true;
            slot = (int)i;
            break;
        }
    }
    return slot;
}

static void _event_bus_release_envelope_ref(uint8_t slot)
{
    event_bus_envelope_t *envelope = &s_envelopes[slot];

    if (!envelope->in_use || envelope->ref_count == 0)
    {
        return;
    }

    --envelope->ref_count;
    if (envelope->ref_count == 0)
    {
        _event_bus_clear_envelope(envelope);
    }
}

static void _event_bus_release_dispatch_item(event_bus_dispatch_item_t *item)
{
    uint8_t envelope_slot = item->envelope_slot;

    memset(item, 0, sizeof(*item));
    _event_bus_release_envelope_ref(envelope_slot);
}

static void _event_bus_rollback_dispatch_batch(event_bus_dispatch_item_t *head)
{
    event_bus_dispatch_item_t *item = head;

    while (item != NULL)
    {
        event_bus_dispatch_item_t *next = item->next;
        _event_bus_release_dispatch_item(item);
        item = next;
    }
}

static bool _event_bus_envelope_matches_snapshot(
    const event_bus_envelope_t *envelope, event_bus_msg_id_t msg_id,
    uint32_t sub_type, const event_bus_snapshot_entry_t *snapshot,
    size_t snapshot_count, size_t ui_count)
{
    if (!envelope->in_use || !envelope->latest_only
            || envelope->state != EVENT_BUS_BATCH_ADMITTED
            || envelope->msg_id != msg_id || envelope->sub_type != sub_type
            || envelope->subscriber_count != ui_count)
    {
        return false;
    }

    const event_bus_dispatch_item_t *item = envelope->head;
    size_t matched_count = 0;
    for (size_t i = 0; i < snapshot_count; ++i)
    {
        if (snapshot[i].context != EVENT_BUS_DISPATCH_UI)
        {
            continue;
        }
        if (item == NULL || item->sub_slot != snapshot[i].slot
                || item->sub_generation != snapshot[i].generation)
        {
            return false;
        }
        item = item->next;
        ++matched_count;
    }
    return matched_count == ui_count && item == NULL;
}

static bool _event_bus_replace_pending_latest(
    event_bus_msg_id_t msg_id, uint32_t sub_type,
    const event_bus_snapshot_entry_t *snapshot, size_t snapshot_count,
    size_t ui_count, const void *payload, size_t payload_size)
{
    bool replaced = false;
    for (size_t i = 0; i < EVENT_BUS_MAX_PENDING_UI_PAYLOADS; ++i)
    {
        event_bus_envelope_t *envelope = &s_envelopes[i];
        if (!_event_bus_envelope_matches_snapshot(envelope, msg_id, sub_type,
                snapshot, snapshot_count,
                ui_count))
        {
            continue;
        }

        envelope->payload_size = payload_size;
        if (payload_size > 0)
        {
            memcpy(envelope->payload.bytes, payload, payload_size);
        }
        replaced = true;
        break;
    }
    return replaced;
}

static void _event_bus_capture_publish_snapshot(
    event_bus_msg_id_t msg_id, uint32_t sub_type,
    event_bus_publish_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; ++i)
    {
        const event_bus_sub_slot_t *slot = &s_subscriptions[i];
        if (!slot->valid || slot->msg_id != msg_id
                || (slot->sub_type != EVENT_BUS_SUB_TYPE_ANY
                    && slot->sub_type != sub_type))
        {
            continue;
        }

        event_bus_snapshot_entry_t *entry = &snapshot->entries[snapshot->count++];
        entry->slot = (uint8_t)i;
        entry->generation = slot->generation;
        entry->cb = slot->cb;
        entry->user_data = slot->user_data;
        entry->context = slot->context;
        if (entry->context == EVENT_BUS_DISPATCH_UI)
        {
            ++snapshot->ui_count;
        }
    }
    snapshot->ui_dispatch = s_ui_dispatch;
    snapshot->wake_request = s_wake_request;
    xSemaphoreGive(s_mutex);
}

static void _event_bus_reset_unbound_items(event_bus_dispatch_item_t *head)
{
    event_bus_dispatch_item_t *item = head;
    while (item != NULL)
    {
        event_bus_dispatch_item_t *next = item->next;
        memset(item, 0, sizeof(*item));
        item = next;
    }
}

static esp_err_t _event_bus_build_ui_items_locked(
    const event_bus_publish_snapshot_t *snapshot, int envelope_index,
    event_bus_msg_id_t msg_id, uint32_t sub_type,
    event_bus_dispatch_item_t **head)
{
    esp_err_t result = ESP_OK;
    event_bus_dispatch_item_t *tail = NULL;
    *head = NULL;

    for (size_t i = 0; i < snapshot->count; ++i)
    {
        const event_bus_snapshot_entry_t *entry = &snapshot->entries[i];
        if (entry->context != EVENT_BUS_DISPATCH_UI)
        {
            continue;
        }

        int item_index = _event_bus_alloc_dispatch_item();
        if (item_index < 0)
        {
            result = ESP_ERR_NO_MEM;
            goto exit;
        }
        event_bus_dispatch_item_t *item = &s_dispatch_items[item_index];
        item->sub_slot = entry->slot;
        item->sub_generation = entry->generation;
        item->envelope_slot = (uint8_t)envelope_index;
        item->msg_id = msg_id;
        item->sub_type = sub_type;
        item->cb = entry->cb;
        item->user_data = entry->user_data;
        item->next = NULL;
        if (*head == NULL)
        {
            *head = item;
        }
        else
        {
            tail->next = item;
        }
        tail = item;
    }

exit:
    if (result != ESP_OK)
    {
        _event_bus_reset_unbound_items(*head);
        *head = NULL;
    }
    return result;
}

static esp_err_t _event_bus_prepare_ui_batch_locked(
    event_bus_msg_id_t msg_id, uint32_t sub_type, const void *payload,
    size_t payload_size, uint32_t flags,
    const event_bus_publish_snapshot_t *snapshot, event_bus_ui_batch_t *batch)
{
    esp_err_t result = ESP_OK;
    memset(batch, 0, sizeof(*batch));
    batch->envelope_index = -1;

    if ((flags & EVENT_BUS_PUBLISH_FLAG_UI_LATEST) != 0)
    {
        batch->coalesced = _event_bus_replace_pending_latest(
                               msg_id, sub_type, snapshot->entries, snapshot->count,
                               snapshot->ui_count, payload, payload_size);
        if (batch->coalesced)
        {
            return ESP_OK;
        }
    }

    batch->envelope_index = _event_bus_alloc_envelope();
    if (batch->envelope_index < 0)
    {
        return ESP_ERR_NO_MEM;
    }
    result = _event_bus_build_ui_items_locked(
                 snapshot, batch->envelope_index, msg_id, sub_type, &batch->head);
    if (result != ESP_OK)
    {
        _event_bus_clear_envelope(&s_envelopes[batch->envelope_index]);
        batch->envelope_index = -1;
        return result;
    }

    event_bus_envelope_t *envelope = &s_envelopes[batch->envelope_index];
    envelope->latest_only = (flags & EVENT_BUS_PUBLISH_FLAG_UI_LATEST) != 0;
    envelope->ref_count = (uint8_t)snapshot->ui_count;
    envelope->subscriber_count = (uint8_t)snapshot->ui_count;
    envelope->state = EVENT_BUS_BATCH_DISPATCHING;
    envelope->msg_id = msg_id;
    envelope->sub_type = sub_type;
    envelope->payload_size = payload_size;
    envelope->head = batch->head;
    if (payload_size > 0)
    {
        memcpy(envelope->payload.bytes, payload, payload_size);
    }
    batch->allocation_generation = envelope->allocation_generation;
    return ESP_OK;
}

static void _event_bus_complete_ui_admission_locked(
    const event_bus_ui_batch_t *batch, esp_err_t dispatch_result)
{
    event_bus_envelope_t *envelope = &s_envelopes[batch->envelope_index];
    if (dispatch_result != ESP_OK)
    {
        _event_bus_rollback_dispatch_batch(batch->head);
    }
    else if (envelope->in_use
             && envelope->allocation_generation == batch->allocation_generation
             && envelope->state == EVENT_BUS_BATCH_DISPATCHING)
    {
        envelope->state = EVENT_BUS_BATCH_ADMITTED;
    }
}

static esp_err_t _event_bus_admit_ui_batch(
    event_bus_msg_id_t msg_id, uint32_t sub_type, const void *payload,
    size_t payload_size, uint32_t flags,
    const event_bus_publish_snapshot_t *snapshot)
{
    esp_err_t result;
    event_bus_ui_batch_t batch;

    /* Do not expose a replaceable batch until enqueue has succeeded. */
    xSemaphoreTake(s_ui_admission_mutex, portMAX_DELAY);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    result = _event_bus_prepare_ui_batch_locked(
                 msg_id, sub_type, payload, payload_size, flags, snapshot, &batch);
    xSemaphoreGive(s_mutex);

    if (result == ESP_OK && !batch.coalesced)
    {
        result = snapshot->ui_dispatch(_event_bus_dispatch_batch, batch.head);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        _event_bus_complete_ui_admission_locked(&batch, result);
        xSemaphoreGive(s_mutex);
    }
    xSemaphoreGive(s_ui_admission_mutex);
    return result;
}

static void _event_bus_request_wake(
    uint32_t flags, event_bus_wake_request_fn wake_request)
{
    if ((flags & EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST) != 0 && wake_request != NULL)
    {
        esp_err_t result = wake_request();
        if (result != ESP_OK)
        {
            LOG_W("wake request rejected: %s", esp_err_to_name(result));
        }
    }
}

static bool _event_bus_subscription_is_current(uint8_t slot, uint64_t generation)
{
    return slot < EVENT_BUS_MAX_SUBSCRIBERS
           && s_subscriptions[slot].valid
           && s_subscriptions[slot].generation == generation;
}

static void _event_bus_dispatch_batch(void *arg)
{
    event_bus_dispatch_item_t *item = (event_bus_dispatch_item_t *)arg;
    bool first_item = true;

    while (item != NULL)
    {
        event_bus_dispatch_item_t *next = item->next;
        event_bus_cb_t cb = NULL;
        void *user_data = NULL;
        event_bus_msg_id_t msg_id = NULL;
        uint32_t sub_type = 0;
        const void *payload = NULL;
        size_t payload_size = 0;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (first_item)
        {
            s_envelopes[item->envelope_slot].state = EVENT_BUS_BATCH_EXECUTING;
            first_item = false;
        }
        if (_event_bus_subscription_is_current(item->sub_slot,
                                               item->sub_generation))
        {
            event_bus_envelope_t *envelope = &s_envelopes[item->envelope_slot];
            cb = item->cb;
            user_data = item->user_data;
            msg_id = item->msg_id;
            sub_type = item->sub_type;
            payload_size = envelope->payload_size;
            if (payload_size > 0)
            {
                payload = envelope->payload.bytes;
            }
        }
        xSemaphoreGive(s_mutex);

        if (cb != NULL)
        {
            cb(msg_id, sub_type, payload, payload_size, user_data);
        }

        /* Keep both item and envelope alive until the callback has returned. */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        _event_bus_release_dispatch_item(item);
        xSemaphoreGive(s_mutex);

        item = next;
    }
}

esp_err_t event_bus_init(void)
{
    esp_err_t result = ESP_OK;
    SemaphoreHandle_t mutex = NULL;
    SemaphoreHandle_t ui_admission_mutex = NULL;
    if (s_initialized)
    {
        return ESP_OK;
    }

    mutex = xSemaphoreCreateMutex();
    if (mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    ui_admission_mutex = xSemaphoreCreateMutex();
    if (ui_admission_mutex == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    memset(s_subscriptions, 0, sizeof(s_subscriptions));
    memset(s_envelopes, 0, sizeof(s_envelopes));
    memset(s_dispatch_items, 0, sizeof(s_dispatch_items));
    for (size_t i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; ++i)
    {
        s_subscriptions[i].generation = UINT64_C(1);
    }

    s_ui_dispatch = NULL;
    s_wake_request = NULL;
    s_mutex = mutex;
    s_ui_admission_mutex = ui_admission_mutex;
    s_initialized = true;

    LOG_I("initialized (subscriptions=%u, envelopes=%u x %u, ui_items=%u)",
          (unsigned)EVENT_BUS_MAX_SUBSCRIBERS,
          (unsigned)EVENT_BUS_MAX_PENDING_UI_PAYLOADS,
          (unsigned)EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
          (unsigned)EVENT_BUS_MAX_PENDING_UI_CALLBACKS);
    return ESP_OK;

cleanup:
    vSemaphoreDelete(mutex);
    return result;
}

esp_err_t event_bus_subscribe(event_bus_msg_id_t msg_id, uint32_t sub_type,
                              event_bus_cb_t cb, void *user_data,
                              event_bus_dispatch_context_t context,
                              event_bus_sub_handle_t *out_handle)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool lock_owned = false;
    if (!s_initialized)
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
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    lock_owned = true;

    if (context == EVENT_BUS_DISPATCH_UI && s_ui_dispatch == NULL)
    {
        goto exit;
    }

    int free_slot = -1;
    for (size_t i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; ++i)
    {
        if (!s_subscriptions[i].valid)
        {
            free_slot = (int)i;
            break;
        }
    }

    if (free_slot < 0)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }

    event_bus_sub_slot_t *slot = &s_subscriptions[free_slot];
    slot->msg_id = msg_id;
    slot->sub_type = sub_type;
    slot->cb = cb;
    slot->user_data = user_data;
    slot->context = context;
    slot->valid = true;
    *out_handle = _event_bus_make_handle((uint8_t)free_slot, slot->generation);
    result = ESP_OK;

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_mutex);
    }
    return result;
}

esp_err_t event_bus_unsubscribe(event_bus_sub_handle_t handle)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool lock_owned = false;
    uint8_t slot_index = 0;
    uint64_t generation = 0;

    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_event_bus_decode_handle(handle, &slot_index, &generation))
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    lock_owned = true;
    event_bus_sub_slot_t *slot = &s_subscriptions[slot_index];
    if (!slot->valid || slot->generation != generation)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }

    slot->valid = false;
    slot->generation = _event_bus_next_generation(slot->generation);
    slot->msg_id = NULL;
    slot->sub_type = 0;
    slot->cb = NULL;
    slot->user_data = NULL;
    slot->context = EVENT_BUS_DISPATCH_PUBLISHER;
    result = ESP_OK;

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_mutex);
    }
    return result;
}

esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (msg_id == NULL || (payload == NULL && payload_size != 0) ||
            (flags & ~(EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST |
                       EVENT_BUS_PUBLISH_FLAG_UI_LATEST)) != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    event_bus_publish_snapshot_t snapshot;
    _event_bus_capture_publish_snapshot(msg_id, sub_type, &snapshot);

    if (snapshot.ui_count > 0)
    {
        if (payload_size > EVENT_BUS_MAX_UI_PAYLOAD_SIZE)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        if (snapshot.ui_dispatch == NULL)
        {
            /* The dispatcher may be removed after subscriptions are admitted. */
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t result = _event_bus_admit_ui_batch(
                               msg_id, sub_type, payload, payload_size, flags, &snapshot);
        if (result != ESP_OK)
        {
            return result;
        }
    }

    _event_bus_request_wake(flags, snapshot.wake_request);

    for (size_t i = 0; i < snapshot.count; ++i)
    {
        const event_bus_snapshot_entry_t *entry = &snapshot.entries[i];
        if (entry->context != EVENT_BUS_DISPATCH_PUBLISHER)
        {
            continue;
        }

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool current = _event_bus_subscription_is_current(entry->slot,
                       entry->generation);
        xSemaphoreGive(s_mutex);
        if (current)
        {
            entry->cb(msg_id, sub_type, payload, payload_size, entry->user_data);
        }
    }
    return ESP_OK;
}

esp_err_t event_bus_register_ui_dispatch(event_bus_ui_dispatch_fn dispatch)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (dispatch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ui_dispatch = dispatch;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t event_bus_unregister_ui_dispatch(
    event_bus_ui_dispatch_fn expected_dispatch)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool lock_owned = false;
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_dispatch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    lock_owned = true;
    if (s_ui_dispatch != expected_dispatch)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    s_ui_dispatch = NULL;
    result = ESP_OK;

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_mutex);
    }
    return result;
}

esp_err_t event_bus_register_wake_requester(event_bus_wake_request_fn request_wake)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (request_wake == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_wake_request = request_wake;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t event_bus_unregister_wake_requester(
    event_bus_wake_request_fn expected_request_wake)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool lock_owned = false;
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_request_wake == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    lock_owned = true;
    if (s_wake_request != expected_request_wake)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    s_wake_request = NULL;
    result = ESP_OK;

exit:
    if (lock_owned)
    {
        xSemaphoreGive(s_mutex);
    }
    return result;
}
