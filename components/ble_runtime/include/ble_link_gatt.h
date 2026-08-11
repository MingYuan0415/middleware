#ifndef __BLE_LINK_GATT_H__
#define __BLE_LINK_GATT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_service.h"
#include "ble_tx_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Device Link ATT MTU cap (profile preferred_att_mtu). */
#define BLE_LINK_GATT_ATT_MTU_MAX 498U

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief GATT wiring configuration.
     */
    typedef struct ble_link_gatt_config
    {
        uint64_t boot_id;             /**< Fresh nonzero value for this boot. */
        uint32_t connection_generation; /**< Current connection generation. */
        uint16_t conn_handle;         /**< Active connection handle. */
        uint8_t peer_addr_type;       /**< SMP peer identity address type. */
        uint8_t peer_addr[6];         /**< SMP peer identity address. */
        uint16_t att_mtu;             /**< Negotiated ATT MTU, 23 default. */
        size_t tx_queue_depth;        /**< TX scheduler queue depth. */
        esp_err_t (*publish_link_state)(const uint8_t *value, size_t len,
                                        void *arg); /**< link_state notify
                                                     *  submit sink. */
        void *publish_arg;
        const ble_link_security_ops_t *security_ops; /**< Security 2 ops, or
                                                  *  NULL for the plaintext
                                                  *  host harness. */
    } ble_link_gatt_config_t;

    /**
     * @brief Install the worker sink for completed Link messages.
     *
     * Set before the host starts and clear only after it stops. A NULL sink keeps
     * synchronous execution for focused host tests.
     */
    void ble_link_gatt_set_work_submit(ble_link_work_submit_fn submit, void *arg);

    /**
     * @brief Register the Device Link service characteristics and initialize
     * the link service.
     *
     * Must be called before the registry is sealed. Registers the five frozen
     * characteristics (link_state read/notify public, session_rx write
     * encrypted-sc-bond, session_tx indicate encrypted-sc-bond, control_rx
     * write authorized, control_tx indicate/notify authorized) and wires the
     * access callbacks to the link service.
     *
     * @param[in] config Configuration, kept for the lifetime.
     * @return ESP_OK or an error from the registry or service init.
     */
    esp_err_t ble_link_gatt_init(const ble_link_gatt_config_t *config);

    /**
     * @brief Reset the GATT wiring (new boot or teardown).
     */
    void ble_link_gatt_reset(void);

    /**
     * @brief Re-initialize the transport service after a runtime restart.
     *
     * Restores the per-connection service state (dispatcher, reassembly,
     * subscription, transactions) with the preserved boot id and output sink.
     * The session boot id and Security 2 epoch allocator are left untouched;
     * callers reset the connection facts separately.
     *
     * @return ESP_OK, or ESP_ERR_INVALID_STATE when not previously initialized.
     */
    esp_err_t ble_link_gatt_restart(void);

    /**
     * @brief Refresh the characteristic handles from the registry.
     *
     * Called by the transport after the NimBLE database registration delivered
     * handle assignments. A characteristic without an assigned handle keeps its
     * previous value (0 before the first assignment).
     */
    void ble_link_gatt_update_handles(void);

    /**
     * @brief Update the session-derived link_state and publish changes.
     *
     * Re-encodes the PublicLinkState from the current session facts and emits
     * it through the publish sink when the value changed. Called by the
     * transport when connection facts change.
     *
     * @return ESP_OK, or ESP_ERR_INVALID_ARG.
     */
    esp_err_t ble_link_gatt_refresh_link_state(void);

    /** @brief Force a fresh snapshot after Security 2 authentication. */
    void ble_link_gatt_authentication_epoch_advance(void);

    /** @brief Force a fresh snapshot after link_state CCCD changes. */
    void ble_link_gatt_cccd_epoch_advance(void);

    /** @brief Retain a best-effort snapshot retry obligation. */
    void ble_link_gatt_mark_link_state_dirty(void);

    /** @brief Mark current link state for an event-driven owner refresh. */
    void ble_link_gatt_request_link_state_refresh(void);

    /** @brief Whether the owner still has an undelivered link-state value. */
    bool ble_link_gatt_link_state_dirty(void);

    /** @brief Whether a failed submit/completion requires a timed retry. */
    bool ble_link_gatt_link_state_retry_pending(void);

    /** @brief Milliseconds until the retained link-state retry is eligible. */
    uint32_t ble_link_gatt_link_state_retry_remaining_ms(void);

    /**
     * @brief Advance the connection identity (new connection or generation).
     *
     * @param[in] generation New connection generation.
     * @param[in] conn_handle New connection handle.
     */
    void ble_link_gatt_set_connection(
        uint32_t generation, uint16_t conn_handle,
        uint8_t peer_addr_type, const uint8_t peer_addr[6]);

    /**
     * @brief Notify the 5000 ms reassembly idle timeout fired for a generation.
     *
     * @param[in] generation Generation the timer was armed for.
     */
    void ble_link_gatt_on_reassembly_idle_generation(
        uint32_t generation, uint32_t epoch);

    /**
     * @brief Update the negotiated ATT MTU.
     *
     * @param[in] mtu Negotiated ATT MTU, at least 23.
     */
    void ble_link_gatt_set_att_mtu(uint16_t mtu);

    /**
     * @brief Read the current ATT MTU.
     * @param[out] out_mtu Receives the clamped MTU.
     * @return ESP_OK or ESP_ERR_INVALID_ARG.
     */
    esp_err_t ble_link_gatt_get_att_mtu(uint32_t *out_mtu);

    /**
     * @brief The registered link_state characteristic handle.
     */
    uint16_t ble_link_gatt_link_state_handle(void);

    /**
     * @brief The registered session_tx characteristic handle.
     */
    uint16_t ble_link_gatt_session_tx_handle(void);

    /**
     * @brief The registered control_tx characteristic handle.
     */
    uint16_t ble_link_gatt_control_tx_handle(void);

    /**
     * @brief The registered session_rx characteristic handle.
     */
    uint16_t ble_link_gatt_session_rx_handle(void);

    /**
     * @brief The registered control_rx characteristic handle.
     */
    uint16_t ble_link_gatt_control_rx_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_GATT_H__ */
