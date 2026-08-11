#ifndef __BLE_NIMBLE_PEER_IDENTITY_H__
#define __BLE_NIMBLE_PEER_IDENTITY_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_NIMBLE_PEER_ADDR_PUBLIC 0U
#define BLE_NIMBLE_PEER_ADDR_RANDOM 1U
#define BLE_NIMBLE_PEER_ADDR_PUBLIC_ID 2U
#define BLE_NIMBLE_PEER_ADDR_RANDOM_ID 3U

/**
 * @brief Check whether an address is a normalized BLE peer identity.
 *
 * Public identities must be nonzero. Random identities must use a valid
 * static-random address: the two most significant bits are one and the
 * remaining 46 bits are neither all zero nor all one.
 *
 * @param[in] address_type BLE public, random, public-ID, or random-ID type.
 * @param[in] address Six-byte Bluetooth address in little-endian order.
 * @return True only for a normalized public or static-random identity.
 */
bool ble_nimble_peer_identity_valid(
    uint8_t address_type, const uint8_t address[6]);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NIMBLE_PEER_IDENTITY_H__ */
