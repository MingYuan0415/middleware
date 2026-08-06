#ifndef __BLE_LINK_STATE_H__
#define __BLE_LINK_STATE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief PublicLinkState flags, frozen by microtech.link.v1. */
#define BLE_LINK_STATE_FLAG_BINDABLE 1U
#define BLE_LINK_STATE_FLAG_BOUND 2U

/** @brief Contract maximum encoded PublicLinkState size. */
#define BLE_LINK_STATE_MAX_ENCODED_BYTES 20U

/** @brief PublicLinkState fields to encode. */
typedef struct ble_link_state
{
    uint32_t protocol_major;
    uint32_t protocol_minor;
    uint32_t profile_major;
    uint32_t profile_minor;
    uint64_t boot_id;
    uint32_t state_flags;
} ble_link_state_t;

/**
 * @brief Encode PublicLinkState as the raw link_state characteristic value.
 *
 * The value carries no framing header and no Security 2 protection. The
 * contract value domain is enforced: version fields must fit a single-byte
 * varint (0-127), boot_id must be nonzero, and state_flags must only use the
 * two defined low bits.
 *
 * @param[in]  state    State to encode.
 * @param[out] out      Buffer, or NULL to query the size.
 * @param[in]  capacity Buffer capacity.
 * @param[out] out_len  Bytes written or required.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM when the buffer is
 *         too small.
 */
esp_err_t ble_link_state_encode(
    const ble_link_state_t *state, uint8_t *out, size_t capacity,
    size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_STATE_H__ */
