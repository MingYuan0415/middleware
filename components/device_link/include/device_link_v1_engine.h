#ifndef __DEVICE_LINK_V1_ENGINE_H__
#define __DEVICE_LINK_V1_ENGINE_H__

#include "device_link_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum device_link_v1_tx_kind
{
    DEVICE_LINK_V1_TX_NONE = 0,
    DEVICE_LINK_V1_TX_RESPONSE,
    DEVICE_LINK_V1_TX_TERMINAL,
    DEVICE_LINK_V1_TX_ORDINARY_STATUS,
} device_link_v1_tx_kind_t;

typedef struct device_link_v1_engine
{
    device_link_v1_operation_record_t record;
    device_link_v1_snapshot_t current_snapshot;
    device_link_v1_snapshot_t ordinary_snapshot;
    device_link_v1_snapshot_t pre_snapshot;
    uint8_t admitted_ssid[DEVICE_LINK_V1_MAX_SSID_BYTES];
    uint8_t admitted_ssid_length;
    uint32_t next_operation_id;
    uint32_t deadline_ms;
    uint8_t response_request_id;
    bool record_present;
    bool accepted_confirmed;
    bool terminal_emitted;
    bool terminal_omitted;
    bool suppress_indications;
    bool ordinary_present;
    bool ids_exhausted;
    bool connected;
    bool pending_accepted;
    bool pending_ack;
    device_link_v1_tx_kind_t in_flight;
} device_link_v1_engine_t;

/** @brief Initialize the slot engine with @p snapshot as GET_STATUS. */
void device_link_v1_engine_init(device_link_v1_engine_t *engine,
                                const device_link_v1_snapshot_t *snapshot);

/** @brief Return whether a Write must be rejected with ATT 0xfe. */
bool device_link_v1_engine_write_blocked(const device_link_v1_engine_t *engine);

/** @brief Return whether an active or terminal record occupies the slot. */
bool device_link_v1_engine_slot_occupied(const device_link_v1_engine_t *engine);

/** @brief Return whether a saved profile is visible in the current snapshot. */
bool device_link_v1_engine_profile_present(const device_link_v1_engine_t *engine);

/**
 * @brief Admit one asynchronous operation.
 *
 * SET_CREDENTIALS retains @p credentials SSID for the success snapshot.
 * Other operations ignore @p credentials.
 *
 * @param credentials SET_CREDENTIALS payload; NULL otherwise.
 * @param[out] operation_id Assigned boot-scoped id on ACCEPTED.
 */
device_link_v1_status_t device_link_v1_engine_start(
    device_link_v1_engine_t *engine, device_link_v1_operation_t operation,
    uint8_t request_id, const device_link_v1_credentials_t *credentials,
    uint32_t *operation_id);

/** @brief Apply ACK_OPERATION to the current slot. */
device_link_v1_status_t device_link_v1_engine_ack(
    device_link_v1_engine_t *engine, uint32_t operation_id,
    uint8_t request_id);

/** @brief Copy the current record or return NOT_FOUND. */
device_link_v1_status_t device_link_v1_engine_get_operation(
    device_link_v1_engine_t *engine,
    device_link_v1_operation_record_t *record);

/**
 * @brief Complete an ACTIVE operation with a terminal snapshot.
 *
 * SCAN success rebuilds the snapshot from the pre-operation state.
 * SET_CREDENTIALS success keeps that state and installs the admitted SSID.
 * A NONE failure that still cannot satisfy success postconditions becomes
 * INTERNAL.
 *
 * @return false when the id, phase, failure, or snapshot is illegal.
 */
bool device_link_v1_engine_complete(
    device_link_v1_engine_t *engine, uint32_t operation_id,
    device_link_v1_wifi_failure_t failure,
    const device_link_v1_network_t *networks, uint8_t count,
    const device_link_v1_snapshot_t *snapshot);

/**
 * @brief Drop an unconfirmed ACTIVE record without a terminal event.
 *
 * @return false when no unconfirmed ACTIVE record is present.
 */
bool device_link_v1_engine_rollback(device_link_v1_engine_t *engine);

/**
 * @brief Arm the finite ACTIVE-record deadline from @p now_ms.
 */
void device_link_v1_engine_arm_deadline(device_link_v1_engine_t *engine,
                                        uint32_t now_ms, uint32_t timeout_ms);

/**
 * @brief Complete an ACTIVE record with TIMEOUT when the deadline is due.
 *
 * @return true when this call terminated the record.
 */
bool device_link_v1_engine_tick(device_link_v1_engine_t *engine,
                                uint32_t now_ms);

/**
 * @brief Remaining milliseconds until the ACTIVE deadline.
 *
 * @return UINT32_MAX when no ACTIVE deadline is armed, 0 when due.
 */
uint32_t device_link_v1_engine_deadline_remaining_ms(
    const device_link_v1_engine_t *engine, uint32_t now_ms);

/**
 * @brief Drop the in-flight indication as undeliverable.
 *
 * Ordinary events are discarded. A terminal event is omitted; the record
 * remains for GET_OPERATION. A response is aborted without confirm.
 */
void device_link_v1_engine_reject_tx(device_link_v1_engine_t *engine);

/** @brief Observe a new Wi-Fi snapshot and retain at most one ordinary event. */
void device_link_v1_engine_observe_snapshot(
    device_link_v1_engine_t *engine,
    const device_link_v1_snapshot_t *snapshot);

/** @brief Arm the command-response indication after a Write is accepted. */
void device_link_v1_engine_arm_response(
    device_link_v1_engine_t *engine, uint8_t request_id,
    bool accepted, bool ack);

/** @brief Return the next indication that may be submitted. */
device_link_v1_tx_kind_t device_link_v1_engine_next_tx(
    device_link_v1_engine_t *engine);

/** @brief Confirm the in-flight indication. */
void device_link_v1_engine_confirm_tx(device_link_v1_engine_t *engine);

/**
 * @brief Drop the in-flight indication without applying confirm side effects.
 *
 * Keeps the slot and connection so the client can recover with GET_OPERATION.
 */
void device_link_v1_engine_abort_tx(device_link_v1_engine_t *engine);

/** @brief Apply BLE disconnect: keep the record, drop unconfirmed TX. */
void device_link_v1_engine_disconnect(device_link_v1_engine_t *engine);

/** @brief Mark the engine connected without replaying events. */
void device_link_v1_engine_connect(device_link_v1_engine_t *engine);

/** @brief Return the authoritative Wi-Fi snapshot. */
const device_link_v1_snapshot_t *device_link_v1_engine_snapshot(
    const device_link_v1_engine_t *engine);

/** @brief Return the retained record, or NULL. */
const device_link_v1_operation_record_t *device_link_v1_engine_record(
    const device_link_v1_engine_t *engine);

#ifdef UNIT_TEST_HOST
device_link_v1_status_t device_link_v1_engine_start_with_id(
    device_link_v1_engine_t *engine, device_link_v1_operation_t operation,
    uint8_t request_id, const device_link_v1_credentials_t *credentials,
    uint32_t operation_id);
void device_link_v1_engine_test_exhaust_ids(device_link_v1_engine_t *engine);
void device_link_v1_engine_test_reboot(device_link_v1_engine_t *engine);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_V1_ENGINE_H__ */
