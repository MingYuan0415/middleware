#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_gap_manager.h"

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

#define TEST_ASSERT_FALSE(condition) \
    do \
    { \
        if ((condition)) \
        { \
            fprintf(stderr, "assertion failed at line %d: !(%s)\n", \
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

static void _feed(ble_gap_manager_event_t *event)
{
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(event));
}

static void test_connect_disconnect_cycle(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 1U;
    event.status = 0;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.connected);
    TEST_ASSERT_EQUAL(1U, snapshot.conn_handle);
    TEST_ASSERT_EQUAL(1U, snapshot.generation);
    TEST_ASSERT_EQUAL(23U, snapshot.mtu);

    event.type = BLE_GAP_MANAGER_EVENT_MTU;
    event.mtu = 185U;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(185U, snapshot.mtu);

    event.type = BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE;
    event.encrypted = true;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.encrypted);

    event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
    event.reason = 8U;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.connected);
    TEST_ASSERT_EQUAL(0U, snapshot.conn_handle);
    TEST_ASSERT_EQUAL(23U, snapshot.mtu);
    TEST_ASSERT_FALSE(snapshot.encrypted);
}

static void test_second_connection_rejected(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 1U;
    _feed(&event);

    event.conn_handle = 2U;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_gap_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(1U, snapshot.conn_handle);
    TEST_ASSERT_EQUAL(1U, snapshot.generation);
}

static void test_second_connection_after_disconnect_admitted(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 1U;
    _feed(&event);
    event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
    _feed(&event);

    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 3U;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(3U, snapshot.conn_handle);
    TEST_ASSERT_EQUAL(2U, snapshot.generation);
}

static void test_late_callbacks_from_stale_generation_ignored(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 1U;
    _feed(&event);
    event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
    _feed(&event);

    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 5U;
    _feed(&event);

    event.type = BLE_GAP_MANAGER_EVENT_MTU;
    event.conn_handle = 1U;
    event.mtu = 500U;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(5U, snapshot.conn_handle);
    TEST_ASSERT_EQUAL(23U, snapshot.mtu);
}

static void test_connect_failure_ignored(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 1U;
    event.status = 34;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.connected);
    TEST_ASSERT_EQUAL(0U, snapshot.generation);
}

static bool s_admit_result;
static unsigned int s_admit_calls;

static bool _admission_cb(void *arg)
{
    (void)arg;
    s_admit_calls++;
    return s_admit_result;
}

static void test_admission_callback_consulted(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    s_admit_calls = 0U;
    s_admit_result = false;
    ble_gap_manager_set_admission_cb(_admission_cb, NULL);
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      ble_gap_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(1U, s_admit_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.connected);
    TEST_ASSERT_EQUAL(0U, snapshot.generation);

    s_admit_result = true;
    event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
    _feed(&event);
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 2U;
    _feed(&event);
    TEST_ASSERT_EQUAL(2U, s_admit_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.connected);
    TEST_ASSERT_EQUAL(2U, snapshot.conn_handle);
}

static void test_invalid_arguments_rejected(void)
{
    ble_gap_manager_init();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_gap_manager_handle_event(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_gap_manager_get_snapshot(NULL));
    ble_gap_manager_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = (ble_gap_manager_event_type_t)99;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_gap_manager_handle_event(&event));
}

static void test_multi_characteristic_subscription(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 1U;
    _feed(&event);

    event.type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE;
    event.attr_handle = 5U;
    event.subscribed = true;
    _feed(&event);
    event.attr_handle = 7U;
    event.subscribed = true;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.subscribed);

    event.attr_handle = 5U;
    event.subscribed = false;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.subscribed);

    event.attr_handle = 7U;
    event.subscribed = false;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.subscribed);
}

static void test_events_ignored_while_disconnected(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_MTU;
    event.conn_handle = 1U;
    event.mtu = 500U;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(23U, snapshot.mtu);

    event.type = BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE;
    event.encrypted = true;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.encrypted);
}

static void test_encrypt_failure_keeps_state(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 1U;
    _feed(&event);
    event.type = BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE;
    event.encrypted = false;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.encrypted);
    event.encrypted = true;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.encrypted);
}

static void test_handle_reuse_after_reconnect(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 5U;
    _feed(&event);
    event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
    _feed(&event);

    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 5U;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.connected);
    TEST_ASSERT_EQUAL(2U, snapshot.generation);
    TEST_ASSERT_EQUAL(23U, snapshot.mtu);

    event.type = BLE_GAP_MANAGER_EVENT_MTU;
    event.mtu = 185U;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(185U, snapshot.mtu);

    event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.connected);
}

static void test_reset_retires_connection(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 5U;
    _feed(&event);
    event.type = BLE_GAP_MANAGER_EVENT_MTU;
    event.mtu = 185U;
    _feed(&event);
    event.type = BLE_GAP_MANAGER_EVENT_RESET;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.connected);
    TEST_ASSERT_EQUAL(0U, snapshot.conn_handle);
    TEST_ASSERT_EQUAL(23U, snapshot.mtu);
    TEST_ASSERT_EQUAL(1U, snapshot.generation);

    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 6U;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.connected);
    TEST_ASSERT_EQUAL(2U, snapshot.generation);
}

int main(void)
{
    test_connect_disconnect_cycle();
    test_second_connection_rejected();
    test_second_connection_after_disconnect_admitted();
    test_late_callbacks_from_stale_generation_ignored();
    test_connect_failure_ignored();
    test_admission_callback_consulted();
    test_invalid_arguments_rejected();
    test_multi_characteristic_subscription();
    test_events_ignored_while_disconnected();
    test_encrypt_failure_keeps_state();
    test_handle_reuse_after_reconnect();
    test_reset_retires_connection();
    printf("ble_gap_manager: all tests passed\n");
    return 0;
}
