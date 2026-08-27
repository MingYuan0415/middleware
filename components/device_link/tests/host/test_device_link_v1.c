#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "device_link_v1.h"

static void _expect_hex(const char *label, const uint8_t *bytes, size_t length,
                        const char *hex)
{
    char actual[256];
    size_t offset = 0U;

    assert(length * 2U < sizeof(actual));
    for (size_t i = 0U; i < length; ++i)
    {
        const int written = snprintf(&actual[offset], sizeof(actual) - offset,
                                     "%02x", bytes[i]);

        assert(written == 2);
        offset += 2U;
    }
    actual[offset] = '\0';
    if (strcmp(actual, hex) != 0)
    {
        fprintf(stderr, "%s: got %s want %s\n", label, actual, hex);
    }
    assert(strcmp(actual, hex) == 0);
}

static void _test_valid_requests(void)
{
    device_link_v1_request_t request;
    const uint8_t get_info[] = {0x01, 0x01};
    const uint8_t set_credentials[] =
    {
        0x04, 0x04, 0x04, 'H', 'o', 'm', 'e', 0x08,
        'p', 'a', 's', 's', 'w', 'o', 'r', 'd', 0x02
    };
    const uint8_t ack[] = {0x09, 0x09, 0x01, 0x00, 0x00, 0x00};

    assert(device_link_v1_decode_request(get_info, sizeof(get_info),
                                         &request) == DEVICE_LINK_V1_DECODE_OK);
    assert(request.opcode == DEVICE_LINK_V1_GET_INFO);
    assert(request.request_id == 1U);
    assert(device_link_v1_decode_request(set_credentials, sizeof(set_credentials),
                                         &request) == DEVICE_LINK_V1_DECODE_OK);
    assert(request.opcode == DEVICE_LINK_V1_SET_CREDENTIALS);
    assert(request.payload.credentials.ssid_length == 4U);
    assert(request.payload.credentials.security == DEVICE_LINK_V1_WIFI_PERSONAL);
    assert(device_link_v1_decode_request(ack, sizeof(ack),
                                         &request) == DEVICE_LINK_V1_DECODE_OK);
    assert(request.payload.operation_id == 1U);
}

static void _test_invalid_requests(void)
{
    device_link_v1_request_t request;
    const uint8_t rid_zero[] = {0x01, 0x00};
    const uint8_t trailing[] = {0x01, 0x01, 0x00};
    const uint8_t msb[] = {0x81, 0x01};
    const uint8_t unknown[] = {0x0a, 0x0a};
    const uint8_t reserved[] = {0x70, 0x0b};
    const uint8_t empty_ssid[] = {0x04, 0x04, 0x00, 0x00, 0x01};
    const uint8_t open_password[] = {0x04, 0x04, 0x01, 'A', 0x01, 'x', 0x01};

    assert(device_link_v1_decode_request(rid_zero, sizeof(rid_zero),
                                         &request) == DEVICE_LINK_V1_DECODE_ATT_VALUE_NOT_ALLOWED);
    assert(device_link_v1_decode_request(trailing, sizeof(trailing),
                                         &request) == DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT);
    assert(device_link_v1_decode_request(msb, sizeof(msb),
                                         &request) == DEVICE_LINK_V1_DECODE_ATT_VALUE_NOT_ALLOWED);
    assert(device_link_v1_decode_request(unknown, sizeof(unknown),
                                         &request) == DEVICE_LINK_V1_DECODE_UNKNOWN_OPCODE);
    assert(request.offending_opcode == 0x0aU);
    assert(device_link_v1_decode_request(reserved, sizeof(reserved),
                                         &request) == DEVICE_LINK_V1_DECODE_UNKNOWN_OPCODE);
    assert(device_link_v1_decode_request(empty_ssid, sizeof(empty_ssid),
                                         &request) == DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT);
    assert(device_link_v1_decode_request(open_password, sizeof(open_password),
                                         &request) == DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT);
}

static void _test_encodings(void)
{
    uint8_t out[64];
    device_link_v1_info_t info =
    {
        .firmware_major = 2U,
        .firmware_minor = 3U,
        .firmware_patch = 4U,
        .pairing_window_open = true,
    };
    device_link_v1_snapshot_t snapshot;
    device_link_v1_operation_record_t record;
    uint32_t operation_id = 1U;

    memset(&snapshot, 0, sizeof(snapshot));
    memset(&record, 0, sizeof(record));
    snapshot.state = DEVICE_LINK_V1_WIFI_CONNECTED;
    snapshot.failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    snapshot.profile_ssid[0] = 'H';
    snapshot.profile_ssid[1] = 'o';
    snapshot.profile_ssid[2] = 'm';
    snapshot.profile_ssid[3] = 'e';
    snapshot.profile_ssid_length = 4U;
    record.operation_id = 1U;
    record.operation = DEVICE_LINK_V1_OPERATION_SCAN;
    record.phase = DEVICE_LINK_V1_OPERATION_ACTIVE;
    record.failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    _expect_hex("get-info-response", out,
                device_link_v1_encode_info_response(1U, &info, out, sizeof(out)),
                "810100010002030401f201");
    _expect_hex("get-status-response", out,
                device_link_v1_encode_status_response(2U, &snapshot, out,
                        sizeof(out)),
                "820200050004486f6d65");
    _expect_hex("scan-accepted", out,
                device_link_v1_encode_response(DEVICE_LINK_V1_SCAN, 3U,
                        DEVICE_LINK_V1_STATUS_ACCEPTED, &operation_id, out,
                        sizeof(out)),
                "83030101000000");
    _expect_hex("get-operation-active", out,
                device_link_v1_encode_operation_response(
                    8U, DEVICE_LINK_V1_STATUS_OK, &record, out, sizeof(out)),
                "8808000100000003010000");
    _expect_hex("ack-operation-ok", out,
                device_link_v1_encode_response(DEVICE_LINK_V1_ACK_OPERATION, 9U,
                        DEVICE_LINK_V1_STATUS_OK, NULL, out, sizeof(out)),
                "890900");
    _expect_hex("get-operation-not-found", out,
                device_link_v1_encode_operation_response(
                    11U, DEVICE_LINK_V1_STATUS_NOT_FOUND, NULL, out,
                    sizeof(out)),
                "880b04");
    _expect_hex("reserved-opcode-error", out,
                device_link_v1_encode_application_error(42U, 0x70U, out,
                        sizeof(out)),
                "802a0870");
    _expect_hex("wifi-status-connected", out,
                device_link_v1_encode_wifi_status(&snapshot, out, sizeof(out)),
                "f001050004486f6d65");
    record.phase = DEVICE_LINK_V1_OPERATION_SUCCEEDED;
    record.count = 1U;
    record.networks[0].ssid[0] = 'H';
    record.networks[0].ssid[1] = 'o';
    record.networks[0].ssid[2] = 'm';
    record.networks[0].ssid[3] = 'e';
    record.networks[0].ssid_length = 4U;
    record.networks[0].security = DEVICE_LINK_V1_WIFI_PERSONAL;
    record.networks[0].rssi_dbm = -42;
    _expect_hex("scan-complete", out,
                device_link_v1_encode_scan_complete(&record, out, sizeof(out)),
                "f00201000000000104486f6d6502d6");
    record.operation_id = 2U;
    record.operation = DEVICE_LINK_V1_OPERATION_SET_CREDENTIALS;
    record.count = 0U;
    _expect_hex("operation-complete", out,
                device_link_v1_encode_operation_complete(&record, out,
                        sizeof(out)),
                "f003020000000400");
}

static void _fill_route(device_link_v1_route_input_t *input, uint8_t opcode,
                        uint8_t request_id, uint16_t mtu, size_t length)
{
    static uint8_t value[8];

    memset(input, 0, sizeof(*input));
    memset(value, 0, sizeof(value));
    value[0] = opcode;
    value[1] = request_id;
    input->att_mtu = mtu;
    input->att_value_length = length;
    input->encrypted = true;
    input->authenticated = true;
    input->subscription_enabled = true;
    input->value = value;
}

static void _test_routing(void)
{
    device_link_v1_route_input_t input;
    device_link_v1_route_result_t result;

    _fill_route(&input, 129U, 0U, 23U, 496U);
    input.encrypted = false;
    device_link_v1_route_write(&input, &result);
    assert(result.kind == DEVICE_LINK_V1_ROUTE_ATT);
    assert(result.att_error == DEVICE_LINK_V1_ATT_INSUFFICIENT_AUTHENTICATION);

    _fill_route(&input, 129U, 0U, 23U, 496U);
    device_link_v1_route_write(&input, &result);
    assert(result.kind == DEVICE_LINK_V1_ROUTE_ATT);
    assert(result.att_error == DEVICE_LINK_V1_ATT_INVALID_ATTRIBUTE_VALUE_LENGTH);

    _fill_route(&input, 129U, 1U, 23U, 2U);
    input.subscription_enabled = false;
    input.indication_outstanding = true;
    device_link_v1_route_write(&input, &result);
    assert(result.kind == DEVICE_LINK_V1_ROUTE_ATT);
    assert(result.att_error == DEVICE_LINK_V1_ATT_VALUE_NOT_ALLOWED);

    _fill_route(&input, 1U, 1U, 498U, 2U);
    input.subscription_enabled = false;
    input.indication_outstanding = true;
    device_link_v1_route_write(&input, &result);
    assert(result.kind == DEVICE_LINK_V1_ROUTE_ATT);
    assert(result.att_error == DEVICE_LINK_V1_ATT_CCCD_NOT_ENABLED);

    _fill_route(&input, 1U, 1U, 498U, 2U);
    input.indication_outstanding = true;
    device_link_v1_route_write(&input, &result);
    assert(result.kind == DEVICE_LINK_V1_ROUTE_ATT);
    assert(result.att_error == DEVICE_LINK_V1_ATT_TX_INDICATION_PENDING);

    _fill_route(&input, 10U, 10U, 23U, 2U);
    device_link_v1_route_write(&input, &result);
    assert(result.kind == DEVICE_LINK_V1_ROUTE_APP);
    assert(result.status == DEVICE_LINK_V1_STATUS_UNSUPPORTED);

    _fill_route(&input, 1U, 12U, 23U, 2U);
    device_link_v1_route_write(&input, &result);
    assert(result.kind == DEVICE_LINK_V1_ROUTE_ADMITTED);

    _fill_route(&input, 2U, 13U, 23U, 2U);
    device_link_v1_route_write(&input, &result);
    assert(result.kind == DEVICE_LINK_V1_ROUTE_APP);
    assert(result.status == DEVICE_LINK_V1_STATUS_MTU_TOO_SMALL);

    _fill_route(&input, 5U, 15U, 498U, 2U);
    input.slot_occupied = true;
    device_link_v1_route_write(&input, &result);
    assert(result.kind == DEVICE_LINK_V1_ROUTE_APP);
    assert(result.status == DEVICE_LINK_V1_STATUS_BUSY);

    _fill_route(&input, 5U, 17U, 498U, 2U);
    input.profile_present = false;
    device_link_v1_route_write(&input, &result);
    assert(result.kind == DEVICE_LINK_V1_ROUTE_APP);
    assert(result.status == DEVICE_LINK_V1_STATUS_NOT_FOUND);
}

static void _test_scan_filter(void)
{
    device_link_v1_scan_source_t source[4];
    device_link_v1_network_t out[5];

    memset(source, 0, sizeof(source));
    source[0].ssid[0] = 'A';
    source[0].ssid_length = 1U;
    source[0].security = DEVICE_LINK_V1_WIFI_OPEN;
    source[0].rssi_dbm = -40;
    source[1].ssid_length = 0U;
    source[1].security = DEVICE_LINK_V1_WIFI_OPEN;
    source[2].ssid[0] = 'B';
    source[2].ssid_length = 1U;
    source[2].security = (device_link_v1_wifi_security_t)0;
    source[2].rssi_dbm = -30;
    source[3].ssid[0] = 'A';
    source[3].ssid_length = 1U;
    source[3].security = DEVICE_LINK_V1_WIFI_PERSONAL;
    source[3].rssi_dbm = -20;
    assert(device_link_v1_filter_scan_networks(source, 4U, out, 5U) == 2U);
    assert(out[0].security == DEVICE_LINK_V1_WIFI_OPEN);
    assert(out[1].security == DEVICE_LINK_V1_WIFI_PERSONAL);
    assert(out[1].ssid[0] == 'A');

    source[0].rssi_dbm = -50;
    source[0].security = DEVICE_LINK_V1_WIFI_PERSONAL;
    source[3].rssi_dbm = -10;
    assert(device_link_v1_filter_scan_networks(source, 4U, out, 5U) == 1U);
    assert(out[0].security == DEVICE_LINK_V1_WIFI_PERSONAL);
    assert(out[0].rssi_dbm == -10);
}

int main(void)
{
    _test_valid_requests();
    _test_invalid_requests();
    _test_encodings();
    _test_routing();
    _test_scan_filter();
    return 0;
}
