#ifndef __BLE_LINK_CODEC_H__
#define __BLE_LINK_CODEC_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum retained unknown fields per message. */
#define BLE_LINK_CODEC_MAX_UNKNOWN_FIELDS 8U

/** @brief Maximum repeated envelope flag values per message. */
#define BLE_LINK_CODEC_MAX_FLAGS 4U

/** @brief One unknown-field byte span in the decoded input buffer. */
typedef struct ble_link_codec_unknown_field
{
    const uint8_t *data;
    size_t len;
} ble_link_codec_unknown_field_t;

/** @brief Envelope body tags, frozen by microtech.link.v1.Envelope. */
typedef enum
{
    BLE_LINK_CODEC_BODY_NONE = 0,
    BLE_LINK_CODEC_BODY_REQUEST = 10,
    BLE_LINK_CODEC_BODY_RESPONSE = 11,
    BLE_LINK_CODEC_BODY_EVENT = 12,
    BLE_LINK_CODEC_BODY_SNAPSHOT = 13,
    BLE_LINK_CODEC_BODY_TRANSFER_CONTROL = 14,
} ble_link_codec_body_t;

/** @brief Request body tags, frozen by microtech.link.v1.Request. */
typedef enum
{
    BLE_LINK_CODEC_REQUEST_NONE = 0,
    BLE_LINK_CODEC_REQUEST_GET_CAPABILITIES = 10,
    BLE_LINK_CODEC_REQUEST_GET_LINK_SNAPSHOT = 11,
    BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE = 12,
    BLE_LINK_CODEC_REQUEST_AUTHORIZE_COMMIT = 13,
    BLE_LINK_CODEC_REQUEST_SUBSCRIBE_EVENTS = 14,
    BLE_LINK_CODEC_REQUEST_GET_AUTHORIZATION = 15,
} ble_link_codec_request_tag_t;

/** @brief Response body tags, frozen by microtech.link.v1.Response. */
typedef enum
{
    BLE_LINK_CODEC_RESPONSE_NONE = 0,
    BLE_LINK_CODEC_RESPONSE_CAPABILITIES = 10,
    BLE_LINK_CODEC_RESPONSE_SNAPSHOT = 11,
    BLE_LINK_CODEC_RESPONSE_AUTHORIZE_PREPARE = 12,
    BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT = 13,
    BLE_LINK_CODEC_RESPONSE_EVENT_SUBSCRIPTION = 14,
} ble_link_codec_response_tag_t;

/** @brief Decoded envelope shell; bodies are exposed as opaque slices. */
typedef struct ble_link_codec_envelope
{
    uint32_t protocol_major;
    uint32_t protocol_minor;
    uint64_t boot_id;
    uint32_t flags;       /**< OR of flags_values; encoder validates equality. */
    uint32_t flags_values[BLE_LINK_CODEC_MAX_FLAGS];
    size_t flags_count;   /**< Number of repeated flag values. */
    ble_link_codec_body_t body;
    const uint8_t *body_data;
    size_t body_len;
    ble_link_codec_unknown_field_t unknown_fields[
        BLE_LINK_CODEC_MAX_UNKNOWN_FIELDS];
    size_t unknown_fields_count;
} ble_link_codec_envelope_t;

/** @brief Decoded request shell; the body payload is opaque. */
typedef struct ble_link_codec_request
{
    uint64_t request_id;
    ble_link_codec_request_tag_t body;
    const uint8_t *body_data;
    size_t body_len;
} ble_link_codec_request_t;

/** @brief Decoded response shell; the body payload is opaque. */
typedef struct ble_link_codec_response
{
    uint64_t request_id;
    uint32_t error;
    ble_link_codec_response_tag_t body;
    const uint8_t *body_data;
    size_t body_len;
} ble_link_codec_response_t;

/**
 * @brief Decode one Envelope from a buffer.
 *
 * The envelope and all slices reference the input buffer and stay valid only
 * while it does. Unknown fields are retained verbatim as individual raw
 * spans (up to BLE_LINK_CODEC_MAX_UNKNOWN_FIELDS) and re-emitted by the
 * encoder in parse order, preserving proto3 unknown-field semantics for the
 * frozen round-trip fixtures. The envelope is rejected when a field uses an
 * invalid wire type for its number, a fixed64 or submessage is truncated,
 * the body is a field this codec does not recognize at the envelope level,
 * more than one body field appears, or more than the unknown-field capacity
 * is exhausted.
 *
 * @param[in]  data Envelope bytes.
 * @param[in]  len  Envelope length.
 * @param[out] out  Decoded shell.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE for a wire
 *         format violation.
 */
esp_err_t ble_link_codec_decode_envelope(
    const uint8_t *data, size_t len, ble_link_codec_envelope_t *out);

/**
 * @brief Decode one Request message (a valid envelope body).
 *
 * @param[in]  data Request bytes.
 * @param[in]  len  Request length.
 * @param[out] out  Decoded shell.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE for a wire
 *         format violation.
 */
esp_err_t ble_link_codec_decode_request(
    const uint8_t *data, size_t len, ble_link_codec_request_t *out);

/**
 * @brief Decode one Response message (a valid envelope body).
 *
 * @param[in]  data Response bytes.
 * @param[in]  len  Response length.
 * @param[out] out  Decoded shell.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE for a wire
 *         format violation.
 */
esp_err_t ble_link_codec_decode_response(
    const uint8_t *data, size_t len, ble_link_codec_response_t *out);

/**
 * @brief Encode one Envelope into a caller buffer.
 *
 * Writes the shell fields followed by the opaque body bytes unchanged. The
 * encoded size can be queried first with a NULL buffer.
 *
 * @param[in]  envelope Shell to encode.
 * @param[out] out      Buffer, or NULL to query the size.
 * @param[in]  capacity Buffer capacity.
 * @param[out] out_len  Bytes written or required.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM when the buffer is
 *         too small.
 */
esp_err_t ble_link_codec_encode_envelope(
    const ble_link_codec_envelope_t *envelope,
    uint8_t *out, size_t capacity, size_t *out_len);

/**
 * @brief Encode one Request into a caller buffer.
 *
 * @param[in]  request  Shell to encode.
 * @param[out] out      Buffer, or NULL to query the size.
 * @param[in]  capacity Buffer capacity.
 * @param[out] out_len  Bytes written or required.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM when the buffer is
 *         too small.
 */
esp_err_t ble_link_codec_encode_request(
    const ble_link_codec_request_t *request,
    uint8_t *out, size_t capacity, size_t *out_len);

/**
 * @brief Encode one Response into a caller buffer.
 *
 * @param[in]  response Shell to encode.
 * @param[out] out      Buffer, or NULL to query the size.
 * @param[in]  capacity Buffer capacity.
 * @param[out] out_len  Bytes written or required.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM when the buffer is
 *         too small.
 */
esp_err_t ble_link_codec_encode_response(
    const ble_link_codec_response_t *response,
    uint8_t *out, size_t capacity, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_CODEC_H__ */
