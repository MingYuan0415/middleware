#ifndef __DEVICE_LINK_WIRE_H__
#define __DEVICE_LINK_WIRE_H__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "device_link_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t device_link_wire_encode_header(
    const device_link_wire_header_t *header,
    uint8_t out[DEVICE_LINK_WIRE_HEADER_BYTES]);

esp_err_t device_link_wire_decode_header(
    const uint8_t *data, size_t len, device_link_wire_header_t *header);

esp_err_t device_link_wire_encode_status(
    device_link_status_t status,
    uint8_t out[DEVICE_LINK_RESPONSE_STATUS_BYTES]);

esp_err_t device_link_wire_decode_status(
    const uint8_t *data, size_t len, device_link_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_WIRE_H__ */
