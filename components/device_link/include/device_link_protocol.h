#ifndef __DEVICE_LINK_PROTOCOL_H__
#define __DEVICE_LINK_PROTOCOL_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_LINK_CORE_MAJOR 2U
#define DEVICE_LINK_CORE_MINOR 1U
#define DEVICE_LINK_PROFILE_MAJOR 2U
#define DEVICE_LINK_PROFILE_MINOR 0U

/* Canonical device-link/v2 profile security requirements. */
#define DEVICE_LINK_SECURITY_ENCRYPTION_KEY_BYTES 32U
#define DEVICE_LINK_SECURITY_MAXIMUM_BONDS 1U
#define DEVICE_LINK_SECURITY_PROTOCOMM_VERSION 2U
#define DEVICE_LINK_SECURITY_PROTOCOMM_PATCH_VERSION 1U
#define DEVICE_LINK_SECURITY_SECURE_CONNECTIONS_ONLY true
#define DEVICE_LINK_SECURITY_LOCAL_CONFIRMATION_FOR_GRANTS true
#define DEVICE_LINK_SECURITY_QR_BOOTSTRAP_USES_POP true
#define DEVICE_LINK_SECURITY_PUBLIC_BOOTSTRAP_USES_SC_CONFIRMATION true

#define DEVICE_LINK_WIRE_HEADER_VERSION 1U
#define DEVICE_LINK_WIRE_HEADER_BYTES 16U
#define DEVICE_LINK_RESPONSE_STATUS_BYTES 2U
#define DEVICE_LINK_FIXED_SLOT_BYTES 512U
#define DEVICE_LINK_MAX_MESSAGE_BYTES 4096U
#define DEVICE_LINK_MAX_SESSION_MESSAGE_BYTES 1024U
#define DEVICE_LINK_MAX_CONTROL_MESSAGE_BYTES 4096U
#define DEVICE_LINK_MAX_DOMAIN_PAYLOAD_BYTES 3072U
#define DEVICE_LINK_MAX_PERMISSIONS 16U
#define DEVICE_LINK_MAX_OPERATIONS 4U
#define DEVICE_LINK_REPLAY_SLOTS 4U
#define DEVICE_LINK_REPLAY_DIGEST_BYTES 32U

#define DEVICE_LINK_DOMAIN_CORE 0U
#define DEVICE_LINK_DOMAIN_WIFI 1U
#define DEVICE_LINK_DOMAIN_CLOUD 2U
#define DEVICE_LINK_DOMAIN_LOCATION 3U
#define DEVICE_LINK_DOMAIN_INVALID 255U

#ifdef __cplusplus
static_assert(DEVICE_LINK_WIRE_HEADER_BYTES == 16U,
              "Device Link v2 application header is fixed at 16 bytes");
static_assert(DEVICE_LINK_RESPONSE_STATUS_BYTES == 2U,
              "Device Link v2 response status is fixed at two bytes");
static_assert(DEVICE_LINK_REPLAY_SLOTS == 4U,
              "Device Link v2 replay storage is fixed at four slots");
#else
_Static_assert(DEVICE_LINK_WIRE_HEADER_BYTES == 16U,
               "Device Link v2 application header is fixed at 16 bytes");
_Static_assert(DEVICE_LINK_RESPONSE_STATUS_BYTES == 2U,
               "Device Link v2 response status is fixed at two bytes");
_Static_assert(DEVICE_LINK_REPLAY_SLOTS == 4U,
               "Device Link v2 replay storage is fixed at four slots");
#endif

#define DEVICE_LINK_PERMISSION_CORE_READ 0x0001U
#define DEVICE_LINK_PERMISSION_CORE_BIND 0x0002U
#define DEVICE_LINK_PERMISSION_CORE_OPERATE 0x0003U
#define DEVICE_LINK_PERMISSION_WIFI_READ 0x0101U
#define DEVICE_LINK_PERMISSION_WIFI_SCAN 0x0102U
#define DEVICE_LINK_PERMISSION_WIFI_WRITE 0x0103U

/** @brief Stable application status values. Zero is never encoded. */
typedef enum device_link_status
{
    DEVICE_LINK_STATUS_OK = 1,
    DEVICE_LINK_STATUS_MALFORMED_FRAME = 2,
    DEVICE_LINK_STATUS_UNSUPPORTED_VERSION = 3,
    DEVICE_LINK_STATUS_UNSUPPORTED_OPERATION = 4,
    DEVICE_LINK_STATUS_UNSUPPORTED_CAPABILITY = 5,
    DEVICE_LINK_STATUS_UNAUTHENTICATED = 6,
    DEVICE_LINK_STATUS_PERMISSION_DENIED = 7,
    DEVICE_LINK_STATUS_CONFIRMATION_REQUIRED = 8,
    DEVICE_LINK_STATUS_INVALID_ARGUMENT = 9,
    DEVICE_LINK_STATUS_BUSY = 10,
    DEVICE_LINK_STATUS_NOT_FOUND = 11,
    DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED = 12,
    DEVICE_LINK_STATUS_CONFLICT = 13,
    DEVICE_LINK_STATUS_UNAVAILABLE = 14,
    DEVICE_LINK_STATUS_STORAGE = 15,
    DEVICE_LINK_STATUS_INTERNAL = 16,
} device_link_status_t;

typedef enum device_link_message_kind
{
    DEVICE_LINK_MESSAGE_INVALID = 0,
    DEVICE_LINK_MESSAGE_REQUEST = 1,
    DEVICE_LINK_MESSAGE_RESPONSE = 2,
    DEVICE_LINK_MESSAGE_EVENT_RESERVED = 3,
} device_link_message_kind_t;

typedef enum device_link_channel
{
    DEVICE_LINK_CHANNEL_SESSION = 1,
    DEVICE_LINK_CHANNEL_CONTROL = 2,
} device_link_channel_t;

typedef enum device_link_admission_class
{
    DEVICE_LINK_ADMISSION_UNBOUND_PUBLIC = 1,
    DEVICE_LINK_ADMISSION_UNBOUND_QR = 2,
    DEVICE_LINK_ADMISSION_BOUND_PUBLIC_READ_ONLY = 3,
    DEVICE_LINK_ADMISSION_VERIFIED_UNAUTHORIZED = 4,
    DEVICE_LINK_ADMISSION_AUTHORIZED = 5,
} device_link_admission_class_t;

typedef struct device_link_wire_header
{
    device_link_message_kind_t kind;
    bool recovery_query;
    uint8_t domain_id;
    uint8_t domain_major;
    uint8_t method_id;
    uint32_t call_id;
    uint64_t boot_id;
} device_link_wire_header_t;

typedef enum device_link_confirmation_kind
{
    DEVICE_LINK_CONFIRMATION_NONE = 0,
    DEVICE_LINK_CONFIRMATION_BINDING,
    DEVICE_LINK_CONFIRMATION_GRANT_EXPANSION,
} device_link_confirmation_kind_t;

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_PROTOCOL_H__ */
