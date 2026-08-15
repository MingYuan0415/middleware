#ifndef __DEVICE_LINK_TLV_H__
#define __DEVICE_LINK_TLV_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_LINK_TLV_MIN_FIELD_ID 1U
#define DEVICE_LINK_TLV_MAX_FIELD_ID 63U
#define DEVICE_LINK_TLV_MAX_DEPTH 2U

typedef enum device_link_tlv_wire_type
{
    DEVICE_LINK_TLV_UNSIGNED = 0,
    DEVICE_LINK_TLV_SIGNED = 1,
    DEVICE_LINK_TLV_LENGTH = 2,
    DEVICE_LINK_TLV_FIXED64 = 3,
} device_link_tlv_wire_type_t;

typedef struct device_link_tlv_field
{
    uint8_t id;
    device_link_tlv_wire_type_t wire_type;
    union
    {
        uint64_t unsigned_value;
        int64_t signed_value;
        uint64_t fixed64_value;
        struct
        {
            const uint8_t *data;
            size_t len;
        } bytes;
    } value;
} device_link_tlv_field_t;

typedef struct device_link_tlv_reader
{
    const uint8_t *data;
    size_t len;
    size_t offset;
    uint8_t last_field_id;
    uint8_t depth;
} device_link_tlv_reader_t;

typedef struct device_link_tlv_writer
{
    uint8_t *data;
    size_t capacity;
    size_t len;
    uint8_t last_field_id;
    esp_err_t error;
} device_link_tlv_writer_t;

#define DEVICE_LINK_TLV_RULE_REQUIRED 0x01U
#define DEVICE_LINK_TLV_RULE_REPEATED 0x02U
#define DEVICE_LINK_TLV_RULE_UTF8 0x04U
#define DEVICE_LINK_TLV_RULE_NONZERO 0x08U
#define DEVICE_LINK_TLV_RULE_BOOL 0x10U
#define DEVICE_LINK_TLV_RULE_MESSAGE 0x20U

struct device_link_tlv_schema;

/** @brief One hand-written field rule derived from the canonical YAML. */
typedef struct device_link_tlv_field_rule
{
    uint8_t id;
    device_link_tlv_wire_type_t wire_type;
    uint8_t flags;
    uint8_t maximum_count;
    uint64_t minimum_unsigned;
    uint64_t maximum_unsigned;
    int64_t minimum_signed;
    int64_t maximum_signed;
    uint16_t minimum_bytes;
    uint16_t maximum_bytes;
    const uint64_t *enum_values;
    size_t enum_count;
    const struct device_link_tlv_schema *nested;
} device_link_tlv_field_rule_t;

/** @brief Bounded message schema used by production domain decoders. */
typedef struct device_link_tlv_schema
{
    const device_link_tlv_field_rule_t *fields;
    size_t field_count;
    size_t maximum_encoded_bytes;
} device_link_tlv_schema_t;

esp_err_t device_link_tlv_reader_init(
    device_link_tlv_reader_t *reader, const uint8_t *data, size_t len);

esp_err_t device_link_tlv_reader_init_nested(
    device_link_tlv_reader_t *reader,
    const device_link_tlv_reader_t *parent,
    const device_link_tlv_field_t *field);

esp_err_t device_link_tlv_reader_next(
    device_link_tlv_reader_t *reader, device_link_tlv_field_t *field,
    bool *has_field);

esp_err_t device_link_tlv_validate_utf8(const uint8_t *data, size_t len);

esp_err_t device_link_tlv_validate_message(
    const uint8_t *data, size_t len,
    const device_link_tlv_schema_t *schema);

void device_link_tlv_writer_init(
    device_link_tlv_writer_t *writer, uint8_t *data, size_t capacity);

esp_err_t device_link_tlv_put_uint(
    device_link_tlv_writer_t *writer, uint8_t field_id, uint64_t value);

esp_err_t device_link_tlv_put_sint(
    device_link_tlv_writer_t *writer, uint8_t field_id, int64_t value);

esp_err_t device_link_tlv_put_bool(
    device_link_tlv_writer_t *writer, uint8_t field_id, bool value);

esp_err_t device_link_tlv_put_fixed64(
    device_link_tlv_writer_t *writer, uint8_t field_id, uint64_t value);

esp_err_t device_link_tlv_put_bytes(
    device_link_tlv_writer_t *writer, uint8_t field_id,
    const uint8_t *data, size_t len);

esp_err_t device_link_tlv_put_string(
    device_link_tlv_writer_t *writer, uint8_t field_id,
    const uint8_t *data, size_t len);

esp_err_t device_link_tlv_writer_finish(
    const device_link_tlv_writer_t *writer, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_TLV_H__ */
