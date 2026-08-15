#ifndef __DEVICE_LINK_ROUTER_H__
#define __DEVICE_LINK_ROUTER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "device_link_protocol.h"
#include "device_link_tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_LINK_METHOD_BOOTSTRAP 0x01U
#define DEVICE_LINK_METHOD_ASYNCHRONOUS 0x02U
#define DEVICE_LINK_STATUS_MASK(status) \
    (UINT32_C(1) << ((uint32_t)(status) - 1U))

typedef struct device_link_request_context
{
    device_link_wire_header_t header;
    uint32_t connection_generation;
    uint32_t security_epoch;
    device_link_channel_t channel;
    device_link_admission_class_t admission;
    bool security_authenticated;
    bool authorized;
    const uint16_t *permissions;
    size_t permission_count;
} device_link_request_context_t;

typedef device_link_status_t (*device_link_method_handler_t)(
    const device_link_request_context_t *context,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len,
    void *arg);

/** @brief Cryptographic digest used for replay equality without retaining
 * the protected request bytes. */
typedef esp_err_t (*device_link_request_digest_fn_t)(
    const uint8_t *request, size_t request_len,
    uint8_t digest[DEVICE_LINK_REPLAY_DIGEST_BYTES], void *arg);

typedef struct device_link_method_descriptor
{
    uint8_t method_id;
    uint8_t flags;
    device_link_channel_t channel;
    uint16_t permission_id;
    uint16_t maximum_request_bytes;
    uint16_t maximum_response_bytes;
    const device_link_tlv_schema_t *request_schema;
    const device_link_tlv_schema_t *response_schema;
    /* Asynchronous methods declare the operation result message frozen in
     * the domain contract: NULL when the method has no operation result,
     * the Empty schema when a SUCCEEDED record must carry no result
     * payload, or the result message schema (e.g. wifi.v1.WifiStatus). */
    const device_link_tlv_schema_t *operation_result_schema;
    uint32_t response_body_status_mask;
    device_link_method_handler_t handler;
    void *handler_arg;
} device_link_method_descriptor_t;

typedef struct device_link_domain_descriptor
{
    uint8_t domain_id;
    uint8_t major;
    uint8_t minor;
    const device_link_method_descriptor_t *methods;
    size_t method_count;
} device_link_domain_descriptor_t;

typedef struct device_link_call_replay
{
    bool active;
    uint32_t last_call_id;
    size_t request_len;
    uint8_t request_digest[DEVICE_LINK_REPLAY_DIGEST_BYTES];
    uint32_t connection_generation;
    uint64_t security_epoch;
    uint8_t domain_id;
    uint8_t method_id;
    uint8_t *response;
    size_t response_capacity;
    size_t response_len;
} device_link_call_replay_t;

typedef struct device_link_router
{
    uint64_t boot_id;
    const device_link_domain_descriptor_t *domains;
    size_t domain_count;
    device_link_call_replay_t *replay;
    size_t replay_count;
    size_t next_replay_slot;
    device_link_request_digest_fn_t digest;
    void *digest_arg;
    bool sealed;
} device_link_router_t;

esp_err_t device_link_domain_descriptors_validate(
    const device_link_domain_descriptor_t *domains, size_t domain_count);

esp_err_t device_link_router_init(
    device_link_router_t *router, uint64_t boot_id,
    const device_link_domain_descriptor_t *domains, size_t domain_count,
    device_link_call_replay_t *replay, size_t replay_count,
    device_link_request_digest_fn_t digest, void *digest_arg);

/** @brief Seal the startup descriptor set and replay storage. */
esp_err_t device_link_router_seal(device_link_router_t *router);

/** @brief Return whether the router has completed startup sealing. */
bool device_link_router_is_sealed(const device_link_router_t *router);

void device_link_call_replay_reset(device_link_call_replay_t *replay);

esp_err_t device_link_router_process(
    device_link_router_t *router,
    const device_link_request_context_t *facts,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len);

bool device_link_permission_set_valid(
    const uint16_t *permissions, size_t permission_count);

bool device_link_permission_contains(
    const uint16_t *permissions, size_t permission_count,
    uint16_t permission);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_ROUTER_H__ */
