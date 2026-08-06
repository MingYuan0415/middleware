#ifndef __BLE_LINK_EVENTS_H__
#define __BLE_LINK_EVENTS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Contract-frozen event sequence maximum (never wraps). */
#define BLE_LINK_EVENTS_MAX_SEQUENCE UINT64_MAX

/** @brief Contract-frozen Snapshot max encoded size. */
#define BLE_LINK_EVENTS_SNAPSHOT_MAX_BYTES 64U

/** @brief LinkState values, frozen by microtech.link.v1. */
typedef enum
{
    BLE_LINK_BINDING_UNSPECIFIED = 0,
    BLE_LINK_BINDING_UNBOUND = 1,
    BLE_LINK_BINDING_PAIRING_WINDOW = 2,
    BLE_LINK_BINDING_BOUND = 3,
} ble_link_binding_state_t;

typedef enum
{
    BLE_LINK_AUTHORIZATION_UNSPECIFIED = 0,
    BLE_LINK_AUTHORIZATION_UNAUTHORIZED = 1,
    BLE_LINK_AUTHORIZATION_BOOTSTRAP_AUTHENTICATED = 2,
    BLE_LINK_AUTHORIZATION_CONFIRMATION_PENDING = 3,
    BLE_LINK_AUTHORIZATION_PREPARED = 4,
    BLE_LINK_AUTHORIZATION_AUTHORIZED = 5,
} ble_link_authorization_state_t;

/** @brief LinkState fields to encode. */
typedef struct ble_link_state_snapshot
{
    uint64_t boot_id;
    ble_link_binding_state_t binding_state;
    ble_link_authorization_state_t authorization_state;
    bool encrypted;
    bool secure_connections_bond_verified;
    bool identity_known;
} ble_link_state_snapshot_t;

/** @brief Snapshot (event_sequence + LinkState). */
typedef struct ble_link_snapshot
{
    uint64_t event_sequence;
    ble_link_state_snapshot_t link_state;
} ble_link_snapshot_t;

/**
 * @brief Initialize the event sequence for a boot.
 *
 * The sequence starts at 0 and produces nonzero monotonic values from the
 * first allocation. A new boot resets its interpretation. All sequence
 * functions must be called from a single owner task; no internal
 * synchronization is provided.
 */
void ble_link_events_init(void);

/**
 * @brief Reset the event sequence to 0 (new boot).
 */
void ble_link_events_reset(void);

/**
 * @brief Allocate the next event sequence number.
 *
 * @return A nonzero monotonically increasing value, or 0 when the maximum
 *         has been reached and publication has stopped for this boot.
 */
uint64_t ble_link_events_next(void);

/**
 * @brief Current baseline: the last allocated sequence, or 0.
 */
uint64_t ble_link_events_baseline(void);

/**
 * @brief Whether the sequence is exhausted (maximum reached).
 */
bool ble_link_events_exhausted(void);

/**
 * @brief Encode a Snapshot for the subscribe response or get_link_snapshot.
 *
 * Value domain is enforced: binding_state and authorization_state must be
 * defined enum values, and boot_id must be nonzero.
 *
 * @param[in]  snapshot Snapshot to encode.
 * @param[out] out      Buffer, or NULL to query the size.
 * @param[in]  capacity Buffer capacity.
 * @param[out] out_len  Bytes written or required.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM when the buffer is
 *         too small or the snapshot exceeds the contract maximum.
 */
esp_err_t ble_link_snapshot_encode(
    const ble_link_snapshot_t *snapshot, uint8_t *out, size_t capacity,
    size_t *out_len);

#ifdef UNIT_TEST_HOST
/**
 * @brief Test-only seam: set the sequence counter directly.
 */
void ble_link_events_test_set_sequence(uint64_t value);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_EVENTS_H__ */
