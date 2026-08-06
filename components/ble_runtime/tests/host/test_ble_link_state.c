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
    /* Frozen fixture: protocol_major=1, profile_major=1,
     * boot_id=72623859790382856, state_flags=1. */
    static const uint8_t expected[] =
    {
        0x08, 0x01, 0x18, 0x01, 0x29, 0x08, 0x07, 0x06,
        0x05, 0x04, 0x03, 0x02, 0x01, 0x30, 0x01,
    };
    ble_link_state_t state;

    memset(&state, 0, sizeof(state));
    state.protocol_major = 1U;
    state.profile_major = 1U;
    state.boot_id = 72623859790382856ULL;
    state.state_flags = BLE_LINK_STATE_FLAG_BINDABLE;
    uint8_t out[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    size_t out_len = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_state_encode(
                          &state, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(sizeof(expected), out_len);
    TEST_ASSERT_EQUAL(0, memcmp(out, expected, sizeof(expected)));
}

static void test_defaults_omitted(void)
{
    ble_link_state_t state;
    uint8_t out[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    size_t out_len = 0U;

    memset(&state, 0, sizeof(state));
    state.boot_id = 42U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_state_encode(
                          &state, out, sizeof(out), &out_len));
    /* Only the fixed64 boot_id field. */
    static const uint8_t expected[] =
    {
        0x29, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
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
    state.protocol_major = 127U;
    state.protocol_minor = 127U;
    state.profile_major = 127U;
    state.profile_minor = 127U;
    state.boot_id = 0xffffffffffffffffULL;
    state.state_flags = BLE_LINK_STATE_FLAG_BINDABLE | BLE_LINK_STATE_FLAG_BOUND;
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
    state.boot_id = 42U;
    state.protocol_major = 128U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_state_encode(
                          &state, out, sizeof(out),
                          &out_len));
    memset(&state, 0, sizeof(state));
    state.boot_id = 42U;
    state.state_flags = 4U;
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
    state.boot_id = 42U;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_link_state_encode(
                          &state, small, sizeof(small),
                          &out_len));
}

static void test_size_query(void)
{
    ble_link_state_t state;
    size_t out_len = 0U;

    memset(&state, 0, sizeof(state));
    state.boot_id = 42U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_state_encode(
                          &state, NULL, 0U, &out_len));
    TEST_ASSERT_EQUAL(9U, out_len);
}

int main(void)
{
    test_golden_public_link_state();
    test_defaults_omitted();
    test_max_encoded_size_within_contract();
    test_invalid_states_rejected();
    test_small_buffer_rejected();
    test_size_query();
    printf("ble_link_state: all tests passed\n");
    return 0;
}
