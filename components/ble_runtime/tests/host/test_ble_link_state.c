#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_state.h"

#define TEST_ASSERT_TRUE(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            fprintf(stderr, "assertion failed at line %d: %s\n", \
                    __LINE__, #condition); \
            abort(); \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL(expected, actual) \
    do \
    { \
        const long expected_value = (long)(expected); \
        const long actual_value = (long)(actual); \
        if (expected_value != actual_value) \
        { \
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%ld != %ld)\n", \
                    __LINE__, #expected, #actual, expected_value, actual_value); \
            abort(); \
        } \
    } while (0)

static void test_golden_public_link_state(void)
{
    /* Frozen fixture: protocol_major=2, profile_major=2, protocol/profile
     * minor=0, boot_id=72623859790382856, state_flags=0xaa
     * (BOUND|BLUETOOTH_ENABLED|AUTHENTICATED|ERROR, a combination that
     * satisfies the v2 cross-flag implications). */
    static const uint8_t expected[] =
    {
        0x02, 0x00, 0x02, 0x00, 0xaa, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    };
    ble_link_state_t state;

    memset(&state, 0, sizeof(state));
    state.protocol_major = 2U;
    state.profile_major = 2U;
    state.boot_id = 72623859790382856ULL;
    state.state_flags = BLE_LINK_STATE_FLAG_BOUND |
                        BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED |
                        BLE_LINK_STATE_FLAG_AUTHENTICATED |
                        BLE_LINK_STATE_FLAG_ERROR;
    uint8_t out[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    size_t out_len = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_state_encode(
                          &state, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(sizeof(expected), out_len);
    TEST_ASSERT_EQUAL(0, memcmp(out, expected, sizeof(expected)));
}

static void test_fixed_width_defaults(void)
{
    ble_link_state_t state;
    uint8_t out[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    size_t out_len = 0U;

    memset(&state, 0, sizeof(state));
    state.protocol_major = 2U;
    state.profile_major = 2U;
    state.boot_id = 42U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_state_encode(
                          &state, out, sizeof(out), &out_len));
    /* All fields are present, including zero versions and flags. */
    static const uint8_t expected[] =
    {
        0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    TEST_ASSERT_EQUAL(sizeof(expected), out_len);
    TEST_ASSERT_EQUAL(0, memcmp(out, expected, sizeof(expected)));
}

static void test_max_encoded_size_within_contract(void)
{
    ble_link_state_t state;
    uint8_t out[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    size_t out_len = 0U;

    memset(&state, 0, sizeof(state));
    state.protocol_major = 2U;
    state.profile_major = 2U;
    state.boot_id = 0xffffffffffffffffULL;
    /* Every defined flag in a combination that satisfies the v2 cross-flag
     * implications (BOUND with AUTHENTICATED/AUTHORIZED, no BINDABLE). */
    state.state_flags = BLE_LINK_STATE_FLAG_BOUND |
                        BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED |
                        BLE_LINK_STATE_FLAG_TRANSITIONING |
                        BLE_LINK_STATE_FLAG_AUTHENTICATED |
                        BLE_LINK_STATE_FLAG_AUTHORIZED |
                        BLE_LINK_STATE_FLAG_ERROR;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_state_encode(
                          &state, out, sizeof(out), &out_len));
    TEST_ASSERT_TRUE(out_len <= BLE_LINK_STATE_MAX_ENCODED_BYTES);
}

static void test_invalid_states_rejected(void)
{
    ble_link_state_t state;
    uint8_t out[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    size_t out_len = 0U;

    memset(&state, 0, sizeof(state));
    state.boot_id = 0U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, out, sizeof(out),
                          &out_len));
    memset(&state, 0, sizeof(state));
    state.protocol_major = 2U;
    state.profile_major = 2U;
    state.boot_id = 42U;
    state.protocol_major = 128U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, out, sizeof(out),
                          &out_len));
    memset(&state, 0, sizeof(state));
    state.protocol_major = 2U;
    state.profile_major = 2U;
    state.boot_id = 42U;
    state.state_flags = 0x100U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, out, sizeof(out),
                          &out_len));
    state.state_flags = BLE_LINK_STATE_FLAG_BINDABLE |
                        BLE_LINK_STATE_FLAG_PUBLIC_DISCOVERY;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, out, sizeof(out),
                          &out_len));
    /* Non-zero minors are frozen at 0. */
    memset(&state, 0, sizeof(state));
    state.protocol_major = 2U;
    state.profile_major = 2U;
    state.boot_id = 42U;
    state.protocol_minor = 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, out, sizeof(out),
                          &out_len));
    memset(&state, 0, sizeof(state));
    state.protocol_major = 2U;
    state.profile_major = 2U;
    state.boot_id = 42U;
    state.profile_minor = 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, out, sizeof(out),
                          &out_len));
    /* AUTHENTICATED implies BOUND. */
    memset(&state, 0, sizeof(state));
    state.protocol_major = 2U;
    state.profile_major = 2U;
    state.boot_id = 42U;
    state.state_flags = BLE_LINK_STATE_FLAG_AUTHENTICATED;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, out, sizeof(out),
                          &out_len));
    /* AUTHORIZED implies AUTHENTICATED and BOUND. */
    state.state_flags = BLE_LINK_STATE_FLAG_BOUND |
                        BLE_LINK_STATE_FLAG_AUTHORIZED;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, out, sizeof(out),
                          &out_len));
    /* BINDABLE excludes BOUND. */
    state.state_flags = BLE_LINK_STATE_FLAG_BINDABLE |
                        BLE_LINK_STATE_FLAG_BOUND;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, out, sizeof(out),
                          &out_len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_state_encode(&state, out, sizeof(out), NULL));
    memset(&state, 0, sizeof(state));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          NULL, out, sizeof(out),
                          &out_len));
}

static void test_small_buffer_rejected(void)
{
    ble_link_state_t state;
    uint8_t small[2];
    size_t out_len = 0U;

    memset(&state, 0, sizeof(state));
    state.protocol_major = 2U;
    state.profile_major = 2U;
    state.boot_id = 42U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, small, sizeof(small),
                          &out_len));
}

static void test_fixed_size(void)
{
    ble_link_state_t state;
    size_t out_len = 0U;

    memset(&state, 0, sizeof(state));
    state.protocol_major = 2U;
    state.profile_major = 2U;
    state.boot_id = 42U;
    uint8_t out[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_state_encode(
                          &state, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_MAX_ENCODED_BYTES, out_len);
}

int main(void)
{
    test_golden_public_link_state();
    test_fixed_width_defaults();
    test_max_encoded_size_within_contract();
    test_invalid_states_rejected();
    test_small_buffer_rejected();
    test_fixed_size();
    printf("ble_link_state: all tests passed\n");
    return 0;
}
