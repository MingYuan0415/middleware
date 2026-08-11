#ifndef __BLE_LINK_REASSEMBLER_H__
#define __BLE_LINK_REASSEMBLER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Contract-frozen framing constants. */
#define BLE_LINK_FRAMING_VERSION 1U
#define BLE_LINK_FRAMING_HEADER_BYTES 8U
#define BLE_LINK_FRAMING_FLAG_START 1U
#define BLE_LINK_FRAMING_FLAG_END 2U
#define BLE_LINK_FRAMING_IDLE_TIMEOUT_MS 5000U

/**
 * @brief One decoded fragment header.
 */
typedef struct ble_link_fragment
{
    uint8_t version;
    uint8_t flags;
    uint16_t frame_id;
    uint16_t total_length;
    uint16_t offset;
    const uint8_t *payload; /**< Payload slice in the input buffer. */
    size_t payload_len;
} ble_link_fragment_t;

/** @brief Outcome of one valid fragment accepted by the reassembler. */
typedef enum
{
    BLE_LINK_REASSEMBLY_NEW_PARTIAL = 0, /**< New bytes appended; frame incomplete. */
    BLE_LINK_REASSEMBLY_DUPLICATE,       /**< Exact latest-fragment duplicate. */
    BLE_LINK_REASSEMBLY_COMPLETE,        /**< New bytes completed the frame. */
} ble_link_reassembly_disposition_t;

/**
 * @brief Reassembly slot for one RX characteristic and connection
 * generation.
 *
 * The caller owns the connection-generation filtering and the idle timeout
 * (BLE_LINK_FRAMING_IDLE_TIMEOUT_MS): it must call
 * ble_link_reassembler_reset when a disconnect, generation change, timeout,
 * or protocol error occurs.
 */
typedef struct ble_link_reassembler
{
    uint16_t frame_id;
    uint16_t total_length;
    uint16_t received;
    uint16_t last_offset;
    uint8_t last_flags;
    size_t last_len;
    uint8_t *buffer;
    size_t capacity;
    bool started;
    bool completed_fragment_valid;
} ble_link_reassembler_t;

/**
 * @brief Initialize a reassembly slot with a caller-owned buffer.
 *
 * @param[in,out] slot     Slot to initialize.
 * @param[out]    buffer   Reassembly buffer.
 * @param[in]     capacity Buffer capacity.
 */
void ble_link_reassembler_init(
    ble_link_reassembler_t *slot, uint8_t *buffer, size_t capacity);

/**
 * @brief Reset the slot, discarding any partial frame.
 *
 * The caller-owned buffer and capacity are preserved; only transient
 * reassembly state is cleared, so the slot stays usable after a reset or a
 * completed frame.
 */
void ble_link_reassembler_reset(ble_link_reassembler_t *slot);

/**
 * @brief Parse one raw characteristic write value into a fragment.
 *
 * Enforces the header shape: the value must be at least
 * BLE_LINK_FRAMING_HEADER_BYTES long and the header fields are little-endian
 * version/flags/frame_id/total_length/offset. A value shorter than the
 * header is rejected.
 *
 * @param[in]  value    Raw characteristic write value.
 * @param[in]  value_len Value length in bytes.
 * @param[out] out      Decoded fragment; payload points into value.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t ble_link_reassembler_parse(
    const uint8_t *value, size_t value_len, ble_link_fragment_t *out);

/**
 * @brief Accept one fragment.
 *
 * Enforces the frozen framing contract: nonzero frame id, no unknown flag
 * bits, a nonempty payload, nonzero total length, START on the first
 * fragment at offset zero, END on the final fragment with
 * offset + payload == total length, monotonically increasing offsets without
 * gaps or conflicting overlap, unchanged total length, and no unexpected
 * START. An exact duplicate of the most recently accepted fragment is
 * accepted without appending, including the final fragment after delivery.
 * Frame IDs identify only the active message: after completion, any nonzero
 * ID may begin the next message. The retained final-fragment tombstone is
 * cleared as soon as a different valid START fragment is accepted.
 *
 * @param[in,out] slot    Reassembly slot.
 * @param[in]     fragment Decoded fragment.
 * @return ESP_OK when the frame is complete (payload delivered via the
 *         slot buffer),
 *         ESP_ERR_NOT_FINISHED after an accepted intermediate fragment,
 *         ESP_ERR_INVALID_ARG for a contract violation, or
 *         ESP_ERR_NO_MEM when the frame exceeds the slot capacity.
 */
esp_err_t ble_link_reassembler_accept(
    ble_link_reassembler_t *slot, const ble_link_fragment_t *fragment);

/**
 * @brief Accept one fragment and report whether it added new bytes.
 *
 * This applies the same validation as ble_link_reassembler_accept(), but
 * returns ESP_OK for every valid fragment and reports the precise outcome in
 * `out_disposition`. Callers use DUPLICATE to avoid refreshing the 5000 ms
 * idle deadline and COMPLETE to deliver the assembled buffer exactly once.
 *
 * @param[in,out] slot            Reassembly slot.
 * @param[in]     fragment        Decoded fragment.
 * @param[out]    out_disposition Acceptance outcome.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a contract violation, or
 *         ESP_ERR_NO_MEM when the frame exceeds the slot capacity.
 */
esp_err_t ble_link_reassembler_accept_ex(
    ble_link_reassembler_t *slot, const ble_link_fragment_t *fragment,
    ble_link_reassembly_disposition_t *out_disposition);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_REASSEMBLER_H__ */
