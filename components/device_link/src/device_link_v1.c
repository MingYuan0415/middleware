#include <string.h>

#include "device_link_v1.h"

static uint32_t _device_link_v1_read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void _device_link_v1_write_u16_le(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void _device_link_v1_write_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static bool _device_link_v1_utf8_ssid_valid(const uint8_t *bytes, size_t length)
{
    size_t offset = 0U;

    while (offset < length)
    {
        const uint8_t lead = bytes[offset];
        uint32_t codepoint;
        size_t units;

        if (lead <= 0x7fU)
        {
            units = 1U;
            codepoint = lead;
        }
        else if ((lead & 0xe0U) == 0xc0U)
        {
            units = 2U;
            codepoint = (uint32_t)(lead & 0x1fU);
        }
        else if ((lead & 0xf0U) == 0xe0U)
        {
            units = 3U;
            codepoint = (uint32_t)(lead & 0x0fU);
        }
        else if ((lead & 0xf8U) == 0xf0U)
        {
            units = 4U;
            codepoint = (uint32_t)(lead & 0x07U);
        }
        else
        {
            return false;
        }
        if ((offset + units) > length)
        {
            return false;
        }
        for (size_t i = 1U; i < units; ++i)
        {
            const uint8_t cont = bytes[offset + i];

            if ((cont & 0xc0U) != 0x80U)
            {
                return false;
            }
            codepoint = (codepoint << 6) | (uint32_t)(cont & 0x3fU);
        }
        if ((units == 1U && codepoint > 0x7fU) ||
                (units == 2U && codepoint < 0x80U) ||
                (units == 3U && codepoint < 0x800U) ||
                (units == 4U && (codepoint < 0x10000U || codepoint > 0x10ffffU)))
        {
            return false;
        }
        if (codepoint >= 0xd800U && codepoint <= 0xdfffU)
        {
            return false;
        }
        if (codepoint <= 0x1fU || (codepoint >= 0x7fU && codepoint <= 0x9fU))
        {
            return false;
        }
        offset += units;
    }
    return true;
}

static bool _device_link_v1_password_bytes_valid(const uint8_t *bytes,
        size_t length)
{
    for (size_t i = 0U; i < length; ++i)
    {
        if (bytes[i] < 0x20U || bytes[i] > 0x7eU)
        {
            return false;
        }
    }
    return true;
}

static bool _device_link_v1_credentials_valid(
    const device_link_v1_credentials_t *credentials)
{
    if (credentials->ssid_length < 1U ||
            credentials->ssid_length > DEVICE_LINK_V1_MAX_SSID_BYTES ||
            !_device_link_v1_utf8_ssid_valid(credentials->ssid,
                    credentials->ssid_length))
    {
        return false;
    }
    if (credentials->security == DEVICE_LINK_V1_WIFI_OPEN)
    {
        return credentials->password_length == 0U;
    }
    if (credentials->security != DEVICE_LINK_V1_WIFI_PERSONAL)
    {
        return false;
    }
    return credentials->password_length >=
           DEVICE_LINK_V1_MIN_PERSONAL_PASSWORD_BYTES &&
           credentials->password_length <= DEVICE_LINK_V1_MAX_PASSWORD_BYTES &&
           _device_link_v1_password_bytes_valid(credentials->password,
                   credentials->password_length);
}

static bool _device_link_v1_profile_required(
    device_link_v1_wifi_state_t state,
    device_link_v1_wifi_failure_t failure)
{
    return state == DEVICE_LINK_V1_WIFI_CONNECTING ||
           state == DEVICE_LINK_V1_WIFI_CONNECTED ||
           failure == DEVICE_LINK_V1_WIFI_FAILURE_AUTHENTICATION ||
           failure == DEVICE_LINK_V1_WIFI_FAILURE_AP_NOT_FOUND ||
           failure == DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT ||
           failure == DEVICE_LINK_V1_WIFI_FAILURE_LINK_LOST;
}

static bool _device_link_v1_state_failure_allowed(
    device_link_v1_wifi_state_t state,
    device_link_v1_wifi_failure_t failure)
{
    switch (state)
    {
    case DEVICE_LINK_V1_WIFI_UNAVAILABLE:
        return failure == DEVICE_LINK_V1_WIFI_FAILURE_RADIO ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_INTERNAL;
    case DEVICE_LINK_V1_WIFI_IDLE:
    case DEVICE_LINK_V1_WIFI_SCANNING:
    case DEVICE_LINK_V1_WIFI_CONNECTING:
    case DEVICE_LINK_V1_WIFI_CONNECTED:
        return failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    case DEVICE_LINK_V1_WIFI_ERROR:
        return failure == DEVICE_LINK_V1_WIFI_FAILURE_AUTHENTICATION ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_AP_NOT_FOUND ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_LINK_LOST ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_RADIO ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_STORAGE ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_INTERNAL;
    default:
        return false;
    }
}

static bool _device_link_v1_network_valid(const device_link_v1_network_t *network)
{
    if (network->ssid_length < 1U ||
            network->ssid_length > DEVICE_LINK_V1_MAX_SSID_BYTES ||
            !_device_link_v1_utf8_ssid_valid(network->ssid, network->ssid_length))
    {
        return false;
    }
    if (network->security != DEVICE_LINK_V1_WIFI_OPEN &&
            network->security != DEVICE_LINK_V1_WIFI_PERSONAL)
    {
        return false;
    }
    return network->rssi_dbm >= -127;
}

static bool _device_link_v1_decode_bytes_u8(
    const uint8_t *value, size_t length, size_t *offset,
    uint8_t *out, uint8_t *out_length, uint8_t min_bytes, uint8_t max_bytes,
    bool utf8)
{
    if (*offset >= length)
    {
        return false;
    }
    const uint8_t field_length = value[*offset];

    *offset += 1U;
    if (field_length < min_bytes || field_length > max_bytes ||
            (*offset + field_length) > length)
    {
        return false;
    }
    if (utf8 && !_device_link_v1_utf8_ssid_valid(&value[*offset], field_length))
    {
        return false;
    }
    if (!utf8 && !_device_link_v1_password_bytes_valid(&value[*offset],
            field_length))
    {
        return false;
    }
    memcpy(out, &value[*offset], field_length);
    *out_length = field_length;
    *offset += field_length;
    return true;
}

static size_t _device_link_v1_encode_bytes_u8(
    uint8_t *out, size_t capacity, size_t offset,
    const uint8_t *bytes, uint8_t length)
{
    if ((offset + 1U + length) > capacity)
    {
        return 0U;
    }
    out[offset] = length;
    if (length > 0U)
    {
        memcpy(&out[offset + 1U], bytes, length);
    }
    return offset + 1U + length;
}

static size_t _device_link_v1_encode_snapshot_fields(
    uint8_t *out, size_t capacity, size_t offset,
    const device_link_v1_snapshot_t *snapshot)
{
    if (!device_link_v1_snapshot_valid(snapshot) || (offset + 2U) > capacity)
    {
        return 0U;
    }
    out[offset] = (uint8_t)snapshot->state;
    out[offset + 1U] = (uint8_t)snapshot->failure;
    return _device_link_v1_encode_bytes_u8(
               out, capacity, offset + 2U, snapshot->profile_ssid,
               snapshot->profile_ssid_length);
}

static size_t _device_link_v1_encode_network(
    uint8_t *out, size_t capacity, size_t offset,
    const device_link_v1_network_t *network)
{
    if (!_device_link_v1_network_valid(network))
    {
        return 0U;
    }
    offset = _device_link_v1_encode_bytes_u8(
                 out, capacity, offset, network->ssid, network->ssid_length);
    if (offset == 0U || (offset + 2U) > capacity)
    {
        return 0U;
    }
    out[offset] = (uint8_t)network->security;
    out[offset + 1U] = (uint8_t)network->rssi_dbm;
    return offset + 2U;
}

static size_t _device_link_v1_encode_networks(
    uint8_t *out, size_t capacity, size_t offset,
    const device_link_v1_network_t *networks, uint8_t count)
{
    if (count > DEVICE_LINK_V1_MAX_SCAN_NETWORKS)
    {
        return 0U;
    }
    for (uint8_t i = 0U; i < count; ++i)
    {
        offset = _device_link_v1_encode_network(out, capacity, offset,
                                                &networks[i]);
        if (offset == 0U)
        {
            return 0U;
        }
    }
    return offset;
}

static bool _device_link_v1_scan_key_equal(
    const device_link_v1_network_t *left,
    const device_link_v1_network_t *right)
{
    return left->security == right->security &&
           left->ssid_length == right->ssid_length &&
           memcmp(left->ssid, right->ssid, left->ssid_length) == 0;
}

bool device_link_v1_att_value_length_valid(size_t length)
{
    return length <= DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES;
}

bool device_link_v1_opcode_known(uint8_t opcode)
{
    return opcode >= DEVICE_LINK_V1_GET_INFO &&
           opcode <= DEVICE_LINK_V1_ACK_OPERATION;
}

bool device_link_v1_opcode_occupies_slot(uint8_t opcode)
{
    return opcode >= DEVICE_LINK_V1_SCAN && opcode <= DEVICE_LINK_V1_FORGET;
}

device_link_v1_decode_t device_link_v1_decode_request(
    const uint8_t *value, size_t length, device_link_v1_request_t *request)
{
    if (request == NULL)
    {
        return DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT;
    }
    memset(request, 0, sizeof(*request));
    if (value == NULL || length < 2U)
    {
        return DEVICE_LINK_V1_DECODE_ATT_VALUE_NOT_ALLOWED;
    }
    const uint8_t opcode = value[0];
    const uint8_t request_id = value[1];

    if ((opcode & 0x80U) != 0U || request_id == 0U)
    {
        return DEVICE_LINK_V1_DECODE_ATT_VALUE_NOT_ALLOWED;
    }
    request->request_id = request_id;
    if (!device_link_v1_opcode_known(opcode))
    {
        request->offending_opcode = opcode;
        return DEVICE_LINK_V1_DECODE_UNKNOWN_OPCODE;
    }
    request->opcode = (device_link_v1_opcode_t)opcode;
    size_t offset = 2U;

    if (opcode == DEVICE_LINK_V1_SET_CREDENTIALS)
    {
        device_link_v1_credentials_t *credentials = &request->payload.credentials;

        if (!_device_link_v1_decode_bytes_u8(
                    value, length, &offset, credentials->ssid,
                    &credentials->ssid_length, 1U, DEVICE_LINK_V1_MAX_SSID_BYTES,
                    true) ||
                !_device_link_v1_decode_bytes_u8(
                    value, length, &offset, credentials->password,
                    &credentials->password_length, 0U,
                    DEVICE_LINK_V1_MAX_PASSWORD_BYTES, false) ||
                offset >= length)
        {
            return DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT;
        }
        credentials->security = (device_link_v1_wifi_security_t)value[offset];
        offset += 1U;
        if (offset != length || !_device_link_v1_credentials_valid(credentials))
        {
            return DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT;
        }
        return DEVICE_LINK_V1_DECODE_OK;
    }
    if (opcode == DEVICE_LINK_V1_ACK_OPERATION)
    {
        if ((offset + 4U) != length)
        {
            return DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT;
        }
        request->payload.operation_id = _device_link_v1_read_u32_le(&value[offset]);
        if (request->payload.operation_id == 0U)
        {
            return DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT;
        }
        return DEVICE_LINK_V1_DECODE_OK;
    }
    if (offset != length)
    {
        return DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT;
    }
    return DEVICE_LINK_V1_DECODE_OK;
}

void device_link_v1_route_write(
    const device_link_v1_route_input_t *input,
    device_link_v1_route_result_t *result)
{
    if (result == NULL)
    {
        return;
    }
    memset(result, 0, sizeof(*result));
    if (input == NULL)
    {
        result->kind = DEVICE_LINK_V1_ROUTE_APP;
        result->status = DEVICE_LINK_V1_STATUS_INTERNAL;
        return;
    }
    if (!input->encrypted || !input->authenticated)
    {
        result->kind = DEVICE_LINK_V1_ROUTE_ATT;
        result->att_error = DEVICE_LINK_V1_ATT_INSUFFICIENT_AUTHENTICATION;
        return;
    }
    if (!device_link_v1_att_value_length_valid(input->att_value_length))
    {
        result->kind = DEVICE_LINK_V1_ROUTE_ATT;
        result->att_error = DEVICE_LINK_V1_ATT_INVALID_ATTRIBUTE_VALUE_LENGTH;
        return;
    }
    const device_link_v1_decode_t decoded = device_link_v1_decode_request(
            input->value, input->att_value_length, &result->request);

    if (decoded == DEVICE_LINK_V1_DECODE_ATT_VALUE_NOT_ALLOWED)
    {
        result->kind = DEVICE_LINK_V1_ROUTE_ATT;
        result->att_error = DEVICE_LINK_V1_ATT_VALUE_NOT_ALLOWED;
        return;
    }
    if (!input->subscription_enabled)
    {
        result->kind = DEVICE_LINK_V1_ROUTE_ATT;
        result->att_error = DEVICE_LINK_V1_ATT_CCCD_NOT_ENABLED;
        return;
    }
    if (input->indication_outstanding)
    {
        result->kind = DEVICE_LINK_V1_ROUTE_ATT;
        result->att_error = DEVICE_LINK_V1_ATT_TX_INDICATION_PENDING;
        return;
    }
    if (decoded == DEVICE_LINK_V1_DECODE_UNKNOWN_OPCODE)
    {
        result->kind = DEVICE_LINK_V1_ROUTE_APP;
        result->status = DEVICE_LINK_V1_STATUS_UNSUPPORTED;
        result->offending_opcode = result->request.offending_opcode;
        return;
    }
    const uint8_t opcode = (uint8_t)result->request.opcode;

    if (opcode != DEVICE_LINK_V1_GET_INFO &&
            input->att_mtu < DEVICE_LINK_V1_REQUIRED_ATT_MTU)
    {
        result->kind = DEVICE_LINK_V1_ROUTE_APP;
        result->status = DEVICE_LINK_V1_STATUS_MTU_TOO_SMALL;
        return;
    }
    if (decoded == DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT)
    {
        result->kind = DEVICE_LINK_V1_ROUTE_APP;
        result->status = DEVICE_LINK_V1_STATUS_INVALID_ARGUMENT;
        return;
    }
    if (device_link_v1_opcode_occupies_slot(opcode) && input->slot_occupied)
    {
        result->kind = DEVICE_LINK_V1_ROUTE_APP;
        result->status = DEVICE_LINK_V1_STATUS_BUSY;
        return;
    }
    if ((opcode == DEVICE_LINK_V1_CONNECT || opcode == DEVICE_LINK_V1_FORGET) &&
            !input->profile_present)
    {
        result->kind = DEVICE_LINK_V1_ROUTE_APP;
        result->status = DEVICE_LINK_V1_STATUS_NOT_FOUND;
        return;
    }
    result->kind = DEVICE_LINK_V1_ROUTE_ADMITTED;
}

size_t device_link_v1_encode_response(
    device_link_v1_opcode_t opcode, uint8_t request_id,
    device_link_v1_status_t status, const void *payload,
    uint8_t *out, size_t capacity)
{
    if (out == NULL || request_id == 0U || capacity < 3U ||
            !device_link_v1_opcode_known((uint8_t)opcode))
    {
        return 0U;
    }
    out[0] = (uint8_t)opcode | DEVICE_LINK_V1_RESPONSE_MASK;
    out[1] = request_id;
    out[2] = (uint8_t)status;
    if (status != DEVICE_LINK_V1_STATUS_OK &&
            status != DEVICE_LINK_V1_STATUS_ACCEPTED)
    {
        return 3U;
    }
    if (status == DEVICE_LINK_V1_STATUS_ACCEPTED)
    {
        const uint32_t *operation_id = payload;

        if (operation_id == NULL || *operation_id == 0U || capacity < 7U)
        {
            return 0U;
        }
        _device_link_v1_write_u32_le(&out[3], *operation_id);
        return 7U;
    }
    return 3U;
}

size_t device_link_v1_encode_info_response(
    uint8_t request_id, const device_link_v1_info_t *info,
    uint8_t *out, size_t capacity)
{
    if (info == NULL || out == NULL ||
            capacity < DEVICE_LINK_V1_MAX_GET_INFO_BYTES)
    {
        return 0U;
    }
    out[0] = (uint8_t)DEVICE_LINK_V1_GET_INFO | DEVICE_LINK_V1_RESPONSE_MASK;
    out[1] = request_id;
    out[2] = (uint8_t)DEVICE_LINK_V1_STATUS_OK;
    out[3] = DEVICE_LINK_V1_PROTOCOL_MAJOR;
    out[4] = DEVICE_LINK_V1_PROTOCOL_MINOR;
    out[5] = info->firmware_major;
    out[6] = info->firmware_minor;
    out[7] = info->firmware_patch;
    out[8] = info->pairing_window_open ? 1U : 0U;
    _device_link_v1_write_u16_le(&out[9], DEVICE_LINK_V1_REQUIRED_ATT_MTU);
    return DEVICE_LINK_V1_MAX_GET_INFO_BYTES;
}

size_t device_link_v1_encode_status_response(
    uint8_t request_id, const device_link_v1_snapshot_t *snapshot,
    uint8_t *out, size_t capacity)
{
    if (out == NULL || capacity < 3U)
    {
        return 0U;
    }
    out[0] = (uint8_t)DEVICE_LINK_V1_GET_STATUS | DEVICE_LINK_V1_RESPONSE_MASK;
    out[1] = request_id;
    out[2] = (uint8_t)DEVICE_LINK_V1_STATUS_OK;
    return _device_link_v1_encode_snapshot_fields(out, capacity, 3U, snapshot);
}

size_t device_link_v1_encode_operation_response(
    uint8_t request_id, device_link_v1_status_t status,
    const device_link_v1_operation_record_t *record,
    uint8_t *out, size_t capacity)
{
    if (out == NULL || capacity < 3U)
    {
        return 0U;
    }
    out[0] = (uint8_t)DEVICE_LINK_V1_GET_OPERATION | DEVICE_LINK_V1_RESPONSE_MASK;
    out[1] = request_id;
    out[2] = (uint8_t)status;
    if (status != DEVICE_LINK_V1_STATUS_OK)
    {
        return 3U;
    }
    if (record == NULL || record->operation_id == 0U || capacity < 11U)
    {
        return 0U;
    }
    _device_link_v1_write_u32_le(&out[3], record->operation_id);
    out[7] = (uint8_t)record->operation;
    out[8] = (uint8_t)record->phase;
    out[9] = (uint8_t)record->failure;
    out[10] = record->count;
    return _device_link_v1_encode_networks(out, capacity, 11U, record->networks,
                                           record->count);
}

size_t device_link_v1_encode_application_error(
    uint8_t request_id, uint8_t offending_opcode,
    uint8_t *out, size_t capacity)
{
    if (out == NULL || request_id == 0U || capacity < 4U ||
            (offending_opcode & 0x80U) != 0U ||
            device_link_v1_opcode_known(offending_opcode))
    {
        return 0U;
    }
    out[0] = DEVICE_LINK_V1_ERROR_OPCODE;
    out[1] = request_id;
    out[2] = (uint8_t)DEVICE_LINK_V1_STATUS_UNSUPPORTED;
    out[3] = offending_opcode;
    return 4U;
}

size_t device_link_v1_encode_wifi_status(
    const device_link_v1_snapshot_t *snapshot, uint8_t *out,
    size_t capacity)
{
    if (out == NULL || capacity < 2U)
    {
        return 0U;
    }
    out[0] = DEVICE_LINK_V1_EVENT_MARKER;
    out[1] = (uint8_t)DEVICE_LINK_V1_WIFI_STATUS;
    return _device_link_v1_encode_snapshot_fields(out, capacity, 2U, snapshot);
}

size_t device_link_v1_encode_scan_complete(
    const device_link_v1_operation_record_t *record, uint8_t *out,
    size_t capacity)
{
    if (record == NULL || out == NULL || record->operation_id == 0U ||
            record->operation != DEVICE_LINK_V1_OPERATION_SCAN ||
            capacity < 8U)
    {
        return 0U;
    }
    if (record->failure != DEVICE_LINK_V1_WIFI_FAILURE_NONE &&
            record->count != 0U)
    {
        return 0U;
    }
    out[0] = DEVICE_LINK_V1_EVENT_MARKER;
    out[1] = (uint8_t)DEVICE_LINK_V1_SCAN_COMPLETE;
    _device_link_v1_write_u32_le(&out[2], record->operation_id);
    out[6] = (uint8_t)record->failure;
    out[7] = record->count;
    return _device_link_v1_encode_networks(out, capacity, 8U, record->networks,
                                           record->count);
}

size_t device_link_v1_encode_operation_complete(
    const device_link_v1_operation_record_t *record, uint8_t *out,
    size_t capacity)
{
    if (record == NULL || out == NULL || record->operation_id == 0U ||
            record->operation == DEVICE_LINK_V1_OPERATION_SCAN ||
            capacity < 8U)
    {
        return 0U;
    }
    out[0] = DEVICE_LINK_V1_EVENT_MARKER;
    out[1] = (uint8_t)DEVICE_LINK_V1_OPERATION_COMPLETE;
    _device_link_v1_write_u32_le(&out[2], record->operation_id);
    out[6] = (uint8_t)record->operation;
    out[7] = (uint8_t)record->failure;
    return 8U;
}

size_t device_link_v1_encode_terminal(
    const device_link_v1_operation_record_t *record, uint8_t *out,
    size_t capacity)
{
    if (record == NULL)
    {
        return 0U;
    }
    if (record->operation == DEVICE_LINK_V1_OPERATION_SCAN)
    {
        return device_link_v1_encode_scan_complete(record, out, capacity);
    }
    return device_link_v1_encode_operation_complete(record, out, capacity);
}

bool device_link_v1_snapshot_equal(const device_link_v1_snapshot_t *left,
                                   const device_link_v1_snapshot_t *right)
{
    if (left == NULL || right == NULL)
    {
        return false;
    }
    return left->state == right->state &&
           left->failure == right->failure &&
           left->profile_ssid_length == right->profile_ssid_length &&
           memcmp(left->profile_ssid, right->profile_ssid,
                  left->profile_ssid_length) == 0;
}

bool device_link_v1_snapshot_valid(const device_link_v1_snapshot_t *snapshot)
{
    if (snapshot == NULL ||
            snapshot->profile_ssid_length > DEVICE_LINK_V1_MAX_SSID_BYTES)
    {
        return false;
    }
    if (snapshot->profile_ssid_length > 0U &&
            !_device_link_v1_utf8_ssid_valid(snapshot->profile_ssid,
                    snapshot->profile_ssid_length))
    {
        return false;
    }
    if (!_device_link_v1_state_failure_allowed(snapshot->state,
            snapshot->failure))
    {
        return false;
    }
    const bool has_profile = snapshot->profile_ssid_length > 0U;

    if (_device_link_v1_profile_required(snapshot->state, snapshot->failure))
    {
        return has_profile;
    }
    return true;
}

bool device_link_v1_failure_allowed(
    device_link_v1_operation_t operation,
    device_link_v1_wifi_failure_t failure)
{
    switch (operation)
    {
    case DEVICE_LINK_V1_OPERATION_SCAN:
        return failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_RADIO ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_INTERNAL;
    case DEVICE_LINK_V1_OPERATION_SET_CREDENTIALS:
        return failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_STORAGE ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_INTERNAL;
    case DEVICE_LINK_V1_OPERATION_CONNECT:
        return failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_AUTHENTICATION ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_AP_NOT_FOUND ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_LINK_LOST ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_RADIO ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_INTERNAL;
    case DEVICE_LINK_V1_OPERATION_DISCONNECT:
        return failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_RADIO ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_INTERNAL;
    case DEVICE_LINK_V1_OPERATION_FORGET:
        return failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_STORAGE ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_RADIO ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT ||
               failure == DEVICE_LINK_V1_WIFI_FAILURE_INTERNAL;
    default:
        return false;
    }
}

uint8_t device_link_v1_filter_scan_networks(
    const device_link_v1_scan_source_t *source, uint8_t source_count,
    device_link_v1_network_t *out, uint8_t out_capacity)
{
    uint8_t count = 0U;

    if (source == NULL || out == NULL || out_capacity == 0U)
    {
        return 0U;
    }
    if (out_capacity > DEVICE_LINK_V1_MAX_SCAN_NETWORKS)
    {
        out_capacity = DEVICE_LINK_V1_MAX_SCAN_NETWORKS;
    }
    for (uint8_t i = 0U; i < source_count; ++i)
    {
        device_link_v1_network_t network;
        bool replaced = false;

        memset(&network, 0, sizeof(network));
        if (source[i].ssid_length < 1U ||
                source[i].ssid_length > DEVICE_LINK_V1_MAX_SSID_BYTES)
        {
            continue;
        }
        memcpy(network.ssid, source[i].ssid, source[i].ssid_length);
        network.ssid_length = source[i].ssid_length;
        network.security = source[i].security;
        network.rssi_dbm = source[i].rssi_dbm;
        if (!_device_link_v1_network_valid(&network))
        {
            continue;
        }
        for (uint8_t j = 0U; j < count; ++j)
        {
            if (_device_link_v1_scan_key_equal(&out[j], &network))
            {
                if (network.rssi_dbm > out[j].rssi_dbm)
                {
                    out[j] = network;
                }
                replaced = true;
                break;
            }
        }
        if (replaced || count >= out_capacity)
        {
            continue;
        }
        out[count] = network;
        ++count;
    }
    return count;
}
