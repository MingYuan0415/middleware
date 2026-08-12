#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_events.h"

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
        const long long expected_value = (long long)(expected); \
        const long long actual_value = (long long)(actual); \
        if (expected_value != actual_value) \
        { \
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%lld != %lld)\n", \
                    __LINE__, #expected, #actual, expected_value, actual_value); \
            abort(); \
        } \
    } while (0)

static void test_golden_link_snapshot(void)
{
    /* Frozen fixture: event_sequence=7, boot_id=72623859790382856,
     * binding=BOUND(3), authorization=AUTHORIZED(5), encrypted/bond/
     * identity=true. */
    static const uint8_t expected[] =
    {
        0x09, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x12, 0x13,
        0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x10, 0x03, 0x18, 0x05, 0x20, 0x01, 0x28, 0x01, 0x30, 0x01,
    };
    /* Fixture hex: 090700000000000000121309080706050403020110031805200128013001 */
    ble_link_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.event_sequence = 7U;
    snapshot.link_state.boot_id = 72623859790382856ULL;
    snapshot.link_state.binding_state = BLE_LINK_BINDING_BOUND;
    snapshot.link_state.authorization_state =
        BLE_LINK_AUTHORIZATION_AUTHORIZED;
    snapshot.link_state.encrypted = true;
    snapshot.link_state.secure_connections_bond_verified = true;
    snapshot.link_state.identity_known = true;
    uint8_t out[BLE_LINK_EVENTS_SNAPSHOT_MAX_BYTES];
    size_t out_len = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_snapshot_encode(
                          &snapshot, out, sizeof(out), &out_len));
    TEST_ASSERT_EQUAL(sizeof(expected), out_len);
    TEST_ASSERT_EQUAL(0, memcmp(out, expected, sizeof(expected)));
}

static void test_snapshot_defaults_omitted(void)
{
    ble_link_snapshot_t snapshot;
    uint8_t out[BLE_LINK_EVENTS_SNAPSHOT_MAX_BYTES];
    size_t out_len = 0U;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.event_sequence = 1U;
    snapshot.link_state.boot_id = 42U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_snapshot_encode(
                          &snapshot, out, sizeof(out), &out_len));
    /* Only boot_id present. */
    static const uint8_t expected[] =
    {
        0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x12, 0x09, 0x09, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    TEST_ASSERT_EQUAL(sizeof(expected), out_len);
    TEST_ASSERT_EQUAL(0, memcmp(out, expected, sizeof(expected)));
}

static void test_snapshot_invalid_rejected(void)
{
    ble_link_snapshot_t snapshot;
    uint8_t out[BLE_LINK_EVENTS_SNAPSHOT_MAX_BYTES];
    size_t out_len = 0U;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.event_sequence = 1U;
    snapshot.link_state.boot_id = 42U;
    snapshot.link_state.binding_state = (ble_link_binding_state_t)4U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_snapshot_encode(
                          &snapshot, out, sizeof(out),
                          &out_len));
    snapshot.link_state.binding_state = BLE_LINK_BINDING_UNBOUND;
    snapshot.link_state.authorization_state =
        (ble_link_authorization_state_t)6U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_snapshot_encode(
                          &snapshot, out, sizeof(out),
                          &out_len));
    snapshot.link_state.authorization_state =
        BLE_LINK_AUTHORIZATION_UNAUTHORIZED;
    snapshot.link_state.boot_id = 0U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_snapshot_encode(
                          &snapshot, out, sizeof(out),
                          &out_len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_snapshot_encode(&snapshot, out, sizeof(out),
                              NULL));
}

static void test_snapshot_size_query_and_small_buffer(void)
{
    ble_link_snapshot_t snapshot;
    size_t out_len = 0U;
    uint8_t small[4];

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.event_sequence = 1U;
    snapshot.link_state.boot_id = 42U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_snapshot_encode(
                          &snapshot, NULL, 0U, &out_len));
    TEST_ASSERT_EQUAL(20U, out_len);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_link_snapshot_encode(
                          &snapshot, small, sizeof(small),
                          &out_len));
}

static void test_sequence_monotonic_and_baseline(void)
{
    ble_link_events_init();
    TEST_ASSERT_EQUAL(1U, ble_link_events_baseline());
    TEST_ASSERT_TRUE(!ble_link_events_exhausted());
    TEST_ASSERT_EQUAL(2U, ble_link_events_next());
    TEST_ASSERT_EQUAL(3U, ble_link_events_next());
    TEST_ASSERT_EQUAL(3U, ble_link_events_baseline());
    TEST_ASSERT_EQUAL(4U, ble_link_events_next());
    /* A new boot resets the sequence. */
    ble_link_events_reset();
    TEST_ASSERT_EQUAL(1U, ble_link_events_baseline());
    TEST_ASSERT_EQUAL(2U, ble_link_events_next());
}

static void test_sequence_exhaustion_boundary(void)
{
    ble_link_events_init();
    ble_link_events_test_set_sequence(UINT64_MAX - 1U);
    TEST_ASSERT_EQUAL(UINT64_MAX, ble_link_events_next());
    TEST_ASSERT_TRUE(ble_link_events_exhausted());
    TEST_ASSERT_EQUAL(UINT64_MAX, ble_link_events_baseline());
    /* Reaching the maximum stops publication for the rest of the boot. */
    TEST_ASSERT_EQUAL(0U, ble_link_events_next());
    TEST_ASSERT_EQUAL(0U, ble_link_events_next());
    TEST_ASSERT_TRUE(ble_link_events_exhausted());
    TEST_ASSERT_EQUAL(UINT64_MAX, ble_link_events_baseline());
}

static void test_snapshot_zero_sequence_rejected(void)
{
    ble_link_snapshot_t snapshot;
    uint8_t out[BLE_LINK_EVENTS_SNAPSHOT_MAX_BYTES];
    size_t out_len = 0U;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.event_sequence = 0U;
    snapshot.link_state.boot_id = 42U;
    ble_link_events_test_set_sequence(0U);
    TEST_ASSERT_EQUAL(0U, ble_link_events_next());
    TEST_ASSERT_EQUAL(0U, ble_link_events_baseline());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_snapshot_encode(
                          &snapshot, out, sizeof(out), &out_len));
}

static void test_snapshot_negative_enum_rejected(void)
{
    ble_link_snapshot_t snapshot;
    uint8_t out[BLE_LINK_EVENTS_SNAPSHOT_MAX_BYTES];
    size_t out_len = 0U;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.event_sequence = 1U;
    snapshot.link_state.boot_id = 42U;
    snapshot.link_state.binding_state = (ble_link_binding_state_t) -1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_snapshot_encode(
                          &snapshot, out, sizeof(out),
                          &out_len));
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.event_sequence = 1U;
    snapshot.link_state.boot_id = 42U;
    snapshot.link_state.authorization_state =
        (ble_link_authorization_state_t) -1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_snapshot_encode(
                          &snapshot, out, sizeof(out),
                          &out_len));
}

static void test_sequence_large_loop(void)
{
    ble_link_events_init();
    uint64_t expected = 2U;

    for (uint64_t i = 0U; i < 1000000U; ++i)
    {
        TEST_ASSERT_EQUAL(expected, ble_link_events_next());
        expected++;
    }
    TEST_ASSERT_EQUAL(1000001U, ble_link_events_baseline());
    TEST_ASSERT_TRUE(!ble_link_events_exhausted());
}

int main(void)
{
    test_golden_link_snapshot();
    test_snapshot_defaults_omitted();
    test_snapshot_invalid_rejected();
    test_snapshot_size_query_and_small_buffer();
    test_sequence_monotonic_and_baseline();
    test_sequence_large_loop();
    test_sequence_exhaustion_boundary();
    test_snapshot_zero_sequence_rejected();
    test_snapshot_negative_enum_rejected();
    printf("ble_link_events: all tests passed\n");
    return 0;
}
