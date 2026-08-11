#ifndef __BLE_TX_SCHEDULER_H__
#define __BLE_TX_SCHEDULER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_operation.h"
#include "ble_port_ops.h"

#ifdef UNIT_TEST_HOST
    void ble_tx_scheduler_test_set_token(uint32_t value);
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Transmission kinds the scheduler serializes. */
typedef enum
{
    BLE_TX_SCHEDULER_KIND_NOTIFY = 0,
    BLE_TX_SCHEDULER_KIND_INDICATE,
} ble_tx_scheduler_kind_t;

/**
 * @brief Outcome of one submitted frame.
 *
 * status is ESP_OK when the notification was sent or the indication was
 * confirmed, ESP_ERR_TIMEOUT on an indication timeout, ESP_FAIL on any other
 * asynchronous transmission error, ESP_ERR_INVALID_STATE when the connection
 * was torn down first, or the port error when the synchronous submit failed.
 */
typedef struct ble_tx_scheduler_result
{
    ble_link_operation_identity_t identity;
    ble_tx_scheduler_kind_t kind;
    uint16_t conn_handle;
    uint16_t value_handle;
    uint32_t flow_id; /**< Owning service flow, or zero when untracked. */
    uint32_t token; /**< Unique nonzero identity of this submitted frame. */
    esp_err_t status;
    bool is_last; /**< True when this frame ends the transaction. */
} ble_tx_scheduler_result_t;

/** @brief Frame completion callback, invoked from the caller's context. */
typedef void (*ble_tx_scheduler_completion_cb_t)(
    const ble_tx_scheduler_result_t *result, void *arg);

/**
 * @brief TX scheduler configuration.
 *
 * The scheduler serializes all transmissions on the current connection: at
 * most one frame is in flight and the next queued frame is sent only after
 * the previous one completed (notification sent, or indication confirmed or
 * timed out), which the Device Link GATT contract requires for fragment
 * ordering. The queue is bounded and reports ESP_ERR_NO_MEM when full, so
 * callers back off instead of overflowing NimBLE mbufs.
 *
 * All entry points must be called serially from a single owner task or under
 * the caller's own synchronization, matching the port event feeding model.
 * The completion callback must not submit frames synchronously; the session
 * layer defers submissions to its worker. Submitted frames, the in-flight
 * frame, and undelivered completions share queue_depth + 1 fixed credits, so
 * terminal completions never require allocation and are delivered exactly
 * once.
 */
typedef struct ble_tx_scheduler_config
{
    size_t queue_depth;      /**< Bounded pending frame capacity. */
    size_t max_frame_bytes;  /**< Max payload per frame. */
    const ble_port_ops_t *ops; /**< Port operations, required. */
    ble_tx_scheduler_completion_cb_t completed; /**< Optional completion. */
    void *completed_arg;
    void (*lock)(void *arg); /**< Optional serialization lock. */
    void (*unlock)(void *arg);
    void *lock_arg;
} ble_tx_scheduler_config_t;

/**
 * @brief Initialize the scheduler with an empty queue.
 *
 * @param[in] config Configuration, kept for the scheduler lifetime.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM.
 */
esp_err_t ble_tx_scheduler_init(const ble_tx_scheduler_config_t *config);

/**
 * @brief Release the scheduler and its configuration.
 *
 * Every outstanding frame (in flight and queued) is completed with
 * ESP_ERR_INVALID_STATE and all pending completions are delivered before
 * the buffers are freed, so no submitted identity is silently lost. After
 * this call every entry point returns ESP_ERR_INVALID_STATE until the next
 * init. The production port calls this during teardown so no callback
 * outlives the lock it uses.
 */
void ble_tx_scheduler_deinit(void);

/**
 * @brief Submit one frame for transmission.
 *
 * When the scheduler is idle the frame is submitted to the port immediately
 * (the port copies the payload synchronously); otherwise it is queued in
 * order. Only one frame is in flight at a time; the next queued frame is
 * sent when the in-flight frame completes through
 * ble_tx_scheduler_handle_notify_tx().
 *
 * The caller freezes generation, Security 2 epoch, flow, kind, and connection
 * in @p identity. Its token must be zero; the scheduler assigns the unique
 * nonzero frame token before the operation becomes visible to the port.
 *
 * @param[in] kind         Transmission kind.
 * @param[in] identity     Complete caller-owned operation identity except for
 *                        the scheduler-assigned token.
 * @param[in] value_handle Characteristic value handle.
 * @param[in] data         Payload; copied synchronously when sent.
 * @param[in] len          Payload length.
 * @param[in] is_last      True when this frame ends the transaction.
 * @return ESP_OK, or the synchronous port transmission error for this frame,
 *         ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE after deinit, or
 *         ESP_ERR_NO_MEM when the queue is full or a buffer allocation
 *         failed.
 */
esp_err_t ble_tx_scheduler_submit(
    ble_tx_scheduler_kind_t kind,
    const ble_link_operation_identity_t *identity,
    uint16_t value_handle, const uint8_t *data, size_t len,
    bool is_last);

/**
 * @brief Feed one NOTIFY_TX port event into the scheduler.
 *
 * Completes the in-flight frame only when the event's complete immutable
 * identity, attribute handle, and indication kind match. Unrelated or stale
 * events are ignored. After completion the next queued frame is sent.
 *
 * @param[in] event Port event.
 * @return ESP_OK when the event completed the in-flight frame,
 *         ESP_ERR_NOT_FOUND when it belonged to no in-flight frame,
 *         ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE after deinit.
 */
esp_err_t ble_tx_scheduler_handle_notify_tx(const ble_port_event_t *event);

/**
 * @brief Drop the queue and the in-flight frame.
 *
 * Called on disconnect or teardown. Every outstanding frame (in flight and
 * queued) completes with ESP_ERR_INVALID_STATE, so N submissions always
 * produce N completions.
 */
void ble_tx_scheduler_reset(void);

/**
 * @brief Query whether a frame is in flight or pending.
 */
bool ble_tx_scheduler_is_busy(void);

/**
 * @brief Fail the in-flight indication with ESP_ERR_TIMEOUT and retire its
 * remaining queued flow frames.
 *
 * Called by the transport when the 2000 ms indication confirmation window
 * expires. The token must match the current in-flight indication, so a
 * late timer from a previous indication is ignored. A notification in
 * flight is unaffected.
 *
 * @param[in] token In-flight indication token from
 *                  ble_tx_scheduler_get_in_flight_token(), or 0 to force.
 * @return ESP_OK when the in-flight indication was timed out,
 *         ESP_ERR_INVALID_STATE when the token did not match (a late timer)
 *         or after deinit.
 */
esp_err_t ble_tx_scheduler_handle_indication_timeout(uint32_t token);

/**
 * @brief Query the current in-flight frame token.
 *
 * @return The token, or 0 when nothing is in flight.
 */
uint32_t ble_tx_scheduler_get_in_flight_token(void);

/** @brief Copy the immutable identity of the current in-flight frame. */
esp_err_t ble_tx_scheduler_get_in_flight_identity(
    ble_link_operation_identity_t *identity);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_TX_SCHEDULER_H__ */
