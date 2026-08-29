#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_adv_manager.h"
#include "ble_nimble_adv_start.h"
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
static uint32_t s_last_stop_generation;
static unsigned int s_timer_armed_ms;
static unsigned int s_timer_cancel_calls;
static uint32_t s_now_ms;
static bool s_guard_host_ready;
static bool s_guard_gate_open;
static bool s_guard_pause_during_gate;
static bool s_guard_pause_during_start;
static unsigned int s_guard_gate_close_calls;
static unsigned int s_guard_physical_start_calls;
static unsigned int s_guard_physical_stop_calls;

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

static esp_err_t _fake_adv_stop(uint32_t generation)
{
    s_stop_calls++;
    s_last_stop_generation = generation;
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
    s_last_stop_generation = 0U;
    s_timer_armed_ms = 0U;
    s_timer_cancel_calls = 0U;
    s_now_ms = 100000U;
    s_guard_host_ready = true;
    s_guard_gate_open = false;
    s_guard_pause_during_gate = false;
    s_guard_pause_during_start = false;
    s_guard_gate_close_calls = 0U;
    s_guard_physical_start_calls = 0U;
    s_guard_physical_stop_calls = 0U;
}

static bool _guard_host_ready(void *arg)
{
    (void)arg;
    return s_guard_host_ready;
}

static esp_err_t _guard_set_pairing_gate(bool open, void *arg)
{
    (void)arg;
    s_guard_gate_open = open;
    if (!open)
    {
        s_guard_gate_close_calls++;
    }
    if (open && s_guard_pause_during_gate)
    {
        s_stop_result = ESP_ERR_NO_MEM;
        TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                          ble_adv_manager_set_paused(true));
    }
    return ESP_OK;
}

static int _guard_physical_start(void *arg)
{
    (void)arg;
    s_guard_physical_start_calls++;
    if (s_guard_pause_during_start)
    {
        TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(true));
    }
    return 0;
}

static int _guard_physical_stop(void *arg)
{
    (void)arg;
    s_guard_physical_stop_calls++;
    return 0;
}

static ble_nimble_adv_start_ops_t _guard_ops(void)
{
    const ble_nimble_adv_start_ops_t ops =
    {
        .host_ready = _guard_host_ready,
        .set_pairing_gate = _guard_set_pairing_gate,
        .start = _guard_physical_start,
        .stop = _guard_physical_stop,
        .arg = NULL,
    };

    return ops;
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
    event.generation = s_last_stop_generation;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
}

static void _emit_adv_stopped_gen(int status, uint32_t generation)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_STOPPED;
    event.status = status;
    event.generation = generation;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
}

static void _emit_connect(int status)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_CONNECT;
    event.status = status;
    event.accepted = status == 0;
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
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
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
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
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
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
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
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
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
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
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
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    s_start_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
}

static void test_async_start_failure_recovers(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(100U, s_timer_armed_ms);
    s_start_result = ESP_OK;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(1U, s_start_calls);
    s_now_ms += 99U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(1U, s_start_calls);
    s_now_ms += 1U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(2U, s_start_calls);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
}

static void test_async_start_failure_backoff_is_bounded(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(100U,
                      ble_adv_manager_get_retry_remaining_ms());
    s_now_ms += 100U;
    ble_adv_manager_poll();
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(200U,
                      ble_adv_manager_get_retry_remaining_ms());
    s_now_ms += 200U;
    ble_adv_manager_poll();
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(400U,
                      ble_adv_manager_get_retry_remaining_ms());
    s_now_ms += 400U;
    ble_adv_manager_poll();
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(800U,
                      ble_adv_manager_get_retry_remaining_ms());
    s_now_ms += 800U;
    ble_adv_manager_poll();
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(1000U,
                      ble_adv_manager_get_retry_remaining_ms());
    s_now_ms += 1000U;
    ble_adv_manager_poll();
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(1000U,
                      ble_adv_manager_get_retry_remaining_ms());
}

static void test_start_target_change_resets_backoff(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(100U,
                      ble_adv_manager_get_retry_remaining_ms());

    s_now_ms += 100U;
    ble_adv_manager_poll();
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(200U,
                      ble_adv_manager_get_retry_remaining_ms());

    /* Expiry changes the logical target from FAST to SLOW. Its first
     * failure starts a fresh backoff sequence even though physical START
     * command generations continue to advance for stale completion safety. */
    s_now_ms += 29900U;
    ble_adv_manager_handle_fast_window_expired();
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(700U, s_last_config.interval_ms);
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(100U,
                      ble_adv_manager_get_retry_remaining_ms());
}

static void test_async_stop_failure_respects_cooldown(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(true));
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    _emit_adv_stopped(ESP_FAIL);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(100U,
                      ble_adv_manager_get_retry_remaining_ms());
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(1U, s_stop_calls);
    s_now_ms += 100U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(2U, s_stop_calls);
    _emit_adv_stopped(ESP_FAIL);
    TEST_ASSERT_EQUAL(200U,
                      ble_adv_manager_get_retry_remaining_ms());
    s_now_ms += 199U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(2U, s_stop_calls);
    s_now_ms += 1U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(3U, s_stop_calls);
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_adv_manager_get_retry_remaining_ms());
}

static void test_retry_deadline_wraps_monotonic_clock(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    s_now_ms = UINT32_MAX - 50U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(ESP_FAIL);
    TEST_ASSERT_EQUAL(100U,
                      ble_adv_manager_get_retry_remaining_ms());
    s_now_ms += 99U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(1U, s_start_calls);
    s_now_ms += 1U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(2U, s_start_calls);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
}

static void test_fast_lease_escalates_slow_adv(void)
{
    ble_adv_lease_t slow_lease;
    ble_adv_lease_t fast_lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &slow_lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &fast_lease, BLE_ADV_MANAGER_MODE_FAST, false));
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

static void test_bindable_control_is_explicit(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, true));
    TEST_ASSERT_TRUE(s_last_config_valid);
    TEST_ASSERT_TRUE(s_last_config.bindable);
    TEST_ASSERT_EQUAL(0U, memcmp(s_uuid, s_last_config.service_uuid, 16U));
}

static void test_bindable_lease_release_restarts_with_closed_gate(void)
{
    ble_adv_lease_t bindable_lease;
    ble_adv_lease_t slow_lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &slow_lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &bindable_lease, BLE_ADV_MANAGER_MODE_SLOW, true));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
    TEST_ASSERT_TRUE(s_last_config.bindable);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_adv_manager_release_lease(bindable_lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
    TEST_ASSERT_TRUE(!s_last_config.bindable);
}

static void test_lease_capacity_enforced(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    for (uint8_t i = 0U; i < BLE_ADV_MANAGER_MAX_LEASES; ++i)
    {
        TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                              &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
}

static void test_stale_adv_started_rejected_by_generation(void)
{
    ble_adv_lease_t lease;
    ble_port_event_t event;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
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
    const uint32_t current_generation = s_last_config.generation;

    _emit_adv_started_gen(ESP_FAIL, current_generation - 1U);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_adv_manager_get_retry_remaining_ms());
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

static void test_stale_adv_stopped_rejected_by_generation(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(true));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    const uint32_t first_generation = s_last_stop_generation;

    TEST_ASSERT_TRUE(first_generation != 0U);
    _emit_adv_stopped_gen(ESP_FAIL, first_generation - 1U);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_adv_manager_get_retry_remaining_ms());
    _emit_adv_stopped_gen(ESP_FAIL, first_generation);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(100U,
                      ble_adv_manager_get_retry_remaining_ms());

    s_now_ms += 100U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(first_generation, s_last_stop_generation);
    _emit_adv_stopped_gen(0, first_generation - 1U);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped_gen(0, first_generation);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());

    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(true));
    TEST_ASSERT_TRUE(s_last_stop_generation > first_generation);
    _emit_adv_stopped(0);
}

static void test_stop_submission_failure_faults(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    s_stop_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    /* The failed release keeps the lease installed so the owner can retry
     * it; the manager keeps a pending stop obligation. */
    s_stop_result = ESP_OK;
    s_now_ms += 100U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    /* A fresh lease starts advertising again. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
}

static void test_paused_keeps_fast_window(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    const unsigned int cancel_before = s_timer_cancel_calls;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(100U, s_last_config.interval_ms);
    /* Pausing stops the physical advertisement but must not destroy the
     * fast window: an unexpired FAST lease resumes at FAST speed. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(true));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(cancel_before, s_timer_cancel_calls);
    /* Resume: the still-armed fast window restores FAST advertising. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(false));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
}

static void test_rejected_connect_ignored(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    _emit_adv_started(0);
    /* A rejected CONNECT (accepted=false) must not mark the manager
     * connected: its disconnect must never retire the real ACL's state. */
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_CONNECT;
    event.status = 0;
    event.accepted = false;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    event.type = BLE_PORT_EVENT_DISCONNECT;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
}


static void test_release_failure_restores_fast_window(void)
{
    /* A failed release keeps the lease AND the exact fast-window state:
     * an active window keeps its original deadline (not a fresh full
     * window), so the retry resumes at FAST without extending the
     * advertisement. */
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    /* Advance most of the fast window. */
    s_now_ms += 20000U;
    const uint32_t remaining_before =
        ble_adv_manager_get_fast_window_remaining_ms();

    TEST_ASSERT_TRUE(remaining_before != UINT32_MAX && remaining_before > 0U);
    s_stop_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    /* A retry before the cooldown must not report convergence or retire
     * the lease while the physical STOP obligation is still outstanding. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    /* The window survived with its ORIGINAL deadline: remaining time is
     * unchanged, and the retry timer was re-armed with the remaining
     * duration (not a fresh full window). */
    TEST_ASSERT_EQUAL(remaining_before,
                      ble_adv_manager_get_fast_window_remaining_ms());
    TEST_ASSERT_EQUAL(remaining_before, s_timer_armed_ms);
    s_stop_result = ESP_OK;
    s_now_ms += 100U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
}


static void test_release_failure_expired_window_stays_closed(void)
{
    /* A release failure while the fast-window deadline has already passed
     * (without the expiry handler having run) must NOT recreate the
     * window: the rollback discovers the expiry, forces the window
     * inactive, announces the cancel, and the state stays closed. */
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    /* Advance past the deadline WITHOUT running the expiry handler: the
     * manager still believes the window is active. */
    s_now_ms += 30000U + 1000U;
    const unsigned int cancels_before = s_timer_cancel_calls;

    s_stop_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    /* The expired window was not resurrected: inactive, no remaining
     * time, and the cancel was announced. */
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_adv_manager_get_fast_window_remaining_ms());
    TEST_ASSERT_TRUE(s_timer_cancel_calls > cancels_before);
    s_stop_result = ESP_OK;
    s_now_ms += 100U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    /* No FAST lease remains: the window stays inactive for the retry. */
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_adv_manager_get_fast_window_remaining_ms());
}


static void test_release_failure_at_exact_deadline_stays_closed(void)
{
    /* The deadline exactly equal to now is expired (remaining == 0): the
     * rollback must not recreate the window. */
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    s_now_ms += 30000U; /* Exactly the fast window. */
    const unsigned int cancels_before = s_timer_cancel_calls;

    s_stop_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_adv_manager_get_fast_window_remaining_ms());
    TEST_ASSERT_TRUE(s_timer_cancel_calls > cancels_before);
    s_stop_result = ESP_OK;
    s_now_ms += 100U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
}

static void test_release_last_fast_lease_cancels_window(void)
{
    /* A successful release of the last FAST lease while the window is
     * still active retires it through the helper: the cancel notification
     * is sent and the remaining time becomes unbounded. */
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    /* Pause: the manager stops advertising while the window stays active. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(true));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_TRUE(ble_adv_manager_get_fast_window_remaining_ms() !=
                     UINT32_MAX);
    const unsigned int cancels_before = s_timer_cancel_calls;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    /* The window was retired and the cancel was announced. */
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_adv_manager_get_fast_window_remaining_ms());
    TEST_ASSERT_TRUE(s_timer_cancel_calls > cancels_before);
}

static void test_deinit_rejects_calls(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    ble_adv_manager_deinit();
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_adv_manager_acquire_lease(
                          &lease,
                          BLE_ADV_MANAGER_MODE_FAST, false));
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
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAST,
                      ble_adv_manager_get_state());
    s_stop_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL,
                      ble_adv_manager_release_lease(lease.lease_id));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    /* The failed release kept the lease; the owner retries it once the
     * stop path recovers. */
    s_stop_result = ESP_OK;
    s_now_ms += 100U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_adv_manager_release_lease(lease.lease_id));
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
                          NULL, BLE_ADV_MANAGER_MODE_FAST, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_adv_manager_acquire_lease(
                          &(ble_adv_lease_t)
    {
        0
    }, (ble_adv_manager_mode_t)99, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_adv_manager_handle_event(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_adv_manager_set_pause_reason(0, true));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_WINDOW_TRANSITION |
            BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP,
            true));
}

static void test_reset_and_sync(void)
{
    ble_adv_lease_t lease;
    ble_port_event_t event;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, false));
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

static void test_bindable_start_stale_after_gate_rolls_back(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, true));
    const uint32_t generation = s_last_config.generation;
    const ble_nimble_adv_start_ops_t ops = _guard_ops();

    /* Model the real barrier: the persistent host gate-open event is in
     * flight while close-window pauses the manager, and the bounded STOP
     * command queue rejects the stop submission. The stale START must still
     * close the gate and must never reach the physical start callback. */
    s_guard_pause_during_gate = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_nimble_adv_start_execute(
                          generation, true, &ops));
    TEST_ASSERT_TRUE(!s_guard_gate_open);
    TEST_ASSERT_EQUAL(1U, s_guard_gate_close_calls);
    TEST_ASSERT_EQUAL(0U, s_guard_physical_start_calls);
    TEST_ASSERT_EQUAL(0U, s_guard_physical_stop_calls);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
}

static void test_bindable_start_stale_during_host_call_is_stopped(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_FAST, true));
    const uint32_t generation = s_last_config.generation;
    const ble_nimble_adv_start_ops_t ops = _guard_ops();

    s_guard_pause_during_start = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_nimble_adv_start_execute(
                          generation, true, &ops));
    TEST_ASSERT_TRUE(!s_guard_gate_open);
    TEST_ASSERT_EQUAL(1U, s_guard_gate_close_calls);
    TEST_ASSERT_EQUAL(1U, s_guard_physical_start_calls);
    TEST_ASSERT_EQUAL(1U, s_guard_physical_stop_calls);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
}

static void test_reset_adv_complete_does_not_requeue_retired_start(void)
{
    ble_adv_lease_t lease;
    ble_port_event_t event;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    const uint32_t retired_generation = s_last_config.generation;

    TEST_ASSERT_TRUE(ble_adv_manager_start_command_current(
                         retired_generation));
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_COMPLETE;
    event.status = ESP_FAIL;
    event.host_reset_pending = true;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(1U, s_start_calls);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());

    event.type = BLE_PORT_EVENT_RESET;
    event.host_reset_pending = false;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_TRUE(!ble_adv_manager_start_command_current(
                         retired_generation));
    TEST_ASSERT_EQUAL(1U, s_start_calls);

    event.type = BLE_PORT_EVENT_SYNC;
    event.status = 0;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(2U, s_start_calls);
    TEST_ASSERT_TRUE(s_last_config.generation != retired_generation);
    TEST_ASSERT_TRUE(!ble_adv_manager_start_command_current(
                         retired_generation));
    TEST_ASSERT_TRUE(ble_adv_manager_start_command_current(
                         s_last_config.generation));
    _emit_adv_started(0);
}

static void test_reset_retires_queued_stop_command(void)
{
    ble_adv_lease_t lease;
    ble_port_event_t event;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(true));
    const uint32_t retired_generation = s_last_stop_generation;

    TEST_ASSERT_TRUE(ble_adv_manager_stop_command_current(
                         retired_generation));
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_RESET;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_TRUE(!ble_adv_manager_stop_command_current(
                         retired_generation));
    event.type = BLE_PORT_EVENT_SYNC;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
}

static void test_adv_complete_cannot_discharge_failed_stop(void)
{
    ble_adv_lease_t lease;
    ble_port_event_t event;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(true));
    _emit_adv_stopped(ESP_FAIL);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(100U, ble_adv_manager_get_retry_remaining_ms());

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_COMPLETE;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(100U, ble_adv_manager_get_retry_remaining_ms());

    s_now_ms += 100U;
    ble_adv_manager_poll();
    TEST_ASSERT_TRUE(ble_adv_manager_stop_command_current(
                         s_last_stop_generation));
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
}

static void test_reset_quarantines_late_stop(void)
{
    ble_adv_lease_t lease;
    ble_port_event_t event;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(0);
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_RESET;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    const unsigned int stop_calls_after_reset = s_stop_calls;

    event.type = BLE_PORT_EVENT_ADV_STOPPED;
    event.status = 1;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(stop_calls_after_reset, s_stop_calls);
    event.type = BLE_PORT_EVENT_SYNC;
    event.status = 0;
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_handle_event(&event));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
}

static void test_adv_complete_without_connection_restarts(void)
{
    ble_adv_lease_t lease;
    ble_port_event_t event;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
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

static void test_pause_preserves_lease_and_resumes(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(true));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(false));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
}

static void test_pause_reasons_are_independent(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());

    TEST_ASSERT_EQUAL(
        ESP_OK, ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_WINDOW_TRANSITION, true));
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(
        ESP_OK, ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP, true));
    TEST_ASSERT_EQUAL(
        ESP_OK, ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_STARTUP_GATE, true));
    TEST_ASSERT_EQUAL(
        ESP_OK, ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_REVOKE, true));

    const unsigned int starts_while_paused = s_start_calls;

    TEST_ASSERT_EQUAL(
        ESP_OK, ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_WINDOW_TRANSITION, false));
    TEST_ASSERT_EQUAL(
        ESP_OK, ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_STARTUP_GATE, false));
    TEST_ASSERT_EQUAL(
        ESP_OK, ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_REVOKE, false));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(starts_while_paused, s_start_calls);

    /* The compatibility owner is independent too: neither it nor a public
     * owner may release the other's pause. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(true));
    TEST_ASSERT_EQUAL(
        ESP_OK, ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP, false));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
    TEST_ASSERT_EQUAL(starts_while_paused, s_start_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_set_paused(false));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STARTING,
                      ble_adv_manager_get_state());
    _emit_adv_started(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_SLOW,
                      ble_adv_manager_get_state());
}

static void test_pause_retries_failed_stop(void)
{
    ble_adv_lease_t lease;

    _reset_harness();
    _init_manager();
    TEST_ASSERT_EQUAL(ESP_OK, ble_adv_manager_acquire_lease(
                          &lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    _emit_adv_started(0);
    s_stop_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, ble_adv_manager_set_paused(true));
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_FAULTED,
                      ble_adv_manager_get_state());
    s_stop_result = ESP_OK;
    s_now_ms += 100U;
    ble_adv_manager_poll();
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPING,
                      ble_adv_manager_get_state());
    _emit_adv_stopped(0);
    TEST_ASSERT_EQUAL(BLE_ADV_MANAGER_STATE_STOPPED,
                      ble_adv_manager_get_state());
}

static void test_adv_payload_encoding(void)
{
    static const uint8_t short_name[] = "MT";
    static const uint8_t service_uuid[] =
    {
        0x31U, 0x6aU, 0x7bU, 0x2fU, 0x4cU, 0x9cU, 0x04U, 0x9cU,
        0x44U, 0x4fU, 0xf6U, 0x65U, 0x10U, 0x8cU, 0x2aU, 0x8fU,
    };
    static const uint8_t expected[] =
    {
        0x02U, 0x01U, 0x06U,
        0x11U, 0x07U, 0x31U, 0x6aU, 0x7bU, 0x2fU, 0x4cU,
        0x9cU, 0x04U, 0x9cU, 0x44U, 0x4fU, 0xf6U, 0x65U,
        0x10U, 0x8cU, 0x2aU, 0x8fU,
        0x03U, 0x08U, 0x4dU, 0x54U,
    };
    ble_port_adv_config_t config =
    {
        .short_name = short_name,
        .short_name_len = sizeof(short_name) - 1U,
        .service_uuid = service_uuid,
        .bindable = false,
    };
    uint8_t payload[BLE_NIMBLE_ADV_DATA_MAX_BYTES];
    size_t payload_len = 0U;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_nimble_adv_encode(&config, payload, &payload_len));
    TEST_ASSERT_EQUAL(sizeof(expected), payload_len);
    TEST_ASSERT_EQUAL(0, memcmp(expected, payload, sizeof(expected)));

    config.bindable = true;
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_nimble_adv_encode(&config, payload, &payload_len));
    TEST_ASSERT_EQUAL(sizeof(expected), payload_len);
    TEST_ASSERT_EQUAL(0, memcmp(expected, payload, sizeof(expected)));

    config.short_name = NULL;
    config.short_name_len = 0U;
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_nimble_adv_encode(&config, payload, &payload_len));
    TEST_ASSERT_EQUAL(21U, payload_len);

    static const uint8_t overlong_name[] = "123456789";

    config.short_name = overlong_name;
    config.short_name_len = sizeof(overlong_name) - 1U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      ble_nimble_adv_encode(&config, payload, &payload_len));
}

int main(void)
{
    test_acquire_fast_starts_fast_adv();
    test_fast_window_expires_falls_back_to_slow();
    test_release_last_lease_stops();
    test_slow_lease_skips_fast();
    test_connect_stops_adv_disconnect_resumes();
    test_start_failure_faults();
    test_async_start_failure_recovers();
    test_async_start_failure_backoff_is_bounded();
    test_start_target_change_resets_backoff();
    test_async_stop_failure_respects_cooldown();
    test_retry_deadline_wraps_monotonic_clock();
    test_fast_lease_escalates_slow_adv();
    test_bindable_control_is_explicit();
    test_bindable_lease_release_restarts_with_closed_gate();
    test_lease_capacity_enforced();
    test_invalid_arguments_rejected();
    test_reset_and_sync();
    test_bindable_start_stale_after_gate_rolls_back();
    test_bindable_start_stale_during_host_call_is_stopped();
    test_reset_adv_complete_does_not_requeue_retired_start();
    test_reset_retires_queued_stop_command();
    test_adv_complete_cannot_discharge_failed_stop();
    test_reset_quarantines_late_stop();
    test_adv_payload_encoding();
    test_adv_complete_without_connection_restarts();
    test_pause_preserves_lease_and_resumes();
    test_pause_reasons_are_independent();
    test_pause_retries_failed_stop();
    test_stale_adv_started_rejected_by_generation();
    test_stale_adv_stopped_rejected_by_generation();
    test_stop_submission_failure_faults();
    test_release_failure_restores_fast_window();
    test_release_failure_expired_window_stays_closed();
    test_release_failure_at_exact_deadline_stays_closed();
    test_release_last_fast_lease_cancels_window();
    test_paused_keeps_fast_window();
    test_rejected_connect_ignored();
    test_last_lease_stop_failure_retries_on_event();
    test_deinit_rejects_calls();
    printf("ble_adv_manager: all tests passed\n");
    return 0;
}
