#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_codec.h"

#define DBG_TAG "ble_link_codec"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

#define BLE_LINK_CODEC_WIRE_VARINT 0U
#define BLE_LINK_CODEC_WIRE_FIXED64 1U
#define BLE_LINK_CODEC_WIRE_LEN 2U
#define BLE_LINK_CODEC_WIRE_FIXED32 5U

typedef struct ble_link_codec_reader
{
    const uint8_t *data;
    size_t len;
    size_t pos;
} ble_link_codec_reader_t;

static esp_err_t _ble_link_codec_read_varint(
    ble_link_codec_reader_t *reader, uint64_t *out)
{
    uint64_t value = 0U;
    unsigned int shift = 0U;

    unsigned int bytes = 0U;

    while (reader->pos < reader->len && bytes < 10U)
    {
        const uint8_t byte = reader->data[reader->pos];

        reader->pos++;
        if (bytes == 9U && (byte & 0xfeU) != 0U)
        {
            return ESP_ERR_INVALID_STATE;
        }
        value |= (uint64_t)(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0U)
        {
            *out = value;
            return ESP_OK;
        }
        shift += 7U;
        bytes++;
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t _ble_link_codec_read_bytes(
    ble_link_codec_reader_t *reader, const uint8_t **out, size_t *out_len)
{
    uint64_t length = 0U;

    if (_ble_link_codec_read_varint(reader, &length) != ESP_OK ||
            length > reader->len - reader->pos)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *out = &reader->data[reader->pos];
    *out_len = (size_t)length;
    reader->pos += (size_t)length;
    return ESP_OK;
}

static esp_err_t _ble_link_codec_read_fixed64(
    ble_link_codec_reader_t *reader, uint64_t *out)
{
    if (reader->len - reader->pos < 8U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t value = 0U;

    for (unsigned int i = 0U; i < 8U; ++i)
    {
        value |= (uint64_t)reader->data[reader->pos + i] << (8U * i);
    }
    reader->pos += 8U;
    *out = value;
    return ESP_OK;
}

static size_t _ble_link_codec_varint_size(uint64_t value)
{
    size_t size = 1U;

    while (value >= 0x80U)
    {
        value >>= 7U;
        size++;
    }
    return size;
}

static void _ble_link_codec_write_varint(
    uint8_t *out, size_t *pos, uint64_t value)
{
    while (value >= 0x80U)
    {
        out[(*pos)++] = (uint8_t)(value & 0x7fU) | 0x80U;
        value >>= 7U;
    }
    out[(*pos)++] = (uint8_t)value;
}

static void _ble_link_codec_write_tag(
    uint8_t *out, size_t *pos, uint32_t field, uint32_t wire)
{
    _ble_link_codec_write_varint(out, pos, ((uint64_t)field << 3U) | wire);
}

static void _ble_link_codec_write_fixed64(
    uint8_t *out, size_t *pos, uint64_t value)
{
    for (unsigned int i = 0U; i < 8U; ++i)
    {
        out[(*pos)++] = (uint8_t)(value >> (8U * i));
    }
}

static void _ble_link_codec_write_bytes(
    uint8_t *out, size_t *pos, const uint8_t *data, size_t len)
{
    _ble_link_codec_write_varint(out, pos, len);
    for (size_t i = 0U; i < len; ++i)
    {
        out[(*pos)++] = data[i];
    }
}

static bool _ble_link_codec_valid_envelope_body(ble_link_codec_body_t body)
{
    return body == BLE_LINK_CODEC_BODY_REQUEST ||
           body == BLE_LINK_CODEC_BODY_RESPONSE ||
           body == BLE_LINK_CODEC_BODY_EVENT ||
           body == BLE_LINK_CODEC_BODY_SNAPSHOT ||
           body == BLE_LINK_CODEC_BODY_TRANSFER_CONTROL;
}

static bool _ble_link_codec_valid_request_body(ble_link_codec_request_tag_t body)
{
    return body == BLE_LINK_CODEC_REQUEST_GET_CAPABILITIES ||
           body == BLE_LINK_CODEC_REQUEST_GET_LINK_SNAPSHOT ||
           body == BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE ||
           body == BLE_LINK_CODEC_REQUEST_AUTHORIZE_COMMIT ||
           body == BLE_LINK_CODEC_REQUEST_SUBSCRIBE_EVENTS ||
           body == BLE_LINK_CODEC_REQUEST_GET_AUTHORIZATION;
}

static bool _ble_link_codec_valid_response_body(
    ble_link_codec_response_tag_t body)
{
    return body == BLE_LINK_CODEC_RESPONSE_CAPABILITIES ||
           body == BLE_LINK_CODEC_RESPONSE_SNAPSHOT ||
           body == BLE_LINK_CODEC_RESPONSE_AUTHORIZE_PREPARE ||
           body == BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT ||
           body == BLE_LINK_CODEC_RESPONSE_EVENT_SUBSCRIPTION;
}

/**
 * @brief Add a size component with overflow detection.
 */
static bool _ble_link_codec_size_add(size_t *size, size_t addend)
{
    if (*size > SIZE_MAX - addend)
    {
        return false;
    }
    *size += addend;
    return true;
}

/**
 * @brief Append one envelope flag value, rejecting duplicates and capacity
 * overflow.
 */
static bool _ble_link_codec_add_flag(
    ble_link_codec_envelope_t *envelope, uint32_t value)
{
    if (value == 0U)
    {
        return false;
    }
    for (size_t i = 0U; i < envelope->flags_count; ++i)
    {
        if (envelope->flags_values[i] == value)
        {
            return false;
        }
    }
    if (envelope->flags_count >= BLE_LINK_CODEC_MAX_FLAGS)
    {
        return false;
    }
    envelope->flags_values[envelope->flags_count] = value;
    envelope->flags_count++;
    envelope->flags |= value;
    return true;
}

esp_err_t ble_link_codec_decode_envelope(
    const uint8_t *data, size_t len, ble_link_codec_envelope_t *out)
{
    ble_link_codec_reader_t reader;

    if (data == NULL || len == 0U || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    reader.data = data;
    reader.len = len;
    reader.pos = 0U;
    while (reader.pos < reader.len)
    {
        const size_t field_start = reader.pos;
        uint64_t tag = 0U;
        uint32_t field;
        uint32_t wire;

        if (_ble_link_codec_read_varint(&reader, &tag) != ESP_OK)
        {
            return ESP_ERR_INVALID_STATE;
        }
        wire = (uint32_t)(tag & 0x7U);
        if ((tag >> 3U) == 0U || (tag >> 3U) > 0x1fffffffU ||
                wire > 5U || wire == 3U || wire == 4U)
        {
            return ESP_ERR_INVALID_STATE;
        }
        field = (uint32_t)(tag >> 3U);
        if (!(field == 1U || field == 2U || field == 3U || field == 4U ||
                (field >= 10U && field <= 15U)))
        {
            /* Unknown field: skip the payload and retain the raw span. */
            if (out->unknown_fields_count >= BLE_LINK_CODEC_MAX_UNKNOWN_FIELDS)
            {
                return ESP_ERR_INVALID_STATE;
            }
            if (wire == BLE_LINK_CODEC_WIRE_VARINT)
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_varint(&reader, &value) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_LEN)
            {
                const uint8_t *ignored = NULL;
                size_t ignored_len = 0U;

                if (_ble_link_codec_read_bytes(&reader, &ignored,
                                               &ignored_len) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_FIXED64)
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_fixed64(&reader, &value) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_FIXED32)
            {
                if (reader.len - reader.pos < 4U)
                {
                    return ESP_ERR_INVALID_STATE;
                }
                reader.pos += 4U;
            }
            out->unknown_fields[out->unknown_fields_count].data =
                &data[field_start];
            out->unknown_fields[out->unknown_fields_count].len =
                reader.pos - field_start;
            out->unknown_fields_count++;
            continue;
        }
        switch (field)
        {
        case 1U: /* protocol_major */
            if (wire != BLE_LINK_CODEC_WIRE_VARINT)
            {
                return ESP_ERR_INVALID_STATE;
            }
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_varint(&reader, &value) != ESP_OK ||
                        value > UINT32_MAX)
                {
                    return ESP_ERR_INVALID_STATE;
                }
                out->protocol_major = (uint32_t)value;
            }
            break;
        case 2U: /* protocol_minor */
            if (wire != BLE_LINK_CODEC_WIRE_VARINT)
            {
                return ESP_ERR_INVALID_STATE;
            }
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_varint(&reader, &value) != ESP_OK ||
                        value > UINT32_MAX)
                {
                    return ESP_ERR_INVALID_STATE;
                }
                out->protocol_minor = (uint32_t)value;
            }
            break;
        case 3U: /* boot_id, fixed64 */
            if (wire != BLE_LINK_CODEC_WIRE_FIXED64)
            {
                return ESP_ERR_INVALID_STATE;
            }
            if (_ble_link_codec_read_fixed64(&reader, &out->boot_id) != ESP_OK)
            {
                return ESP_ERR_INVALID_STATE;
            }
            break;
        case 4U: /* flags, repeated varint (packed or unpacked) */
            if (wire == BLE_LINK_CODEC_WIRE_VARINT)
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_varint(&reader, &value) != ESP_OK ||
                        value > UINT32_MAX)
                {
                    return ESP_ERR_INVALID_STATE;
                }
                if (!_ble_link_codec_add_flag(out, (uint32_t)value))
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_LEN)
            {
                const uint8_t *packed = NULL;
                size_t packed_len = 0U;

                if (_ble_link_codec_read_bytes(&reader, &packed,
                                               &packed_len) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
                ble_link_codec_reader_t packed_reader;

                packed_reader.data = packed;
                packed_reader.len = packed_len;
                packed_reader.pos = 0U;
                while (packed_reader.pos < packed_reader.len)
                {
                    uint64_t value = 0U;

                    if (_ble_link_codec_read_varint(&packed_reader, &value) !=
                            ESP_OK || value > UINT32_MAX)
                    {
                        return ESP_ERR_INVALID_STATE;
                    }
                    if (!_ble_link_codec_add_flag(out, (uint32_t)value))
                    {
                        return ESP_ERR_INVALID_STATE;
                    }
                }
            }
            else
            {
                return ESP_ERR_INVALID_STATE;
            }
            break;
        case 10U: /* request */
        case 11U: /* response */
        case 12U: /* event */
        case 13U: /* snapshot */
        case 14U: /* transfer_control */
            if (wire != BLE_LINK_CODEC_WIRE_LEN)
            {
                return ESP_ERR_INVALID_STATE;
            }
            if (out->body != BLE_LINK_CODEC_BODY_NONE)
            {
                return ESP_ERR_INVALID_STATE;
            }
            if (_ble_link_codec_read_bytes(&reader, &out->body_data,
                                           &out->body_len) != ESP_OK)
            {
                return ESP_ERR_INVALID_STATE;
            }
            out->body = (ble_link_codec_body_t)field;
            break;
        default:
            /* Unknown field: skip by wire type so a round trip keeps it. */
            if (wire == BLE_LINK_CODEC_WIRE_VARINT)
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_varint(&reader, &value) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_LEN)
            {
                const uint8_t *ignored = NULL;
                size_t ignored_len = 0U;

                if (_ble_link_codec_read_bytes(&reader, &ignored,
                                               &ignored_len) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_FIXED64)
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_fixed64(&reader, &value) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_FIXED32)
            {
                if (reader.len - reader.pos < 4U)
                {
                    return ESP_ERR_INVALID_STATE;
                }
                reader.pos += 4U;
            }
            else
            {
                return ESP_ERR_INVALID_STATE;
            }
            break;
        }
    }
    return ESP_OK;
}

esp_err_t ble_link_codec_decode_request(
    const uint8_t *data, size_t len, ble_link_codec_request_t *out)
{
    ble_link_codec_reader_t reader;

    if (data == NULL || len == 0U || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    reader.data = data;
    reader.len = len;
    reader.pos = 0U;
    while (reader.pos < reader.len)
    {
        uint64_t tag = 0U;
        uint32_t field;
        uint32_t wire;

        if (_ble_link_codec_read_varint(&reader, &tag) != ESP_OK)
        {
            return ESP_ERR_INVALID_STATE;
        }
        wire = (uint32_t)(tag & 0x7U);
        if ((tag >> 3U) == 0U || (tag >> 3U) > 0x1fffffffU ||
                wire > 5U || wire == 3U || wire == 4U)
        {
            return ESP_ERR_INVALID_STATE;
        }
        field = (uint32_t)(tag >> 3U);
        switch (field)
        {
        case 1U: /* request_id, fixed64 */
            if (wire != BLE_LINK_CODEC_WIRE_FIXED64)
            {
                return ESP_ERR_INVALID_STATE;
            }
            if (_ble_link_codec_read_fixed64(&reader, &out->request_id) != ESP_OK)
            {
                return ESP_ERR_INVALID_STATE;
            }
            break;
        case 10U: /* get_capabilities */
        case 11U: /* get_link_snapshot */
        case 12U: /* authorize_prepare */
        case 13U: /* authorize_commit */
        case 14U: /* subscribe_events */
        case 15U: /* get_authorization */
            if (wire != BLE_LINK_CODEC_WIRE_LEN)
            {
                return ESP_ERR_INVALID_STATE;
            }
            if (out->body != BLE_LINK_CODEC_REQUEST_NONE)
            {
                return ESP_ERR_INVALID_STATE;
            }
            if (_ble_link_codec_read_bytes(&reader, &out->body_data,
                                           &out->body_len) != ESP_OK)
            {
                return ESP_ERR_INVALID_STATE;
            }
            out->body = (ble_link_codec_request_tag_t)field;
            break;
        default:
            if (wire == BLE_LINK_CODEC_WIRE_VARINT)
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_varint(&reader, &value) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_LEN)
            {
                const uint8_t *ignored = NULL;
                size_t ignored_len = 0U;

                if (_ble_link_codec_read_bytes(&reader, &ignored,
                                               &ignored_len) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_FIXED64)
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_fixed64(&reader, &value) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_FIXED32)
            {
                if (reader.len - reader.pos < 4U)
                {
                    return ESP_ERR_INVALID_STATE;
                }
                reader.pos += 4U;
            }
            else
            {
                return ESP_ERR_INVALID_STATE;
            }
            break;
        }
    }
    return ESP_OK;
}

esp_err_t ble_link_codec_decode_response(
    const uint8_t *data, size_t len, ble_link_codec_response_t *out)
{
    ble_link_codec_reader_t reader;

    if (data == NULL || len == 0U || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    reader.data = data;
    reader.len = len;
    reader.pos = 0U;
    while (reader.pos < reader.len)
    {
        uint64_t tag = 0U;
        uint32_t field;
        uint32_t wire;

        if (_ble_link_codec_read_varint(&reader, &tag) != ESP_OK)
        {
            return ESP_ERR_INVALID_STATE;
        }
        wire = (uint32_t)(tag & 0x7U);
        if ((tag >> 3U) == 0U || (tag >> 3U) > 0x1fffffffU ||
                wire > 5U || wire == 3U || wire == 4U)
        {
            return ESP_ERR_INVALID_STATE;
        }
        field = (uint32_t)(tag >> 3U);
        switch (field)
        {
        case 1U: /* request_id, fixed64 */
            if (wire != BLE_LINK_CODEC_WIRE_FIXED64)
            {
                return ESP_ERR_INVALID_STATE;
            }
            if (_ble_link_codec_read_fixed64(&reader, &out->request_id) != ESP_OK)
            {
                return ESP_ERR_INVALID_STATE;
            }
            break;
        case 2U: /* error, varint */
            if (wire != BLE_LINK_CODEC_WIRE_VARINT)
            {
                return ESP_ERR_INVALID_STATE;
            }
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_varint(&reader, &value) != ESP_OK ||
                        value > UINT32_MAX)
                {
                    return ESP_ERR_INVALID_STATE;
                }
                out->error = (uint32_t)value;
            }
            break;
        case 10U: /* capabilities */
        case 11U: /* snapshot */
        case 12U: /* authorization_prepare */
        case 13U: /* authorization_result */
        case 14U: /* event_subscription */
            if (wire != BLE_LINK_CODEC_WIRE_LEN)
            {
                return ESP_ERR_INVALID_STATE;
            }
            if (out->body != BLE_LINK_CODEC_RESPONSE_NONE)
            {
                return ESP_ERR_INVALID_STATE;
            }
            if (_ble_link_codec_read_bytes(&reader, &out->body_data,
                                           &out->body_len) != ESP_OK)
            {
                return ESP_ERR_INVALID_STATE;
            }
            out->body = (ble_link_codec_response_tag_t)field;
            break;
        default:
            if (wire == BLE_LINK_CODEC_WIRE_VARINT)
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_varint(&reader, &value) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_LEN)
            {
                const uint8_t *ignored = NULL;
                size_t ignored_len = 0U;

                if (_ble_link_codec_read_bytes(&reader, &ignored,
                                               &ignored_len) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_FIXED64)
            {
                uint64_t value = 0U;

                if (_ble_link_codec_read_fixed64(&reader, &value) != ESP_OK)
                {
                    return ESP_ERR_INVALID_STATE;
                }
            }
            else if (wire == BLE_LINK_CODEC_WIRE_FIXED32)
            {
                if (reader.len - reader.pos < 4U)
                {
                    return ESP_ERR_INVALID_STATE;
                }
                reader.pos += 4U;
            }
            else
            {
                return ESP_ERR_INVALID_STATE;
            }
            break;
        }
    }
    return ESP_OK;
}

esp_err_t ble_link_codec_encode_envelope(
    const ble_link_codec_envelope_t *envelope,
    uint8_t *out, size_t capacity, size_t *out_len)
{
    size_t size = 0U;

    if (envelope == NULL || out_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (envelope->body != BLE_LINK_CODEC_BODY_NONE &&
            (!_ble_link_codec_valid_envelope_body(envelope->body) ||
             (envelope->body_len > 0U && envelope->body_data == NULL)))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (envelope->unknown_fields_count > BLE_LINK_CODEC_MAX_UNKNOWN_FIELDS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < envelope->unknown_fields_count; ++i)
    {
        if (envelope->unknown_fields[i].data == NULL)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (envelope->protocol_major != 0U &&
            !_ble_link_codec_size_add(
                &size, 1U + _ble_link_codec_varint_size(
                    envelope->protocol_major)))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (envelope->protocol_minor != 0U &&
            !_ble_link_codec_size_add(
                &size, 1U + _ble_link_codec_varint_size(
                    envelope->protocol_minor)))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (envelope->boot_id != 0U && !_ble_link_codec_size_add(&size, 9U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (envelope->flags_count > 0U)
    {
        if (envelope->flags_count > BLE_LINK_CODEC_MAX_FLAGS)
        {
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t or_value = 0U;
        size_t payload_size = 0U;

        for (size_t i = 0U; i < envelope->flags_count; ++i)
        {
            const uint32_t value = envelope->flags_values[i];

            if (value == 0U)
            {
                return ESP_ERR_INVALID_ARG;
            }
            for (size_t j = 0U; j < i; ++j)
            {
                if (envelope->flags_values[j] == value)
                {
                    return ESP_ERR_INVALID_ARG;
                }
            }
            or_value |= value;
            payload_size += _ble_link_codec_varint_size(value);
        }
        if (or_value != envelope->flags)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (!_ble_link_codec_size_add(
                    &size, 1U + _ble_link_codec_varint_size(payload_size) +
                    payload_size))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (envelope->flags != 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (envelope->body != BLE_LINK_CODEC_BODY_NONE)
    {
        if (envelope->body_len > SIZE_MAX -
                (1U + _ble_link_codec_varint_size(envelope->body_len)))
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (!_ble_link_codec_size_add(
                    &size, 1U + _ble_link_codec_varint_size(envelope->body_len) +
                    envelope->body_len))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (size_t i = 0U; i < envelope->unknown_fields_count; ++i)
    {
        if (!_ble_link_codec_size_add(&size, envelope->unknown_fields[i].len))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (out == NULL)
    {
        *out_len = size;
        return ESP_OK;
    }
    if (capacity < size)
    {
        return ESP_ERR_NO_MEM;
    }
    size_t pos = 0U;

    if (envelope->protocol_major != 0U)
    {
        _ble_link_codec_write_tag(out, &pos, 1U, BLE_LINK_CODEC_WIRE_VARINT);
        _ble_link_codec_write_varint(out, &pos, envelope->protocol_major);
    }
    if (envelope->protocol_minor != 0U)
    {
        _ble_link_codec_write_tag(out, &pos, 2U, BLE_LINK_CODEC_WIRE_VARINT);
        _ble_link_codec_write_varint(out, &pos, envelope->protocol_minor);
    }
    if (envelope->boot_id != 0U)
    {
        _ble_link_codec_write_tag(out, &pos, 3U, BLE_LINK_CODEC_WIRE_FIXED64);
        _ble_link_codec_write_fixed64(out, &pos, envelope->boot_id);
    }
    if (envelope->flags_count > 0U)
    {
        size_t payload_size = 0U;

        for (size_t i = 0U; i < envelope->flags_count; ++i)
        {
            payload_size +=
                _ble_link_codec_varint_size(envelope->flags_values[i]);
        }
        _ble_link_codec_write_tag(out, &pos, 4U, BLE_LINK_CODEC_WIRE_LEN);
        _ble_link_codec_write_varint(out, &pos, payload_size);
        for (size_t i = 0U; i < envelope->flags_count; ++i)
        {
            _ble_link_codec_write_varint(out, &pos, envelope->flags_values[i]);
        }
    }
    if (envelope->body != BLE_LINK_CODEC_BODY_NONE)
    {
        _ble_link_codec_write_tag(out, &pos, envelope->body,
                                  BLE_LINK_CODEC_WIRE_LEN);
        _ble_link_codec_write_bytes(out, &pos, envelope->body_data,
                                    envelope->body_len);
    }
    for (size_t i = 0U; i < envelope->unknown_fields_count; ++i)
    {
        for (size_t j = 0U; j < envelope->unknown_fields[i].len; ++j)
        {
            out[pos++] = envelope->unknown_fields[i].data[j];
        }
    }
    *out_len = size;
    return ESP_OK;
}

esp_err_t ble_link_codec_encode_request(
    const ble_link_codec_request_t *request,
    uint8_t *out, size_t capacity, size_t *out_len)
{
    size_t size = 0U;

    if (request == NULL || out_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->body != BLE_LINK_CODEC_REQUEST_NONE &&
            (!_ble_link_codec_valid_request_body(request->body) ||
             (request->body_len > 0U && request->body_data == NULL)))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->request_id != 0U && !_ble_link_codec_size_add(&size, 9U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->body != BLE_LINK_CODEC_REQUEST_NONE)
    {
        if (request->body_len > SIZE_MAX -
                (1U + _ble_link_codec_varint_size(request->body_len)))
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (!_ble_link_codec_size_add(
                    &size, 1U + _ble_link_codec_varint_size(request->body_len) +
                    request->body_len))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (out == NULL)
    {
        *out_len = size;
        return ESP_OK;
    }
    if (capacity < size)
    {
        return ESP_ERR_NO_MEM;
    }
    size_t pos = 0U;

    if (request->request_id != 0U)
    {
        _ble_link_codec_write_tag(out, &pos, 1U, BLE_LINK_CODEC_WIRE_FIXED64);
        _ble_link_codec_write_fixed64(out, &pos, request->request_id);
    }
    if (request->body != BLE_LINK_CODEC_REQUEST_NONE)
    {
        _ble_link_codec_write_tag(out, &pos, request->body,
                                  BLE_LINK_CODEC_WIRE_LEN);
        _ble_link_codec_write_bytes(out, &pos, request->body_data,
                                    request->body_len);
    }
    *out_len = size;
    return ESP_OK;
}

esp_err_t ble_link_codec_encode_response(
    const ble_link_codec_response_t *response,
    uint8_t *out, size_t capacity, size_t *out_len)
{
    size_t size = 0U;

    if (response == NULL || out_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (response->body != BLE_LINK_CODEC_RESPONSE_NONE &&
            (!_ble_link_codec_valid_response_body(response->body) ||
             (response->body_len > 0U && response->body_data == NULL)))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (response->request_id != 0U && !_ble_link_codec_size_add(&size, 9U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (response->error != 0U &&
            !_ble_link_codec_size_add(
                &size, 1U + _ble_link_codec_varint_size(response->error)))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (response->body != BLE_LINK_CODEC_RESPONSE_NONE)
    {
        if (response->body_len > SIZE_MAX -
                (1U + _ble_link_codec_varint_size(response->body_len)))
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (!_ble_link_codec_size_add(
                    &size, 1U + _ble_link_codec_varint_size(response->body_len) +
                    response->body_len))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (out == NULL)
    {
        *out_len = size;
        return ESP_OK;
    }
    if (capacity < size)
    {
        return ESP_ERR_NO_MEM;
    }
    size_t pos = 0U;

    if (response->request_id != 0U)
    {
        _ble_link_codec_write_tag(out, &pos, 1U, BLE_LINK_CODEC_WIRE_FIXED64);
        _ble_link_codec_write_fixed64(out, &pos, response->request_id);
    }
    if (response->error != 0U)
    {
        _ble_link_codec_write_tag(out, &pos, 2U, BLE_LINK_CODEC_WIRE_VARINT);
        _ble_link_codec_write_varint(out, &pos, response->error);
    }
    if (response->body != BLE_LINK_CODEC_RESPONSE_NONE)
    {
        _ble_link_codec_write_tag(out, &pos, response->body,
                                  BLE_LINK_CODEC_WIRE_LEN);
        _ble_link_codec_write_bytes(out, &pos, response->body_data,
                                    response->body_len);
    }
    *out_len = size;
    return ESP_OK;
}
