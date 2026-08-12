#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_events.h"

#define DBG_TAG "ble_link_events"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

static uint64_t s_sequence;

#ifdef UNIT_TEST_HOST
void ble_link_events_test_set_sequence(uint64_t value)
{
    s_sequence = value;
}
#endif

void ble_link_events_init(void)
{
    s_sequence = 1U;
}

void ble_link_events_reset(void)
{
    s_sequence = 1U;
}

uint64_t ble_link_events_next(void)
{
    if (s_sequence == 0U || s_sequence >= BLE_LINK_EVENTS_MAX_SEQUENCE)
    {
        return 0U;
    }
    s_sequence++;
    return s_sequence;
}

uint64_t ble_link_events_baseline(void)
{
    return s_sequence;
}

bool ble_link_events_exhausted(void)
{
    return s_sequence >= BLE_LINK_EVENTS_MAX_SEQUENCE;
}

static void _ble_link_snapshot_write_varint(uint8_t *out, size_t *pos,
        uint64_t value)
{
    while (value >= 0x80U)
    {
        out[(*pos)++] = (uint8_t)(value & 0x7fU) | 0x80U;
        value >>= 7U;
    }
    out[(*pos)++] = (uint8_t)value;
}

static void _ble_link_snapshot_write_tag(uint8_t *out, size_t *pos,
        uint32_t field, uint32_t wire)
{
    _ble_link_snapshot_write_varint(out, pos,
                                    ((uint64_t)field << 3U) | wire);
}

static void _ble_link_snapshot_write_fixed64(uint8_t *out, size_t *pos,
        uint64_t value)
{
    for (unsigned int i = 0U; i < 8U; ++i)
    {
        out[(*pos)++] = (uint8_t)(value >> (8U * i));
    }
}

static size_t _ble_link_snapshot_link_state_size(
    const ble_link_state_snapshot_t *state)
{
    size_t size = 9U; /* boot_id fixed64. */
    uint64_t pending = 0U;

    if (state->binding_state != BLE_LINK_BINDING_UNSPECIFIED)
    {
        pending++;
    }
    if (state->authorization_state != BLE_LINK_AUTHORIZATION_UNSPECIFIED)
    {
        pending++;
    }
    if (state->encrypted)
    {
        pending++;
    }
    if (state->secure_connections_bond_verified)
    {
        pending++;
    }
    if (state->identity_known)
    {
        pending++;
    }
    size += pending * 2U; /* each bool/enum field: tag + single-byte varint. */
    return size;
}

static void _ble_link_snapshot_write_link_state(
    uint8_t *out, size_t *pos, const ble_link_state_snapshot_t *state)
{
    _ble_link_snapshot_write_tag(out, pos, 1U, 1U);
    _ble_link_snapshot_write_fixed64(out, pos, state->boot_id);
    if (state->binding_state != BLE_LINK_BINDING_UNSPECIFIED)
    {
        _ble_link_snapshot_write_tag(out, pos, 2U, 0U);
        _ble_link_snapshot_write_varint(out, pos, state->binding_state);
    }
    if (state->authorization_state != BLE_LINK_AUTHORIZATION_UNSPECIFIED)
    {
        _ble_link_snapshot_write_tag(out, pos, 3U, 0U);
        _ble_link_snapshot_write_varint(out, pos, state->authorization_state);
    }
    if (state->encrypted)
    {
        _ble_link_snapshot_write_tag(out, pos, 4U, 0U);
        _ble_link_snapshot_write_varint(out, pos, 1U);
    }
    if (state->secure_connections_bond_verified)
    {
        _ble_link_snapshot_write_tag(out, pos, 5U, 0U);
        _ble_link_snapshot_write_varint(out, pos, 1U);
    }
    if (state->identity_known)
    {
        _ble_link_snapshot_write_tag(out, pos, 6U, 0U);
        _ble_link_snapshot_write_varint(out, pos, 1U);
    }
}

static bool _ble_link_snapshot_valid(const ble_link_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->event_sequence == 0U ||
            snapshot->link_state.boot_id == 0U)
    {
        return false;
    }
    const uint64_t binding =
        (uint64_t)snapshot->link_state.binding_state;
    const uint64_t authorization =
        (uint64_t)snapshot->link_state.authorization_state;

    if (binding > BLE_LINK_BINDING_BOUND)
    {
        return false;
    }
    if (authorization > BLE_LINK_AUTHORIZATION_AUTHORIZED)
    {
        return false;
    }
    return true;
}

esp_err_t ble_link_snapshot_encode(
    const ble_link_snapshot_t *snapshot, uint8_t *out, size_t capacity,
    size_t *out_len)
{
    if (out_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_ble_link_snapshot_valid(snapshot))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t link_state_size =
        _ble_link_snapshot_link_state_size(&snapshot->link_state);
    const size_t size = 9U + 2U + link_state_size;

    if (size > BLE_LINK_EVENTS_SNAPSHOT_MAX_BYTES)
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

    _ble_link_snapshot_write_tag(out, &pos, 1U, 1U);
    _ble_link_snapshot_write_fixed64(out, &pos, snapshot->event_sequence);
    _ble_link_snapshot_write_tag(out, &pos, 2U, 2U);
    _ble_link_snapshot_write_varint(out, &pos, link_state_size);
    _ble_link_snapshot_write_link_state(out, &pos, &snapshot->link_state);
    *out_len = size;
    return ESP_OK;
}
