#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_gap_manager.h"
#include "ble_link_cleanup_obligation.h"

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
    ble_gap_manager_snapshot_t snapshot;

    if (event->type != BLE_GAP_MANAGER_EVENT_CONNECT &&
            event->type != BLE_GAP_MANAGER_EVENT_ADV_COMPLETE &&
            event->type != BLE_GAP_MANAGER_EVENT_RESET)
    {
        memset(&event->identity, 0, sizeof(event->identity));
        if (ble_gap_manager_get_snapshot(&snapshot) == ESP_OK &&
                snapshot.connected &&
                snapshot.conn_handle == event->conn_handle)
        {
            event->identity.generation = snapshot.generation;
            event->identity.conn_handle = event->conn_handle;
            switch (event->type)
            {
            case BLE_GAP_MANAGER_EVENT_DISCONNECT:
                event->identity.kind = BLE_LINK_OPERATION_DISCONNECT;
                break;
            case BLE_GAP_MANAGER_EVENT_MTU:
                event->identity.kind = BLE_LINK_OPERATION_MTU;
                break;
            case BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE:
                event->identity.kind = BLE_LINK_OPERATION_ENCRYPT_CHANGE;
                break;
            case BLE_GAP_MANAGER_EVENT_SUBSCRIBE:
                event->identity.kind = BLE_LINK_OPERATION_SUBSCRIBE;
                break;
            default:
                break;
            }
        }
    }
    const esp_err_t _feed_result = ble_gap_manager_handle_event(event);

    if (_feed_result != ESP_OK)
    {
        fprintf(stderr, "feed failed type=%d handle=%u result=%d\n",
                (int)event->type, event->conn_handle, _feed_result);
        abort();
    }
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

static bool _cleanup_admission_cb(void *arg)
{
    return ble_link_cleanup_admission_allowed(arg);
}

static void test_retained_cleanup_rejects_new_connection(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;
    ble_link_cleanup_state_t cleanup;
    ble_link_cleanup_request_t request;
    ble_link_cleanup_request_t due;

    memset(&request, 0, sizeof(request));
    request.identity = (ble_link_operation_identity_t)
    {
        .generation = 1U,
        .security_epoch = 2U,
        .flow_id = 3U,
        .token = 4U,
        .kind = BLE_LINK_OPERATION_PROVISIONAL_DISCARD,
        .conn_handle = 5U,
    };
    request.provisional = true;
    request.delete_all_if_unresolved = true;
    ble_link_cleanup_reset(&cleanup);
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(
                         &cleanup, &request, 0U));
    ble_gap_manager_init();
    ble_gap_manager_set_admission_cb(_cleanup_admission_cb, &cleanup);
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 7U;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      ble_gap_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.connected);
    TEST_ASSERT_EQUAL(0U, snapshot.generation);

    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&cleanup, 0U, &due));
    ble_link_cleanup_finish(&cleanup, &due, true, 0U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.connected);
    TEST_ASSERT_EQUAL(1U, snapshot.generation);
}


static bool s_disconnect_in_callback;

static bool _admission_cb_aborting(void *arg)
{
    (void)arg;
    /* The callback itself is admitting; the cancellation is performed by
     * the injected DISCONNECT while it runs outside the manager lock. */
    if (s_disconnect_in_callback)
    {
        ble_gap_manager_event_t event;

        memset(&event, 0, sizeof(event));
        event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
        event.conn_handle = 7U;
        (void)ble_gap_manager_handle_event(&event);
    }
    return true;
}

static void test_admission_cancelled_during_callback(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    s_disconnect_in_callback = true;
    ble_gap_manager_set_admission_cb(_admission_cb_aborting, NULL);
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 7U;
    /* The callback cancelled its own reservation: the connect is
     * rejected and nothing is committed. */
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      ble_gap_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.connected);

    /* A later connect on the same handle is admitted normally. */
    s_disconnect_in_callback = false;
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 7U;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.connected);
    TEST_ASSERT_EQUAL(7U, snapshot.conn_handle);
}

static void test_admission_token_exhaustion_fails_closed(void)
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    s_disconnect_in_callback = false;
    ble_gap_manager_set_admission_cb(_admission_cb_aborting, NULL);
    ble_gap_manager_test_set_admission_token(UINT32_MAX);
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_gap_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.connected);
    TEST_ASSERT_EQUAL(0U, snapshot.generation);
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
    event.notify = true;
    _feed(&event);
    event.attr_handle = 7U;
    event.subscribed = true;
    event.indicate = true;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.subscribed);

    event.attr_handle = 5U;
    event.subscribed = false;
    event.notify = false;
    event.indicate = false;
    _feed(&event);
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.subscribed);

    event.attr_handle = 7U;
    event.subscribed = false;
    event.notify = false;
    event.indicate = false;
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

static void test_same_handle_stale_generation_ignored(void)
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
    _feed(&event);

    memset(&event.identity, 0, sizeof(event.identity));
    event.type = BLE_GAP_MANAGER_EVENT_MTU;
    event.conn_handle = 5U;
    event.mtu = 498U;
    event.identity.generation = 1U;
    event.identity.kind = BLE_LINK_OPERATION_MTU;
    event.identity.conn_handle = 5U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.connected);
    TEST_ASSERT_EQUAL(2U, snapshot.generation);
    TEST_ASSERT_EQUAL(23U, snapshot.mtu);

    event.type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE;
    event.attr_handle = 9U;
    event.notify = true;
    event.identity.kind = BLE_LINK_OPERATION_SUBSCRIBE;
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&event));
    TEST_ASSERT_FALSE(ble_gap_manager_is_subscribed(5U, 9U));

    event.type = BLE_GAP_MANAGER_EVENT_DISCONNECT;
    event.identity.kind = BLE_LINK_OPERATION_DISCONNECT;
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.connected);
    TEST_ASSERT_EQUAL(2U, snapshot.generation);
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

static void test_generation_exhaustion_rejects_connection(void)
{
    ble_gap_manager_event_t event;

    ble_gap_manager_init();
    ble_gap_manager_test_set_generation(UINT32_MAX);
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
    event.conn_handle = 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_gap_manager_handle_event(&event));
}

int main(void)
{
    test_connect_disconnect_cycle();
    test_second_connection_rejected();
    test_second_connection_after_disconnect_admitted();
    test_late_callbacks_from_stale_generation_ignored();
    test_connect_failure_ignored();
    test_admission_callback_consulted();
    test_retained_cleanup_rejects_new_connection();
    test_admission_cancelled_during_callback();
    test_admission_token_exhaustion_fails_closed();
    test_invalid_arguments_rejected();
    test_multi_characteristic_subscription();
    test_events_ignored_while_disconnected();
    test_encrypt_failure_keeps_state();
    test_handle_reuse_after_reconnect();
    test_same_handle_stale_generation_ignored();
    test_reset_retires_connection();
    test_generation_exhaustion_rejects_connection();
    printf("ble_gap_manager: all tests passed\n");
    return 0;
}
