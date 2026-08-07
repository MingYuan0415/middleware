#ifndef __BLE_LINK_SERVICE_H__
#define __BLE_LINK_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_events.h"
#include "ble_link_security_ops.h"
#include "device_link_security_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum bytes of one assembled control message (profile 4096). */
#define BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES 4096U

/** @brief Maximum bytes of one assembled session message (profile 1024). */
#define BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES 1024U

/** @brief Transport type byte: Security 2 handshake wire. */
#define BLE_LINK_SERVICE_TRANSPORT_TYPE_HANDSHAKE 0x00U

/** @brief Transport type byte: AES-GCM ciphertext of an Envelope. */
#define BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED 0x01U

/** @brief Maximum number of concurrent event subscribers. */
#define BLE_LINK_SERVICE_MAX_SUBSCRIBERS 1U

/** @brief Authorization credential length (matches the auth record). */
#define BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES \
    DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES

/** @brief Fixed fake event key length while encrypted events are deferred. */
#define BLE_LINK_SERVICE_EVENT_KEY_BYTES 16U

/** @brief Fixed fake nonce prefix length. */
#define BLE_LINK_SERVICE_NONCE_PREFIX_BYTES 4U

/** @brief Authorization txn expiry (frozen authorize-prepare-response). */
#define BLE_LINK_SERVICE_AUTH_EXPIRES_MS 600000U

/** @brief TX characteristic and PDU kind for the framed value. */
typedef enum
{
    BLE_LINK_SERVICE_TX_SESSION = 0,    /**< session_tx, indication. */
    BLE_LINK_SERVICE_TX_CONTROL_RESPONSE, /**< control_tx, indication. */
    BLE_LINK_SERVICE_TX_CONTROL_EVENT,    /**< control_tx, notification. */
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
 * @param[in] arg     Argument from service init.
 * @return ESP_OK, or an error to abort the transaction.
 */
typedef esp_err_t (*ble_link_service_output_t)(
    const uint8_t *value, size_t len,
    ble_link_service_tx_channel_t channel, bool is_last, void *arg);

/**
 * @brief Runtime facts the service needs per request.
 */
typedef struct ble_link_service_facts
{
    uint64_t active_boot_id;
    uint32_t connection_generation;
    uint32_t preferred_att_mtu;
    bool encrypted;
    bool session_authenticated;
    bool authorized;
    bool identity_known;
    bool secure_connections_bond_verified;
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
 * @param[in] max_pending_frames Max frames one response may need in the
 *                              TX queue; a response requiring more fails
 *                              closed (the session is terminated).
 */
void ble_link_service_init(
    uint64_t boot_id, ble_link_service_output_t output, void *arg,
    const ble_link_security_ops_t *security, size_t max_pending_frames);

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

/**
 * @brief Feed one raw characteristic write value.
 *
 * Runs the production path: fragment parse and reassembly, envelope and
 * request decode, channel admission, request dispatch, and response
 * framing through the output sink. Control requests require an authorized
 * session; the bootstrap authorize_prepare/authorize_commit flow is
 * admitted with an authenticated session only. Intermediate fragments
 * return ESP_ERR_NOT_FINISHED without side effects. A connection
 * generation change discards the partial frame, subscription, and pending
 * authorization transaction.
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
 * @brief Complete the response transaction.
 *
 * Called by the transport when the final fragment's indication confirms.
 */
void ble_link_service_response_completed(void);

/**
 * @brief Clear all per-connection session state after a disconnect.
 *
 * Resets the reassemblers, subscription, authorization transaction,
 * dispatcher request ids, and the response transaction gate. The Security
 * 2 session close is owned by the transport caller.
 */
void ble_link_service_clear_session_state(void);

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
 * @brief Accept or deny the pending binding confirmation.
 *
 * Accepting arms the active authorize transaction so a subsequent
 * AuthorizeCommit persists the authorization record. Denying invalidates
 * the transaction: any later commit of it is rejected. Without an active
 * transaction the call has no effect.
 *
 * @param[in] accept True to confirm, false to deny.
 */
void ble_link_service_confirm_binding(bool accept);

/**
 * @brief Report whether a binding awaits local confirmation.
 *
 * @return True while an authorize transaction is active and not yet
 *         confirmed (or denied).
 */
bool ble_link_service_pending_confirmation(void);

/**
 * @brief Process one plaintext Envelope and produce the plaintext response
 * envelope.
 *
 * Invoked by the Security 2 adapter's request callback after decryption.
 * Decodes and validates the Envelope, dispatches the request, and fills
 * the response Envelope (allocated). Failures return an error and the
 * caller closes the session.
 *
 * @param[in] msg Plaintext Envelope bytes.
 * @param[in] len Envelope length.
 * @param[out] response Allocated response Envelope.
 * @param[out] response_len Response length.
 * @return ESP_OK, or a protocol/admission error.
 */
esp_err_t ble_link_service_process_plaintext(
    const uint8_t *msg, size_t len,
    uint8_t **response, size_t *response_len);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_SERVICE_H__ */
