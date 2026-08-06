#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_port_ops.h"

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

static unsigned int s_callback_a_calls;
static unsigned int s_callback_b_calls;
static ble_port_event_type_t s_last_type_a;
static ble_port_event_type_t s_last_type_b;
static uint16_t s_last_handle_a;
static unsigned int s_order[8];
static unsigned int s_order_count;

static void _callback_a(const ble_port_event_t *event, void *arg)
{
    (void)arg;
    s_callback_a_calls++;
    s_last_type_a = event->type;
    s_last_handle_a = event->conn_handle;
    s_order[s_order_count++] = 1U;
}

static void _callback_b(const ble_port_event_t *event, void *arg)
{
    (void)arg;
    s_callback_b_calls++;
    s_last_type_b = event->type;
    s_order[s_order_count++] = 2U;
}

static void _callback_c(const ble_port_event_t *event, void *arg)
{
    (void)event;
    (void)arg;
    s_order[s_order_count++] = 3U;
}

static void _reset_order(void)
{
    s_order_count = 0U;
    memset(s_order, 0, sizeof(s_order));
}

static void test_dispatch_fans_out_in_order(void)
{
    ble_port_event_t event;

    _reset_order();
    ble_event_router_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_b, NULL));
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_MTU;
    event.conn_handle = 7U;
    event.mtu = 185U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
    TEST_ASSERT_EQUAL(1U, s_callback_a_calls);
    TEST_ASSERT_EQUAL(1U, s_callback_b_calls);
    TEST_ASSERT_EQUAL(BLE_PORT_EVENT_MTU, s_last_type_a);
    TEST_ASSERT_EQUAL(BLE_PORT_EVENT_MTU, s_last_type_b);
    TEST_ASSERT_EQUAL(7U, s_last_handle_a);
    TEST_ASSERT_EQUAL(2U, s_order_count);
    TEST_ASSERT_EQUAL(1U, s_order[0]);
    TEST_ASSERT_EQUAL(2U, s_order[1]);
}

static void test_unregister_preserves_order(void)
{
    ble_port_event_t event;

    _reset_order();
    ble_event_router_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_b, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_c, NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_event_router_unregister(_callback_b, NULL));
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_COMPLETE;
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
    TEST_ASSERT_EQUAL(2U, s_order_count);
    TEST_ASSERT_EQUAL(1U, s_order[0]);
    TEST_ASSERT_EQUAL(3U, s_order[1]);
    _reset_order();
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_unregister(_callback_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
    TEST_ASSERT_EQUAL(1U, s_order_count);
    TEST_ASSERT_EQUAL(3U, s_order[0]);
}

static void test_unregister_stops_delivery(void)
{
    ble_port_event_t event;

    s_callback_a_calls = 0U;
    s_callback_b_calls = 0U;
    ble_event_router_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_b, NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_event_router_unregister(_callback_a, NULL));
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_DISCONNECT;
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
    TEST_ASSERT_EQUAL(0U, s_callback_a_calls);
    TEST_ASSERT_EQUAL(1U, s_callback_b_calls);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_event_router_unregister(_callback_a, NULL));
}

static void test_duplicate_registration_idempotent(void)
{
    ble_event_router_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_unregister(_callback_a, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_event_router_unregister(_callback_a, NULL));
}

static void test_consumer_limit_enforced(void)
{
    ble_event_router_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_b, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_a, (void *)1));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_b, (void *)1));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      ble_event_router_register(_callback_a, (void *)2));
}

static void test_dispatch_snapshot_is_stable(void)
{
    ble_port_event_t event;

    _reset_order();
    ble_event_router_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_b, NULL));
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_CONNECT;
    event.conn_handle = 3U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
    TEST_ASSERT_EQUAL(2U, s_order_count);
    TEST_ASSERT_EQUAL(1U, s_order[0]);
    TEST_ASSERT_EQUAL(2U, s_order[1]);
    _reset_order();
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_event_router_unregister(_callback_a, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
    TEST_ASSERT_EQUAL(1U, s_order_count);
    TEST_ASSERT_EQUAL(2U, s_order[0]);
}

static void _callback_self_unregister(const ble_port_event_t *event, void *arg)
{
    (void)event;
    (void)arg;
    s_order[s_order_count++] = 4U;
    (void)ble_event_router_unregister(_callback_self_unregister, NULL);
}

static void _callback_register_late(const ble_port_event_t *event, void *arg)
{
    (void)event;
    (void)arg;
    s_order[s_order_count++] = 5U;
    (void)ble_event_router_register(_callback_c, NULL);
}

static void test_dispatch_mutation_during_callback(void)
{
    ble_port_event_t event;

    _reset_order();
    ble_event_router_init();
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_event_router_register(_callback_self_unregister, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_callback_b, NULL));
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_CONNECT;
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
    TEST_ASSERT_EQUAL(2U, s_order_count);
    TEST_ASSERT_EQUAL(4U, s_order[0]);
    TEST_ASSERT_EQUAL(2U, s_order[1]);
    _reset_order();
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_event_router_register(_callback_register_late, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
    TEST_ASSERT_EQUAL(2U, s_order_count);
    TEST_ASSERT_EQUAL(2U, s_order[0]);
    TEST_ASSERT_EQUAL(5U, s_order[1]);
    _reset_order();
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
    TEST_ASSERT_EQUAL(3U, s_order_count);
    TEST_ASSERT_EQUAL(2U, s_order[0]);
    TEST_ASSERT_EQUAL(5U, s_order[1]);
    TEST_ASSERT_EQUAL(3U, s_order[2]);
}

static void test_invalid_arguments_rejected(void)
{
    ble_port_event_t event;

    ble_event_router_init();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_event_router_register(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_event_router_unregister(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_event_router_dispatch(NULL));
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_CONNECT;
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
}

int main(void)
{
    test_dispatch_fans_out_in_order();
    test_unregister_preserves_order();
    test_unregister_stops_delivery();
    test_duplicate_registration_idempotent();
    test_consumer_limit_enforced();
    test_dispatch_snapshot_is_stable();
    test_dispatch_mutation_during_callback();
    test_invalid_arguments_rejected();
    printf("ble_event_router: all tests passed\n");
    return 0;
}
