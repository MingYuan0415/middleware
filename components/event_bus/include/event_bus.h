/**
 * @brief Thread-safe fixed-capacity event bus.
 */

#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Message descriptor defined through EVENT_BUS_DEFINE_ID().
 *
 * The identity of a message is the address of its descriptor, so comparisons
 * stay a pointer compare. The name is only used for diagnostics.
 */
typedef struct event_bus_msg
{
    const char *name; /**< Stable symbol name used by logs and statistics. */
} event_bus_msg_t;

/** @brief Opaque message identity defined through EVENT_BUS_DEFINE_ID(). */
typedef const event_bus_msg_t *event_bus_msg_id_t;

/**
 * @brief Declare one externally defined message identity.
 *
 * @param name is the message identity symbol.
 */
#define EVENT_BUS_DECLARE_ID(name) extern const event_bus_msg_id_t name

/**
 * @brief Define one message identity in exactly one source file.
 *
 * @param name is the message identity symbol.
 */
#define EVENT_BUS_DEFINE_ID(symbol)                                        \
    static const event_bus_msg_t s_##symbol##_desc = { .name = #symbol };   \
    const event_bus_msg_id_t symbol = &s_##symbol##_desc

/**
 * @brief Return a declared message identity.
 *
 * @param name is the message identity symbol.
 */
#define EVENT_BUS_ID(name) (name)

/** @brief Invalid subscription handle sentinel. */
#define EVENT_BUS_SUB_HANDLE_INVALID UINT64_C(0)
/** @brief Match every subtype during subscription. */
#define EVENT_BUS_SUB_TYPE_ANY UINT32_MAX
/** @brief Ask the registered requester to wake the screen after admission. */
#define EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST (UINT32_C(1) << 0)
/** @brief Replace an equivalent pending UI state snapshot. */
#define EVENT_BUS_PUBLISH_FLAG_UI_LATEST     (UINT32_C(1) << 1)
/** @brief Deliver to both paths or neither (default: independent paths). */
#define EVENT_BUS_PUBLISH_FLAG_ATOMIC        (UINT32_C(1) << 2)
/** @brief Compatibility alias for EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST. */
#define EVENT_BUS_PUBLISH_FLAG_WAKE_SCREEN EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST

/** @brief Maximum number of live subscriptions. */
#define EVENT_BUS_MAX_SUBSCRIBERS CONFIG_EVENT_BUS_SUBSCRIBER_CAPACITY
/** @brief Maximum number of pending UI batches. */
#define EVENT_BUS_MAX_PENDING_UI_BATCHES CONFIG_EVENT_BUS_UI_BATCH_CAPACITY
/** @brief Maximum payload size copied for UI dispatch. */
#define EVENT_BUS_MAX_UI_PAYLOAD_SIZE CONFIG_EVENT_BUS_UI_PAYLOAD_SIZE

/**
 * @brief Generation-protected subscription handle.
 *
 * @note The handle carries a 56-bit subscription generation. After 2^56
 *       subscriptions it wraps and a stale handle could alias a newer
 *       subscription; this is not reachable in practice.
 */
typedef uint64_t event_bus_sub_handle_t;

/**
 * @brief Context in which a subscriber callback must run.
 */
typedef enum
{
    EVENT_BUS_DISPATCH_PUBLISHER = 0, /**< Run in the publishing task. */
    EVENT_BUS_DISPATCH_UI,            /**< Run through the UI dispatcher. */
} event_bus_dispatch_context_t;

/**
 * @brief Receive one event-bus message.
 *
 * @note payload is immutable and remains valid only for the callback duration.
 *
 * @param msg_id identifies the published message.
 * @param sub_type identifies the published subtype.
 * @param payload points to the immutable payload, or NULL for an empty payload.
 * @param payload_size is the payload size in bytes.
 * @param user_data is the opaque subscription context.
 */
typedef void (*event_bus_cb_t)(event_bus_msg_id_t msg_id, uint32_t sub_type,
                               const void *payload, size_t payload_size,
                               void *user_data);

/**
 * @brief Enqueue cb(arg) for execution by the UI worker.
 * @note cb must not be invoked inline by the dispatcher.
 * @note On failure, cb must not have run and must not run later.
 *
 * @param cb is the event-bus batch callback to enqueue.
 * @param arg is the opaque callback argument.
 *
 * @return ESP_OK when admitted, otherwise an ESP-IDF error.
 */
typedef esp_err_t (*event_bus_ui_dispatch_fn)(void (*cb)(void *), void *arg);

/**
 * @brief Request screen wake-up without waiting for hardware or UI work.
 * @note Implementations must be non-blocking and may only notify a worker.
 *
 * @return ESP_OK when admitted, otherwise an ESP-IDF error.
 */
typedef esp_err_t (*event_bus_wake_request_fn)(void);

/**
 * @brief Per-publication outcome for both dispatch paths.
 *
 * @note sync callbacks run even when the UI path fails, except when
 *       EVENT_BUS_PUBLISH_FLAG_ATOMIC is set. Use @ref event_bus_publish_ex
 *       to observe both results.
 */
typedef struct
{
    uint16_t matched;        /**< Subscribers matching the message/subtype. */
    uint16_t sync_delivered; /**< Publisher callbacks that were invoked. */
    uint16_t ui_targets;     /**< UI subscribers targeted by the batch. */
    bool ui_queued;          /**< A new UI batch was admitted. */
    bool ui_coalesced;       /**< The publication replaced a pending snapshot. */
    esp_err_t sync_result;   /**< Result of the publisher path. */
    esp_err_t ui_result;     /**< Result of the UI path; ESP_OK when unused. */
} event_bus_publish_report_t;

/**
 * @brief Dispatch counters collected under the event-bus lock.
 */
typedef struct
{
    uint32_t publishes;        /**< Accepted publications. */
    uint32_t coalesced;        /**< UI snapshots replaced in place. */
    uint32_t sync_delivered;   /**< Publisher callbacks invoked. */
    uint32_t ui_batches;       /**< UI batches admitted to the dispatcher. */
    uint32_t drop_oversize;    /**< UI payloads rejected for their size. */
    uint32_t drop_no_dispatcher; /**< UI payloads without a dispatcher. */
    uint32_t drop_no_mem;      /**< UI payloads rejected by the batch pool. */
    uint32_t late_subscriber_skips; /**< Late matching subscribers skipped. */
    uint8_t envelope_peak;     /**< Highest number of allocated envelopes. */
} event_bus_stats_t;

/**
 * @brief Initialize the singleton event bus. Task-only; not ISR-safe.
 * @note Call once during single-threaded application startup.
 *
 * @return ESP_OK when initialized; ESP_ERR_NO_MEM when mutex creation fails.
 */
esp_err_t event_bus_init(void);

/**
 * @brief Release the singleton state. Task-only; not ISR-safe.
 * @note The caller must guarantee that no publication or dispatch is running.
 */
void event_bus_deinit(void);

/**
 * @brief Subscribe to a message and subtype.
 *
 * Task-only; not ISR-safe. UI subscriptions are rejected with
 * ESP_ERR_INVALID_STATE until a UI dispatcher has been registered. A
 * successful handle includes both the slot and its publication generation, so
 * stale handles cannot affect a later subscriber.
 *
 * @param msg_id identifies the message to observe.
 * @param sub_type selects one subtype or EVENT_BUS_SUB_TYPE_ANY.
 * @param cb is the callback invoked for a matching publication.
 * @param user_data is the opaque callback context.
 * @param context selects publisher-task or UI-worker dispatch.
 * @param out_handle receives the generation-protected subscription handle.
 *
 * @return ESP_OK when subscribed; ESP_ERR_INVALID_ARG for invalid arguments;
 *         ESP_ERR_INVALID_STATE when unavailable; ESP_ERR_NO_MEM when full.
 */
esp_err_t event_bus_subscribe(event_bus_msg_id_t msg_id, uint32_t sub_type,
                              event_bus_cb_t cb, void *user_data,
                              event_bus_dispatch_context_t context,
                              event_bus_sub_handle_t *out_handle);

/**
 * @brief Cancel a subscription without waiting for in-flight callbacks.
 *
 * Task-only; not ISR-safe. The subscription slot is invalidated, so no new
 * publisher callback can acquire it and a queued UI callback is skipped; a
 * callback which already passed its liveness check still runs. Use
 * @ref event_bus_unsubscribe_sync when user_data is released immediately
 * after this call.
 *
 * @param handle identifies the subscription generation to cancel.
 *
 * @return ESP_OK when canceled; ESP_ERR_INVALID_ARG for an invalid handle;
 *         ESP_ERR_NOT_FOUND for a stale handle; otherwise an ESP-IDF error.
 */
esp_err_t event_bus_unsubscribe(event_bus_sub_handle_t handle);

/**
 * @brief Cancel a subscription and wait for its publisher callbacks to finish.
 *
 * Task-only; not ISR-safe. A UI subscription must be canceled on the
 * dispatcher task, which serializes this call with its UI callbacks; the
 * subscription's publisher callbacks are waited for as described below. The
 * slot stays reserved while any of its publisher callbacks are still running,
 * so a timed-out call temporarily reduces the usable subscriber capacity.
 * While callbacks are still running, calling again with the same handle
 * resumes the wait instead of returning ESP_ERR_NOT_FOUND.
 *
 * @note Calling this from inside a publisher callback cannot make progress and
 *       returns ESP_ERR_TIMEOUT once the deadline expires.
 *
 * @param handle identifies the subscription generation to cancel.
 * @param timeout_ms bounds the wait for in-flight callbacks.
 *
 * @return ESP_OK when canceled and drained, including a repeated call after a
 *         previous timeout once the callbacks have drained; ESP_ERR_TIMEOUT
 *         when callbacks are still running; ESP_ERR_NOT_FOUND when the handle
 *         did not identify a canceled subscription; otherwise the same errors
 *         as event_bus_unsubscribe().
 */
esp_err_t event_bus_unsubscribe_sync(event_bus_sub_handle_t handle,
                                     uint32_t timeout_ms);

/**
 * @brief Wait until no publisher callback is running anywhere.
 *
 * Task-only; not ISR-safe.
 *
 * @note The caller must also stop publishing; otherwise new callbacks can start
 *       after this returns.
 *
 * @param timeout_ms bounds the wait.
 *
 * @return ESP_OK when drained; ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t event_bus_flush_sync(uint32_t timeout_ms);

/**
 * @brief Publish a message.
 *
 * Task-only; not ISR-safe. Publisher callbacks execute before return and
 * receive the caller's payload. UI callbacks receive one shared, aligned,
 * immutable payload copy. Publisher callbacks must remain bounded and must
 * not publish another event synchronously; hand follow-up publication to the
 * state owner's worker. If at least one matching UI subscriber exists,
 * payload_size must not exceed EVENT_BUS_MAX_UI_PAYLOAD_SIZE. payload may be
 * NULL only when payload_size is zero. EVENT_BUS_PUBLISH_FLAG_UI_LATEST
 * coalesces matching pending UI snapshots by message and subtype; the newest
 * publication decides which subscribers receive the batch. It must not be
 * used for edge, command, audit, or counter events. Publisher callbacks and
 * wake requests are never coalesced. EVENT_BUS_PUBLISH_FLAG_ATOMIC suppresses
 * the publisher path when the UI path is rejected, for callers whose own
 * retry loop must not observe the same event twice.
 *
 * @param msg_id identifies the message to publish.
 * @param sub_type identifies the message subtype.
 * @param payload points to immutable caller data, or NULL when size is zero.
 * @param payload_size is the payload size in bytes.
 * @param flags contains EVENT_BUS_PUBLISH_FLAG_* options.
 *
 * @return ESP_OK when every applicable path succeeded; otherwise the first
 *         path error. Publisher callbacks run even when the UI path fails,
 *         unless EVENT_BUS_PUBLISH_FLAG_ATOMIC is set.
 */
esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags);

/**
 * @brief Publish a message and report both dispatch paths.
 *
 * Task-only; not ISR-safe.
 *
 * @note out_report is zeroed before argument validation, so an early error
 *       does not leave stale fields.
 *
 * @param msg_id identifies the message to publish.
 * @param sub_type identifies the message subtype.
 * @param payload points to immutable caller data, or NULL when size is zero.
 * @param payload_size is the payload size in bytes.
 * @param flags contains EVENT_BUS_PUBLISH_FLAG_* options.
 * @param out_report receives the per-path outcome, or NULL when unused.
 *
 * @return ESP_OK when every applicable path succeeded; otherwise the first
 *         path error.
 */
esp_err_t event_bus_publish_ex(event_bus_msg_id_t msg_id, uint32_t sub_type,
                               const void *payload, size_t payload_size,
                               uint32_t flags,
                               event_bus_publish_report_t *out_report);

/**
 * @brief Register the UI worker enqueue function.
 *
 * Task-only; not ISR-safe.
 *
 * @note Atomically replaces any previously registered dispatcher.
 *
 * @param dispatch is the non-inline UI enqueue function.
 * @param queue_depth is the dispatcher queue depth available to the bus.
 *
 * @return ESP_OK when registered; otherwise an ESP-IDF error.
 */
esp_err_t event_bus_register_ui_dispatch(event_bus_ui_dispatch_fn dispatch,
        uint32_t queue_depth);

/**
 * @brief Remove the UI dispatcher only when it is still expected_dispatch.
 *
 * Task-only; not ISR-safe. Returns ESP_ERR_NOT_FOUND when no dispatcher is
 * registered or a replacement owner has registered a different function.
 * Success prevents later publish snapshots from acquiring expected_dispatch,
 * but it is not a quiescence barrier: a publish which acquired the function
 * before this call may still invoke it. Stop publishers and drain admitted UI
 * work before destroying dispatcher-owned state.
 *
 * @param expected_dispatch is the dispatcher owned by the caller.
 *
 * @return ESP_OK when cleared; ESP_ERR_INVALID_STATE before event_bus_init();
 *         ESP_ERR_INVALID_ARG for NULL; ESP_ERR_NOT_FOUND for no match.
 */
esp_err_t event_bus_unregister_ui_dispatch(
    event_bus_ui_dispatch_fn expected_dispatch);

/**
 * @brief Release every pending (dispatching or admitted) UI batch.
 *
 * Task-only; not ISR-safe. This is a teardown hook for a dispatcher whose
 * queue has been destroyed, so no batch callback can run afterwards. Calling
 * it while a dispatcher may still deliver callbacks is a use-after-free. A
 * batch which is already executing is left to its dispatcher.
 *
 * @return ESP_OK when the pending pool was released.
 */
esp_err_t event_bus_abandon_pending_ui(void);

/**
 * @brief Register the non-blocking app-control wake requester.
 *
 * Task-only; not ISR-safe.
 *
 * @note Atomically replaces any previously registered requester.
 *
 * @param request_wake is the non-blocking requester to register.
 *
 * @return ESP_OK when registered; otherwise an ESP-IDF error.
 */
esp_err_t event_bus_register_wake_requester(event_bus_wake_request_fn request_wake);

/**
 * @brief Remove the wake requester only when it is still expected_request_wake.
 *
 * Task-only; not ISR-safe. Returns ESP_ERR_NOT_FOUND when no requester is
 * registered or a replacement owner has registered a different function.
 * Success prevents later publish snapshots from acquiring
 * expected_request_wake, but a publish which acquired it before this call may
 * still invoke it.
 *
 * @param expected_request_wake is the requester owned by the caller.
 *
 * @return ESP_OK when cleared; ESP_ERR_INVALID_STATE before event_bus_init();
 *         ESP_ERR_INVALID_ARG for NULL; ESP_ERR_NOT_FOUND for no match.
 */
esp_err_t event_bus_unregister_wake_requester(
    event_bus_wake_request_fn expected_request_wake);

/**
 * @brief Read the dispatch counters.
 *
 * Task-only; not ISR-safe.
 *
 * @param out_stats receives a consistent copy.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE before event_bus_init();
 *         ESP_ERR_INVALID_ARG for NULL.
 */
esp_err_t event_bus_get_stats(event_bus_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
