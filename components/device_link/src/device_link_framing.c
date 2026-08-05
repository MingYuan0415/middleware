#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "device_link_framing.h"

#define DEVICE_LINK_FRAMING_KNOWN_FLAGS \
    (DEVICE_LINK_FRAMING_FLAG_START | DEVICE_LINK_FRAMING_FLAG_END)

static bool _framing_header_valid(
    const device_link_fragment_header_t *header)
{
    if (header == NULL)
    {
        return false;
    }
    if (header->version != DEVICE_LINK_FRAMING_VERSION)
    {
        return false;
    }
    if ((header->flags & ~DEVICE_LINK_FRAMING_KNOWN_FLAGS) != 0U)
    {
        return false;
    }
    if (header->frame_id == 0U)
    {
        return false;
    }
    if (header->total_length == 0U)
    {
        return false;
    }
    return true;
}

static bool _framing_bounds_valid(
    const device_link_fragment_header_t *header,
    const uint8_t *payload, size_t payload_len)
{
    const bool end = (header->flags & DEVICE_LINK_FRAMING_FLAG_END) != 0U;
    const bool fills = (size_t)header->offset + payload_len >=
                       header->total_length;
    if (payload == NULL && payload_len != 0U)
    {
        return false;
    }
    if (payload_len == 0U)
    {
        return false;
    }
    if (fills && (size_t)header->offset + payload_len != header->total_length)
    {
        return false;
    }
    if (end != fills)
    {
        return false;
    }
    return true;
}

device_link_frame_status_t device_link_framing_encode(
    const device_link_fragment_header_t *header,
    const uint8_t *payload, size_t payload_len,
    uint8_t *out, size_t out_capacity, size_t *out_len)
{
    if (out == NULL || out_len == NULL)
    {
        return DEVICE_LINK_FRAME_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    if (!_framing_header_valid(header) ||
            !_framing_bounds_valid(header, payload, payload_len))
    {
        return DEVICE_LINK_FRAME_ERR_INVALID_FRAGMENT;
    }
    if (payload_len > DEVICE_LINK_FRAMING_MAX_VALUE_BYTES -
            DEVICE_LINK_FRAMING_HEADER_BYTES)
    {
        return DEVICE_LINK_FRAME_ERR_INVALID_ARG;
    }
    if (out_capacity < DEVICE_LINK_FRAMING_HEADER_BYTES + payload_len)
    {
        return DEVICE_LINK_FRAME_ERR_INVALID_ARG;
    }
    out[0] = header->version;
    out[1] = header->flags;
    out[2] = (uint8_t)(header->frame_id & 0xFFU);
    out[3] = (uint8_t)(header->frame_id >> 8U);
    out[4] = (uint8_t)(header->total_length & 0xFFU);
    out[5] = (uint8_t)(header->total_length >> 8U);
    out[6] = (uint8_t)(header->offset & 0xFFU);
    out[7] = (uint8_t)(header->offset >> 8U);
    if (payload_len > 0U)
    {
        memcpy(out + DEVICE_LINK_FRAMING_HEADER_BYTES, payload, payload_len);
    }
    *out_len = DEVICE_LINK_FRAMING_HEADER_BYTES + payload_len;
    return DEVICE_LINK_FRAME_OK;
}

device_link_frame_status_t device_link_framing_parse(
    const uint8_t *value, size_t value_len,
    device_link_fragment_header_t *header,
    const uint8_t **payload, size_t *payload_len)
{
    if (value == NULL || header == NULL || payload == NULL || payload_len == NULL)
    {
        return DEVICE_LINK_FRAME_ERR_INVALID_ARG;
    }
    if (value_len < DEVICE_LINK_FRAMING_HEADER_BYTES)
    {
        return DEVICE_LINK_FRAME_ERR_INVALID_FRAGMENT;
    }
    if (value_len > DEVICE_LINK_FRAMING_MAX_VALUE_BYTES)
    {
        return DEVICE_LINK_FRAME_ERR_INVALID_FRAGMENT;
    }
    header->version = value[0];
    header->flags = value[1];
    header->frame_id = (uint16_t)(value[2] | ((uint16_t)value[3] << 8U));
    header->total_length = (uint16_t)(value[4] | ((uint16_t)value[5] << 8U));
    header->offset = (uint16_t)(value[6] | ((uint16_t)value[7] << 8U));
    if (!_framing_header_valid(header))
    {
        return DEVICE_LINK_FRAME_ERR_INVALID_FRAGMENT;
    }
    *payload = value + DEVICE_LINK_FRAMING_HEADER_BYTES;
    *payload_len = value_len - DEVICE_LINK_FRAMING_HEADER_BYTES;
    if (!_framing_bounds_valid(header, *payload, *payload_len))
    {
        return DEVICE_LINK_FRAME_ERR_INVALID_FRAGMENT;
    }
    return DEVICE_LINK_FRAME_OK;
}

void device_link_reassembler_init(
    device_link_reassembler_t *reassembler,
    uint8_t *buffer, size_t capacity)
{
    if (reassembler == NULL)
    {
        return;
    }
    memset(reassembler, 0, sizeof(*reassembler));
    reassembler->buffer = buffer;
    reassembler->capacity = capacity;
    reassembler->failed = buffer == NULL || capacity == 0U;
}

void device_link_reassembler_reset(device_link_reassembler_t *reassembler)
{
    if (reassembler == NULL)
    {
        return;
    }
    uint8_t *buffer = reassembler->buffer;
    const size_t capacity = reassembler->capacity;
    const bool failed = reassembler->failed && buffer == NULL;
    memset(reassembler, 0, sizeof(*reassembler));
    reassembler->buffer = buffer;
    reassembler->capacity = capacity;
    reassembler->failed = failed;
}

static device_link_frame_result_t _reassembler_reject(
    device_link_reassembler_t *reassembler)
{
    reassembler->failed = true;
    return DEVICE_LINK_FRAME_REJECTED;
}

device_link_frame_result_t device_link_reassembler_feed(
    device_link_reassembler_t *reassembler,
    const uint8_t *value, size_t value_len, size_t *delivered_len)
{
    device_link_fragment_header_t header;
    const uint8_t *payload;
    size_t payload_len;

    if (delivered_len != NULL)
    {
        *delivered_len = 0U;
    }
    if (reassembler == NULL)
    {
        return DEVICE_LINK_FRAME_REJECTED;
    }
    if (reassembler->failed)
    {
        return DEVICE_LINK_FRAME_REJECTED;
    }
    if (reassembler->complete)
    {
        return DEVICE_LINK_FRAME_REJECTED;
    }
    if (value_len == 0U ||
            device_link_framing_parse(value, value_len, &header, &payload,
                                      &payload_len) != DEVICE_LINK_FRAME_OK)
    {
        return _reassembler_reject(reassembler);
    }
    if (reassembler->started &&
            value_len == reassembler->last_value_len &&
            memcmp(value, reassembler->last_value, value_len) == 0)
    {
        return DEVICE_LINK_FRAME_DUPLICATE;
    }
    if (!reassembler->started)
    {
        if ((header.flags & DEVICE_LINK_FRAMING_FLAG_START) == 0U ||
                header.offset != 0U)
        {
            return _reassembler_reject(reassembler);
        }
        if ((size_t)header.total_length > reassembler->capacity)
        {
            return _reassembler_reject(reassembler);
        }
        reassembler->frame_id = header.frame_id;
        reassembler->total_length = header.total_length;
        reassembler->started = true;
    }
    else
    {
        if ((header.flags & DEVICE_LINK_FRAMING_FLAG_START) != 0U)
        {
            return _reassembler_reject(reassembler);
        }
        if (header.frame_id != reassembler->frame_id)
        {
            return _reassembler_reject(reassembler);
        }
        if (header.total_length != reassembler->total_length)
        {
            return _reassembler_reject(reassembler);
        }
        if (header.offset < reassembler->next_offset)
        {
            return _reassembler_reject(reassembler);
        }
        if (header.offset != reassembler->next_offset)
        {
            return _reassembler_reject(reassembler);
        }
    }
    if (reassembler->next_offset + payload_len > reassembler->total_length)
    {
        return _reassembler_reject(reassembler);
    }
    memcpy(reassembler->buffer + reassembler->next_offset, payload, payload_len);
    reassembler->next_offset += payload_len;
    reassembler->last_value_len = value_len;
    memcpy(reassembler->last_value, value, value_len);
    if ((header.flags & DEVICE_LINK_FRAMING_FLAG_END) != 0U)
    {
        reassembler->complete = true;
        if (delivered_len != NULL)
        {
            *delivered_len = reassembler->total_length;
        }
        return DEVICE_LINK_FRAME_COMPLETE;
    }
    return DEVICE_LINK_FRAME_ACCEPTED;
}
