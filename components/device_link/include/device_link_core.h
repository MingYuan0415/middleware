#ifndef __DEVICE_LINK_CORE_H__
#define __DEVICE_LINK_CORE_H__

#include <stddef.h>

#include "device_link_router.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef device_link_status_t (*device_link_core_method_fn)(
    const device_link_request_context_t *context,
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, size_t *response_len,
    void *arg);

typedef struct device_link_core_callbacks
{
    device_link_core_method_fn method;
    void *arg;
} device_link_core_callbacks_t;

typedef struct device_link_core
{
    device_link_domain_descriptor_t domain;
    device_link_method_descriptor_t methods[7];
    device_link_core_callbacks_t callbacks;
} device_link_core_t;

/** @brief Build the immutable Core v2 descriptor set. */
esp_err_t device_link_core_init(
    device_link_core_t *core, const device_link_core_callbacks_t *callbacks);

#ifdef UNIT_TEST_HOST
/**
 * @brief Test-only seam: the frozen request schema of a Core v2 method.
 *
 * @param[in] method_id Method id 1..7.
 * @return Schema, or NULL for an invalid method id.
 */
const device_link_tlv_schema_t *device_link_core_test_request_schema(
    uint8_t method_id);

/**
 * @brief Test-only seam: the frozen response schema of a Core v2 method.
 *
 * @param[in] method_id Method id 1..7.
 * @return Schema, or NULL for an invalid method id.
 */
const device_link_tlv_schema_t *device_link_core_test_response_schema(
    uint8_t method_id);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_CORE_H__ */
