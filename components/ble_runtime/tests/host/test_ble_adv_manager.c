#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_adv_manager.h"
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

static unsigned int s_start_calls;
static unsigned int s_stop_calls;
static esp_err_t s_start_result;
static esp_err_t s_stop_result;
static ble_port_adv_config_t s_last_config;
static bool s_last_config_valid;
static unsigned int s_timer_armed_ms;
static unsigned int s_timer_cancel_calls;
static uint32_t s_now_ms;

static const uint8_t s_uuid[16] =
{
    0xa3, 0x4e, 0x85, 0x57, 0x11, 0x3d, 0x8a, 0xa2,
    0x59, 0x4e, 0xbb, 0xb4, 0x92, 0x31, 0x20, 0x3e,
};

static esp_err_t _fake_adv_start(const ble_port_adv_config_t *config)
{
    s_start_calls++;
    s_last_config_valid = true;
    s_last_config = *config;
    return s_start_result;
}

static esp_err_t _fake_adv_stop(void)
{
    s_stop_calls++;
    return s_stop_result;
}

static esp_err_t _fake_notify(uint16_t conn_handle, uint16_t value_handle,
                              const uint8_t *data, size_t len)
{
    (void)conn_handle;
    (void)value_handle;
    (void)data;
    (void)len;
    return ESP_OK;
}

static esp_err_t _fake_indicate(uint16_t conn_handle, uint16_t value_handle,
                                const uint8_t *data, size_t len)
{
    (void)conn_handle;
    (void)value_handle;
    (void)data;
    (void)len;
    return ESP_OK;
}

static const ble_port_ops_t s_fake_ops =
{
    .adv_start = _fake_adv_start,
    .adv_stop = _fake_adv_stop,
    .notify = _fake_notify,
    .indicate = _fake_indicate,
};

static uint32_t _fake_now_ms(void)
{
    return s_now_ms;
}

static void _fake_arm_timer(uint32_t delay_ms, void *arg)
{
    (void)arg;
    if (delay_ms == 0U)
    {
        s_timer_cancel_calls++;
    }
    else
    {
        s_timer_armed_ms = delay_ms;
    }
}

static void _reset_harness(void)
{
    s_start_calls = 0U;
    s_stop_calls = 0U;
    s_start_result = ESP_OK;
    s_stop_result = ESP_OK;
    s_last_config_valid = false;
    s_timer_armed_ms = 0U;
    s_timer_cancel_calls = 0U;
    s_now_ms = 100000U;
}

static void _init_manager(void)
{
    static const uint8_t short_name[] = "MT";
    static const ble_adv_manager_config_t config =
    {
        .fast_interval_ms = 100U,
        .slow_interval_ms = 700U,
        .fast_window_ms = 30000U,
        .short_name = short_name,
        .short_name_len = sizeof(short_name) - 1U,
        .service_uuid = s_uuid,
        .adv_version = 1U,
        .now_ms = _fake_now_ms,
        .arm_timer = _fake_arm_timer,
        .timer_arg = NULL,
        .ops = &s_fake_ops,
        .lock = NULL,
        .unlock = NULL,
        .lock_arg = NULL,
    };

    ble_adv_manager_init(&config);
}

static void _emit_adv_started(int status)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_STARTED;
    event.status = status;
    event.generation = s_last_config_valid ? s_last_config.generation : 0U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
}

static void _emit_adv_started_gen(int status, uint32_t generation)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_STARTED;
    event.status = status;
    event.generation = generation;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
}

static void _emit_adv_stopped(int status)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_STOPPED;
    event.status = status;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
}

static void _emit_connect(int status)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_CONNECT;
    event.status = status;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
}

static void _emit_disconnect(void)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_DISCONNECT;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
}

static void test_acquire_fast_starts_fast_adv(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    TEST_ASSERT_EQUAL(1U, lease.lease_id);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(1U, s_start_calls);
    TEST_ASSERT_EQUAL(0U, s_stop_calls);
    TEST_ASSERT_TRUE(s_last_config_valid);
    TEST_ASSERT_EQUAL(100U, s_last_config.interval_ms);
    TEST_ASSERT_EQUAL(30000U, s_timer_armed_ms);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
}

static void test_fast_window_expires_falls_back_to_slow(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    s_now_ms += 30000U;
    ble_adv_manager_handle_fast_window_expired();
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    TEST_ASSERT_EQUAL(1U, s_timer_cancel_calls);
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(2U, s_start_calls);
    TEST_ASSERT_TRUE(s_last_config_valid);
    TEST_ASSERT_EQUAL(700U, s_last_config.interval_ms);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
}

static void test_release_last_lease_stops(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(1U, s_start_calls);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_adv_manager_release_lease(lease.lease_id));
}

static void test_slow_lease_skips_fast(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false,
                          NULL));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_TRUE(s_last_config_valid);
    TEST_ASSERT_EQUAL(700U, s_last_config.interval_ms);
    TEST_ASSERT_EQUAL(0U, s_timer_armed_ms);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
}

static void test_connect_stops_adv_disconnect_resumes(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    _emit_connect(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(1U, s_start_calls);
    _emit_disconnect();
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(2U, s_start_calls);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
}

static void test_start_failure_faults(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    s_start_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    s_start_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
}

static void test_fast_lease_escalates_slow_adv(void)
{
    ble_adv_lease_t slow_lease;
    ble_adv_lease_t fast_lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &slow_lease, BLE_ADV_MANAGER_MODE_SLOW,
                          false, NULL));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &fast_lease, BLE_ADV_MANAGER_MODE_FAST,
                          false, NULL));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_TRUE(s_last_config_valid);
    TEST_ASSERT_EQUAL(100U, s_last_config.interval_ms);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_release_lease(fast_lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_TRUE(s_last_config_valid);
    TEST_ASSERT_EQUAL(700U, s_last_config.interval_ms);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_release_lease(slow_lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
}

static void test_bindable_payload_built(void)
{
    const uint8_t discriminator[3] = {0xef, 0xcd, 0xab};
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, true,
                          discriminator));
    TEST_ASSERT_TRUE(s_last_config_valid);
    TEST_ASSERT_EQUAL(5U, s_last_config.service_data_len);
    TEST_ASSERT_EQUAL(0x01U, s_last_config.service_data[0]);
    TEST_ASSERT_EQUAL(0x01U, s_last_config.service_data[1]);
    TEST_ASSERT_EQUAL(0xefU, s_last_config.service_data[2]);
    TEST_ASSERT_EQUAL(0xcdU, s_last_config.service_data[3]);
    TEST_ASSERT_EQUAL(0xabU, s_last_config.service_data[4]);
    TEST_ASSERT_EQUAL(0U, memcmp(s_uuid, s_last_config.service_uuid, 16U));
}

static void test_bindable_lease_release_clears_payload(void)
{
    const uint8_t discriminator[3] = {0xef, 0xcd, 0xab};
    ble_adv_lease_t bindable_lease;
    ble_adv_lease_t slow_lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &slow_lease, BLE_ADV_MANAGER_MODE_SLOW,
                          false, NULL));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &bindable_lease, BLE_ADV_MANAGER_MODE_SLOW,
                          true, discriminator));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(0x01U, s_last_config.service_data[1]);
    TEST_ASSERT_EQUAL(0xefU, s_last_config.service_data[2]);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_adv_manager_release_lease(bindable_lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(0x00U, s_last_config.service_data[1]);
    TEST_ASSERT_EQUAL(0x00U, s_last_config.service_data[2]);
    TEST_ASSERT_EQUAL(0x00U, s_last_config.service_data[3]);
    TEST_ASSERT_EQUAL(0x00U, s_last_config.service_data[4]);
}

static void test_lease_capacity_enforced(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    for (uint8_t i = 0U; i < BLE_ADV_MANAGER_MAX_LEASES; ++i)
    {
        TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                              &lease, BLE_ADV_MANAGER_MODE_SLOW,
                              false, NULL));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW,
                          false, NULL));
}

static void test_stale_adv_started_rejected_by_generation(void)
{
    ble_adv_lease_t lease;
    ble_port_event_t event;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_RESET;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_SYNC;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_STARTED;
    event.status = 0;
    event.generation = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    _emit_adv_started_gen(0, s_last_config.generation);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
}

static void test_stop_submission_failure_faults(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    s_stop_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    s_stop_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
}

static void test_deinit_rejects_calls(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    ble_adv_manager_deinit();
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_adv_manager_acquire_lease(
                          &lease,
                          BLE_ADV_MANAGER_MODE_FAST,
                          false, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_adv_manager_handle_event(&(ble_port_event_t)
    {
        .type = BLE_PORT_EVENT_CONNECT
    }));
}

static void test_last_lease_stop_failure_retries_on_event(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    s_stop_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    s_stop_result = ESP_OK;
    _emit_disconnect();
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
}

static void test_invalid_arguments_rejected(void)
{
    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_adv_manager_acquire_lease(
                          NULL, BLE_ADV_MANAGER_MODE_FAST,
                          false, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_adv_manager_acquire_lease(
                          &(ble_adv_lease_t)
    {
        0
    }, BLE_ADV_MANAGER_MODE_FAST, true, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_adv_manager_handle_event(NULL));
}

static void test_reset_and_sync(void)
{
    ble_adv_lease_t lease;
    ble_port_event_t event;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false,
                          NULL));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_RESET;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_SYNC;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
}

static void test_adv_complete_without_connection_restarts(void)
{
    ble_adv_lease_t lease;
    ble_port_event_t event;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false,
                          NULL));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_COMPLETE;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(2U, s_start_calls);
}

int main(void)
{
    test_acquire_fast_starts_fast_adv();
    test_fast_window_expires_falls_back_to_slow();
    test_release_last_lease_stops();
    test_slow_lease_skips_fast();
    test_connect_stops_adv_disconnect_resumes();
    test_start_failure_faults();
    test_fast_lease_escalates_slow_adv();
    test_bindable_payload_built();
    test_bindable_lease_release_clears_payload();
    test_lease_capacity_enforced();
    test_invalid_arguments_rejected();
    test_reset_and_sync();
    test_adv_complete_without_connection_restarts();
    test_stale_adv_started_rejected_by_generation();
    test_stop_submission_failure_faults();
    test_last_lease_stop_failure_retries_on_event();
    test_deinit_rejects_calls();
    printf("ble_adv_manager: all tests passed\n");
    return 0;
}
