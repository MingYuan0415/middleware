#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_nimble_peer_identity.h"

#define BLE_NIMBLE_PEER_ADDRESS_BYTES 6U
#define BLE_NIMBLE_STATIC_RANDOM_MASK 0xc0U
#define BLE_NIMBLE_RPA_RANDOM_BITS 0x40U
#define BLE_NIMBLE_RANDOM_PART_MASK 0x3fU

bool ble_nimble_peer_identity_valid(
    uint8_t address_type, const uint8_t address[6])
{
    if (address == NULL)
    {
        return false;
    }
    bool nonzero = false;

    for (size_t i = 0U; i < BLE_NIMBLE_PEER_ADDRESS_BYTES; ++i)
    {
        nonzero = nonzero || address[i] != 0U;
    }
    if (address_type == BLE_NIMBLE_PEER_ADDR_PUBLIC ||
            address_type == BLE_NIMBLE_PEER_ADDR_PUBLIC_ID)
    {
        return nonzero;
    }
    if (address_type != BLE_NIMBLE_PEER_ADDR_RANDOM &&
            address_type != BLE_NIMBLE_PEER_ADDR_RANDOM_ID)
    {
        return false;
    }
    if ((address[5] & BLE_NIMBLE_STATIC_RANDOM_MASK) !=
            BLE_NIMBLE_STATIC_RANDOM_MASK)
    {
        return false;
    }
    bool random_part_nonzero =
        (address[5] & BLE_NIMBLE_RANDOM_PART_MASK) != 0U;
    bool random_part_not_all_one =
        (address[5] & BLE_NIMBLE_RANDOM_PART_MASK) !=
        BLE_NIMBLE_RANDOM_PART_MASK;

    for (size_t i = 0U; i < BLE_NIMBLE_PEER_ADDRESS_BYTES - 1U; ++i)
    {
        random_part_nonzero = random_part_nonzero || address[i] != 0U;
        random_part_not_all_one = random_part_not_all_one ||
                                  address[i] != UINT8_MAX;
    }
    return random_part_nonzero && random_part_not_all_one;
}

bool ble_nimble_peer_rpa_reference_valid(
    uint8_t address_type, const uint8_t address[6])
{
    if (address == NULL)
    {
        return false;
    }
    bool nonzero = false;

    for (size_t i = 0U; i < BLE_NIMBLE_PEER_ADDRESS_BYTES; ++i)
    {
        nonzero = nonzero || address[i] != 0U;
    }
    if (!nonzero)
    {
        return address_type == BLE_NIMBLE_PEER_ADDR_PUBLIC;
    }
    return address_type == BLE_NIMBLE_PEER_ADDR_RANDOM &&
           (address[5] & BLE_NIMBLE_STATIC_RANDOM_MASK) ==
           BLE_NIMBLE_RPA_RANDOM_BITS;
}
