#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "device_link_tlv.h"

static size_t _uleb_size(uint64_t value)
{
    size_t size = 1U;

    while (value >= 0x80U)
    {
        value >>= 7U;
        size++;
    }
    return size;
}

static esp_err_t _decode_uleb(
    const uint8_t *data, size_t len, size_t *used, uint64_t *value)
{
    uint64_t decoded = 0U;

    if (data == NULL || used == NULL || value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < len && i < 10U; ++i)
    {
        const uint8_t byte = data[i];

        if (i == 9U && byte > 1U)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        decoded |= (uint64_t)(byte & 0x7fU) << (i * 7U);
        if ((byte & 0x80U) == 0U)
        {
            const size_t count = i + 1U;

            if (_uleb_size(decoded) != count)
            {
                return ESP_ERR_INVALID_RESPONSE;
            }
            *used = count;
            *value = decoded;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t _writer_reserve(
    device_link_tlv_writer_t *writer, uint8_t field_id,
    device_link_tlv_wire_type_t wire_type, size_t bytes)
{
    if (writer == NULL || writer->data == NULL ||
            field_id < DEVICE_LINK_TLV_MIN_FIELD_ID ||
            field_id > DEVICE_LINK_TLV_MAX_FIELD_ID ||
            field_id < writer->last_field_id ||
            wire_type > DEVICE_LINK_TLV_FIXED64)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (writer->error != ESP_OK)
    {
        return writer->error;
    }
    if (bytes > writer->capacity - writer->len)
    {
        writer->error = ESP_ERR_NO_MEM;
        return writer->error;
    }
    writer->data[writer->len++] = (uint8_t)(field_id << 2U) |
                                  (uint8_t)wire_type;
    writer->last_field_id = field_id;
    return ESP_OK;
}

static esp_err_t _writer_uleb(
    device_link_tlv_writer_t *writer, uint8_t field_id,
    device_link_tlv_wire_type_t wire_type, uint64_t value)
{
    const size_t size = _uleb_size(value);
    esp_err_t result = _writer_reserve(writer, field_id, wire_type,
                                       size + 1U);

    if (result != ESP_OK)
    {
        return result;
    }
    do
    {
        uint8_t byte = (uint8_t)(value & 0x7fU);

        value >>= 7U;
        if (value != 0U)
        {
            byte |= 0x80U;
        }
        writer->data[writer->len++] = byte;
    }
    while (value != 0U);
    return ESP_OK;
}

esp_err_t device_link_tlv_reader_init(
    device_link_tlv_reader_t *reader, const uint8_t *data, size_t len)
{
    if (reader == NULL || (data == NULL && len != 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(reader, 0, sizeof(*reader));
    reader->data = data;
    reader->len = len;
    return ESP_OK;
}

esp_err_t device_link_tlv_reader_init_nested(
    device_link_tlv_reader_t *reader,
    const device_link_tlv_reader_t *parent,
    const device_link_tlv_field_t *field)
{
    if (reader == NULL || parent == NULL || field == NULL ||
            field->wire_type != DEVICE_LINK_TLV_LENGTH ||
            parent->depth >= DEVICE_LINK_TLV_MAX_DEPTH)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = device_link_tlv_reader_init(
                           reader, field->value.bytes.data,
                           field->value.bytes.len);

    if (result == ESP_OK)
    {
        reader->depth = (uint8_t)(parent->depth + 1U);
    }
    return result;
}

esp_err_t device_link_tlv_reader_next(
    device_link_tlv_reader_t *reader, device_link_tlv_field_t *field,
    bool *has_field)
{
    if (reader == NULL || field == NULL || has_field == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (reader->offset == reader->len)
    {
        *has_field = false;
        return ESP_OK;
    }
    if (reader->offset > reader->len)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t key = reader->data[reader->offset++];
    const uint8_t field_id = key >> 2U;
    const device_link_tlv_wire_type_t wire_type =
        (device_link_tlv_wire_type_t)(key & 0x03U);

    if (field_id < DEVICE_LINK_TLV_MIN_FIELD_ID ||
            field_id < reader->last_field_id)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memset(field, 0, sizeof(*field));
    field->id = field_id;
    field->wire_type = wire_type;
    reader->last_field_id = field_id;
    if (wire_type == DEVICE_LINK_TLV_UNSIGNED ||
            wire_type == DEVICE_LINK_TLV_SIGNED)
    {
        size_t used = 0U;
        uint64_t value = 0U;
        esp_err_t result = _decode_uleb(
                               &reader->data[reader->offset],
                               reader->len - reader->offset, &used, &value);

        if (result != ESP_OK)
        {
            return result;
        }
        reader->offset += used;
        if (wire_type == DEVICE_LINK_TLV_UNSIGNED)
        {
            field->value.unsigned_value = value;
        }
        else if ((value & 1U) == 0U)
        {
            field->value.signed_value = (int64_t)(value >> 1U);
        }
        else
        {
            field->value.signed_value = -((int64_t)(value >> 1U)) - 1;
        }
    }
    else if (wire_type == DEVICE_LINK_TLV_LENGTH)
    {
        if (reader->len - reader->offset < 2U)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const size_t size = (size_t)reader->data[reader->offset] |
                            (size_t)reader->data[reader->offset + 1U] << 8U;

        reader->offset += 2U;
        if (size > reader->len - reader->offset)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        field->value.bytes.data = &reader->data[reader->offset];
        field->value.bytes.len = size;
        reader->offset += size;
    }
    else
    {
        if (reader->len - reader->offset < 8U)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        for (size_t i = 0U; i < 8U; ++i)
        {
            field->value.fixed64_value |=
                (uint64_t)reader->data[reader->offset + i] << (i * 8U);
        }
        reader->offset += 8U;
    }
    *has_field = true;
    return ESP_OK;
}

esp_err_t device_link_tlv_validate_utf8(const uint8_t *data, size_t len)
{
    if (data == NULL && len != 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < len;)
    {
        const uint8_t first = data[i++];
        uint32_t codepoint = 0U;
        size_t continuation = 0U;

        if (first <= 0x7fU)
        {
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU)
        {
            codepoint = first & 0x1fU;
            continuation = 1U;
        }
        else if (first >= 0xe0U && first <= 0xefU)
        {
            codepoint = first & 0x0fU;
            continuation = 2U;
        }
        else if (first >= 0xf0U && first <= 0xf4U)
        {
            codepoint = first & 0x07U;
            continuation = 3U;
        }
        else
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (continuation > len - i)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        for (size_t j = 0U; j < continuation; ++j)
        {
            const uint8_t byte = data[i++];

            if ((byte & 0xc0U) != 0x80U)
            {
                return ESP_ERR_INVALID_RESPONSE;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3fU);
        }
        if ((continuation == 2U && codepoint < 0x800U) ||
                (continuation == 3U && codepoint < 0x10000U) ||
                codepoint > 0x10ffffU ||
                (codepoint >= 0xd800U && codepoint <= 0xdfffU))
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

static const device_link_tlv_field_rule_t *_find_rule(
    const device_link_tlv_schema_t *schema, uint8_t field_id,
    size_t *rule_index)
{
    for (size_t i = 0U; i < schema->field_count; ++i)
    {
        if (schema->fields[i].id == field_id)
        {
            if (rule_index != NULL)
            {
                *rule_index = i;
            }
            return &schema->fields[i];
        }
        if (schema->fields[i].id > field_id)
        {
            break;
        }
    }
    return NULL;
}

static bool _enum_contains(
    const device_link_tlv_field_rule_t *rule, uint64_t value)
{
    if (rule->enum_values == NULL)
    {
        return true;
    }
    for (size_t i = 0U; i < rule->enum_count; ++i)
    {
        if (rule->enum_values[i] == value)
        {
            return true;
        }
    }
    return false;
}

static esp_err_t _validate_rule(
    const device_link_tlv_reader_t *reader,
    const device_link_tlv_field_t *field,
    const device_link_tlv_field_rule_t *rule)
{
    if (field->wire_type != rule->wire_type)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (field->wire_type == DEVICE_LINK_TLV_UNSIGNED)
    {
        const uint64_t value = field->value.unsigned_value;

        if ((rule->flags & DEVICE_LINK_TLV_RULE_NONZERO) != 0U &&
                value == 0U)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if ((rule->flags & DEVICE_LINK_TLV_RULE_BOOL) != 0U && value > 1U)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (value < rule->minimum_unsigned ||
                value > rule->maximum_unsigned ||
                !_enum_contains(rule, value))
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    else if (field->wire_type == DEVICE_LINK_TLV_SIGNED)
    {
        if (field->value.signed_value < rule->minimum_signed ||
                field->value.signed_value > rule->maximum_signed)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    else if (field->wire_type == DEVICE_LINK_TLV_FIXED64)
    {
        if ((rule->flags & DEVICE_LINK_TLV_RULE_NONZERO) != 0U &&
                field->value.fixed64_value == 0U)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    else
    {
        const size_t len = field->value.bytes.len;

        if (len < rule->minimum_bytes || len > rule->maximum_bytes)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if ((rule->flags & DEVICE_LINK_TLV_RULE_NONZERO) != 0U)
        {
            bool all_zero = true;

            for (size_t i = 0U; i < len; ++i)
            {
                if (field->value.bytes.data[i] != 0U)
                {
                    all_zero = false;
                    break;
                }
            }
            if (all_zero)
            {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        if ((rule->flags & DEVICE_LINK_TLV_RULE_UTF8) != 0U &&
                device_link_tlv_validate_utf8(
                    field->value.bytes.data, len) != ESP_OK)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if ((rule->flags & DEVICE_LINK_TLV_RULE_MESSAGE) != 0U)
        {
            if (rule->nested == NULL ||
                    reader->depth >= DEVICE_LINK_TLV_MAX_DEPTH)
            {
                return ESP_ERR_INVALID_RESPONSE;
            }
            device_link_tlv_reader_t nested;
            esp_err_t result = device_link_tlv_reader_init_nested(
                                   &nested, reader, field);

            if (result != ESP_OK)
            {
                return result;
            }
            result = device_link_tlv_validate_message(
                         nested.data, nested.len, rule->nested);
            if (result != ESP_OK)
            {
                return result;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t _schema_valid(const device_link_tlv_schema_t *schema)
{
    if (schema == NULL ||
            (schema->fields == NULL && schema->field_count != 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < schema->field_count; ++i)
    {
        const device_link_tlv_field_rule_t *rule = &schema->fields[i];

        if (rule->id < DEVICE_LINK_TLV_MIN_FIELD_ID ||
                rule->id > DEVICE_LINK_TLV_MAX_FIELD_ID ||
                rule->wire_type > DEVICE_LINK_TLV_FIXED64 ||
                (i > 0U && schema->fields[i - 1U].id >= rule->id) ||
                ((rule->flags & DEVICE_LINK_TLV_RULE_REPEATED) != 0U &&
                 rule->maximum_count == 0U) ||
                ((rule->flags & (DEVICE_LINK_TLV_RULE_UNIQUE |
                                 DEVICE_LINK_TLV_RULE_SORTED)) != 0U &&
                 rule->wire_type != DEVICE_LINK_TLV_UNSIGNED))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t _validate_message_depth(
    const uint8_t *data, size_t len,
    const device_link_tlv_schema_t *schema, uint8_t depth)
{
    if ((data == NULL && len != 0U) || _schema_valid(schema) != ESP_OK ||
            len > schema->maximum_encoded_bytes ||
            depth > DEVICE_LINK_TLV_MAX_DEPTH || schema->field_count > 63U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t counts[63] = {0};
    /* Strictly ascending previous values for repeated UNIQUE/SORTED uint
     * lists (the permission lists of the authorization contract). */
    uint64_t previous[63] = {0};
    device_link_tlv_reader_t reader;
    esp_err_t result = device_link_tlv_reader_init(&reader, data, len);

    if (result != ESP_OK)
    {
        return result;
    }
    reader.depth = depth;
    while (true)
    {
        device_link_tlv_field_t field;
        bool has_field = false;

        result = device_link_tlv_reader_next(&reader, &field, &has_field);
        if (result != ESP_OK || !has_field)
        {
            break;
        }
        size_t rule_index = 0U;
        const device_link_tlv_field_rule_t *rule =
            _find_rule(schema, field.id, &rule_index);

        if (rule == NULL)
        {
            continue;
        }
        if (counts[rule_index] == UINT8_MAX)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        counts[rule_index]++;
        if (((rule->flags & DEVICE_LINK_TLV_RULE_REPEATED) == 0U &&
                counts[rule_index] > 1U) ||
                ((rule->flags & DEVICE_LINK_TLV_RULE_REPEATED) != 0U &&
                 counts[rule_index] > rule->maximum_count))
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if ((rule->flags & (DEVICE_LINK_TLV_RULE_UNIQUE |
                            DEVICE_LINK_TLV_RULE_SORTED)) != 0U)
        {
            /* The schema validator restricts these flags to uint rules.
             * Both flags require a strictly ascending value sequence, which
             * is unique by construction. */
            if (counts[rule_index] > 1U &&
                    field.value.unsigned_value <= previous[rule_index])
            {
                return ESP_ERR_INVALID_RESPONSE;
            }
            previous[rule_index] = field.value.unsigned_value;
        }
        if ((rule->flags & DEVICE_LINK_TLV_RULE_MESSAGE) != 0U)
        {
            if (rule->nested == NULL || depth >= DEVICE_LINK_TLV_MAX_DEPTH)
            {
                return ESP_ERR_INVALID_RESPONSE;
            }
            result = _validate_message_depth(
                         field.value.bytes.data, field.value.bytes.len,
                         rule->nested, (uint8_t)(depth + 1U));
        }
        else
        {
            result = _validate_rule(&reader, &field, rule);
        }
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (result != ESP_OK)
    {
        return result;
    }
    for (size_t i = 0U; i < schema->field_count; ++i)
    {
        if ((schema->fields[i].flags & DEVICE_LINK_TLV_RULE_REQUIRED) != 0U &&
                counts[i] == 0U)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

esp_err_t device_link_tlv_validate_message(
    const uint8_t *data, size_t len,
    const device_link_tlv_schema_t *schema)
{
    return _validate_message_depth(data, len, schema, 0U);
}

void device_link_tlv_writer_init(
    device_link_tlv_writer_t *writer, uint8_t *data, size_t capacity)
{
    if (writer == NULL)
    {
        return;
    }
    memset(writer, 0, sizeof(*writer));
    writer->data = data;
    writer->capacity = capacity;
    if (data == NULL && capacity != 0U)
    {
        writer->error = ESP_ERR_INVALID_ARG;
    }
}

esp_err_t device_link_tlv_put_uint(
    device_link_tlv_writer_t *writer, uint8_t field_id, uint64_t value)
{
    return _writer_uleb(writer, field_id, DEVICE_LINK_TLV_UNSIGNED, value);
}

esp_err_t device_link_tlv_put_sint(
    device_link_tlv_writer_t *writer, uint8_t field_id, int64_t value)
{
    const uint64_t encoded = value >= 0 ? (uint64_t)value * 2U :
                             (uint64_t)(-(value + 1)) * 2U + 1U;

    return _writer_uleb(writer, field_id, DEVICE_LINK_TLV_SIGNED, encoded);
}

esp_err_t device_link_tlv_put_bool(
    device_link_tlv_writer_t *writer, uint8_t field_id, bool value)
{
    return device_link_tlv_put_uint(writer, field_id, value ? 1U : 0U);
}

esp_err_t device_link_tlv_put_fixed64(
    device_link_tlv_writer_t *writer, uint8_t field_id, uint64_t value)
{
    esp_err_t result = _writer_reserve(writer, field_id,
                                       DEVICE_LINK_TLV_FIXED64, 9U);

    if (result != ESP_OK)
    {
        return result;
    }
    for (size_t i = 0U; i < 8U; ++i)
    {
        writer->data[writer->len++] = (uint8_t)(value >> (i * 8U));
    }
    return ESP_OK;
}

esp_err_t device_link_tlv_put_bytes(
    device_link_tlv_writer_t *writer, uint8_t field_id,
    const uint8_t *data, size_t len)
{
    if ((data == NULL && len != 0U) || len > UINT16_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _writer_reserve(writer, field_id,
                                       DEVICE_LINK_TLV_LENGTH, len + 3U);

    if (result != ESP_OK)
    {
        return result;
    }
    writer->data[writer->len++] = (uint8_t)len;
    writer->data[writer->len++] = (uint8_t)(len >> 8U);
    if (len != 0U)
    {
        memcpy(&writer->data[writer->len], data, len);
        writer->len += len;
    }
    return ESP_OK;
}

esp_err_t device_link_tlv_put_string(
    device_link_tlv_writer_t *writer, uint8_t field_id,
    const uint8_t *data, size_t len)
{
    esp_err_t result = device_link_tlv_validate_utf8(data, len);

    return result == ESP_OK ? device_link_tlv_put_bytes(
               writer, field_id, data, len) : result;
}

esp_err_t device_link_tlv_writer_finish(
    const device_link_tlv_writer_t *writer, size_t *out_len)
{
    if (writer == NULL || out_len == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (writer->error != ESP_OK)
    {
        return writer->error;
    }
    *out_len = writer->len;
    return ESP_OK;
}
