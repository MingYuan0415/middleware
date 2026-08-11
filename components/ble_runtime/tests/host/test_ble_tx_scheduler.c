#include <pthread.h>
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

#define TEST_GENERATION 101U
#define TEST_SECURITY_EPOCH 7U

static esp_err_t _test_submit(
    ble_tx_scheduler_kind_t kind, uint16_t conn_handle,
    uint16_t value_handle, const uint8_t *data, size_t len,
    bool is_last, uint32_t flow_id)
{
    const ble_link_operation_identity_t identity =
    {
        .generation = TEST_GENERATION,
        .security_epoch = TEST_SECURITY_EPOCH,
        .flow_id = flow_id,
        .kind = kind == BLE_TX_SCHEDULER_KIND_INDICATE ?
        BLE_LINK_OPERATION_TX_INDICATE :
        BLE_LINK_OPERATION_TX_NOTIFY,
        .conn_handle = conn_handle,
    };

    return (ble_tx_scheduler_submit)(
               kind, &identity, value_handle, data, len, is_last);
}

#define ble_tx_scheduler_submit(kind, conn, attr, data, len, is_last) \
    _test_submit((kind), (conn), (attr), (data), (len), (is_last), 0U)
#define ble_tx_scheduler_submit_flow(kind, conn, attr, data, len, is_last, flow) \
    _test_submit((kind), (conn), (attr), (data), (len), (is_last), (flow))

static unsigned int s_notify_calls;
static unsigned int s_indicate_calls;
static unsigned int s_completions;
static esp_err_t s_last_completion_status;
static uint16_t s_last_completion_attr;
static uint32_t s_last_completion_flow;
static uint32_t s_last_completion_token;
static ble_link_operation_identity_t s_last_completion_identity;
static bool s_last_completion_is_last;
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
static pthread_mutex_t s_real_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct producer_context
{
    pthread_barrier_t *barrier;
    uint32_t flow_id;
    esp_err_t result;
} producer_context_t;

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

static void _real_lock(void *arg)
{
    pthread_mutex_t *mutex = arg;

    (void)pthread_mutex_lock(mutex);
}

static void _real_unlock(void *arg)
{
    pthread_mutex_t *mutex = arg;

    (void)pthread_mutex_unlock(mutex);
}

static void *_producer_submit(void *arg)
{
    static const uint8_t payload[2] = {0x33, 0xcc};
    producer_context_t *context = arg;

    (void)pthread_barrier_wait(context->barrier);
    context->result = ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true, context->flow_id);
    return NULL;
}

static void _fire_sync_notify_tx(uint16_t conn_handle, uint16_t attr_handle,
                                 bool indication)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_NOTIFY_TX;
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_tx_scheduler_get_in_flight_identity(
                          &event.identity));
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
static esp_err_t s_reentrant_submit_result;

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

static esp_err_t _fake_notify_reset_resubmit(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len)
{
    static const uint8_t replacement[2] = {0x55, 0xaa};

    (void)data;
    (void)len;
    s_notify_calls++;
    s_in_ops = true;
    if (s_notify_calls == 1U)
    {
        ble_tx_scheduler_reset();
        s_reentrant_submit_result = ble_tx_scheduler_submit_flow(
                                        BLE_TX_SCHEDULER_KIND_NOTIFY,
                                        conn_handle, value_handle,
                                        replacement, sizeof(replacement),
                                        true, 202U);
    }
    else
    {
        _fire_sync_notify_tx(conn_handle, value_handle, false);
    }
    s_in_ops = false;
    return ESP_OK;
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

static const ble_port_ops_t s_reset_resubmit_ops =
{
    .adv_start = NULL,
    .adv_stop = NULL,
    .notify = _fake_notify_reset_resubmit,
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
    s_last_completion_flow = result->flow_id;
    s_last_completion_token = result->token;
    s_last_completion_identity = result->identity;
    s_last_completion_is_last = result->is_last;
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

static esp_err_t _emit_notify_tx_identity(
    const ble_link_operation_identity_t *identity,
    uint16_t conn_handle, uint16_t attr_handle,
    bool indication, int tx_result)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_NOTIFY_TX;
    event.identity = *identity;
    event.conn_handle = conn_handle;
    event.attr_handle = attr_handle;
    event.indication = indication;
    event.tx_result = tx_result;

    return ble_tx_scheduler_handle_notify_tx(&event);
}

static void _emit_notify_tx(uint16_t conn_handle, uint16_t attr_handle,
                            bool indication, int tx_result)
{
    ble_link_operation_identity_t identity;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_tx_scheduler_get_in_flight_identity(&identity));
    const esp_err_t result = _emit_notify_tx_identity(
                                 &identity, conn_handle, attr_handle,
                                 indication, tx_result);

    TEST_ASSERT_TRUE(result == ESP_OK || result == ESP_ERR_NOT_FOUND ||
                     result == ESP_FAIL);
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
    s_completion_in_ops_violations = 0U;
    s_notify_calls = 0U;
    s_indicate_calls = 0U;
    s_completions = 0U;
    s_reset_completions = 0U;
    s_notify_data_len = 0U;
    s_indicate_data_len = 0U;
    s_last_completion_flow = 0U;
    s_last_completion_token = 0U;
    memset(&s_last_completion_identity, 0,
           sizeof(s_last_completion_identity));
    s_last_completion_is_last = false;
    s_indicate_active = false;
    s_lock_depth = 0U;
    s_max_lock_depth = 0U;
    s_in_ops = false;
    s_completion_in_ops_violations = 0U;
    s_completion_sequence_count = 0U;
    s_fake_notify_result = ESP_OK;
    s_reentrant_submit_result = ESP_OK;
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
                          payload, sizeof(payload), true));
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
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
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
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(1U, s_indicate_calls);
    TEST_ASSERT_TRUE(s_indicate_active);
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_EQUAL(ESP_OK, s_last_completion_status);
    TEST_ASSERT_FALSE(s_indicate_active);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_flow_identity_survives_completion(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true, 41U));
    const uint32_t token = ble_tx_scheduler_get_in_flight_token();

    TEST_ASSERT_TRUE(token != 0U);
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    TEST_ASSERT_EQUAL(41U, s_last_completion_flow);
    TEST_ASSERT_EQUAL(token, s_last_completion_token);
    TEST_ASSERT_EQUAL(TEST_GENERATION,
                      s_last_completion_identity.generation);
    TEST_ASSERT_EQUAL(TEST_SECURITY_EPOCH,
                      s_last_completion_identity.security_epoch);
    TEST_ASSERT_EQUAL(BLE_LINK_OPERATION_TX_INDICATE,
                      s_last_completion_identity.kind);
    TEST_ASSERT_EQUAL(7U, s_last_completion_identity.conn_handle);
    TEST_ASSERT_TRUE(s_last_completion_is_last);

    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(0U, s_last_completion_flow);
    TEST_ASSERT_TRUE(s_last_completion_token != 0U);
}

static void test_reset_completion_and_new_flow_are_distinct(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true, 100U));
    const uint32_t old_token = ble_tx_scheduler_get_in_flight_token();

    ble_tx_scheduler_reset();
    TEST_ASSERT_EQUAL(100U, s_last_completion_flow);
    TEST_ASSERT_EQUAL(old_token, s_last_completion_token);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, s_last_completion_status);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true, 101U));
    const uint32_t new_token = ble_tx_scheduler_get_in_flight_token();

    TEST_ASSERT_TRUE(new_token != 0U && new_token != old_token);
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    TEST_ASSERT_EQUAL(101U, s_last_completion_flow);
    TEST_ASSERT_EQUAL(new_token, s_last_completion_token);
}

static void test_stale_full_identity_completion_is_ignored(void)
{
    const uint8_t payload[2] = {0x01, 0x02};
    ble_link_operation_identity_t old_identity;
    ble_link_operation_identity_t new_identity =
    {
        .generation = TEST_GENERATION + 1U,
        .security_epoch = TEST_SECURITY_EPOCH + 1U,
        .flow_id = 102U,
        .kind = BLE_LINK_OPERATION_TX_INDICATE,
        .conn_handle = 7U,
    };

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true, 101U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_tx_scheduler_get_in_flight_identity(&old_identity));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_tx_scheduler_handle_indication_timeout(
                          old_identity.token));
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_EQUAL(ESP_OK, (ble_tx_scheduler_submit)(
                          BLE_TX_SCHEDULER_KIND_INDICATE, &new_identity, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, _emit_notify_tx_identity(
                          &old_identity, 7U, 9U, true,
                          BLE_PORT_TX_CONFIRMED));
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_TRUE(ble_tx_scheduler_is_busy());
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_tx_scheduler_get_in_flight_identity(&new_identity));
    TEST_ASSERT_EQUAL(ESP_OK, _emit_notify_tx_identity(
                          &new_identity, 7U, 9U, true,
                          BLE_PORT_TX_CONFIRMED));
    TEST_ASSERT_EQUAL(2U, s_completions);
    TEST_ASSERT_EQUAL(TEST_GENERATION + 1U,
                      s_last_completion_identity.generation);
    TEST_ASSERT_EQUAL(TEST_SECURITY_EPOCH + 1U,
                      s_last_completion_identity.security_epoch);
    TEST_ASSERT_EQUAL(102U, s_last_completion_identity.flow_id);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_indicate_timeout_completes_with_timeout(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true));
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
                          payload, sizeof(payload), true));
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
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U,
                          9U, payload, sizeof(payload), true));
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    /* The two queued notifications drained synchronously. */
    TEST_ASSERT_EQUAL(2U, s_notify_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(3U, s_notify_calls);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_two_producers_share_fixed_credits(void)
{
    static ble_tx_scheduler_config_t config;
    const uint8_t payload[2] = {0x01, 0x02};
    pthread_barrier_t barrier;
    pthread_t first_thread;
    pthread_t second_thread;
    producer_context_t first =
    {
        .barrier = &barrier,
        .flow_id = 301U,
        .result = ESP_FAIL,
    };
    producer_context_t second =
    {
        .barrier = &barrier,
        .flow_id = 302U,
        .result = ESP_FAIL,
    };

    _init_scheduler(2U);
    config.queue_depth = 2U;
    config.max_frame_bytes = 100U;
    config.ops = &s_fake_ops;
    config.completed = _completion;
    config.completed_arg = NULL;
    config.lock = _real_lock;
    config.unlock = _real_unlock;
    config.lock_arg = &s_real_mutex;
    ble_tx_scheduler_deinit();
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true, 300U));
    TEST_ASSERT_EQUAL(0, pthread_barrier_init(&barrier, NULL, 3U));
    TEST_ASSERT_EQUAL(0, pthread_create(
                          &first_thread, NULL, _producer_submit, &first));
    TEST_ASSERT_EQUAL(0, pthread_create(
                          &second_thread, NULL, _producer_submit, &second));
    (void)pthread_barrier_wait(&barrier);
    TEST_ASSERT_EQUAL(0, pthread_join(first_thread, NULL));
    TEST_ASSERT_EQUAL(0, pthread_join(second_thread, NULL));
    TEST_ASSERT_EQUAL(0, pthread_barrier_destroy(&barrier));
    TEST_ASSERT_EQUAL(ESP_OK, first.result);
    TEST_ASSERT_EQUAL(ESP_OK, second.result);
    TEST_ASSERT_TRUE(ble_tx_scheduler_is_busy());
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    TEST_ASSERT_EQUAL(3U, s_completions);
    TEST_ASSERT_EQUAL(2U, s_notify_calls);
    TEST_ASSERT_EQUAL(0U, s_completion_in_ops_violations);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_reset_drops_queue_and_completes_in_flight(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    ble_tx_scheduler_reset();
    /* Every submitted frame completes: 1 in flight + 1 queued. */
    TEST_ASSERT_EQUAL(2U, s_reset_completions);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, s_last_completion_status);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
    TEST_ASSERT_EQUAL(0U, s_notify_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(1U, s_notify_calls);
}

static void test_sync_notify_tx_reentrant(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(2U, s_notify_calls);
    TEST_ASSERT_EQUAL(2U, s_completions);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
    TEST_ASSERT_EQUAL(1U, s_max_lock_depth);
    TEST_ASSERT_EQUAL(0U, s_completion_in_ops_violations);
    TEST_ASSERT_EQUAL(2U, s_completion_sequence_count);
    TEST_ASSERT_EQUAL(ESP_OK, s_completion_sequence[0]);
    TEST_ASSERT_EQUAL(ESP_OK, s_completion_sequence[1]);
}

static void test_reset_during_port_call_preserves_completions(void)
{
    static ble_tx_scheduler_config_t config;
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    config.queue_depth = 4U;
    config.max_frame_bytes = 100U;
    config.ops = &s_reset_resubmit_ops;
    config.completed = _completion;
    config.completed_arg = NULL;
    config.lock = _fake_lock;
    config.unlock = _fake_unlock;
    config.lock_arg = NULL;
    ble_tx_scheduler_deinit();
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true, 201U));
    TEST_ASSERT_EQUAL(ESP_OK, s_reentrant_submit_result);
    TEST_ASSERT_EQUAL(2U, s_notify_calls);
    TEST_ASSERT_EQUAL(2U, s_completions);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, s_completion_sequence[0]);
    TEST_ASSERT_EQUAL(ESP_OK, s_completion_sequence[1]);
    TEST_ASSERT_EQUAL(202U, s_last_completion_flow);
    TEST_ASSERT_EQUAL(0U, s_completion_in_ops_violations);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_sync_indicate_sent_keeps_in_flight(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(1U, s_indicate_calls);
    TEST_ASSERT_EQUAL(0U, s_completions);
    TEST_ASSERT_TRUE(ble_tx_scheduler_is_busy());
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_EQUAL(ESP_OK, s_last_completion_status);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_indication_timeout_completes_frame(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(1U, s_indicate_calls);
    TEST_ASSERT_EQUAL(0U, s_completions);
    TEST_ASSERT_TRUE(ble_tx_scheduler_is_busy());
    const uint32_t token = ble_tx_scheduler_get_in_flight_token();

    TEST_ASSERT_TRUE(token != 0U);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_tx_scheduler_handle_indication_timeout(token));
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_last_completion_status);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_timeout_allows_new_submission(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true));
    /* The indication timeout retires its flow. */
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_TIMEOUT);
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_last_completion_status);
    /* A new operation on the same ACL is admitted without a scheduler reset. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, payload,
                          sizeof(payload), true));
}

static void test_timer_timeout_allows_new_submission(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true));
    const uint32_t token = ble_tx_scheduler_get_in_flight_token();

    /* The 2 s timer expiry path. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_tx_scheduler_handle_indication_timeout(token));
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_last_completion_status);
    /* A new operation is admitted without waiting for disconnect/reset. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, payload,
                          sizeof(payload), true));
}

static void test_indicate_timeout_completes_queued_flow(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), false, 77U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), false, 77U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true, 77U));
    TEST_ASSERT_EQUAL(0U, s_completions);
    /* The timeout retires every queued frame in the same response flow. */
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_TIMEOUT);
    TEST_ASSERT_EQUAL(3U, s_completions);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_last_completion_status);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_completion_sequence[0]);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_completion_sequence[1]);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_completion_sequence[2]);
    TEST_ASSERT_EQUAL(1U, s_indicate_calls);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_timer_timeout_preserves_independent_notification(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), false, 88U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit_flow(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true, 88U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    const uint32_t token = ble_tx_scheduler_get_in_flight_token();

    TEST_ASSERT_TRUE(token != 0U);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_tx_scheduler_handle_indication_timeout(token));
    TEST_ASSERT_EQUAL(3U, s_completions);
    TEST_ASSERT_EQUAL(ESP_OK, s_last_completion_status);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_completion_sequence[0]);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_completion_sequence[1]);
    TEST_ASSERT_EQUAL(ESP_OK, s_completion_sequence[2]);
    TEST_ASSERT_EQUAL(1U, s_notify_calls);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
}

static void test_stale_indication_timeout_ignored(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true));
    const uint32_t first_token = ble_tx_scheduler_get_in_flight_token();

    /* Confirm the first indication. */
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    TEST_ASSERT_EQUAL(1U, s_completions);
    /* A second indication starts. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_INDICATE, 7U, 9U,
                          payload, sizeof(payload), true));
    /* A late timeout for the first indication is rejected. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_tx_scheduler_handle_indication_timeout(
                          first_token));
    TEST_ASSERT_EQUAL(1U, s_completions);
    TEST_ASSERT_TRUE(ble_tx_scheduler_is_busy());
}

static void test_frame_slot_reused_with_larger_payload(void)
{
    const uint8_t small[1] = {0x01};
    uint8_t large[60];

    memset(large, 0xab, sizeof(large));
    _init_scheduler(1U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          small, sizeof(small), true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          large, sizeof(large), true));
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
    /* The synchronous failure is returned to the submit that caused it. */
    TEST_ASSERT_EQUAL(ESP_FAIL, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    /* A best-effort notification failure is local to that frame. */
    TEST_ASSERT_EQUAL(ESP_FAIL, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
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
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(0U, s_completions);
    _emit_notify_tx(7U, 9U, true, BLE_PORT_TX_CONFIRMED);
    /* The indicate confirms; both best-effort notifications run and report
     * their own failures without poisoning the scheduler. */
    TEST_ASSERT_EQUAL(3U, s_completions);
    TEST_ASSERT_EQUAL(ESP_FAIL, s_last_completion_status);
    TEST_ASSERT_FALSE(ble_tx_scheduler_is_busy());
    TEST_ASSERT_EQUAL(0U, s_completion_in_ops_violations);
    TEST_ASSERT_EQUAL(3U, s_completion_sequence_count);
    TEST_ASSERT_EQUAL(ESP_OK, s_completion_sequence[0]);
    TEST_ASSERT_EQUAL(ESP_FAIL, s_completion_sequence[1]);
    TEST_ASSERT_EQUAL(ESP_FAIL, s_completion_sequence[2]);
}

static void test_token_exhaustion_rejects_submit(void)
{
    const uint8_t payload[2] = {0x01, 0x02};

    _init_scheduler(4U);
    ble_tx_scheduler_test_set_token(UINT32_MAX - 2U);
    /* One fresh token (MAX-1) remains allocatable. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, 7U, 9U,
                          payload, sizeof(payload), true));
    /* The next submission cannot be assigned a fresh token. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, payload,
                          sizeof(payload), true));
}

static void test_invalid_arguments_rejected(void)
{
    const uint8_t payload[2] = {0x01, 0x02};
    ble_link_operation_identity_t identity =
    {
        .generation = TEST_GENERATION,
        .security_epoch = TEST_SECURITY_EPOCH,
        .kind = BLE_LINK_OPERATION_TX_NOTIFY,
        .conn_handle = 7U,
    };

    _init_scheduler(4U);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, (ble_tx_scheduler_submit)(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, NULL, 9U,
                          payload, sizeof(payload), true));
    identity.generation = 0U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, (ble_tx_scheduler_submit)(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, &identity, 9U,
                          payload, sizeof(payload), true));
    identity.generation = TEST_GENERATION;
    identity.token = 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, (ble_tx_scheduler_submit)(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, &identity, 9U,
                          payload, sizeof(payload), true));
    identity.token = 0U;
    identity.kind = BLE_LINK_OPERATION_TX_INDICATE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, (ble_tx_scheduler_submit)(
                          BLE_TX_SCHEDULER_KIND_NOTIFY, &identity, 9U,
                          payload, sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_tx_scheduler_submit(
                          (ble_tx_scheduler_kind_t)99U,
                          7U, 9U, payload,
                          sizeof(payload), true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, NULL, 2U, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, payload, 0U, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, payload, 101U, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_tx_scheduler_handle_notify_tx(NULL));
    ble_tx_scheduler_deinit();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_tx_scheduler_submit(
                          BLE_TX_SCHEDULER_KIND_NOTIFY,
                          7U, 9U, payload,
                          sizeof(payload), true));
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
    test_flow_identity_survives_completion();
    test_reset_completion_and_new_flow_are_distinct();
    test_stale_full_identity_completion_is_ignored();
    test_indicate_timeout_completes_with_timeout();
    test_unrelated_notify_tx_ignored();
    test_queue_full_reports_no_mem();
    test_two_producers_share_fixed_credits();
    test_reset_drops_queue_and_completes_in_flight();
    test_sync_notify_tx_reentrant();
    test_reset_during_port_call_preserves_completions();
    test_sync_indicate_sent_keeps_in_flight();
    test_indication_timeout_completes_frame();
    test_token_exhaustion_rejects_submit();
    test_stale_indication_timeout_ignored();
    test_timer_timeout_allows_new_submission();
    test_timeout_allows_new_submission();
    test_indicate_timeout_completes_queued_flow();
    test_timer_timeout_preserves_independent_notification();
    test_frame_slot_reused_with_larger_payload();
    test_consecutive_failures_all_complete();
    test_queued_failures_all_complete_after_confirm();
    test_invalid_arguments_rejected();
    ble_tx_scheduler_deinit();
    printf("ble_tx_scheduler: all tests passed\n");
    return 0;
}
