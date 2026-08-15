#include <stddef.h>
#include <stdint.h>

#include "device_link_wire.h"

#define DEVICE_LINK_META_VERSION_SHIFT 4U
#define DEVICE_LINK_META_KIND_SHIFT 2U
#define DEVICE_LINK_META_KIND_MASK 0x0cU
#define DEVICE_LINK_META_RESERVED_MASK 0x02U
#define DEVICE_LINK_META_RECOVERY_QUERY 0x01U

static void _write_u32(uint8_t *out, uint32_t value)
{
    for (size_t i = 0U; i < 4U; ++i)
    {
        out[i] = (uint8_t)(value >> (i * 8U));
    }
}

static void _write_u64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0U; i < 8U; ++i)
    {
        out[i] = (uint8_t)(value >> (i * 8U));
    }
}

static uint32_t _read_u32(const uint8_t *data)
{
    uint32_t value = 0U;

    for (size_t i = 0U; i < 4U; ++i)
    {
        value |= (uint32_t)data[i] << (i * 8U);
    }
    return value;
}

static uint64_t _read_u64(const uint8_t *data)
{
    uint64_t value = 0U;

    for (size_t i = 0U; i < 8U; ++i)
    {
        value |= (uint64_t)data[i] << (i * 8U);
    }
    return value;
}

static bool _header_valid(const device_link_wire_header_t *header)
{
    return header != NULL &&
           (header->kind == DEVICE_LINK_MESSAGE_REQUEST ||
            header->kind == DEVICE_LINK_MESSAGE_RESPONSE) &&
           header->domain_id != DEVICE_LINK_DOMAIN_INVALID &&
           header->domain_major != 0U && header->method_id != 0U &&
           header->call_id != 0U && header->boot_id != 0U &&
           (!header->recovery_query ||
            (header->kind == DEVICE_LINK_MESSAGE_REQUEST &&
             header->domain_id == DEVICE_LINK_DOMAIN_CORE &&
             (header->method_id == 5U || header->method_id == 6U)));
}

esp_err_t device_link_wire_encode_header(
    const device_link_wire_header_t *header,
    uint8_t out[DEVICE_LINK_WIRE_HEADER_BYTES])
{
    if (!_header_valid(header) || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = (uint8_t)(DEVICE_LINK_WIRE_HEADER_VERSION <<
                       DEVICE_LINK_META_VERSION_SHIFT) |
             (uint8_t)((uint8_t)header->kind << DEVICE_LINK_META_KIND_SHIFT) |
             (header->recovery_query ? DEVICE_LINK_META_RECOVERY_QUERY : 0U);
    out[1] = header->domain_id;
    out[2] = header->domain_major;
    out[3] = header->method_id;
    _write_u32(&out[4], header->call_id);
    _write_u64(&out[8], header->boot_id);
    return ESP_OK;
}

esp_err_t device_link_wire_decode_header(
    const uint8_t *data, size_t len, device_link_wire_header_t *header)
{
    if (data == NULL || header == NULL || len < DEVICE_LINK_WIRE_HEADER_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if ((data[0] >> DEVICE_LINK_META_VERSION_SHIFT) !=
            DEVICE_LINK_WIRE_HEADER_VERSION ||
            (data[0] & DEVICE_LINK_META_RESERVED_MASK) != 0U)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    header->kind = (device_link_message_kind_t)(
                       (data[0] & DEVICE_LINK_META_KIND_MASK) >>
                       DEVICE_LINK_META_KIND_SHIFT);
    header->recovery_query =
        (data[0] & DEVICE_LINK_META_RECOVERY_QUERY) != 0U;
    header->domain_id = data[1];
    header->domain_major = data[2];
    header->method_id = data[3];
    header->call_id = _read_u32(&data[4]);
    header->boot_id = _read_u64(&data[8]);
    return _header_valid(header) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t device_link_wire_encode_status(
    device_link_status_t status,
    uint8_t out[DEVICE_LINK_RESPONSE_STATUS_BYTES])
{
    if (out == NULL || status < DEVICE_LINK_STATUS_OK ||
            status > DEVICE_LINK_STATUS_INTERNAL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = (uint8_t)status;
    out[1] = 0U;
    return ESP_OK;
}

esp_err_t device_link_wire_decode_status(
    const uint8_t *data, size_t len, device_link_status_t *status)
{
    if (data == NULL || status == NULL ||
            len < DEVICE_LINK_RESPONSE_STATUS_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t value = (uint16_t)data[0] | (uint16_t)data[1] << 8U;

    if (value < DEVICE_LINK_STATUS_OK || value > DEVICE_LINK_STATUS_INTERNAL)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *status = (device_link_status_t)value;
    return ESP_OK;
}
