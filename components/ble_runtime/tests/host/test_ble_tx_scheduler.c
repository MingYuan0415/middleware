#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_port_ops.h"
#include "ble_tx_scheduler.h"

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

static unsigned int s_notify_calls;
static unsigned int s_indicate_calls;
static unsigned int s_completions;
static esp_err_t s_last_completion_status;
static uint16_t s_last_completion_attr;
static unsigned int s_reset_completions;
static unsigned int s_notify_data_len;
static unsigned int s_indicate_data_len;
static bool s_indicate_active;

static unsigned int s_lock_depth;
static unsigned int s_max_lock_depth;
static bool s_in_ops;
static unsigned int s_completion_in_ops_violations;
static esp_err_t s_completion_sequence[16];
static unsigned int s_completion_sequence_count;

static void _fake_lock(void *arg)
{
    (void)arg;
    s_lock_depth++;
    if (s_lock_depth > s_max_lock_depth)
    {
        s_max_lock_depth = s_lock_depth;
    }
}

static void _fake_unlock(void *arg)
{
    (void)arg;
    s_lock_depth--;
}

static void _fire_sync_notify_tx(uint16_t conn_handle, uint16_t attr_handle,
                                 bool indication)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_NOTIFY_TX;
    event.conn_handle = conn_handle;
    event.attr_handle = attr_handle;
    event.indication = indication;
    event.tx_result = BLE_PORT_TX_SENT;
    (void)ble_tx_scheduler_handle_notify_tx(&event);
}

static esp_err_t _fake_notify(uint16_t conn_handle, uint16_t value_handle,
                              const uint8_t *data, size_t len)
{
    (void)data;
    s_notify_calls++;
    s_notify_data_len = (unsigned int)len;
    s_in_ops = true;
    _fire_sync_notify_tx(conn_handle, value_handle, false);
    s_in_ops = false;
    return ESP_OK;
}

static esp_err_t _fake_indicate(uint16_t conn_handle, uint16_t value_handle,
                                const uint8_t *data, size_t len)
{
    (void)data;
    s_indicate_calls++;
    s_indicate_data_len = (unsigned int)len;
    s_indicate_active = true;
    s_in_ops = true;
    _fire_sync_notify_tx(conn_handle, value_handle, true);
    s_in_ops = false;
    return ESP_OK;
}

static esp_err_t s_fake_notify_result;

static esp_err_t _fake_notify_failing(uint16_t conn_handle,
                                      uint16_t value_handle,
                                      const uint8_t *data, size_t len)
{
    (void)conn_handle;
    (void)value_handle;
    (void)data;
    (void)len;
    s_notify_calls++;
    s_in_ops = true;
    s_in_ops = false;
    return s_fake_notify_result;
}

static const ble_port_ops_t s_fake_ops =
{
    .adv_start = NULL,
    .adv_stop = NULL,
    .notify = _fake_notify,
    .indicate = _fake_indicate,
};

static const ble_port_ops_t s_failing_ops =
{
    .adv_start = NULL,
    .adv_stop = NULL,
    .notify = _fake_notify_failing,
    .indicate = _fake_indicate,
};

static const ble_port_ops_t *s_active_ops = &s_fake_ops;

static void _completion(const ble_tx_scheduler_result_t *result, void *arg)
{
    (void)arg;
    if (s_in_ops || s_lock_depth != 0U)
    {
        s_completion_in_ops_violations++;
    }
    s_completions++;
    s_last_completion_status = result->status;
    s_last_completion_attr = result->value_handle;
    if (s_completion_sequence_count <
            sizeof(s_completion_sequence) / sizeof(s_completion_sequence[0]))
    {
        s_completion_sequence[s_completion_sequence_count++] = result->status;
    }
    if (result->status == ESP_ERR_INVALID_STATE)
    {
        s_reset_completions++;
    }
    s_indicate_active = false;
}

static void _emit_notify_tx(uint16_t conn_handle, uint16_t attr_handle,
                            bool indication, int tx_result)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_NOTIFY_TX;
    event.conn_handle = conn_handle;
    event.attr_handle = attr_handle;
    event.indication = indication;
    event.tx_result = tx_result;
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_handle_notify_tx(&event));
}

static void _init_scheduler(size_t depth)
{
    static ble_tx_scheduler_config_t config =
    {
        .queue_depth = 0U,
        .max_frame_bytes = 100U,
        .ops = &s_fake_ops,
        .completed = _completion,
        .completed_arg = NULL,
        .lock = NULL,
        .unlock = NULL,
        .lock_arg = NULL,
    };

    ble_tx_scheduler_deinit();
    s_notify_calls = 0U;
    s_indicate_calls = 0U;
    s_completions = 0U;
    s_reset_completions = 0U;
    s_notify_data_len = 0U;
    s_indicate_data_len = 0U;
    s_indicate_active = false;
    s_lock_depth = 0U;
    s_max_lock_depth = 0U;
    s_in_ops = false;
    s_completion_in_ops_violations = 0U;
    s_completion_sequence_count = 0U;
    s_fake_notify_result = ESP_OK;
    s_active_ops = &s_fake_ops;
    config.queue_depth = depth;
    config.ops = s_active_ops;
    config.lock = _fake_lock;
    config.unlock = _fake_unlock;
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_init(&config));
}

static void test_idle_submit_sends_immediately(void)
{
    const uint8_t payload[4] = {0x01, 0x02, 0x03, 0x04};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(1U, s_notify_calls);
    TEST_ASSERT_EQUAL(0U, s_indicate_calls);
    TEST_ASSERT_EQUAL(4U, s_notify_data_len);
    /* Without a synchronous event the frame completes locally. */
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_one_in_flight_serializes(void)
{
    const uint8_t payload[2] = {0xaa, 0xbb};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    /* The synchronous NOTIFY_TX completes each frame in order. */
    TEST_ASSERT_EQUAL(3U, s_notify_calls);
    TEST_ASSERT_EQUAL(3U, s_completions);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_indicate_waits_for_confirmation(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(1U, s_indicate_calls);
    TEST_ASSERT_TRUE(s_indicate_active);
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_EQUAL(ESP_OK, s_last_completion_status);
    TEST_ASSERT_FALSE(s_indicate_active);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_indicate_timeout_completes_with_timeout(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload)));
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_TIMEOUT);
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_last_completion_status);
}

static void test_unrelated_notify_tx_ignored(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(0U, s_completions);
    _emit_notify_tx(8U, 9U, true, BLE_PORT_TX_CONFIRMED);
    _emit_notify_tx(7U, 10U, true, BLE_PORT_TX_CONFIRMED);
    _emit_notify_tx(7U, 9U, false, BLE_PORT_TX_SENT);
    TEST_ASSERT_EQUAL(0U, s_completions);
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_EQUAL(ESP_OK, s_last_completion_status);
}

static void test_queue_full_reports_no_mem(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(2U);
    /* An in-flight indication holds the queue so notifications queue up. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U,
                          9U, payload, sizeof(payload)));
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    /* The two queued notifications drained synchronously. */
    TEST_ASSERT_EQUAL(2U, s_notify_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(3U, s_notify_calls);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_reset_drops_queue_and_completes_in_flight(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    ble_tx_scheduler_reset();
    TEST_ASSERT_EQUAL(1U, s_reset_completions);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, s_last_completion_status);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
    TEST_ASSERT_EQUAL(0U, s_notify_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(1U, s_notify_calls);
}

static void test_sync_notify_tx_reentrant(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(2U, s_notify_calls);
    TEST_ASSERT_EQUAL(2U, s_completions);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
    TEST_ASSERT_EQUAL(1U, s_max_lock_depth);
    TEST_ASSERT_EQUAL(0U, s_completion_in_ops_violations);
    TEST_ASSERT_EQUAL(2U, s_completion_sequence_count);
    TEST_ASSERT_EQUAL(ESP_OK, s_completion_sequence[0]);
    TEST_ASSERT_EQUAL(ESP_OK, s_completion_sequence[1]);
}

static void test_sync_indicate_sent_keeps_in_flight(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(1U, s_indicate_calls);
    TEST_ASSERT_EQUAL(0U, s_completions);
    TEST_ASSERT_TRUE(ble_tx_scheduler_is_busy());
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_EQUAL(ESP_OK, s_last_completion_status);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_frame_slot_reused_with_larger_payload(void)
{
    const uint8_t small[1] = {0x01};
    uint8_t large[60];

    memset(large, 0xab, sizeof(large));
    _init_scheduler(1U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          small, sizeof(small)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          large, sizeof(large)));
    TEST_ASSERT_EQUAL(60U, s_notify_data_len);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_consecutive_failures_all_complete(void)
{
    static ble_tx_scheduler_config_t config;
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    s_fake_notify_result = ESP_FAIL;
    s_active_ops = &s_failing_ops;
    config.queue_depth = 4U;
    config.max_frame_bytes = 100U;
    config.ops = s_active_ops;
    config.completed = _completion;
    config.completed_arg = NULL;
    config.lock = _fake_lock;
    config.unlock = _fake_unlock;
    config.lock_arg = NULL;
    ble_tx_scheduler_deinit();
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(2U, s_completions);
    TEST_ASSERT_EQUAL(ESP_FAIL, s_last_completion_status);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_queued_failures_all_complete_after_confirm(void)
{
    static ble_tx_scheduler_config_t config;
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    s_fake_notify_result = ESP_FAIL;
    s_active_ops = &s_failing_ops;
    config.queue_depth = 4U;
    config.max_frame_bytes = 100U;
    config.ops = &s_failing_ops;
    config.completed = _completion;
    config.completed_arg = NULL;
    config.lock = _fake_lock;
    config.unlock = _fake_unlock;
    config.lock_arg = NULL;
    ble_tx_scheduler_deinit();
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(0U, s_completions);
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    TEST_ASSERT_EQUAL(3U, s_completions);
    TEST_ASSERT_EQUAL(ESP_FAIL, s_last_completion_status);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
    TEST_ASSERT_EQUAL(0U, s_completion_in_ops_violations);
    TEST_ASSERT_EQUAL(3U, s_completion_sequence_count);
    TEST_ASSERT_EQUAL(ESP_OK, s_completion_sequence[0]);
    TEST_ASSERT_EQUAL(ESP_FAIL, s_completion_sequence[1]);
    TEST_ASSERT_EQUAL(ESP_FAIL, s_completion_sequence[2]);
}

static void test_invalid_arguments_rejected(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_tx_scheduler_submit(
                          (ble_tx_scheduler_kind_t)99U,
                          7U, 9U, payload,
                          sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, NULL, 2U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, payload, 0U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, payload, 101U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_tx_scheduler_handle_notify_tx(NULL));
    ble_tx_scheduler_deinit();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, payload,
                          sizeof(payload)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_tx_scheduler_handle_notify_tx(&(ble_port_event_t)
    {
        .type = BLE_PORT_EVENT_NOTIFY_TX
    }));
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

int main(void)
{
    test_idle_submit_sends_immediately();
    test_one_in_flight_serializes();
    test_indicate_waits_for_confirmation();
    test_indicate_timeout_completes_with_timeout();
    test_unrelated_notify_tx_ignored();
    test_queue_full_reports_no_mem();
    test_reset_drops_queue_and_completes_in_flight();
    test_sync_notify_tx_reentrant();
    test_sync_indicate_sent_keeps_in_flight();
    test_frame_slot_reused_with_larger_payload();
    test_consecutive_failures_all_complete();
    test_queued_failures_all_complete_after_confirm();
    test_invalid_arguments_rejected();
    ble_tx_scheduler_deinit();
    printf("ble_tx_scheduler: all tests passed\n");
    return 0;
}
