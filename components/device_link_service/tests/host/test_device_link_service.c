#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host_freertos.h"

#include "ble_adv_manager.h"
#include "ble_link_session.h"
#include "ble_port_ops.h"
#include "ble_runtime.h"
#include "device_link_service.h"
#include "event_bus.h"

#define TEST_WINDOW_MS 200U
#define TEST_FAST_INTERVAL_MS 100U
#define TEST_SLOW_INTERVAL_MS 700U
#define TEST_FAST_WINDOW_MS 30000U

static const uint8_t s_test_service_uuid[16] =
{
    0xa3, 0x4e, 0x85, 0x57, 0x11, 0x3d, 0x8a, 0xa2,
    0x59, 0x4e, 0xbb, 0xb4, 0x92, 0x31, 0x20, 0x3e,
};

static pthread_mutex_t s_adv_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_adv_started;
static bool s_adv_stopped;
static uint16_t s_adv_interval_ms;
static uint32_t s_adv_generation;
static uint8_t s_adv_service_data[5];
static size_t s_adv_service_data_len;
static unsigned s_adv_start_count;
static unsigned s_adv_stop_count;
static bool s_fail_adv_start;
static bool s_port_started;
static bool s_port_stopped;

static void _adv_lock_cb(void *arg)
{
    (void)arg;
    (void)pthread_mutex_lock(&s_adv_lock);
}

static void _adv_unlock_cb(void *arg)
{
    (void)arg;
    (void)pthread_mutex_unlock(&s_adv_lock);
}

static uint32_t _adv_now_ms(void)
{
    return (uint32_t)xTaskGetTickCount();
}

static esp_err_t _fake_adv_start(const ble_port_adv_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_fail_adv_start)
    {
        return ESP_FAIL;
    }
    s_adv_started = true;
    s_adv_stopped = false;
    s_adv_interval_ms = config->interval_ms;
    s_adv_generation = config->generation;
    s_adv_service_data_len = config->service_data_len;
    if (config->service_data != NULL && config->service_data_len > 0U)
    {
        memcpy(s_adv_service_data, config->service_data,
               config->service_data_len);
    }
    s_adv_start_count++;
    return ESP_OK;
}

static esp_err_t _fake_adv_stop(void)
{
    s_adv_started = false;
    s_adv_stopped = true;
    s_adv_stop_count++;
    return ESP_OK;
}

static const ble_port_ops_t s_test_ops =
{
    .adv_start = _fake_adv_start,
    .adv_stop = _fake_adv_stop,
    .notify = NULL,
    .indicate = NULL,
};

static ble_adv_manager_config_t s_adv_config;
static uint8_t s_adv_payload_buffer[5];
static const ble_runtime_host_port_t s_test_port;

static esp_err_t _fake_port_init(void)
{
    memset(&s_adv_config, 0, sizeof(s_adv_config));
    s_adv_config.fast_interval_ms = TEST_FAST_INTERVAL_MS;
    s_adv_config.slow_interval_ms = TEST_SLOW_INTERVAL_MS;
    s_adv_config.fast_window_ms = TEST_FAST_WINDOW_MS;
    s_adv_config.short_name = (const uint8_t *)"MT";
    s_adv_config.short_name_len = 2U;
    s_adv_config.service_uuid = s_test_service_uuid;
    s_adv_config.adv_version = 1U;
    s_adv_config.now_ms = _adv_now_ms;
    s_adv_config.arm_timer = NULL;
    s_adv_config.timer_arg = NULL;
    s_adv_config.ops = &s_test_ops;
    s_adv_config.lock = _adv_lock_cb;
    s_adv_config.unlock = _adv_unlock_cb;
    s_adv_config.lock_arg = NULL;
    s_adv_payload_buffer[0] = 1U;
    s_adv_payload_buffer[1] = 0U;
    s_adv_payload_buffer[2] = 0U;
    s_adv_payload_buffer[3] = 0U;
    s_adv_payload_buffer[4] = 0U;
    ble_adv_manager_init(&s_adv_config);
    return ESP_OK;
}

static esp_err_t _fake_port_start(void)
{
    s_port_started = true;
    return ESP_OK;
}

static esp_err_t _fake_port_stop(void)
{
    s_port_stopped = true;
    s_port_started = false;
    return ESP_OK;
}

static esp_err_t _fake_port_deinit(void)
{
    ble_adv_manager_deinit();
    return ESP_OK;
}

static const ble_runtime_host_port_t s_test_port =
{
    .init = _fake_port_init,
    .start = _fake_port_start,
    .stop = _fake_port_stop,
    .deinit = _fake_port_deinit,
};

static device_link_service_config_t s_config;
static uint8_t s_random_counter;

static pthread_mutex_t s_publish_lock = PTHREAD_MUTEX_INITIALIZER;
static device_link_service_snapshot_t s_last_published;
static bool s_published;
static unsigned s_publish_count_value;

esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags)
{
    (void)flags;
    assert(msg_id == DEVICE_LINK_SERVICE_MSG);
    assert(sub_type == DEVICE_LINK_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT);
    assert(payload != NULL &&
           payload_size == sizeof(device_link_service_snapshot_t));
    (void)pthread_mutex_lock(&s_publish_lock);
    memcpy(&s_last_published, payload, sizeof(s_last_published));
    s_published = true;
    s_publish_count_value++;
    (void)pthread_mutex_unlock(&s_publish_lock);
    return ESP_OK;
}

static unsigned _publish_count(void)
{
    (void)pthread_mutex_lock(&s_publish_lock);
    const unsigned count = s_publish_count_value;

    (void)pthread_mutex_unlock(&s_publish_lock);
    return count;
}

static unsigned _wait_publish_count(unsigned minimum, uint32_t timeout_ms)
{
    for (uint32_t elapsed = 0U; elapsed < timeout_ms; elapsed += 2U)
    {
        if (_publish_count() >= minimum)
        {
            return _publish_count();
        }
        const struct timespec delay =
        {
            .tv_sec = 0,
            .tv_nsec = 2000000L,
        };

        (void)nanosleep(&delay, NULL);
    }
    return _publish_count();
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *bytes = buf;

    for (size_t i = 0U; i < len; ++i)
    {
        bytes[i] = s_random_counter++;
    }
}

static void _reset_host(void)
{
    host_freertos_reset_controls();
    s_random_counter = 0U;
    s_adv_started = false;
    s_adv_stopped = false;
    s_adv_interval_ms = 0U;
    s_adv_generation = 0U;
    s_adv_service_data_len = 0U;
    s_adv_start_count = 0U;
    s_adv_stop_count = 0U;
    s_port_started = false;
    s_port_stopped = false;
    (void)pthread_mutex_lock(&s_publish_lock);
    s_published = false;
    s_publish_count_value = 0U;
    memset(&s_last_published, 0, sizeof(s_last_published));
    (void)pthread_mutex_unlock(&s_publish_lock);
}

static void _adv_feed_started(void)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_STARTED;
    event.status = 0;
    event.generation = s_adv_generation;
    assert(ble_adv_manager_handle_event(&event) == ESP_OK);
}

static void _adv_feed_stopped(void)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_ADV_STOPPED;
    event.status = 0;
    (void)ble_adv_manager_handle_event(&event);
    if (s_adv_started)
    {
        _adv_feed_started();
    }
}

static void _adv_converge(void)
{
    if (s_adv_started && !s_adv_stopped)
    {
        _adv_feed_started();
    }
    else if (s_adv_stopped)
    {
        _adv_feed_stopped();
    }
}

static bool _wait_for(bool (*predicate)(void), uint32_t timeout_ms)
{
    for (uint32_t elapsed = 0U; elapsed < timeout_ms; elapsed += 2U)
    {
        if (predicate())
        {
            return true;
        }
        const struct timespec delay =
        {
            .tv_sec = 0,
            .tv_nsec = 2000000L,
        };

        (void)nanosleep(&delay, NULL);
    }
    return predicate();
}

static bool _status_available(void)
{
    device_link_service_status_t status;

    return device_link_service_get_status(&status) == ESP_OK &&
           status.available;
}

static bool _status_active(void)
{
    device_link_service_status_t status;

    return device_link_service_get_status(&status) == ESP_OK &&
           status.active;
}

static bool _status_not_active(void)
{
    device_link_service_status_t status;

    return device_link_service_get_status(&status) == ESP_OK &&
           !status.active;
}

static bool _status_connected(bool connected)
{
    device_link_service_status_t status;

    return device_link_service_get_status(&status) == ESP_OK &&
           status.client_connected == connected;
}

static bool _status_connected_now(void)
{
    return _status_connected(true);
}

static bool _status_disconnected(void)
{
    return _status_connected(false);
}

static bool _status_suspended(void)
{
    device_link_service_status_t status;

    return device_link_service_get_status(&status) == ESP_OK &&
           status.state == DEVICE_LINK_SERVICE_STATE_SUSPENDED;
}

static bool _status_advertising(void)
{
    device_link_service_status_t status;

    return device_link_service_get_status(&status) == ESP_OK &&
           status.state == DEVICE_LINK_SERVICE_STATE_ADVERTISING;
}

static void _pump_ms(uint32_t ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 1000000L,
    };

    for (uint32_t elapsed = 0U; elapsed < ms; elapsed += 2U)
    {
        (void)nanosleep(&delay, NULL);
    }
}

static void _init_service(void)
{
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    assert(device_link_service_init(&s_config) == ESP_OK);
    _adv_converge();
    assert(_wait_for(_status_available, 500U));
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_ADVERTISING);
    assert(!status.active);
    assert(!status.client_connected);
    assert(!status.qr_ready);
    assert(s_port_started);
    assert(s_adv_started);
    assert(s_adv_interval_ms == TEST_SLOW_INTERVAL_MS);
    assert(s_adv_service_data_len == 5U);
    assert(s_adv_service_data[0] == 1U);
    assert(s_adv_service_data[1] == 0U);
    assert(s_adv_service_data[2] == 0U);
    assert(s_adv_service_data[3] == 0U);
    assert(s_adv_service_data[4] == 0U);
}

static void _deinit_service(void)
{
    assert(device_link_service_deinit(DEVICE_LINK_SERVICE_WAIT_FOREVER) ==
           ESP_OK);
    assert(!s_port_started);
    assert(s_port_stopped);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_tasks() == 0U);
    assert(host_freertos_live_semaphores() == 0U);
}

static const char *_qr_field(const char *qr, const char *name, char *output,
                             size_t capacity)
{
    char pattern[64];

    (void)snprintf(pattern, sizeof(pattern), "\"%s\":\"", name);
    const char *start = strstr(qr, pattern);

    if (start == NULL)
    {
        return NULL;
    }
    start += strlen(pattern);
    const char *end = strchr(start, '"');

    if (end == NULL || (size_t)(end - start) >= capacity)
    {
        return NULL;
    }
    const size_t length = (size_t)(end - start);

    memcpy(output, start, length);
    output[length] = '\0';
    return output;
}

static int _base64url_decode_char(char value)
{
    if (value >= 'A' && value <= 'Z')
    {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z')
    {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9')
    {
        return value - '0' + 52;
    }
    if (value == '-')
    {
        return 62;
    }
    if (value == '_')
    {
        return 63;
    }
    return -1;
}

static void _base64url_decode_4(const char *input, uint8_t output[3])
{
    const int a = _base64url_decode_char(input[0]);
    const int b = _base64url_decode_char(input[1]);
    const int c = _base64url_decode_char(input[2]);
    const int d = _base64url_decode_char(input[3]);

    assert(a >= 0 && b >= 0 && c >= 0 && d >= 0);
    output[0] = (uint8_t)((a << 2) | (b >> 4));
    output[1] = (uint8_t)(((b & 0xf) << 4) | (c >> 2));
    output[2] = (uint8_t)(((c & 0x3) << 6) | d);
}

static void _test_bad_configuration(void)
{
    _reset_host();
    memset(&s_config, 0, sizeof(s_config));
    assert(device_link_service_init(NULL) == ESP_ERR_INVALID_ARG);
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    assert(device_link_service_init(&s_config) == ESP_OK);
    assert(device_link_service_init(&s_config) == ESP_ERR_INVALID_STATE);
    assert(device_link_service_deinit(DEVICE_LINK_SERVICE_WAIT_FOREVER) ==
           ESP_OK);
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = NULL;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    assert(device_link_service_init(&s_config) == ESP_ERR_INVALID_ARG);
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 0U;
    s_config.window_ms = TEST_WINDOW_MS;
    assert(device_link_service_init(&s_config) == ESP_ERR_INVALID_ARG);
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = 0U;
    assert(device_link_service_init(&s_config) == ESP_ERR_INVALID_ARG);
}

static void _test_start_failure_rolls_back(void)
{
    _reset_host();
    ble_runtime_host_port_t failing_port = s_test_port;

    failing_port.start = NULL;
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &failing_port;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    assert(device_link_service_init(&s_config) == ESP_ERR_INVALID_ARG);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_tasks() == 0U);
}

static void _test_window_lifecycle(void)
{
    _reset_host();
    _init_service();

    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    _adv_converge();
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_WINDOW);
    assert(status.active);
    assert(status.qr_ready);
    assert(status.window_remaining_ms <= TEST_WINDOW_MS);

    char qr[DEVICE_LINK_SERVICE_QR_MAX_BYTES];
    size_t qr_length = 0U;

    assert(device_link_service_copy_qr(qr, sizeof(qr), &qr_length) == ESP_OK);
    assert(qr_length > 0U);
    char field[64];

    assert(_qr_field(qr, "ver", field, sizeof(field)) != NULL);
    assert(strcmp(field, "link-v1") == 0);
    assert(_qr_field(qr, "name", field, sizeof(field)) != NULL);
    assert(strcmp(field, "MT") == 0);
    assert(_qr_field(qr, "service", field, sizeof(field)) != NULL);
    assert(strcmp(field,
                  "3e203192-b4bb-4e59-a28a-3d1157854ea3") == 0);
    assert(_qr_field(qr, "discriminator", field, sizeof(field)) != NULL);
    assert(strlen(field) == 4U);
    uint8_t qr_discriminator[3];

    _base64url_decode_4(field, qr_discriminator);
    assert(s_adv_service_data_len == 5U);
    assert(s_adv_service_data[1] == 1U);
    assert(memcmp(qr_discriminator, &s_adv_service_data[2], 3U) == 0);
    assert(_qr_field(qr, "pop", field, sizeof(field)) != NULL);
    assert(strlen(field) == 22U);
    char *expires = strstr(qr, "\"expires_in_ms\":");

    assert(expires != NULL);
    assert(strstr(expires, "200") != NULL);
    assert((ble_link_session_get_state_flags() &
            BLE_LINK_STATE_FLAG_BINDABLE) != 0U);
    assert(device_link_service_is_busy());

    host_freertos_advance_ticks(TEST_WINDOW_MS + 100U);
    assert(_wait_for(_status_not_active, 500U));
    _adv_converge();
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(!status.active);
    assert(!status.qr_ready);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_ADVERTISING);
    assert(device_link_service_copy_qr(
               qr, sizeof(qr), &qr_length) == ESP_ERR_NOT_FOUND);
    assert((ble_link_session_get_state_flags() &
            BLE_LINK_STATE_FLAG_BINDABLE) == 0U);
    assert(!device_link_service_is_busy());
    _deinit_service();
}

static void _test_close_window(void)
{
    _reset_host();
    _init_service();

    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    _adv_converge();
    assert(device_link_service_close_window() == ESP_OK);
    assert(_wait_for(_status_not_active, 500U));
    _adv_converge();
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(!status.active);
    assert(!status.qr_ready);
    char qr[DEVICE_LINK_SERVICE_QR_MAX_BYTES];
    size_t qr_length = 0U;

    assert(device_link_service_copy_qr(
               qr, sizeof(qr), &qr_length) == ESP_ERR_NOT_FOUND);
    assert(device_link_service_close_window() == ESP_OK);
    _deinit_service();
}

static void _test_connect_events(void)
{
    _reset_host();
    _init_service();

    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_CONNECT;
    event.conn_handle = 1U;
    event.status = 0;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    assert(_wait_for(_status_connected_now, 500U));
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.client_connected);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_CONNECTED);
    assert(device_link_service_is_busy());

    /* A late disconnect for a retired handle must not clear the live
     * connection. */
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_DISCONNECT;
    event.conn_handle = 7U;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    _pump_ms(20U);
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.client_connected);

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_DISCONNECT;
    event.conn_handle = 1U;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    assert(_wait_for(_status_disconnected, 500U));
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(!status.client_connected);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_ADVERTISING);
    assert(!device_link_service_is_busy());
    _deinit_service();
}

static void _test_suspend_resume(void)
{
    _reset_host();
    _init_service();

    assert(device_link_service_suspend(1000U) == ESP_OK);
    assert(device_link_service_suspend(1000U) == ESP_OK);
    device_link_service_status_t status;

    assert(_wait_for(
               _status_suspended, 500U));
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_SUSPENDED);
    /* The open is admitted to the FIFO but the worker rejects it while
     * suspended, so no window may appear. */
    assert(device_link_service_open_window() == ESP_OK);
    _pump_ms(30U);
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(!status.active);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_SUSPENDED);
    assert(device_link_service_resume(1000U) == ESP_OK);
    assert(device_link_service_resume(1000U) == ESP_OK);
    assert(_wait_for(_status_advertising, 500U));
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(!status.active);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_ADVERTISING);

    /* Resume restores the idle state only; the window opens exclusively on
     * an explicit user open. */
    char qr[DEVICE_LINK_SERVICE_QR_MAX_BYTES];
    size_t qr_length = 0U;

    assert(device_link_service_copy_qr(
               qr, sizeof(qr), &qr_length) == ESP_ERR_NOT_FOUND);
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    _adv_converge();
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.active);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_WINDOW);
    char first_discriminator[5];

    assert(device_link_service_copy_qr(qr, sizeof(qr), &qr_length) == ESP_OK);
    assert(_qr_field(qr, "discriminator", first_discriminator,
                     sizeof(first_discriminator)) != NULL);
    assert(device_link_service_close_window() == ESP_OK);
    assert(_wait_for(_status_not_active, 500U));
    _adv_converge();
    assert(device_link_service_suspend(1000U) == ESP_OK);
    assert(device_link_service_resume(1000U) == ESP_OK);
    assert(_wait_for(_status_advertising, 500U));
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(!status.active);
    assert(device_link_service_copy_qr(
               qr, sizeof(qr), &qr_length) == ESP_ERR_NOT_FOUND);
    _deinit_service();
}

static void _test_close_after_pending_open(void)
{
    _reset_host();
    _init_service();

    /* A close issued immediately after an open must land after it in the
     * worker FIFO: the open publish (active) is followed by the close
     * publish (idle), and the final state must be idle. */
    const unsigned before = _publish_count();

    assert(device_link_service_open_window() == ESP_OK);
    assert(device_link_service_close_window() == ESP_OK);
    assert(_wait_publish_count(before + 2U, 500U) >= before + 2U);
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(!status.active);
    assert(!status.qr_ready);
    char qr[DEVICE_LINK_SERVICE_QR_MAX_BYTES];
    size_t qr_length = 0U;

    assert(device_link_service_copy_qr(
               qr, sizeof(qr), &qr_length) == ESP_ERR_NOT_FOUND);
    _deinit_service();
}

static void _test_remaining_time_publishes_periodically(void)
{
    _reset_host();
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = 60000U;
    assert(device_link_service_init(&s_config) == ESP_OK);
    _adv_converge();
    assert(_wait_for(_status_available, 500U));

    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    _adv_converge();
    const unsigned before = _publish_count();

    host_freertos_advance_ticks(1100U);
    const unsigned first = _wait_publish_count(before + 1U, 500U);

    assert(first > before);
    host_freertos_advance_ticks(1100U);
    const unsigned second = _wait_publish_count(first + 1U, 500U);

    assert(second > first);
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.window_remaining_ms > 0U);
    assert(status.window_remaining_ms <= s_config.window_ms);
    _deinit_service();
}

static bool _status_error_set(void)
{
    device_link_service_status_t status;

    return device_link_service_get_status(&status) == ESP_OK &&
           status.last_error != ESP_OK;
}

static void _test_open_failure_publishes_error(void)
{
    _reset_host();
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = 60000U;
    assert(device_link_service_init(&s_config) == ESP_OK);
    _adv_converge();
    assert(_wait_for(_status_available, 500U));

    /* First open converges to a stop; the restart then fails, faulting the
     * advertising manager. The service window is open but the advertisement
     * is gone, so the owner closes it. */
    s_fail_adv_start = true;
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    _adv_feed_stopped();
    assert(device_link_service_close_window() == ESP_OK);
    assert(_wait_for(_status_not_active, 500U));

    /* A second open now fails synchronously inside the worker and the
     * failure is published through last_error without opening a window. */
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_error_set, 500U));
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(!status.active);
    assert((ble_link_session_get_state_flags() &
            BLE_LINK_STATE_FLAG_BINDABLE) == 0U);
    s_fail_adv_start = false;
    _deinit_service();
}

static void _test_suspend_waits_for_its_own_command(void)
{
    _reset_host();
    _init_service();

    assert(device_link_service_suspend(1000U) == ESP_OK);
    assert(_wait_for(_status_suspended, 500U));
    /* A resume queued ahead of a second suspend must not let the second
     * suspend acknowledge against the stale pre-resume state: the
     * sequence-based acknowledgement waits until the worker applied the
     * resume and then the suspend. */
    assert(device_link_service_resume(1000U) == ESP_OK);
    assert(device_link_service_suspend(1000U) == ESP_OK);
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_SUSPENDED);
    assert(!status.active);
    _deinit_service();
}

static void _test_deinit_while_window_open(void)
{
    _reset_host();
    _init_service();

    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    assert(device_link_service_deinit(DEVICE_LINK_SERVICE_WAIT_FOREVER) ==
           ESP_OK);
    assert(!s_port_started);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_tasks() == 0U);
    assert(host_freertos_live_semaphores() == 0U);
    assert((ble_link_session_get_state_flags() &
            BLE_LINK_STATE_FLAG_BINDABLE) == 0U);
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_ERR_INVALID_STATE);
}

static void _test_reinit_after_deinit(void)
{
    _reset_host();
    _init_service();
    _deinit_service();
    _init_service();
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    _deinit_service();
}

int main(void)
{
    _test_bad_configuration();
    _test_start_failure_rolls_back();
    _test_window_lifecycle();
    _test_close_window();
    _test_connect_events();
    _test_suspend_resume();
    _test_close_after_pending_open();
    _test_remaining_time_publishes_periodically();
    _test_open_failure_publishes_error();
    _test_suspend_waits_for_its_own_command();
    _test_deinit_while_window_open();
    _test_reinit_after_deinit();
    puts("device_link_service host tests passed");
    return 0;
}
