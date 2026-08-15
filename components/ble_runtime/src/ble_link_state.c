#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_state.h"

#define DBG_TAG "ble_link_state"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

static bool _ble_link_state_valid(const ble_link_state_t *state)
{
    if (state == NULL || state->boot_id == 0U ||
            state->protocol_major != BLE_LINK_STATE_PROTOCOL_MAJOR ||
            state->protocol_minor != BLE_LINK_STATE_PROTOCOL_MINOR ||
            state->profile_major != BLE_LINK_STATE_PROFILE_MAJOR ||
            state->profile_minor != BLE_LINK_STATE_PROFILE_MINOR)
    {
        return false;
    }
    const uint32_t defined = BLE_LINK_STATE_FLAG_BINDABLE |
                             BLE_LINK_STATE_FLAG_BOUND |
                             BLE_LINK_STATE_FLAG_PUBLIC_DISCOVERY |
                             BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED |
                             BLE_LINK_STATE_FLAG_TRANSITIONING |
                             BLE_LINK_STATE_FLAG_AUTHENTICATED |
                             BLE_LINK_STATE_FLAG_AUTHORIZED |
                             BLE_LINK_STATE_FLAG_ERROR;
    const uint32_t flags = state->state_flags;

    if ((flags & ~defined) != 0U)
    {
        return false;
    }
    /* Cross-flag implications frozen by the v2 GATT contract:
     * AUTHORIZED implies AUTHENTICATED and BOUND; AUTHENTICATED implies
     * BOUND; BINDABLE is mutually exclusive with PUBLIC_DISCOVERY and
     * also excludes BOUND. */
    if ((flags & BLE_LINK_STATE_FLAG_AUTHORIZED) != 0U &&
            ((flags & BLE_LINK_STATE_FLAG_AUTHENTICATED) == 0U ||
             (flags & BLE_LINK_STATE_FLAG_BOUND) == 0U))
    {
        return false;
    }
    if ((flags & BLE_LINK_STATE_FLAG_AUTHENTICATED) != 0U &&
            (flags & BLE_LINK_STATE_FLAG_BOUND) == 0U)
    {
        return false;
    }
    if ((flags & BLE_LINK_STATE_FLAG_BINDABLE) != 0U &&
            ((flags & BLE_LINK_STATE_FLAG_PUBLIC_DISCOVERY) != 0U ||
             (flags & BLE_LINK_STATE_FLAG_BOUND) != 0U))
    {
        return false;
    }
    return true;
}

static void _ble_link_state_write_u32(uint8_t *out, uint32_t value)
{
    for (unsigned int i = 0U; i < 4U; ++i)
    {
        out[i] = (uint8_t)(value >> (8U * i));
    }
}

static void _ble_link_state_write_u64(uint8_t *out, uint64_t value)
{
    for (unsigned int i = 0U; i < 8U; ++i)
    {
        out[i] = (uint8_t)(value >> (8U * i));
    }
}

esp_err_t ble_link_state_encode(
    const ble_link_state_t *state, uint8_t *out, size_t capacity,
    size_t *out_len)
{
    if (out_len == NULL || out == NULL || capacity <
            BLE_LINK_STATE_MAX_ENCODED_BYTES ||
            !_ble_link_state_valid(state))
    {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = (uint8_t)state->protocol_major;
    out[1] = (uint8_t)state->protocol_minor;
    out[2] = (uint8_t)state->profile_major;
    out[3] = (uint8_t)state->profile_minor;
    _ble_link_state_write_u32(&out[4], state->state_flags);
    _ble_link_state_write_u64(&out[8], state->boot_id);
    *out_len = BLE_LINK_STATE_MAX_ENCODED_BYTES;
    return ESP_OK;
}
