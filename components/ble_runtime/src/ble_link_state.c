#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_state.h"

#define DBG_TAG "ble_link_state"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

#define BLE_LINK_STATE_MAX_VERSION 127U

static void _ble_link_state_write_varint(uint8_t *out, size_t *pos,
        uint32_t value)
{
    while (value >= 0x80U)
    {
        out[(*pos)++] = (uint8_t)(value & 0x7fU) | 0x80U;
        value >>= 7U;
    }
    out[(*pos)++] = (uint8_t)value;
}

static void _ble_link_state_write_tag(uint8_t *out, size_t *pos,
                                      uint32_t field, uint32_t wire)
{
    _ble_link_state_write_varint(out, pos, ((uint32_t)field << 3U) | wire);
}

static void _ble_link_state_write_fixed64(uint8_t *out, size_t *pos,
        uint64_t value)
{
    for (unsigned int i = 0U; i < 8U; ++i)
    {
        out[(*pos)++] = (uint8_t)(value >> (8U * i));
    }
}

static bool _ble_link_state_valid(const ble_link_state_t *state)
{
    if (state == NULL || state->boot_id == 0U)
    {
        return false;
    }
    if (state->protocol_major > BLE_LINK_STATE_MAX_VERSION ||
            state->protocol_minor > BLE_LINK_STATE_MAX_VERSION ||
            state->profile_major > BLE_LINK_STATE_MAX_VERSION ||
            state->profile_minor > BLE_LINK_STATE_MAX_VERSION)
    {
        return false;
    }
    const uint32_t defined = BLE_LINK_STATE_FLAG_BINDABLE |
                             BLE_LINK_STATE_FLAG_BOUND;

    return (state->state_flags & ~defined) == 0U;
}

esp_err_t ble_link_state_encode(
    const ble_link_state_t *state, uint8_t *out, size_t capacity,
    size_t *out_len)
{
    size_t size = 0U;

    if (out_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_ble_link_state_valid(state))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (state->protocol_major != 0U)
    {
        size += 2U;
    }
    if (state->protocol_minor != 0U)
    {
        size += 2U;
    }
    if (state->profile_major != 0U)
    {
        size += 2U;
    }
    if (state->profile_minor != 0U)
    {
        size += 2U;
    }
    size += 9U; /* boot_id fixed64 field. */
    if (state->state_flags != 0U)
    {
        size += 2U;
    }
    if (size > BLE_LINK_STATE_MAX_ENCODED_BYTES)
    {
        return ESP_ERR_NO_MEM;
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

    if (state->protocol_major != 0U)
    {
        _ble_link_state_write_tag(out, &pos, 1U, 0U);
        _ble_link_state_write_varint(out, &pos, state->protocol_major);
    }
    if (state->protocol_minor != 0U)
    {
        _ble_link_state_write_tag(out, &pos, 2U, 0U);
        _ble_link_state_write_varint(out, &pos, state->protocol_minor);
    }
    if (state->profile_major != 0U)
    {
        _ble_link_state_write_tag(out, &pos, 3U, 0U);
        _ble_link_state_write_varint(out, &pos, state->profile_major);
    }
    if (state->profile_minor != 0U)
    {
        _ble_link_state_write_tag(out, &pos, 4U, 0U);
        _ble_link_state_write_varint(out, &pos, state->profile_minor);
    }
    _ble_link_state_write_tag(out, &pos, 5U, 1U);
    _ble_link_state_write_fixed64(out, &pos, state->boot_id);
    if (state->state_flags != 0U)
    {
        _ble_link_state_write_tag(out, &pos, 6U, 0U);
        _ble_link_state_write_varint(out, &pos, state->state_flags);
    }
    *out_len = size;
    return ESP_OK;
}
