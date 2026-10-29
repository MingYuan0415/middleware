#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdatomic.h>
#include "ble_adv_manager.h"
#include "ble_gap_manager.h"
#include "ble_gatt_registry.h"
#include "ble_link_gatt.h"
#include "ble_link_service.h"
#include "ble_link_session.h"
#include "ble_nimble_port.h"
#include "ble_port_ops.h"
#include "ble_runtime.h"
#include "ble_tx_scheduler.h"
#include "connectivity_manager.h"
#include "device_link_service.h"
#include "device_link_wifi_adapter.h"
#include "device_link_v1.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_freertos.h"
#include "nv_storage.h"

EVENT_BUS_DEFINE_ID(CONNECTIVITY_MANAGER_MSG);
static StaticSemaphore_t s_event_bus_mutexes[2];
static unsigned s_event_bus_mutex_count;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (s_event_bus_mutex_count >= 2U)
    {
        return NULL;
    }
    return xSemaphoreCreateMutexStatic(&s_event_bus_mutexes[s_event_bus_mutex_count++]);
}

static StaticSemaphore_t s_event_bus_signal;

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return xSemaphoreCreateBinaryStatic(&s_event_bus_signal);
}

#define BOOT_ID UINT64_C(0x1020304050607080)
#define CONN_HANDLE UINT16_C(7)
#define RX_HANDLE UINT16_C(0x10)
#define TX_HANDLE UINT16_C(0x12)
#define TEST_WINDOW_MS UINT32_C(120000)

#define TEST_ASSERT(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            fprintf(stderr, "assertion failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #condition); \
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
            fprintf(stderr, "assertion failed at %s:%d: %s == %s (%lld != %lld)\n", \
                    __FILE__, __LINE__, #expected, #actual, expected_value, \
                    actual_value); \
            abort(); \
        } \
    } while (0)

static const uint8_t s_device_link_uuid[16] =
{
    0x31, 0x6a, 0x7b, 0x2f, 0x4c, 0x9c, 0x04, 0x9c,
    0x44, 0x4f, 0xf6, 0x65, 0x10, 0x8c, 0x2a, 0x8f,
};
static const uint8_t s_command_rx_uuid[16] =
{
    0x31, 0x6a, 0x7b, 0x2f, 0x4c, 0x9c, 0x04, 0x9c,
    0x44, 0x4f, 0xf6, 0x65, 0x11, 0x8c, 0x2a, 0x8f,
};
static const uint8_t s_server_tx_uuid[16] =
{
    0x31, 0x6a, 0x7b, 0x2f, 0x4c, 0x9c, 0x04, 0x9c,
    0x44, 0x4f, 0xf6, 0x65, 0x12, 0x8c, 0x2a, 0x8f,
};
static const uint8_t s_peer_address[6] = { 1U, 2U, 3U, 4U, 5U, 6U };

static uint8_t s_last_frame[DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES];
static size_t s_last_frame_len;
static unsigned s_frame_count;
static unsigned s_received_frames;
static ble_link_operation_identity_t s_frame_identity;
static atomic_uint s_accepted_frames;
static atomic_uint s_terminal_frames;
static atomic_uint s_last_manager_id;
static atomic_bool s_port_running;
static bool s_gatt_initialized;
static uint32_t s_current_generation;
static pthread_mutex_t s_observation_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_frame_changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_scheduler_lock = PTHREAD_MUTEX_INITIALIZER;

static void _scheduler_lock(void *arg)
{
    (void)arg;
    (void)pthread_mutex_lock(&s_scheduler_lock);
}

static void _scheduler_unlock(void *arg)
{
    (void)arg;
    (void)pthread_mutex_unlock(&s_scheduler_lock);
}
static atomic_uint s_wrapped_completion_calls;
static atomic_bool s_block_completion;
static uint32_t s_completed_operation;
static esp_err_t s_completion_result;


static uint32_t _u32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8U) |
           ((uint32_t)value[2] << 16U) | ((uint32_t)value[3] << 24U);
}

static void _capture_frame(const uint8_t *data, size_t len)
{
    TEST_ASSERT(data != NULL && len <= sizeof(s_last_frame));
    ble_link_operation_identity_t identity;
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_get_in_flight_identity(&identity));
    (void)pthread_mutex_lock(&s_observation_lock);
    s_frame_identity = identity;
    memcpy(s_last_frame, data, len);
    s_last_frame_len = len;
    ++s_frame_count;
    if (len >= 3U && data[0] != DEVICE_LINK_V1_EVENT_MARKER &&
            data[2] == DEVICE_LINK_V1_STATUS_ACCEPTED)
    {
        ++s_accepted_frames;
    }
    if (len >= 2U && data[0] == DEVICE_LINK_V1_EVENT_MARKER &&
            (data[1] == DEVICE_LINK_V1_SCAN_COMPLETE ||
             data[1] == DEVICE_LINK_V1_OPERATION_COMPLETE))
    {
        ++s_terminal_frames;
    }
    (void)pthread_cond_broadcast(&s_frame_changed);
    (void)pthread_mutex_unlock(&s_observation_lock);
}

static esp_err_t _fake_adv_start(const ble_port_adv_config_t *config)
{
    TEST_ASSERT(config != NULL);
    const ble_port_event_t event =
    {
        .type = BLE_PORT_EVENT_ADV_STARTED,
        .status = ESP_OK,
        .generation = config->generation,
    };
    (void)ble_event_router_dispatch(&event);
    return ESP_OK;
}

static esp_err_t _fake_adv_stop(uint32_t generation)
{
    const ble_port_event_t event =
    {
        .type = BLE_PORT_EVENT_ADV_STOPPED,
        .status = ESP_OK,
        .generation = generation,
    };
    (void)ble_event_router_dispatch(&event);
    return ESP_OK;
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
    TEST_ASSERT(conn_handle == CONN_HANDLE && value_handle == TX_HANDLE);
    _capture_frame(data, len);
    return ESP_OK;
}

static const ble_port_ops_t s_port_ops =
{
    .adv_start = _fake_adv_start,
    .adv_stop = _fake_adv_stop,
    .notify = _fake_notify,
    .indicate = _fake_indicate,
};

static uint32_t _now_ms(void)
{
    return (uint32_t)xTaskGetTickCount();
}

static void _scheduler_completed(const ble_tx_scheduler_result_t *result,
                                 void *arg)
{
    (void)arg;
    if (result->kind == BLE_TX_SCHEDULER_KIND_INDICATE && result->flow_id != 0U)
    {
        (void)ble_link_service_tx_completed(&result->identity, result->status);
    }
}

static const ble_adv_manager_config_t s_adv_config =
{
    .fast_interval_ms = 40U,
    .slow_interval_ms = 200U,
    .fast_window_ms = TEST_WINDOW_MS,
    .short_name = (const uint8_t *)"MT",
    .short_name_len = 2U,
    .service_uuid = s_device_link_uuid,
    .now_ms = _now_ms,
    .ops = &s_port_ops,
};

static void _transport_event(const ble_port_event_t *event, void *arg)
{
    (void)arg;
    TEST_ASSERT(event != NULL);
    switch (event->type)
    {
    case BLE_PORT_EVENT_CONNECT:
    {
        const esp_err_t result = ble_link_session_handle_event(
                                     event->identity.generation,
                                     BLE_LINK_SESSION_EVENT_ACL_CONNECTED);
        TEST_ASSERT_EQUAL(ESP_OK, result);
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_on_connect(
                              event->identity.generation, event->conn_handle));
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_set_connection(
                              event->identity.generation, event->conn_handle,
                              0U, s_peer_address));
        break;
    }
    case BLE_PORT_EVENT_DISCONNECT:
    {
        ble_gap_manager_snapshot_t snapshot;
        TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
        const bool current = snapshot.connected &&
                             snapshot.generation == event->identity.generation &&
                             snapshot.conn_handle == event->conn_handle;
        ble_gap_manager_event_t gap =
        {
            .type = BLE_GAP_MANAGER_EVENT_DISCONNECT,
            .identity = event->identity,
            .conn_handle = event->conn_handle,
        };
        const esp_err_t result = ble_gap_manager_handle_event(&gap);
        TEST_ASSERT(result == ESP_OK || result == ESP_ERR_NOT_FOUND);
        if (current)
        {
            ble_tx_scheduler_reset();
            (void)ble_link_session_handle_event(event->identity.generation,
                                                BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED);
            const esp_err_t cleared = ble_link_service_clear_session_state_if_current(
                                          &event->identity);
            TEST_ASSERT(cleared == ESP_OK || cleared == ESP_ERR_NOT_FOUND);
        }
        break;
    }
    case BLE_PORT_EVENT_RESET:
    {
        ble_gap_manager_snapshot_t snapshot;
        TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
        if (snapshot.connected)
        {
            (void)ble_link_session_handle_event(
                snapshot.generation, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED);
        }
        ble_link_service_clear_session_state();
        ble_tx_scheduler_reset();
        const ble_gap_manager_event_t reset =
        {
            .type = BLE_GAP_MANAGER_EVENT_RESET,
        };
        TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&reset));
        break;
    }
    case BLE_PORT_EVENT_MTU:
    {
        ble_gap_manager_event_t gap =
        {
            .type = BLE_GAP_MANAGER_EVENT_MTU,
            .identity = event->identity,
            .conn_handle = event->conn_handle,
            .mtu = event->mtu,
        };
        TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&gap));
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_set_att_mtu(
                              event->identity.generation, event->conn_handle,
                              event->mtu));
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_set_att_mtu(
                              event->identity.generation, event->conn_handle,
                              event->mtu));
        break;
    }
    case BLE_PORT_EVENT_SUBSCRIBE:
    {
        ble_gap_manager_event_t gap =
        {
            .type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE,
            .identity = event->identity,
            .conn_handle = event->conn_handle,
            .attr_handle = event->attr_handle,
            .subscribed = event->subscribed,
            .notify = event->notify,
            .indicate = event->indicate,
        };
        TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&gap));
        if (event->attr_handle == TX_HANDLE && event->indicate)
        {
            ble_link_service_set_transport_admitted(
                event->identity.generation, event->conn_handle, true);
        }
        break;
    }
    case BLE_PORT_EVENT_ENC_CHANGE:
        if (event->encrypted)
        {
            TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                                  event->identity.generation,
                                  BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
        }
        break;
    case BLE_PORT_EVENT_NOTIFY_TX:
        (void)ble_tx_scheduler_handle_notify_tx(event);
        break;
    default:
        break;
    }
}

static esp_err_t _port_init(void)
{
    static const ble_tx_scheduler_config_t scheduler =
    {
        .queue_depth = 8U,
        .max_frame_bytes = DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES,
        .ops = &s_port_ops,
        .completed = _scheduler_completed,
        .lock = _scheduler_lock,
        .unlock = _scheduler_unlock,
        .lock_arg = NULL,
    };
    ble_event_router_init();
    ble_gap_manager_init();
    ble_adv_manager_init(&s_adv_config);
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_init(&scheduler));
    if (s_gatt_initialized)
    {
        ble_link_gatt_update_handles();
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_restart());
    }
    else
    {
        ble_gatt_registry_init();
        const ble_link_gatt_config_t gatt =
        {
            .boot_id = BOOT_ID,
            .att_mtu = DEVICE_LINK_V1_REQUIRED_ATT_MTU,
            .tx_queue_depth = 8U,
        };
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_init(&gatt));
        TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                              s_command_rx_uuid, RX_HANDLE));
        TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                              s_server_tx_uuid, TX_HANDLE));
        ble_link_gatt_update_handles();
        s_gatt_initialized = true;
    }
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_register(_transport_event, NULL));
    s_port_running = true;
    return ESP_OK;
}

static esp_err_t _port_start(void)
{
    return ESP_OK;
}

static esp_err_t _port_gate(bool open)
{
    (void)open;
    return ESP_OK;
}

static esp_err_t _port_reset_store(void)
{
    return ESP_OK;
}

static esp_err_t _port_stop(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, ble_nimble_port_request_disconnect());
    s_port_running = false;
    return ESP_OK;
}

static esp_err_t _port_deinit(void)
{
    ble_tx_scheduler_deinit();
    ble_link_gatt_reset();
    ble_adv_manager_deinit();
    (void)ble_event_router_unregister(_transport_event, NULL);
    s_port_running = false;
    return ESP_OK;
}

static const ble_runtime_host_port_t s_runtime_port =
{
    .init = _port_init,
    .start = _port_start,
    .set_pairing_gate = _port_gate,
    .reset_peer_store = _port_reset_store,
    .stop = _port_stop,
    .deinit = _port_deinit,
};

static const device_link_service_config_t s_service_config =
{
    .runtime_port = &s_runtime_port,
    .task_priority = 2U,
    .window_ms = TEST_WINDOW_MS,
    .startup_mode = DEVICE_LINK_SERVICE_STARTUP_NORMAL,
    .firmware_major = 1U,
    .firmware_minor = 2U,
    .firmware_patch = 3U,
};

/* The production NimBLE port is the hardware boundary of this fixture. */
esp_err_t ble_nimble_port_wait_for_host_sync(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return s_port_running ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t ble_nimble_port_begin_cleanup_drain(void)
{
    return ESP_OK;
}
bool ble_nimble_port_cleanup_pending(void)
{
    return false;
}
esp_err_t ble_nimble_port_set_pairing_window(bool open)
{
    (void)open;
    return ESP_OK;
}
esp_err_t ble_nimble_port_request_disconnect(void)
{
    ble_gap_manager_snapshot_t snapshot;
    if (ble_gap_manager_get_snapshot(&snapshot) != ESP_OK || !snapshot.connected)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const ble_port_event_t event =
    {
        .type = BLE_PORT_EVENT_DISCONNECT,
        .conn_handle = snapshot.conn_handle,
        .identity =
        {
            .generation = snapshot.generation,
            .conn_handle = snapshot.conn_handle,
            .kind = BLE_LINK_OPERATION_DISCONNECT,
        },
    };
    return ble_event_router_dispatch(&event);
}
void ble_nimble_port_numeric_comparison_cancel(void) {}
esp_err_t ble_nimble_port_numeric_comparison_reply(uint64_t token, bool accept)
{
    (void)token;
    (void)accept;
    return ESP_OK;
}
esp_err_t ble_nimble_port_revoke_binding(void)
{
    return ESP_OK;
}
esp_err_t ble_nimble_port_revoke_journal_begin(void)
{
    return ESP_OK;
}
esp_err_t ble_nimble_port_revoke_journal_pending(bool *out_pending)
{
    if (out_pending == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_pending = false;
    return ESP_OK;
}
esp_err_t ble_nimble_port_revoke_journal_end(void)
{
    return ESP_OK;
}

void connectivity_manager_fake_set_request_hook(
    void (*hook)(connectivity_manager_operation_id_t, void *), void *arg);
void connectivity_manager_fake_reset(void);
void connectivity_manager_fake_set_status(
    const connectivity_manager_status_snapshot_t *snapshot);

static void _request_hook(connectivity_manager_operation_id_t id, void *arg)
{
    (void)arg;
    s_last_manager_id = id;
}

esp_err_t __real_ble_link_service_complete_operation(
    uint32_t operation_id, device_link_v1_wifi_failure_t failure,
    const device_link_v1_network_t *networks, uint8_t count,
    const device_link_v1_snapshot_t *snapshot);
bool ble_link_service_test_operation_present(uint32_t operation_id);

esp_err_t __wrap_ble_link_service_complete_operation(
    uint32_t operation_id, device_link_v1_wifi_failure_t failure,
    const device_link_v1_network_t *networks, uint8_t count,
    const device_link_v1_snapshot_t *snapshot)
{
    (void)atomic_fetch_add(&s_wrapped_completion_calls, 1U);
    const esp_err_t result = atomic_load(&s_block_completion) ? ESP_FAIL :
                             __real_ble_link_service_complete_operation(
                                 operation_id, failure, networks, count, snapshot);
    (void)pthread_mutex_lock(&s_observation_lock);
    s_completed_operation = operation_id;
    s_completion_result = result;
    (void)pthread_cond_broadcast(&s_frame_changed);
    (void)pthread_mutex_unlock(&s_observation_lock);
    return result;
}

static void _wait_bridge_result(uint32_t operation_id, esp_err_t result)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    (void)pthread_mutex_lock(&s_observation_lock);
    while (s_completed_operation != operation_id || s_completion_result != result)
    {
        TEST_ASSERT_EQUAL(0, pthread_cond_timedwait(
                              &s_frame_changed, &s_observation_lock, &deadline));
    }
    (void)pthread_mutex_unlock(&s_observation_lock);
}


static void _confirm_frame(const ble_link_operation_identity_t *identity)
{
    const ble_port_event_t event =
    {
        .type = BLE_PORT_EVENT_NOTIFY_TX,
        .identity = *identity,
        .conn_handle = CONN_HANDLE,
        .attr_handle = TX_HANDLE,
        .indication = true,
        .tx_result = BLE_PORT_TX_CONFIRMED,
    };
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
}

static size_t _receive_frame(uint8_t *frame,
                             ble_link_operation_identity_t *identity,
                             const struct timespec *deadline)
{
    for (;;)
    {
        (void)pthread_mutex_lock(&s_observation_lock);
        while (s_frame_count == s_received_frames)
        {
            TEST_ASSERT_EQUAL(0, pthread_cond_timedwait(
                                  &s_frame_changed, &s_observation_lock, deadline));
        }
        const size_t length = s_last_frame_len;
        memcpy(frame, s_last_frame, length);
        *identity = s_frame_identity;
        s_received_frames = s_frame_count;
        (void)pthread_mutex_unlock(&s_observation_lock);
        if (identity->generation == s_current_generation)
        {
            return length;
        }
        /* A reset/disconnect retires any unread frame from the old ACL. */
    }
}

static uint32_t _write_command_response(const uint8_t *value, size_t length,
                                        uint8_t *response, size_t *response_len)
{
    const ble_gatt_registry_characteristic_t *characteristic = NULL;
    ble_gatt_registry_access_context_t context =
    {
        .op = BLE_GATT_REGISTRY_OP_WRITE_CHR,
        .write_data = value,
        .write_len = (uint16_t)length,
    };
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_handle(
                          RX_HANDLE, &characteristic));
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    uint8_t captured_frame[DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES];
    ble_link_operation_identity_t identity;
    for (;;)
    {
        const int result = characteristic->access_cb(
                               CONN_HANDLE, RX_HANDLE, &context, characteristic->arg);
        if (result == 0)
        {
            break;
        }
        TEST_ASSERT_EQUAL(DEVICE_LINK_V1_ATT_TX_INDICATION_PENDING, result);
        (void)_receive_frame(captured_frame, &identity, &deadline);
        _confirm_frame(&identity);
    }
    const uint8_t expected_opcode = value[0] | DEVICE_LINK_V1_RESPONSE_MASK;
    size_t captured_length;
    for (;;)
    {
        captured_length = _receive_frame(captured_frame, &identity, &deadline);
        if (captured_length >= 3U && captured_frame[0] == expected_opcode &&
                captured_frame[1] == value[1])
        {
            break;
        }
        _confirm_frame(&identity);
    }
    TEST_ASSERT(captured_length >= 3U);
    if (response != NULL)
    {
        memcpy(response, captured_frame, captured_length);
    }
    if (response_len != NULL)
    {
        *response_len = captured_length;
    }
    const uint32_t operation_id = captured_length >= 7U ?
                                  _u32(&captured_frame[3]) : 0U;
    _confirm_frame(&identity);
    return operation_id;

}

static uint32_t _write_command(const uint8_t *value, size_t length)
{
    return _write_command_response(value, length, NULL, NULL);
}

static void _query_and_ack(uint32_t operation_id,
                           device_link_v1_operation_t operation,
                           device_link_v1_wifi_failure_t failure)
{
    const uint8_t query[] = { DEVICE_LINK_V1_GET_OPERATION, 9U };
    uint8_t response[DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES];
    size_t response_len = 0U;
    (void)_write_command_response(query, sizeof(query), response, &response_len);
    TEST_ASSERT(response_len >= 11U);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_OK, response[2]);
    TEST_ASSERT_EQUAL(operation_id, _u32(&response[3]));
    TEST_ASSERT_EQUAL(operation, response[7]);
    TEST_ASSERT_EQUAL(failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE ?
                      DEVICE_LINK_V1_OPERATION_SUCCEEDED :
                      DEVICE_LINK_V1_OPERATION_FAILED, response[8]);
    TEST_ASSERT_EQUAL(failure, response[9]);
    if (operation == DEVICE_LINK_V1_OPERATION_SCAN &&
            failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE)
    {
        static const uint8_t network[] =
        {
            11U, 'r', 'e', 'c', 'o', 'v', 'e', 'r', 'y', '-', 'a', 'p',
            DEVICE_LINK_V1_WIFI_OPEN, (uint8_t) -40,
        };
        TEST_ASSERT_EQUAL(1U, response[10]);
        TEST_ASSERT_EQUAL(11U + sizeof(network), response_len);
        TEST_ASSERT(memcmp(&response[11], network, sizeof(network)) == 0);
    }
    else
    {
        TEST_ASSERT_EQUAL(0U, response[10]);
    }

    const uint8_t ack[] =
    {
        DEVICE_LINK_V1_ACK_OPERATION, 10U,
        (uint8_t)operation_id, (uint8_t)(operation_id >> 8U),
        (uint8_t)(operation_id >> 16U), (uint8_t)(operation_id >> 24U),
    };
    (void)_write_command_response(ack, sizeof(ack), response, &response_len);
    TEST_ASSERT(response_len >= 3U);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_OK, response[2]);
    TEST_ASSERT(!ble_link_service_test_operation_present(operation_id));
    (void)_write_command_response(query, sizeof(query), response, &response_len);
    TEST_ASSERT_EQUAL(3U, response_len);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_NOT_FOUND, response[2]);
}

static uint32_t _connect(void)
{
    ble_gap_manager_event_t gap =
    {
        .type = BLE_GAP_MANAGER_EVENT_CONNECT,
        .status = 0,
        .conn_handle = CONN_HANDLE,
    };
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&gap));
    ble_gap_manager_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    const uint32_t generation = snapshot.generation;
    const ble_port_event_t connected =
    {
        .type = BLE_PORT_EVENT_CONNECT,
        .status = 0,
        .accepted = true,
        .conn_handle = CONN_HANDLE,
        .identity =
        {
            .generation = generation,
            .conn_handle = CONN_HANDLE,
            .kind = BLE_LINK_OPERATION_CONNECT,
        },
    };
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&connected));
    const ble_port_event_t mtu =
    {
        .type = BLE_PORT_EVENT_MTU,
        .conn_handle = CONN_HANDLE,
        .mtu = DEVICE_LINK_V1_REQUIRED_ATT_MTU,
        .identity =
        {
            .generation = generation,
            .conn_handle = CONN_HANDLE,
            .kind = BLE_LINK_OPERATION_MTU,
        },
    };
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&mtu));
    const ble_port_event_t subscribe =
    {
        .type = BLE_PORT_EVENT_SUBSCRIBE,
        .conn_handle = CONN_HANDLE,
        .attr_handle = TX_HANDLE,
        .subscribed = true,
        .indicate = true,
        .identity =
        {
            .generation = generation,
            .conn_handle = CONN_HANDLE,
            .kind = BLE_LINK_OPERATION_SUBSCRIBE,
        },
    };
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&subscribe));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(true, 1U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(generation, true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          generation, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    const ble_port_event_t encrypted =
    {
        .type = BLE_PORT_EVENT_ENC_CHANGE,
        .conn_handle = CONN_HANDLE,
        .encrypted = true,
        .identity =
        {
            .generation = generation,
            .conn_handle = CONN_HANDLE,
            .kind = BLE_LINK_OPERATION_ENCRYPT_CHANGE,
        },
    };
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&encrypted));
    s_current_generation = generation;
    return generation;
}

static void _disconnect(uint32_t generation)
{
    const ble_port_event_t event =
    {
        .type = BLE_PORT_EVENT_DISCONNECT,
        .conn_handle = CONN_HANDLE,
        .identity =
        {
            .generation = generation,
            .conn_handle = CONN_HANDLE,
            .kind = BLE_LINK_OPERATION_DISCONNECT,
        },
    };
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
}
static void _reset_event(void)
{
    const ble_port_event_t event =
    {
        .type = BLE_PORT_EVENT_RESET,
    };
    TEST_ASSERT_EQUAL(ESP_OK, ble_event_router_dispatch(&event));
}

static void _publish_status(connectivity_manager_operation_id_t manager_id,
                            uint64_t generation, bool complete,
                            esp_err_t result,
                            connectivity_manager_failure_t failure,
                            connectivity_manager_state_t state)
{
    connectivity_manager_status_snapshot_t status;
    memset(&status, 0, sizeof(status));
    status.generation = generation;
    status.operation_id = manager_id;
    status.state = state;
    status.failure = failure;
    status.last_error = result;
    status.operation_complete = complete;
    status.available = true;
    status.radio_available = true;
    status.saved_profile = true;
    status.profile_persisted = true;
    memcpy(status.ssid, "stored-ap", sizeof("stored-ap"));
    TEST_ASSERT_EQUAL(ESP_OK, event_bus_publish(
                          CONNECTIVITY_MANAGER_MSG,
                          CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                          &status, sizeof(status), 0U));
}

static void _publish_scan(connectivity_manager_operation_id_t manager_id,
                          uint64_t generation, esp_err_t result)
{
    connectivity_manager_scan_snapshot_t scan;
    memset(&scan, 0, sizeof(scan));
    scan.generation = generation;
    scan.operation_id = manager_id;
    scan.last_error = result;
    scan.running = false;
    scan.record_count = result == ESP_OK ? 1U : 0U;
    if (result == ESP_OK)
    {
        strcpy(scan.records[0].ssid, "recovery-ap");
        scan.records[0].security = CONNECTIVITY_MANAGER_SECURITY_OPEN;
        scan.records[0].rssi = -40;
    }
    TEST_ASSERT_EQUAL(ESP_OK, event_bus_publish(
                          CONNECTIVITY_MANAGER_MSG,
                          CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                          &scan, sizeof(scan), 0U));
}

static void _init_fixture(void)
{
    host_freertos_reset_controls();
    nv_storage_fake_reset();
    connectivity_manager_fake_reset();
    connectivity_manager_fake_set_request_hook(_request_hook, NULL);
    (void)pthread_mutex_lock(&s_observation_lock);
    s_last_frame_len = 0U;
    s_frame_count = 0U;
    s_received_frames = 0U;
    s_completed_operation = 0U;
    s_completion_result = ESP_OK;
    (void)pthread_mutex_unlock(&s_observation_lock);
    s_last_manager_id = 0U;
    s_port_running = false;
    s_block_completion = false;
    s_wrapped_completion_calls = 0U;
    s_accepted_frames = 0U;
    s_terminal_frames = 0U;
    const connectivity_manager_status_snapshot_t manager_status =
    {
        .generation = 2U,
        .state = CONNECTIVITY_MANAGER_STATE_IP_READY,
        .failure = CONNECTIVITY_MANAGER_FAILURE_NONE,
        .available = true,
        .radio_available = true,
        .saved_profile = true,
        .profile_persisted = true,
        .ipv4_address = UINT32_C(0x01020304),
        .ssid = "stored-ap",
    };
    connectivity_manager_fake_set_status(&manager_status);
    TEST_ASSERT_EQUAL(ESP_OK, event_bus_init());
    TEST_ASSERT_EQUAL(ESP_OK, device_link_service_init(&s_service_config));
    const uint32_t generation = _connect();
    ble_link_dispatcher_facts_t facts;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_get_facts(generation, &facts));
    TEST_ASSERT(facts.encrypted);
    bool bond_verified = false;
    bool identity_known = false;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_get_security_facts(
                          generation, &bond_verified, &identity_known));
    TEST_ASSERT(bond_verified && identity_known);
}

static void _deinit_fixture(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, device_link_service_deinit(2000U));
    TEST_ASSERT(host_freertos_wait_no_tasks(1000U));
}

static void test_disconnect_recovers_scan_and_connect(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 1U };
    const uint32_t scan_id = _write_command(scan, sizeof(scan));
    TEST_ASSERT(scan_id != 0U && s_last_manager_id != 0U);
    const uint32_t first_generation = s_current_generation;
    const uint32_t first_manager_id = s_last_manager_id;
    _disconnect(first_generation);

    _publish_scan(first_manager_id, 1U, ESP_OK);
    _wait_bridge_result(scan_id, ESP_OK);
    TEST_ASSERT(ble_link_service_test_operation_present(scan_id));
    const unsigned accepted = atomic_load(&s_accepted_frames);
    const unsigned terminals = atomic_load(&s_terminal_frames);

    const uint32_t second_generation = _connect();
    TEST_ASSERT(second_generation != first_generation);
    TEST_ASSERT(ble_link_service_test_operation_present(scan_id));
    _disconnect(first_generation);
    _query_and_ack(scan_id, DEVICE_LINK_V1_OPERATION_SCAN,
                   DEVICE_LINK_V1_WIFI_FAILURE_NONE);
    TEST_ASSERT_EQUAL(accepted, atomic_load(&s_accepted_frames));
    TEST_ASSERT_EQUAL(terminals, atomic_load(&s_terminal_frames));
    const uint8_t connect[] = { DEVICE_LINK_V1_CONNECT, 2U };
    const uint32_t connect_id = _write_command(connect, sizeof(connect));
    TEST_ASSERT(ble_link_service_test_operation_present(connect_id));
    TEST_ASSERT(connect_id != 0U && s_last_manager_id != first_manager_id);
    const uint32_t connect_manager_id = s_last_manager_id;
    _publish_status(connect_manager_id, 3U, true, ESP_OK,
                    CONNECTIVITY_MANAGER_FAILURE_NONE,
                    CONNECTIVITY_MANAGER_STATE_IP_READY);
    _wait_bridge_result(connect_id, ESP_OK);
    _query_and_ack(connect_id, DEVICE_LINK_V1_OPERATION_CONNECT,
                   DEVICE_LINK_V1_WIFI_FAILURE_NONE);
    const uint8_t reset_scan[] = { DEVICE_LINK_V1_SCAN, 5U };
    const uint32_t reset_scan_id = _write_command(reset_scan, sizeof(reset_scan));
    const uint32_t reset_manager_id = s_last_manager_id;
    _reset_event();
    _publish_scan(reset_manager_id, 4U, ESP_OK);
    _wait_bridge_result(reset_scan_id, ESP_OK);
    const uint32_t reset_generation = _connect();
    TEST_ASSERT(reset_generation != second_generation);
    _query_and_ack(reset_scan_id, DEVICE_LINK_V1_OPERATION_SCAN,
                   DEVICE_LINK_V1_WIFI_FAILURE_NONE);
    puts("recovery: disconnect/reset -> original ID, succeeded, complete scan payload, ACK -> not found");
}

static void test_transient_terminal_retry_and_late_completion_isolation(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 3U };
    const uint32_t operation_id = _write_command(scan, sizeof(scan));
    const uint32_t manager_id = atomic_load(&s_last_manager_id);
    s_block_completion = true;
    _publish_scan(manager_id, 10U, ESP_FAIL);
    _wait_bridge_result(operation_id, ESP_FAIL);
    _disconnect(s_current_generation);
    /* A newer backend success cannot replace the retained failure. */
    _publish_scan(manager_id, 11U, ESP_OK);
    s_block_completion = false;
    ble_link_service_wake_owner();
    _wait_bridge_result(operation_id, ESP_OK);
    (void)_connect();
    _query_and_ack(operation_id, DEVICE_LINK_V1_OPERATION_SCAN,
                   DEVICE_LINK_V1_WIFI_FAILURE_RADIO);
    puts("recovery: blocked terminal -> disconnect -> worker retry retains original failure");
}

static void test_disabled_bluetooth_deadline_and_deinit_retirement(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 4U };
    const uint32_t operation_id = _write_command(scan, sizeof(scan));
    const uint32_t manager_id = atomic_load(&s_last_manager_id);
    TEST_ASSERT_EQUAL(ESP_OK, device_link_service_set_enabled(false, 2000U));
    host_freertos_advance_ticks(pdMS_TO_TICKS(DEVICE_LINK_V1_OPERATION_TIMEOUT_MS));
    ble_link_service_wake_owner();
    /* A late backend result must observe the worker's timeout tombstone. */
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    while (ble_link_service_operation_timeout_remaining_ms() != UINT32_MAX)
    {
        struct timespec now;
        (void)clock_gettime(CLOCK_REALTIME, &now);
        TEST_ASSERT(now.tv_sec < deadline.tv_sec);
        vTaskDelay(1U);
    }
    _publish_scan(manager_id, 20U, ESP_OK);
    _wait_bridge_result(operation_id, ESP_ERR_NOT_FOUND);
    TEST_ASSERT_EQUAL(ESP_OK, device_link_service_set_enabled(true, 2000U));
    (void)_connect();
    _query_and_ack(operation_id, DEVICE_LINK_V1_OPERATION_SCAN,
                   DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT);

    const uint32_t retired_id = _write_command(scan, sizeof(scan));
    TEST_ASSERT(retired_id != operation_id);
    const uint32_t retired_manager_id = atomic_load(&s_last_manager_id);
    _deinit_fixture();
    const unsigned completions = atomic_load(&s_wrapped_completion_calls);
    _publish_scan(retired_manager_id, 21U, ESP_OK);
    device_link_wifi_adapter_tick();
    TEST_ASSERT_EQUAL(completions, atomic_load(&s_wrapped_completion_calls));
    puts("recovery: Bluetooth-off worker deadline -> timeout record; deinit retires pending bridge");
}


int main(void)
{
    _init_fixture();
    test_disconnect_recovers_scan_and_connect();
    test_transient_terminal_retry_and_late_completion_isolation();
    test_disabled_bluetooth_deadline_and_deinit_retirement();
    puts("ble wifi recovery: passed");
    return 0;
}
