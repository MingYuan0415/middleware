#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ble_nimble_peer_identity.h"

static void test_public_identity_requires_nonzero_address(void)
{
    static const uint8_t zero[6] = {0U};
    static const uint8_t public_identity[6] =
    {
        1U, 2U, 3U, 4U, 5U, 6U,
    };

    assert(!ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_PUBLIC, zero));
    assert(!ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_PUBLIC_ID, zero));
    assert(ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_PUBLIC, public_identity));
    assert(ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_PUBLIC_ID, public_identity));
}

static void test_random_identity_requires_static_address_class(void)
{
    static const uint8_t valid_static[6] =
    {
        1U, 2U, 3U, 4U, 5U, 0xc0U,
    };
    static const uint8_t nrpa[6] =
    {
        1U, 2U, 3U, 4U, 5U, 0x00U,
    };
    static const uint8_t rpa[6] =
    {
        1U, 2U, 3U, 4U, 5U, 0x40U,
    };
    static const uint8_t reserved[6] =
    {
        1U, 2U, 3U, 4U, 5U, 0x80U,
    };

    assert(ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_RANDOM, valid_static));
    assert(ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_RANDOM_ID, valid_static));
    assert(!ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_RANDOM, nrpa));
    assert(!ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_RANDOM, rpa));
    assert(!ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_RANDOM, reserved));
}

static void test_static_random_part_rejects_uniform_limits(void)
{
    static const uint8_t all_zero_random_part[6] =
    {
        0U, 0U, 0U, 0U, 0U, 0xc0U,
    };
    static const uint8_t all_one_random_part[6] =
    {
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
    };
    static const uint8_t low_boundary[6] =
    {
        1U, 0U, 0U, 0U, 0U, 0xc0U,
    };
    static const uint8_t high_boundary[6] =
    {
        0xfeU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
    };

    assert(!ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_RANDOM, all_zero_random_part));
    assert(!ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_RANDOM, all_one_random_part));
    assert(ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_RANDOM, low_boundary));
    assert(ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_RANDOM_ID, high_boundary));
}

static void test_unknown_type_and_null_address_are_rejected(void)
{
    static const uint8_t valid_static[6] =
    {
        1U, 2U, 3U, 4U, 5U, 0xc0U,
    };

    assert(!ble_nimble_peer_identity_valid(4U, valid_static));
    assert(!ble_nimble_peer_identity_valid(
               BLE_NIMBLE_PEER_ADDR_PUBLIC, NULL));
}

int main(void)
{
    test_public_identity_requires_nonzero_address();
    test_random_identity_requires_static_address_class();
    test_static_random_part_rejects_uniform_limits();
    test_unknown_type_and_null_address_are_rejected();
    puts("ble_nimble_peer_identity: all tests passed");
    return 0;
}
