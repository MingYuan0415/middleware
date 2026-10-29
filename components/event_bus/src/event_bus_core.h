/**
 * @brief Event-bus state machine.
 *
 * Functions which read or mutate shared state expect the caller to hold the
 * event-bus mutex. Pure helpers and initialization or reset of a private
 * instance do not.
 */

#ifndef EVENT_BUS_CORE_H
#define EVENT_BUS_CORE_H

#include "event_bus.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Slot index which does not identify a subscription. */
#define EVENT_BUS_SLOT_NONE (-1)

/** @brief One live subscription. */
typedef struct
{
    bool valid;
    uint8_t context;   /**< event_bus_dispatch_context_t. */
    uint16_t in_flight; /**< Publisher callbacks currently running. */
    event_bus_msg_id_t msg_id;
    uint32_t sub_type;
    event_bus_cb_t cb;
    void *user_data;
    uint64_t subscribe_seq; /**< Global sequence of the subscription. */
} event_bus_sub_slot_t;

/** @brief Lifecycle of one pending UI batch. */
typedef enum
{
    EVENT_BUS_ENV_FREE = 0,
    EVENT_BUS_ENV_DISPATCHING,
    EVENT_BUS_ENV_ADMITTED,
    EVENT_BUS_ENV_EXECUTING,
} event_bus_env_state_t;

/** @brief One pending UI batch. */
typedef struct
{
    event_bus_env_state_t state;
    bool latest_only;
    uint16_t reserved;
    event_bus_msg_id_t msg_id;
    uint32_t sub_type;
    uint64_t publish_seq; /**< Sequence which decides recipient visibility. */
    size_t payload_size;
} event_bus_envelope_t;

/** @brief Aligned UI payload storage. */
typedef union
{
    max_align_t alignment;
    uint8_t bytes[EVENT_BUS_MAX_UI_PAYLOAD_SIZE];
} event_bus_payload_t;

/** @brief Event-bus state owned by the singleton or a host test. */
typedef struct
{
    event_bus_sub_slot_t subscriptions[EVENT_BUS_MAX_SUBSCRIBERS];
    event_bus_envelope_t envelopes[EVENT_BUS_MAX_PENDING_UI_BATCHES];
    event_bus_payload_t *payloads; /**< Caller-owned array of batch payloads. */
    uint64_t active_slots;
    uint64_t seq;       /**< Publication/subscription sequence. */
    uint32_t in_flight; /**< Publisher callbacks currently running. */
    uint8_t pending_batches;
    uint8_t reserved[3];
    event_bus_stats_t stats;
} event_bus_core_t;

/** @brief Snapshot of one subscription taken for callback invocation. */
typedef struct
{
    bool live;
    event_bus_cb_t cb;
    void *user_data;
} event_bus_target_t;

/**
 * @brief Bind the core to a payload array and clear all state.
 *
 * @param core is the state to initialize.
 * @param payloads points to EVENT_BUS_MAX_PENDING_UI_BATCHES payload slots.
 *
 * @note The payload pool is zeroed.
 */
void event_bus_core_init(event_bus_core_t *core, event_bus_payload_t *payloads);

/**
 * @brief Clear subscriptions, batches, and counters, wipe the payload pool,
 *        and keep the sequence monotonic.
 *
 * @param core is the state to reset.
 */
void event_bus_core_reset(event_bus_core_t *core);

/**
 * @brief Allocate the next subscription or publication sequence, skipping the
 *        invalid zero and any value whose low handle bits would be zero.
 *
 * @param core is the state to advance.
 *
 * @return the new non-zero sequence with a non-zero handle generation.
 */
uint64_t event_bus_core_next_seq(event_bus_core_t *core);

/**
 * @brief Report whether a subscriber existed before a publication.
 *
 * @param subscribe_seq is the subscription sequence.
 * @param publish_seq is the publication sequence.
 *
 * @return true when the subscriber must receive the publication.
 */
bool event_bus_core_visible(uint64_t subscribe_seq, uint64_t publish_seq);

/**
 * @brief Report whether one slot matches a message and subtype.
 *
 * @param slot is the subscription slot.
 * @param msg_id identifies the message.
 * @param sub_type identifies the subtype.
 *
 * @return true when the slot matches.
 */
bool event_bus_core_matches(const event_bus_sub_slot_t *slot,
                            event_bus_msg_id_t msg_id, uint32_t sub_type);

/**
 * @brief Add one subscription.
 *
 * @param core is the state to modify.
 * @param msg_id identifies the message to observe.
 * @param sub_type selects one subtype or EVENT_BUS_SUB_TYPE_ANY.
 * @param cb is the callback to invoke.
 * @param user_data is the opaque callback context.
 * @param context selects publisher-task or UI-worker dispatch.
 * @param out_handle receives the encoded handle.
 *
 * @return the slot index, or EVENT_BUS_SLOT_NONE when the pool is full.
 */
int event_bus_core_subscribe(event_bus_core_t *core, event_bus_msg_id_t msg_id,
                             uint32_t sub_type, event_bus_cb_t cb,
                             void *user_data,
                             event_bus_dispatch_context_t context,
                             event_bus_sub_handle_t *out_handle);

/**
 * @brief Split a handle into its slot and sequence.
 *
 * @param handle is the encoded handle.
 * @param out_slot receives the slot index.
 * @param out_seq receives the subscription sequence.
 *
 * @return true when the handle is well-formed.
 */
bool event_bus_core_decode_handle(event_bus_sub_handle_t handle,
                                  size_t *out_slot, uint64_t *out_seq);

/**
 * @brief Remove one subscription.
 *
 * @param core is the state to modify.
 * @param handle identifies the subscription generation to remove.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a malformed handle, or
 *         ESP_ERR_NOT_FOUND for a stale handle.
 */
esp_err_t event_bus_core_unsubscribe(event_bus_core_t *core,
                                     event_bus_sub_handle_t handle);

/**
 * @brief Count subscribers which a publication would reach.
 *
 * @param core is the read-only state.
 * @param msg_id identifies the message.
 * @param sub_type identifies the subtype.
 * @param publish_seq is the publication sequence.
 * @param ui_count receives the UI subscriber count.
 * @param sync_count receives the publisher subscriber count.
 */
void event_bus_core_count(const event_bus_core_t *core,
                          event_bus_msg_id_t msg_id, uint32_t sub_type,
                          uint64_t publish_seq, size_t *ui_count,
                          size_t *sync_count);

/**
 * @brief Take one callback target and account for its in-flight lifetime.
 *
 * @note Only EVENT_BUS_DISPATCH_PUBLISHER acquisitions are counted in flight;
 *       UI callbacks are serialized by their dispatcher.
 *
 * @param core is the state to modify.
 * @param slot is the subscription index.
 * @param msg_id identifies the message.
 * @param sub_type identifies the subtype.
 * @param context is the dispatch context to require.
 * @param publish_seq is the publication sequence.
 *
 * @return the target; live is false when the slot is stale or mismatched.
 */
event_bus_target_t event_bus_core_acquire(event_bus_core_t *core, size_t slot,
        event_bus_msg_id_t msg_id, uint32_t sub_type,
        event_bus_dispatch_context_t context, uint64_t publish_seq);

/**
 * @brief Release the in-flight accounting taken by a publisher acquisition.
 *
 * @param core is the state to modify.
 * @param slot is the subscription index.
 */
void event_bus_core_release(event_bus_core_t *core, size_t slot);

/**
 * @brief Report the publisher callbacks running for one subscription.
 *
 * @param core is the state to read.
 * @param slot is the subscription index.
 *
 * @return the number of running callbacks, or zero for an invalid slot.
 */
size_t event_bus_core_slot_in_flight(const event_bus_core_t *core, size_t slot);

/**
 * @brief Find an admitted UI snapshot for coalescing.
 *
 * @param core is the read-only state.
 * @param msg_id identifies the message.
 * @param sub_type identifies the subtype.
 *
 * @return an envelope index, or EVENT_BUS_SLOT_NONE.
 */
int event_bus_core_find_admitted(const event_bus_core_t *core,
                                 event_bus_msg_id_t msg_id, uint32_t sub_type);

/**
 * @brief Replace a pending snapshot and adopt the newest sequence.
 *
 * @param core is the state to modify.
 * @param envelope is the admitted envelope index.
 * @param payload is the new payload.
 * @param payload_size is the new payload size.
 * @param publish_seq is the sequence of the replacing publication.
 *
 * @return true when the payload was replaced.
 */
bool event_bus_core_replace(event_bus_core_t *core, int envelope,
                            const void *payload, size_t payload_size,
                            uint64_t publish_seq);

/**
 * @brief Allocate one DISPATCHING envelope.
 *
 * @param core is the state to modify.
 * @param msg_id identifies the message.
 * @param sub_type identifies the subtype.
 * @param payload is the payload to copy.
 * @param payload_size is the payload size.
 * @param latest_only enables coalescing for this envelope.
 * @param publish_seq is the publication sequence.
 *
 * @return the envelope index, or EVENT_BUS_SLOT_NONE when the pool is full.
 */
int event_bus_core_begin_batch(event_bus_core_t *core,
                               event_bus_msg_id_t msg_id, uint32_t sub_type,
                               const void *payload, size_t payload_size,
                               bool latest_only, uint64_t publish_seq);

/**
 * @brief Finish admission: mark the batch admitted or free it on failure.
 *
 * @param core is the state to modify.
 * @param envelope is the envelope index.
 * @param dispatch_result is the UI dispatcher result.
 */
void event_bus_core_commit_batch(event_bus_core_t *core, int envelope,
                                 esp_err_t dispatch_result);

/**
 * @brief Release one envelope and wipe the bytes it exposed.
 *
 * @param core is the state to modify.
 * @param envelope is the envelope index.
 */
void event_bus_core_free_batch(event_bus_core_t *core, int envelope);

/**
 * @brief Report whether a removed subscription still owns its slot.
 *
 * @param core is the state to read.
 * @param slot is the subscription index.
 * @param seq is the handle sequence to match.
 *
 * @return true when the slot is invalid and its generation matches seq.
 */
bool event_bus_core_slot_owned(const event_bus_core_t *core, size_t slot,
                               uint64_t seq);

/**
 * @brief Report whether a removed subscription still has callbacks running.
 *
 * @param core is the state to read.
 * @param slot is the subscription index.
 * @param seq is the handle sequence to match.
 *
 * @return true when the slot belongs to seq and is draining.
 */
bool event_bus_core_draining(const event_bus_core_t *core, size_t slot,
                             uint64_t seq);

/**
 * @brief Release every pending batch except one which is executing.
 *
 * @param core is the state to modify.
 *
 * @return the number of released batches.
 */
size_t event_bus_core_abandon(event_bus_core_t *core);

/**
 * @brief Report the publisher callbacks currently running anywhere.
 *
 * @param core is the state to read.
 *
 * @return the number of running publisher callbacks.
 */
size_t event_bus_core_in_flight(const event_bus_core_t *core);

#endif /* EVENT_BUS_CORE_H */
