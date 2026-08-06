#ifndef __BLE_LINK_SERVICE_H__
#define __BLE_LINK_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum bytes of one assembled control message (profile 4096). */
#define BLE_LINK_SERVICE_MAX_CONTROL_MESSAGE_BYTES 4096U

/** @brief Maximum bytes of one assembled session message (profile 1024). */
#define BLE_LINK_SERVICE_MAX_SESSION_MESSAGE_BYTES 1024U

/** @brief Maximum number of concurrent event subscribers. */
#define BLE_LINK_SERVICE_MAX_SUBSCRIBERS 1U

/** @brief Fake authorization credential length while crypto is deferred. */
#define BLE_LINK_SERVICE_AUTH_CREDENTIAL_BYTES 16U

/** @brief Fixed fake event key length while encrypted events are deferred. */
#define BLE_LINK_SERVICE_EVENT_KEY_BYTES 16U

/** @brief Fixed fake nonce prefix length. */
#define BLE_LINK_SERVICE_NONCE_PREFIX_BYTES 4U

/** @brief Authorization txn expiry (frozen authorize-prepare-response). */
#define BLE_LINK_SERVICE_AUTH_EXPIRES_MS 600000U

/** @brief TX characteristic the framed value belongs on. */
typedef enum
{
    BLE_LINK_SERVICE_TX_SESSION = 0,
    BLE_LINK_SERVICE_TX_CONTROL,
} ble_link_service_tx_channel_t;

/**
 * @brief Output sink for one fully framed outbound value.
 *
 * @param[in] value   Framed value bytes.
 * @param[in] len     Value length.
 * @param[in] channel TX characteristic channel.
 * @param[in] arg     Argument from service init.
 */
typedef void (*ble_link_service_output_t)(
    const uint8_t *value, size_t len,
    ble_link_service_tx_channel_t channel, void *arg);

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
} ble_link_service_facts_t;

/**
 * @brief Initialize the service.
 *
 * @param[in] boot_id Fresh nonzero value for this boot.
 * @param[in] output  Outbound sink, required.
 * @param[in] arg     Sink argument.
 */
void ble_link_service_init(
    uint64_t boot_id, ble_link_service_output_t output, void *arg);

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

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_SERVICE_H__ */
