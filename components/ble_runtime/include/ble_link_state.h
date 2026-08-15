#ifndef __BLE_LINK_STATE_H__
#define __BLE_LINK_STATE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief PublicLinkState flags, frozen by microtech.link.core.v2. */
#define BLE_LINK_STATE_FLAG_BINDABLE UINT32_C(0x01)
#define BLE_LINK_STATE_FLAG_BOUND UINT32_C(0x02)
#define BLE_LINK_STATE_FLAG_PUBLIC_DISCOVERY UINT32_C(0x04)
#define BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED UINT32_C(0x08)
#define BLE_LINK_STATE_FLAG_TRANSITIONING UINT32_C(0x10)
#define BLE_LINK_STATE_FLAG_AUTHENTICATED UINT32_C(0x20)
#define BLE_LINK_STATE_FLAG_AUTHORIZED UINT32_C(0x40)
#define BLE_LINK_STATE_FLAG_ERROR UINT32_C(0x80)
#define BLE_LINK_STATE_PROTOCOL_MAJOR 2U
#define BLE_LINK_STATE_PROTOCOL_MINOR 0U
#define BLE_LINK_STATE_PROFILE_MAJOR 2U
#define BLE_LINK_STATE_PROFILE_MINOR 0U

/** @brief Contract maximum encoded PublicLinkState size. */
#define BLE_LINK_STATE_MAX_ENCODED_BYTES 16U

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
 * public value is always exactly 16 bytes: four one-byte versions, a
 * little-endian uint32 flags word, and a little-endian uint64 boot ID.
 *
 * @param[in]  state    State to encode.
 * @param[out] out      Buffer. NULL is not a valid encoding destination.
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
