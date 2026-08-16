#ifndef __BLE_LINK_SERVICE_H__
#define __BLE_LINK_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_events.h"
#include "ble_link_reassembler.h"
#include "ble_link_security_ops.h"
#include "device_link_operation.h"
#include "device_link_security_auth.h"
#include "device_link_router.h"

#ifdef UNIT_TEST_HOST
    /** @brief Test-only seam: force the authorize deadline value. */
    void ble_link_service_test_set_auth_deadline_ticks(uint32_t ticks);
    /** @brief Test-only seam: copy one operation-table record by id. */
    esp_err_t ble_link_service_test_copy_operation(
        uint64_t operation_id, device_link_operation_t *operation);
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum bytes of one assembled control message (profile 4096). */
#define BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES 4096U

/** @brief Fixed ingress work slots owned by the service worker boundary. */
#define BLE_LINK_SERVICE_WORK_SLOTS 4U

/** @brief Maximum bytes of one assembled session message (profile 1024). */
#define BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES 1024U

/** @brief Transport type byte: Security 2 handshake wire. */
#define BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE 0x00U

/** @brief Transport type byte: protected Device Link v2 application data. */
#define BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED 0x01U

/** @brief Maximum number of concurrent event subscribers. */
#define BLE_LINK_SERVICE_MAX_SUBSCRIBERS 1U

/** @brief Authorization credential length (matches the auth record). */
#define BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES \
    DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES

/** @brief Authorization txn expiry (frozen authorize-prepare-response).
 *          Contract bound: expires_in_ms in [1, 120000]. */
#define BLE_LINK_SERVICE_AUTH_EXPIRES_MS 120000U

/** @brief TX characteristic and PDU kind for the framed value. */
typedef enum
{
    BLE_LINK_SERVICE_TX_SESSION = 0,    /**< session_tx, indication. */
    BLE_LINK_SERVICE_TX_CONTROL_RESPONSE, /**< control_tx, indication. */
} ble_link_service_tx_channel_t;

/**
 * @brief Output sink for one fully framed outbound value.
 *
 * Returns ESP_OK when the fragment was accepted for transmission, or an
 * error to abort the outbound transaction (the caller stops emitting
 * further fragments and the framing contract closes the associated
 * session).
 *
 * @param[in] value   Framed value bytes.
 * @param[in] len     Value length.
 * @param[in] channel TX characteristic channel.
 * @param[in] flow_id Nonzero response flow identity, or zero for an
 *                    independent event notification.
 * @param[in] arg     Argument from service init.
 * @return ESP_OK, or an error to abort the transaction.
 */
typedef esp_err_t (*ble_link_service_output_t)(
    const uint8_t *value, size_t len,
    ble_link_service_tx_channel_t channel, bool is_last, uint32_t flow_id,
    void *arg);

/**
 * @brief Runtime facts the service needs per request.
 */
typedef struct ble_link_service_facts
{
    uint64_t active_boot_id;
    uint32_t connection_generation;
    uint32_t security_epoch;
    uint32_t preferred_att_mtu;
    uint16_t conn_handle;
    bool encrypted;
    bool session_authenticated;
    bool authorized;
    bool identity_known;
    bool secure_connections_bond_verified;
    bool pairing_window_open; /**< Window state captured at ACL connect. */
    uint8_t peer_addr_type; /**< SMP peer identity address type. */
    uint8_t peer_addr[6]; /**< SMP peer identity address. */
} ble_link_service_facts_t;

/**
 * @brief Initialize the service.
 *
 * @param[in] boot_id   Fresh nonzero value for this boot.
 * @param[in] output    Outbound sink, required.
 * @param[in] arg       Sink argument.
 * @param[in] security Security 2 operations, or NULL when no session is
 *                     wired (host test harness); requests then run in
 *                     plaintext and responses are sent plaintext.
 * @param[in] max_pending_frames Reserved; outbound responses stream one
 *                              fragment at a time and no longer depend on
 *                              the TX queue depth.
 */
void ble_link_service_init(
    uint64_t boot_id, ble_link_service_output_t output, void *arg,
    const ble_link_security_ops_t *security, size_t max_pending_frames);

/**
 * @brief Freeze optional domain descriptors before the next service init.
 *
 * Descriptors are copied into the service's static startup table and cannot
 * be changed while the service is initialized. Passing NULL/zero clears the
 * optional set; Core v2 remains registered in all cases.
 */
esp_err_t ble_link_service_set_domain_descriptors(
    const device_link_domain_descriptor_t *domains, size_t domain_count);

/**
 * @brief Admit one asynchronous domain operation into the Core v2 table.
 *
 * Called by a domain adapter when it accepts an asynchronous method: the
 * table allocates the operation id returned in @p out_operation_id (this
 * id, not the adapter's lower-layer id, is encoded into OperationAccepted
 * and used by GetOperation/CancelOperation). @p owner_id carries the
 * adapter's own operation identity so the completion bridge can match
 * terminal events back to the record.
 *
 * @param[in]  domain_id   Registered domain id.
 * @param[in]  method_id   Asynchronous method id.
 * @param[in]  owner_id    Adapter-owned operation identity.
 * @param[in]  cancel      Cancel callback invoked by CancelOperation.
 * @param[in]  cancel_arg  Cancel callback argument.
 * @param[out] out_operation_id Table operation id (nonzero).
 * @return ESP_OK, ESP_ERR_INVALID_STATE when the v2 table is unavailable,
 *         or an allocation error.
 */
esp_err_t ble_link_service_async_operation_start(
    uint8_t domain_id, uint8_t method_id, uint64_t owner_id,
    device_link_operation_cancel_t cancel, void *cancel_arg,
    uint64_t *out_operation_id);

/**
 * @brief Update the live operation owned by @p owner_id.
 *
 * The completion bridge calls this when the adapter's lower layer reports
 * a terminal or progress event. The frozen operation-state semantics
 * (in-flight error=OK, FAILED non-OK, non-success no payload) are enforced
 * by the table.
 *
 * @param[in] owner_id Adapter-owned operation identity.
 * @param[in] state    New state.
 * @param[in] status   LinkError status.
 * @param[in] result   Result payload (only for SUCCEEDED with a declared
 *                     operation result).
 * @param[in] result_len Payload length.
 * @return ESP_OK, ESP_ERR_NOT_FOUND for an unknown or terminal owner, or
 *         ESP_ERR_INVALID_ARG for a semantics violation.
 */
esp_err_t ble_link_service_async_operation_update(
    uint64_t owner_id, device_link_operation_state_t state,
    device_link_status_t status, const uint8_t *result, size_t result_len);

/**
 * @brief Reset the service (new boot or full teardown).
 */
void ble_link_service_reset(void);

/** @brief RX characteristic channel the value arrived on. */
typedef enum
{
    BLE_LINK_SERVICE_RX_SESSION = 0,
    BLE_LINK_SERVICE_RX_CONTROL,
} ble_link_service_rx_channel_t;

/** @brief Opaque completed ingress message transferred to a worker. */
typedef struct ble_link_work ble_link_work_t;

/** @brief Submit one completed ingress message to its execution worker. */
typedef esp_err_t (*ble_link_work_submit_fn)(ble_link_work_t *work, void *arg);

/**
 * @brief Feed one raw characteristic write value.
 *
 * Runs the production path: fragment parse and reassembly, fixed v2 header
 * and Typed-TLV request decode, channel admission, request dispatch, and response
 * framing through the output sink. The bootstrap flow
 * (authorize_prepare/authorize_commit/get_authorization) and the
 * capabilities/snapshot reads are admitted on the session channel with an
 * authenticated session; control requests require an authorized session;
 * capabilities and snapshot are also admitted on the control channel.
 * Intermediate fragments return ESP_ERR_NOT_FINISHED without side effects.
 * A connection generation change discards the partial frame,
 * subscription, and pending authorization transaction.
 *
 * @param[in] facts   Runtime connection facts.
 * @param[in] channel RX characteristic channel.
 * @param[in] value   Raw characteristic write value.
 * @param[in] len     Value length.
 * @return ESP_OK when the frame was processed, ESP_ERR_NOT_FINISHED for an
 *         intermediate fragment, ESP_ERR_INVALID_ARG/INVALID_STATE for a
 *         protocol or admission violation, or ESP_ERR_NO_MEM.
 */
esp_err_t ble_link_service_feed(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const uint8_t *value, size_t len);

/**
 * @brief Validate and reassemble one GATT fragment without executing protocol work.
 *
 * A completed message is copied into worker-owned memory. The caller owns
 * `out_work` on success and must execute or release it. Incomplete frames
 * return ESP_ERR_NOT_FINISHED with a NULL work item. While a completed
 * handshake is queued or a replacement Cmd0 is delayed, exact duplicate
 * fragments are absorbed and any different ingress returns
 * ESP_ERR_NOT_ALLOWED synchronously with a NULL work item.
 */
esp_err_t ble_link_service_accept(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const uint8_t *value, size_t len,
    ble_link_work_t **out_work);

/** @brief Execute one completed message in the protocol worker context. */
esp_err_t ble_link_service_execute(ble_link_work_t *work);

/** @brief Zeroize and release a completed ingress message. */
void ble_link_service_release_work(ble_link_work_t *work);

/** @brief Worker wake callback: prompt the owner to call pump_tx(). */
typedef void (*ble_link_service_wake_fn_t)(void *arg);

/**
 * @brief Register the worker wake callback.
 *
 * The TX completion callback only records a fact. The completion path wakes
 * the owner through this callback (e.g. a task notification), and the owner
 * calls pump_tx() from its task context. The callback must be non-blocking
 * and callable from the transport completion context. The service invokes it
 * while holding its recursive state lock; it may signal the owner but must not
 * wait for owner work to complete. Disabling the callback is a barrier against
 * an in-progress invocation.
 *
 * @param[in] wake Wake callback, or NULL to disable.
 * @param[in] arg  Callback argument.
 */
void ble_link_service_set_worker_wake(
    ble_link_service_wake_fn_t wake, void *arg);

/** @brief Signal the registered service owner without performing owner work. */
void ble_link_service_wake_owner(void);

/**
 * @brief Whether a response transaction is in flight (waiting for the
 * final indication confirmation).
 *
 * The contract requires one transaction at a time: a new request is
 * rejected with BUSY while the previous response has not confirmed.
 */
bool ble_link_service_response_in_flight(void);

/**
 * @brief Abort all pending response transactions.
 *
 * Called on a transmission failure or disconnect; clears the transaction
 * gate so later requests are not stuck BUSY.
 */
void ble_link_service_abort_transactions(void);

/**
 * @brief Whether an old indication currently blocks a retained Cmd0.
 *
 * Used by the indication-timeout owner to apply the contract's stronger
 * outcome: discard the retained Cmd0 and terminate that exact ACL.
 */
bool ble_link_service_delayed_replacement_pending(uint32_t generation);

/**
 * @brief Record one confirmed response fragment.
 *
 * The transport calls this from its completion callback. It only records an
 * immutable completion fact; the caller must invoke
 * ble_link_service_pump_tx() from the service worker to emit the next
 * fragment. A stale or unrelated flow ID is ignored and returns
 * ESP_ERR_NOT_FOUND.
 */
esp_err_t ble_link_service_response_completed(uint32_t flow_id, bool is_last);

/**
 * @brief Advance a response stream from the service owner task.
 *
 * This is the only API that may submit the next response fragment after a
 * confirmation. It also emits one deferred BUSY response, when present,
 * after the active response has fully completed.
 *
 * @return ESP_OK, or an error that caused the current session to fail closed.
 */
esp_err_t ble_link_service_pump_tx(void);

/**
 * @brief Return the nearest retained cleanup/replacement retry deadline.
 *
 * @return Remaining milliseconds, zero when work is due, or UINT32_MAX
 *         when no retained storage obligation exists.
 */
uint32_t ble_link_service_retained_retry_remaining_ms(void);

/** @brief Whether any retained cleanup or replacement obligation exists. */
bool ble_link_service_retained_cleanup_pending(void);

/**
 * @brief Register remote replacement work from a NimBLE host callback.
 *
 * Registration only stores immutable identity and wakes the Device Link
 * owner. If Commit already reached COMMIT_PROBED, Commit wins and the
 * replacement is rejected. Otherwise subsequent request execution is blocked
 * until the owner transfers the retained replacement to the security port.
 */
esp_err_t ble_link_service_register_remote_replacement(
    const ble_link_operation_identity_t *identity);

/**
 * @brief Clear all per-connection session state after a disconnect.
 *
 * Resets the reassemblers, subscription, authorization transaction,
 * dispatcher request ids, and the response transaction gate. The external
 * Security 2 close runs inside the same critical section as the state
 * reset, so a stale clear can never close a newer generation's session.
 */
void ble_link_service_clear_session_state(void);

/**
 * @brief Clear session state only when an immutable host identity is current.
 *
 * The generation and connection handle are checked against both executed
 * facts and accepted ingress while holding the service owner lock. Session-
 * only failures also require an exact Security 2 epoch. ACL-terminal
 * DISCONNECT, RESET, and TERMINATE operations deliberately retire queued work
 * and cover every epoch of that exact ACL, including a Cmd0 that advanced
 * while the terminal event waited for the owner lock.
 *
 * @param[in] identity Host operation that requires the session teardown.
 * @return ESP_OK when cleared, ESP_ERR_NOT_FOUND for a stale identity, or
 *         ESP_ERR_INVALID_ARG for an invalid teardown identity.
 */
esp_err_t ble_link_service_clear_session_state_if_current(
    const ble_link_operation_identity_t *identity);

/**
 * @brief Expire the active authorize transaction when its deadline passed.
 *
 * Called periodically by the device-link worker tick so the UI snapshot
 * state converges to BOOTSTRAP_AUTHENTICATED after `expires_in_ms`.
 *
 * @return ESP_OK.
 */
esp_err_t ble_link_service_auth_expiry_tick(void);

/**
 * @brief Return the remaining authorize transaction lifetime.
 *
 * The result is derived from the same absolute deadline consumed by
 * ble_link_service_auth_expiry_tick(). It lets the owner sleep until the
 * exact deadline instead of relying on a periodic timer event.
 *
 * @return Remaining milliseconds, zero when due, or UINT32_MAX when no
 *         authorize transaction has an expiry obligation.
 */
uint32_t ble_link_service_auth_expiry_remaining_ms(void);

/**
 * @brief Query whether a partial frame is being reassembled on a channel.
 *
 * @param[in] channel RX channel to inspect.
 * @return True when a partial frame exists on the channel.
 */
bool ble_link_service_has_partial_frame(
    ble_link_service_rx_channel_t channel);

/**
 * @brief Handle the reassembly idle timeout (5000 ms without a fragment).
 *
 * Clears the reassembly slot, subscription, and authorization transaction,
 * and closes the associated Security 2 session per the framing contract.
 * The close is validated against the current generation, so a late timeout
 * from a retired generation has no effect.
 *
 * @param[in] generation Current connection generation.
 */
void ble_link_service_idle_timeout(uint32_t generation);

/** @brief Apply an idle timeout only to the matching ingress epoch. */
void ble_link_service_idle_timeout_epoch(
    uint32_t generation, uint32_t epoch);

/** @brief Snapshot one reassembly slot and its invalidation epoch. */
esp_err_t ble_link_service_get_reassembly_state(
    ble_link_service_rx_channel_t channel,
    bool *out_partial, uint32_t *out_epoch);

/** @brief Snapshot reassembly state and the latest accepted disposition. */
esp_err_t ble_link_service_get_reassembly_state_ex(
    ble_link_service_rx_channel_t channel,
    bool *out_partial, uint32_t *out_epoch,
    ble_link_reassembly_disposition_t *out_disposition);

/**
 * @brief Publish a link_state_changed event to the current subscriber.
 *
 * Allocates the next event sequence and emits the framed event through the
 * output sink. When the sequence is exhausted or no subscriber exists, no
 * output is produced and ESP_OK is returned.
 *
 * @param[in] link_state Current LinkState values.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG.
 */
esp_err_t ble_link_service_publish_link_state(
    const ble_link_service_facts_t *facts,
    const ble_link_state_snapshot_t *link_state);

/**
 * @brief Security 2 authentication transition.
 *
 * Invoked by the adapter before the first protected request dispatches.
 * Inside a pairing window only the Security 2 session opens; outside a
 * window the committed record is identity-matched and restores the bound
 * and authorized state. Runs in the adapter's unlocked callback context.
 *
 * @param[in] arg Unused.
 * @return ESP_OK, or an error to fail the session closed.
 */
esp_err_t ble_link_service_on_authenticated(void *arg);

/**
 * @brief Accept or deny the pending binding confirmation.
 *
 * Accepting arms the exact Commit-probed authorize transaction so a fresh
 * AuthorizeCommit persists the authorization record. Denying invalidates
 * that transaction. A stale token or generation is rejected.
 *
 * @param[in] token Nonzero token returned by
 *                  ble_link_service_confirmation_token().
 * @param[in] accept True to confirm, false to deny.
 * @return ESP_OK, or ESP_ERR_INVALID_STATE for a stale decision.
 */
esp_err_t ble_link_service_confirm_binding(uint64_t token, bool accept);

/**
 * @brief Report whether a binding awaits local confirmation.
 *
 * @return True only after a valid Commit probe and before the local
 *         decision.
 */
bool ble_link_service_pending_confirmation(void);

/**
 * @brief Return the token of the transaction awaiting local confirmation.
 *
 * @return Nonzero boot-scoped token, or zero when no Commit probe is
 *         awaiting a local decision.
 */
uint64_t ble_link_service_confirmation_token(void);

/**
 * @brief Process one decrypted Device Link v2 message and produce its
 * plaintext response message.
 *
 * Invoked by the Security 2 adapter's request callback after decryption.
 * Decodes and validates the fixed header and Typed-TLV request, dispatches
 * it, and fills the response message (allocated). Failures return an error and the
 * caller closes the session.
 *
 * @param[in] msg Decrypted Device Link v2 request bytes.
 * @param[in] len Request length.
 * @param[out] response Allocated Device Link v2 response bytes.
 * @param[out] response_len Response length.
 * @return ESP_OK, or a protocol/admission error.
 */
esp_err_t ble_link_service_process_plaintext(
    const uint8_t *msg, size_t len,
    uint8_t **response, size_t *response_len);

/** @brief Release a response returned by ble_link_service_process_plaintext. */
void ble_link_service_release_plaintext(
    uint8_t *response, size_t response_len);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_SERVICE_H__ */
