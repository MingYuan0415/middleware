#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_reassembler.h"

#define DBG_TAG "ble_link_reassembler"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

void ble_link_reassembler_init(
    ble_link_reassembler_t *slot, uint8_t *buffer, size_t capacity)
{
    if (slot == NULL)
    {
        return;
    }
    memset(slot, 0, sizeof(*slot));
    slot->buffer = buffer;
    slot->capacity = capacity;
}

void ble_link_reassembler_reset(ble_link_reassembler_t *slot)
{
    if (slot == NULL)
    {
        return;
    }
    uint8_t *buffer = slot->buffer;
    const size_t capacity = slot->capacity;

    memset(slot, 0, sizeof(*slot));
    slot->buffer = buffer;
    slot->capacity = capacity;
}

static bool _ble_link_reassembler_duplicate(
    const ble_link_reassembler_t *slot, const ble_link_fragment_t *fragment)
{
    return slot->frame_id == fragment->frame_id &&
           slot->total_length == fragment->total_length &&
           slot->last_offset == fragment->offset &&
           slot->last_len == fragment->payload_len &&
           slot->last_flags == fragment->flags &&
           memcmp(&slot->buffer[fragment->offset], fragment->payload,
                  fragment->payload_len) == 0;
}

static void _ble_link_reassembler_begin(
    ble_link_reassembler_t *slot, const ble_link_fragment_t *fragment)
{
    slot->frame_id = fragment->frame_id;
    slot->total_length = fragment->total_length;
    slot->received = 0U;
    slot->last_offset = 0U;
    slot->last_flags = 0U;
    slot->last_len = 0U;
    slot->started = true;
    slot->completed_fragment_valid = false;
}

esp_err_t ble_link_reassembler_parse(
    const uint8_t *value, size_t value_len, ble_link_fragment_t *out)
{
    if (value == NULL || out == NULL ||
            value_len < BLE_LINK_FRAMING_HEADER_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->version = value[0];
    out->flags = value[1];
    out->frame_id = (uint16_t)(value[2] | ((uint16_t)value[3] << 8U));
    out->total_length = (uint16_t)(value[4] | ((uint16_t)value[5] << 8U));
    out->offset = (uint16_t)(value[6] | ((uint16_t)value[7] << 8U));
    out->payload = &value[BLE_LINK_FRAMING_HEADER_BYTES];
    out->payload_len = value_len - BLE_LINK_FRAMING_HEADER_BYTES;
    return ESP_OK;
}

esp_err_t ble_link_reassembler_accept(
    ble_link_reassembler_t *slot, const ble_link_fragment_t *fragment)
{
    ble_link_reassembly_disposition_t disposition;
    const esp_err_t result = ble_link_reassembler_accept_ex(
                                 slot, fragment, &disposition);

    if (result != ESP_OK)
    {
        return result;
    }
    return disposition == BLE_LINK_REASSEMBLY_COMPLETE
           ? ESP_OK
           : ESP_ERR_NOT_FINISHED;
}

esp_err_t ble_link_reassembler_accept_ex(
    ble_link_reassembler_t *slot, const ble_link_fragment_t *fragment,
    ble_link_reassembly_disposition_t *out_disposition)
{
    if (slot == NULL || slot->buffer == NULL || fragment == NULL ||
            fragment->payload == NULL || out_disposition == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const bool start = (fragment->flags & BLE_LINK_FRAMING_FLAG_START) != 0U;
    const bool end = (fragment->flags & BLE_LINK_FRAMING_FLAG_END) != 0U;

    if (fragment->version != BLE_LINK_FRAMING_VERSION)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if ((fragment->flags & ~(BLE_LINK_FRAMING_FLAG_START |
                             BLE_LINK_FRAMING_FLAG_END)) != 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (fragment->frame_id == 0U || fragment->total_length == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (fragment->payload_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (fragment->offset > fragment->total_length ||
            fragment->payload_len >
            (size_t)fragment->total_length - fragment->offset)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!slot->started && slot->completed_fragment_valid &&
            _ble_link_reassembler_duplicate(slot, fragment))
    {
        *out_disposition = BLE_LINK_REASSEMBLY_DUPLICATE;
        return ESP_OK;
    }
    if (!slot->started)
    {
        if (!start || fragment->offset != 0U)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if ((size_t)fragment->total_length > slot->capacity)
        {
            return ESP_ERR_NO_MEM;
        }
        _ble_link_reassembler_begin(slot, fragment);
    }
    else
    {
        if (fragment->frame_id != slot->frame_id)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (fragment->total_length != slot->total_length)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (fragment->offset < slot->received)
    {
        const bool duplicate =
            _ble_link_reassembler_duplicate(slot, fragment);

        if (!duplicate)
        {
            return ESP_ERR_INVALID_ARG;
        }
        *out_disposition = BLE_LINK_REASSEMBLY_DUPLICATE;
        return ESP_OK;
    }
    if (fragment->offset > slot->received)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot->received > 0U && start)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (fragment->offset == slot->received)
    {
        memcpy(&slot->buffer[slot->received], fragment->payload,
               fragment->payload_len);
        slot->received += (uint16_t)fragment->payload_len;
        slot->last_offset = fragment->offset;
        slot->last_len = fragment->payload_len;
        slot->last_flags = fragment->flags;
    }
    else
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (end)
    {
        if (slot->received != slot->total_length)
        {
            return ESP_ERR_INVALID_ARG;
        }
        slot->started = false;
        slot->completed_fragment_valid = true;
        *out_disposition = BLE_LINK_REASSEMBLY_COMPLETE;
        return ESP_OK;
    }
    if (slot->received == slot->total_length)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_disposition = BLE_LINK_REASSEMBLY_NEW_PARTIAL;
    return ESP_OK;
}
