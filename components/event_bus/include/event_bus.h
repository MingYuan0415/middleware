/**
 * @brief Thread-safe fixed-capacity event bus.
 */

#ifndef __EVENT_BUS_H__
#define __EVENT_BUS_H__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque message identity defined through EVENT_BUS_DEFINE_ID(). */
typedef const void *event_bus_msg_id_t;

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
#define EVENT_BUS_DEFINE_ID(name)       \
    static const uint8_t s_##name##_id; \
    const event_bus_msg_id_t name = &s_##name##_id

/**
 * @brief Return a declared message identity.
 *
 * @param name is the message identity symbol.
 */
#define EVENT_BUS_ID(name) (name)

/**
 * @brief Compatibility alias for EVENT_BUS_ID().
 *
 * @param name is the message identity symbol.
 */
#define ID(name) EVENT_BUS_ID(name)

/** @brief Invalid subscription handle sentinel. */
#define EVENT_BUS_SUB_HANDLE_INVALID UINT64_C(0)
/** @brief Match every subtype during subscription. */
#define EVENT_BUS_SUB_TYPE_ANY UINT32_MAX
/** @brief Ask the registered requester to wake the screen after admission. */
#define EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST (UINT32_C(1) << 0)
/** @brief Replace an equivalent pending UI state snapshot. */
#define EVENT_BUS_PUBLISH_FLAG_UI_LATEST     (UINT32_C(1) << 1)
/** @brief Compatibility alias for EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST. */
#define EVENT_BUS_PUBLISH_FLAG_WAKE_SCREEN EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST

/** @brief Maximum number of live subscriptions. */
#define EVENT_BUS_MAX_SUBSCRIBERS CONFIG_EVENT_BUS_SUBSCRIBER_CAPACITY
/** @brief Maximum number of pending UI callback items. */
#define EVENT_BUS_MAX_PENDING_UI_CALLBACKS CONFIG_EVENT_BUS_UI_CALLBACK_CAPACITY
/** @brief Maximum number of shared pending UI payloads. */
#define EVENT_BUS_MAX_PENDING_UI_PAYLOADS CONFIG_EVENT_BUS_UI_PAYLOAD_CAPACITY
/** @brief Maximum payload size copied for UI dispatch. */
#define EVENT_BUS_MAX_UI_PAYLOAD_SIZE CONFIG_EVENT_BUS_UI_PAYLOAD_SIZE

/** @brief Generation-protected subscription handle. */
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
 * @brief Initialize the singleton event bus. Task-only; not ISR-safe.
 * @note Call once during single-threaded application startup.
 *
 * @return ESP_OK when initialized; ESP_ERR_NO_MEM when mutex creation fails.
 */
esp_err_t event_bus_init(void);

/**
 * @brief Subscribe to a message and subtype.
 *
 * Task-only; not ISR-safe. UI subscriptions are rejected with
 * ESP_ERR_INVALID_STATE until a UI dispatcher has been registered. A
 * successful handle includes both the slot and its generation; stale handles
 * cannot affect a later subscriber.
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
 * @brief Cancel a subscription.
 *
 * Task-only; not ISR-safe. This invalidates queued UI callbacks and future
 * publisher snapshots. It does not wait for a callback which has already
 * passed its liveness check, so the owner must separately synchronize
 * user_data destruction with publishers and the UI worker.
 *
 * @param handle identifies the subscription generation to cancel.
 *
 * @return ESP_OK when canceled; ESP_ERR_INVALID_ARG for an invalid handle;
 *         ESP_ERR_NOT_FOUND for a stale handle; otherwise an ESP-IDF error.
 */
esp_err_t event_bus_unsubscribe(event_bus_sub_handle_t handle);

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
 * coalesces matching
 * pending UI state snapshots by message, subtype, and exact UI subscriber
 * slot/generation set. It must not be used for edge, command, audit, or
 * counter events. Publisher callbacks and wake requests are never coalesced.
 *
 * @param msg_id identifies the message to publish.
 * @param sub_type identifies the message subtype.
 * @param payload points to immutable caller data, or NULL when size is zero.
 * @param payload_size is the payload size in bytes.
 * @param flags contains EVENT_BUS_PUBLISH_FLAG_* options.
 *
 * @return ESP_OK when dispatched; ESP_ERR_INVALID_ARG or ESP_ERR_INVALID_SIZE
 *         for invalid input; otherwise an admission or dispatcher error.
 */
esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags);

/**
 * @brief Register the UI worker enqueue function.
 * @note Atomically replaces any previously registered dispatcher.
 *
 * @param dispatch is the non-inline UI enqueue function.
 *
 * @return ESP_OK when registered; otherwise an ESP-IDF error.
 */
esp_err_t event_bus_register_ui_dispatch(event_bus_ui_dispatch_fn dispatch);

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
 * @brief Register the non-blocking app-control wake requester.
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

#ifdef __cplusplus
}
#endif

#endif /* __EVENT_BUS_H__ */
