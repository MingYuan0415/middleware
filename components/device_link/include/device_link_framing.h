#ifndef __DEVICE_LINK_FRAMING_H__
#define __DEVICE_LINK_FRAMING_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Framing version fixed by the Device Link v1 contract. */
#define DEVICE_LINK_FRAMING_VERSION 1U

/** @brief Fragment header size in bytes. */
#define DEVICE_LINK_FRAMING_HEADER_BYTES 8U

/** @brief First fragment of a message. */
#define DEVICE_LINK_FRAMING_FLAG_START 0x01U

/** @brief Final fragment of a message. */
#define DEVICE_LINK_FRAMING_FLAG_END 0x02U

/** @brief Largest representable total message length. */
#define DEVICE_LINK_FRAMING_MAX_TOTAL_LENGTH 0xFFFFU

/** @brief Maximum accepted GATT value size (ATT MTU 498 minus ATT header). */
#define DEVICE_LINK_FRAMING_MAX_VALUE_BYTES 495U

/** @brief Decoded fragment header. */
typedef struct device_link_fragment_header
{
    uint8_t version;    /**< Framing version, must be 1. */
    uint8_t flags;      /**< START/END bitmask, no unknown bits. */
    uint16_t frame_id;  /**< Nonzero correlation for one message. */
    uint16_t total_length; /**< Complete message length in every fragment. */
    uint16_t offset;    /**< Payload offset inside the message. */
} device_link_fragment_header_t;

/** @brief Status returned by framing operations. */
typedef enum
{
    DEVICE_LINK_FRAME_OK = 0,
    DEVICE_LINK_FRAME_ERR_INVALID_ARG,   /**< Invalid parameters or capacity. */
    DEVICE_LINK_FRAME_ERR_INVALID_FRAGMENT, /**< Fragment violates the contract. */
    DEVICE_LINK_FRAME_ERR_BAD_STATE,     /**< Reassembler slot is unusable. */
} device_link_frame_status_t;

/** @brief Result of feeding one fragment into a reassembler. */
typedef enum
{
    DEVICE_LINK_FRAME_ACCEPTED = 0,  /**< Fragment stored, message continues. */
    DEVICE_LINK_FRAME_COMPLETE,      /**< Message complete, payload available. */
    DEVICE_LINK_FRAME_DUPLICATE,     /**< Exact duplicate, no state change. */
    DEVICE_LINK_FRAME_REJECTED,      /**< Contract violation, reset required. */
} device_link_frame_result_t;

/** @brief One reassembly slot for a connection generation and channel. */
typedef struct device_link_reassembler
{
    uint8_t *buffer;       /**< Caller-owned payload storage. */
    size_t capacity;       /**< Storage capacity, at least total_length. */
    size_t total_length;   /**< Expected message length once known. */
    size_t next_offset;    /**< Next expected payload offset. */
    uint16_t frame_id;     /**< Active frame ID once started. */
    bool started;          /**< First fragment accepted. */
    bool complete;         /**< Message delivered once. */
    bool failed;           /**< Slot rejected and awaiting reset. */
    uint8_t last_value[DEVICE_LINK_FRAMING_MAX_VALUE_BYTES];
    size_t last_value_len; /**< Length of the most recent accepted value. */
} device_link_reassembler_t;

/**
 * @brief Encode one fragment according to the Device Link framing contract.
 *
 * @param[in]  header       Header with version, flags, frame ID, total and
 *                          offset; the version must equal 1 and flags must
 *                          contain no unknown bits.
 * @param[in]  payload      Fragment payload, nonempty.
 * @param[in]  payload_len  Payload length.
 * @param[out] out          Destination buffer of at least
 *                          DEVICE_LINK_FRAMING_HEADER_BYTES + payload_len.
 * @param[in]  out_capacity Destination capacity.
 * @param[out] out_len      Bytes written on success.
 *
 * @return DEVICE_LINK_FRAME_OK or an invalid-argument status.
 */
device_link_frame_status_t device_link_framing_encode(
    const device_link_fragment_header_t *header,
    const uint8_t *payload, size_t payload_len,
    uint8_t *out, size_t out_capacity, size_t *out_len);

/**
 * @brief Validate and split one GATT value into header and payload.
 *
 * @param[in]  value        Raw GATT characteristic value.
 * @param[in]  value_len    Value length.
 * @param[out] header       Decoded and statically validated header.
 * @param[out] payload      Payload pointer inside value.
 * @param[out] payload_len  Payload length.
 *
 * @return DEVICE_LINK_FRAME_OK or DEVICE_LINK_FRAME_ERR_INVALID_FRAGMENT.
 */
device_link_frame_status_t device_link_framing_parse(
    const uint8_t *value, size_t value_len,
    device_link_fragment_header_t *header,
    const uint8_t **payload, size_t *payload_len);

/**
 * @brief Bind a reassembler slot to caller-owned storage.
 *
 * @param[in,out] reassembler Slot to initialize and reset.
 * @param[in]     buffer      Payload storage, at least capacity bytes.
 * @param[in]     capacity    Storage capacity.
 */
void device_link_reassembler_init(
    device_link_reassembler_t *reassembler,
    uint8_t *buffer, size_t capacity);

/**
 * @brief Reset the slot after completion, disconnect, or rejection.
 *
 * @param[in,out] reassembler Slot to reset.
 */
void device_link_reassembler_reset(device_link_reassembler_t *reassembler);

/**
 * @brief Feed one fragment into the slot.
 *
 * A first fragment must carry START, offset zero, and a nonempty payload.
 * Later fragments must continue the active frame in order. An exact duplicate
 * of the most recent value is accepted without state change. The final
 * fragment must carry END and reach the total length.
 *
 * @param[in,out] reassembler  Active slot.
 * @param[in]     value        Raw GATT characteristic value.
 * @param[in]     value_len    Value length.
 * @param[out]    delivered_len Message length on DEVICE_LINK_FRAME_COMPLETE.
 *
 * @return DEVICE_LINK_FRAME_ACCEPTED, DEVICE_LINK_FRAME_COMPLETE,
 *         DEVICE_LINK_FRAME_DUPLICATE, or DEVICE_LINK_FRAME_REJECTED.
 */
device_link_frame_result_t device_link_reassembler_feed(
    device_link_reassembler_t *reassembler,
    const uint8_t *value, size_t value_len, size_t *delivered_len);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_FRAMING_H__ */
