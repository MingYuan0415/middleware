#ifndef __DEVICE_LINK_V1_H__
#define __DEVICE_LINK_V1_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_LINK_V1_PROTOCOL_MAJOR 1U
#define DEVICE_LINK_V1_PROTOCOL_MINOR 0U
#define DEVICE_LINK_V1_REQUIRED_ATT_MTU 498U
#define DEVICE_LINK_V1_MINIMUM_ATT_MTU 23U
#define DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES 495U
#define DEVICE_LINK_V1_MAX_SSID_BYTES 32U
#define DEVICE_LINK_V1_MAX_PASSWORD_BYTES 63U
#define DEVICE_LINK_V1_MIN_PERSONAL_PASSWORD_BYTES 8U
#define DEVICE_LINK_V1_MAX_SCAN_NETWORKS 5U
#define DEVICE_LINK_V1_MAX_GET_INFO_BYTES 11U
#define DEVICE_LINK_V1_MAX_GET_OPERATION_BYTES 186U
#define DEVICE_LINK_V1_EVENT_MARKER 0xf0U
#define DEVICE_LINK_V1_RESPONSE_MASK 0x80U
#define DEVICE_LINK_V1_ERROR_OPCODE 0x80U

#define DEVICE_LINK_V1_ATT_INSUFFICIENT_AUTHENTICATION 0x05U
#define DEVICE_LINK_V1_ATT_INSUFFICIENT_ENCRYPTION 0x0fU
#define DEVICE_LINK_V1_ATT_INSUFFICIENT_RESOURCES 0x11U
#define DEVICE_LINK_V1_ATT_INVALID_ATTRIBUTE_VALUE_LENGTH 0x0dU
#define DEVICE_LINK_V1_ATT_VALUE_NOT_ALLOWED 0x13U
#define DEVICE_LINK_V1_OPERATION_TIMEOUT_MS 60000U
#define DEVICE_LINK_V1_ATT_CCCD_NOT_ENABLED 0xfdu
#define DEVICE_LINK_V1_ATT_TX_INDICATION_PENDING 0xfeU

typedef enum device_link_v1_status
{
    DEVICE_LINK_V1_STATUS_OK = 0,
    DEVICE_LINK_V1_STATUS_ACCEPTED = 1,
    DEVICE_LINK_V1_STATUS_BUSY = 2,
    DEVICE_LINK_V1_STATUS_INVALID_ARGUMENT = 3,
    DEVICE_LINK_V1_STATUS_NOT_FOUND = 4,
    DEVICE_LINK_V1_STATUS_UNAVAILABLE = 5,
    DEVICE_LINK_V1_STATUS_STORAGE = 6,
    DEVICE_LINK_V1_STATUS_MTU_TOO_SMALL = 7,
    DEVICE_LINK_V1_STATUS_UNSUPPORTED = 8,
    DEVICE_LINK_V1_STATUS_INTERNAL = 9,
} device_link_v1_status_t;

typedef enum device_link_v1_opcode
{
    DEVICE_LINK_V1_GET_INFO = 1,
    DEVICE_LINK_V1_GET_STATUS = 2,
    DEVICE_LINK_V1_SCAN = 3,
    DEVICE_LINK_V1_SET_CREDENTIALS = 4,
    DEVICE_LINK_V1_CONNECT = 5,
    DEVICE_LINK_V1_DISCONNECT = 6,
    DEVICE_LINK_V1_FORGET = 7,
    DEVICE_LINK_V1_GET_OPERATION = 8,
    DEVICE_LINK_V1_ACK_OPERATION = 9,
} device_link_v1_opcode_t;

typedef enum device_link_v1_event
{
    DEVICE_LINK_V1_WIFI_STATUS = 1,
    DEVICE_LINK_V1_SCAN_COMPLETE = 2,
    DEVICE_LINK_V1_OPERATION_COMPLETE = 3,
} device_link_v1_event_t;

typedef enum device_link_v1_wifi_security
{
    DEVICE_LINK_V1_WIFI_OPEN = 1,
    DEVICE_LINK_V1_WIFI_PERSONAL = 2,
} device_link_v1_wifi_security_t;

typedef enum device_link_v1_wifi_state
{
    DEVICE_LINK_V1_WIFI_UNAVAILABLE = 1,
    DEVICE_LINK_V1_WIFI_IDLE = 2,
    DEVICE_LINK_V1_WIFI_SCANNING = 3,
    DEVICE_LINK_V1_WIFI_CONNECTING = 4,
    DEVICE_LINK_V1_WIFI_CONNECTED = 5,
    DEVICE_LINK_V1_WIFI_ERROR = 6,
} device_link_v1_wifi_state_t;

typedef enum device_link_v1_wifi_failure
{
    DEVICE_LINK_V1_WIFI_FAILURE_NONE = 0,
    DEVICE_LINK_V1_WIFI_FAILURE_AUTHENTICATION = 1,
    DEVICE_LINK_V1_WIFI_FAILURE_AP_NOT_FOUND = 2,
    DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT = 3,
    DEVICE_LINK_V1_WIFI_FAILURE_LINK_LOST = 4,
    DEVICE_LINK_V1_WIFI_FAILURE_RADIO = 5,
    DEVICE_LINK_V1_WIFI_FAILURE_STORAGE = 6,
    DEVICE_LINK_V1_WIFI_FAILURE_INTERNAL = 7,
} device_link_v1_wifi_failure_t;

typedef enum device_link_v1_operation
{
    DEVICE_LINK_V1_OPERATION_SCAN = 3,
    DEVICE_LINK_V1_OPERATION_SET_CREDENTIALS = 4,
    DEVICE_LINK_V1_OPERATION_CONNECT = 5,
    DEVICE_LINK_V1_OPERATION_DISCONNECT = 6,
    DEVICE_LINK_V1_OPERATION_FORGET = 7,
} device_link_v1_operation_t;

typedef enum device_link_v1_operation_phase
{
    DEVICE_LINK_V1_OPERATION_ACTIVE = 1,
    DEVICE_LINK_V1_OPERATION_SUCCEEDED = 2,
    DEVICE_LINK_V1_OPERATION_FAILED = 3,
} device_link_v1_operation_phase_t;

typedef enum device_link_v1_decode
{
    DEVICE_LINK_V1_DECODE_OK = 0,
    DEVICE_LINK_V1_DECODE_ATT_VALUE_NOT_ALLOWED,
    DEVICE_LINK_V1_DECODE_UNKNOWN_OPCODE,
    DEVICE_LINK_V1_DECODE_INVALID_ARGUMENT,
} device_link_v1_decode_t;

typedef enum device_link_v1_route_kind
{
    DEVICE_LINK_V1_ROUTE_ADMITTED = 0,
    DEVICE_LINK_V1_ROUTE_ATT,
    DEVICE_LINK_V1_ROUTE_APP,
} device_link_v1_route_kind_t;

typedef struct device_link_v1_network
{
    uint8_t ssid[DEVICE_LINK_V1_MAX_SSID_BYTES];
    uint8_t ssid_length;
    device_link_v1_wifi_security_t security;
    int8_t rssi_dbm;
} device_link_v1_network_t;

typedef struct device_link_v1_scan_source
{
    uint8_t ssid[DEVICE_LINK_V1_MAX_SSID_BYTES];
    uint8_t ssid_length;
    device_link_v1_wifi_security_t security;
    int8_t rssi_dbm;
} device_link_v1_scan_source_t;

typedef struct device_link_v1_snapshot
{
    device_link_v1_wifi_state_t state;
    device_link_v1_wifi_failure_t failure;
    uint8_t profile_ssid[DEVICE_LINK_V1_MAX_SSID_BYTES];
    uint8_t profile_ssid_length;
} device_link_v1_snapshot_t;

typedef struct device_link_v1_credentials
{
    uint8_t ssid[DEVICE_LINK_V1_MAX_SSID_BYTES];
    uint8_t ssid_length;
    uint8_t password[DEVICE_LINK_V1_MAX_PASSWORD_BYTES];
    uint8_t password_length;
    device_link_v1_wifi_security_t security;
} device_link_v1_credentials_t;

typedef struct device_link_v1_info
{
    uint8_t firmware_major;
    uint8_t firmware_minor;
    uint8_t firmware_patch;
    bool pairing_window_open;
} device_link_v1_info_t;

typedef struct device_link_v1_operation_record
{
    uint32_t operation_id;
    device_link_v1_operation_t operation;
    device_link_v1_operation_phase_t phase;
    device_link_v1_wifi_failure_t failure;
    device_link_v1_network_t networks[DEVICE_LINK_V1_MAX_SCAN_NETWORKS];
    uint8_t count;
} device_link_v1_operation_record_t;

typedef struct device_link_v1_request
{
    device_link_v1_opcode_t opcode;
    uint8_t request_id;
    uint8_t offending_opcode;
    union
    {
        device_link_v1_credentials_t credentials;
        uint32_t operation_id;
    } payload;
} device_link_v1_request_t;

typedef struct device_link_v1_route_input
{
    uint16_t att_mtu;
    size_t att_value_length;
    bool encrypted;
    bool authenticated;
    bool subscription_enabled;
    bool indication_outstanding;
    bool slot_occupied;
    bool profile_present;
    const uint8_t *value;
} device_link_v1_route_input_t;

typedef struct device_link_v1_route_result
{
    device_link_v1_route_kind_t kind;
    uint8_t att_error;
    device_link_v1_status_t status;
    uint8_t offending_opcode;
    device_link_v1_request_t request;
} device_link_v1_route_result_t;

/** @brief Return whether @p length is a legal ATT Value size. */
bool device_link_v1_att_value_length_valid(size_t length);

/** @brief Return whether @p opcode is a known v1 command. */
bool device_link_v1_opcode_known(uint8_t opcode);

/** @brief Return whether @p opcode occupies the operation slot. */
bool device_link_v1_opcode_occupies_slot(uint8_t opcode);

/**
 * @brief Decode one command_rx ATT Value.
 *
 * @param[in]  value             ATT Value bytes.
 * @param[in]  length            ATT Value length, already <= 495.
 * @param[out] request           Decoded request on OK or UNKNOWN_OPCODE.
 * @return Decode result. ATT_VALUE_NOT_ALLOWED maps to ATT 0x13.
 */
device_link_v1_decode_t device_link_v1_decode_request(
    const uint8_t *value, size_t length, device_link_v1_request_t *request);

/**
 * @brief Apply GATT then application admission precedence.
 *
 * @param[in]  input  Connection and write facts.
 * @param[out] result Route decision and decoded request when admitted.
 */
void device_link_v1_route_write(
    const device_link_v1_route_input_t *input,
    device_link_v1_route_result_t *result);

/** @brief Encode a success or empty-error command response. */
size_t device_link_v1_encode_response(
    device_link_v1_opcode_t opcode, uint8_t request_id,
    device_link_v1_status_t status, const void *payload,
    uint8_t *out, size_t capacity);

/** @brief Encode GET_INFO payload plus response header. */
size_t device_link_v1_encode_info_response(
    uint8_t request_id, const device_link_v1_info_t *info,
    uint8_t *out, size_t capacity);

/** @brief Encode GET_STATUS payload plus response header. */
size_t device_link_v1_encode_status_response(
    uint8_t request_id, const device_link_v1_snapshot_t *snapshot,
    uint8_t *out, size_t capacity);

/** @brief Encode GET_OPERATION payload plus response header. */
size_t device_link_v1_encode_operation_response(
    uint8_t request_id, device_link_v1_status_t status,
    const device_link_v1_operation_record_t *record,
    uint8_t *out, size_t capacity);

/** @brief Encode the four-byte unknown-opcode envelope. */
size_t device_link_v1_encode_application_error(
    uint8_t request_id, uint8_t offending_opcode,
    uint8_t *out, size_t capacity);

/** @brief Encode WIFI_STATUS. */
size_t device_link_v1_encode_wifi_status(
    const device_link_v1_snapshot_t *snapshot, uint8_t *out,
    size_t capacity);

/** @brief Encode SCAN_COMPLETE from a retained record. */
size_t device_link_v1_encode_scan_complete(
    const device_link_v1_operation_record_t *record, uint8_t *out,
    size_t capacity);

/** @brief Encode OPERATION_COMPLETE from a retained record. */
size_t device_link_v1_encode_operation_complete(
    const device_link_v1_operation_record_t *record, uint8_t *out,
    size_t capacity);

/** @brief Encode the matching terminal event for @p record. */
size_t device_link_v1_encode_terminal(
    const device_link_v1_operation_record_t *record, uint8_t *out,
    size_t capacity);

/** @brief Return whether two snapshots have identical wire fields. */
bool device_link_v1_snapshot_equal(const device_link_v1_snapshot_t *left,
                                   const device_link_v1_snapshot_t *right);

/** @brief Return whether @p snapshot is a representable WIFI_STATUS. */
bool device_link_v1_snapshot_valid(const device_link_v1_snapshot_t *snapshot);

/** @brief Return whether @p failure may terminate @p operation. */
bool device_link_v1_failure_allowed(
    device_link_v1_operation_t operation,
    device_link_v1_wifi_failure_t failure);

/**
 * @brief Filter source scan rows into at most five wire records.
 *
 * @return Number of retained records.
 */
uint8_t device_link_v1_filter_scan_networks(
    const device_link_v1_scan_source_t *source, uint8_t source_count,
    device_link_v1_network_t *out, uint8_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_V1_H__ */
