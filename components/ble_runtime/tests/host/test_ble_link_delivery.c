#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_gap_manager.h"
#include "ble_gatt_registry.h"
#include "ble_link_gatt.h"
#include "ble_link_service.h"
#include "ble_link_session.h"
#include "ble_tx_scheduler.h"
#include "device_link_v1.h"

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
            fprintf(stderr, "assertion failed at line %d: %s == %s (%lld != %lld)\n", \
                    __LINE__, #expected, #actual, expected_value, actual_value); \
            abort(); \
        } \
    } while (0)

#define BOOT_ID UINT64_C(72623859790382856)
#define CONN_HANDLE 7U
#define RX_HANDLE 0x10U
#define TX_HANDLE 0x12U
#define FRAME_CAPACITY 32U

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
static const uint8_t s_peer_addr[6] = { 1U, 2U, 3U, 4U, 5U, 6U };

typedef struct captured_frame
{
    uint8_t value[DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES];
    size_t length;
    ble_link_operation_identity_t identity;
} captured_frame_t;

static captured_frame_t s_frames[FRAME_CAPACITY];
static unsigned s_frame_count;
static unsigned s_completion_count;
static unsigned s_wake_count;
static bool s_owner_pending;
static bool s_in_completion;
static uint32_t s_generation;
static uint32_t s_operation_id;
static unsigned s_backend_submits;
static esp_err_t s_last_completion_status;

static void _wake(void *arg)
{
    (void)arg;
    /* The real owner runs later: completion must never synchronously pump. */
    s_owner_pending = true;
    ++s_wake_count;
}

static void _completed(const ble_tx_scheduler_result_t *result, void *arg)
{
    (void)arg;
    TEST_ASSERT_TRUE(!s_in_completion);
    TEST_ASSERT_EQUAL(BLE_TX_SCHEDULER_KIND_INDICATE, result->kind);
    TEST_ASSERT_EQUAL(TX_HANDLE, result->value_handle);
    TEST_ASSERT_TRUE(result->identity.token != 0U);
    TEST_ASSERT_TRUE(result->identity.flow_id != 0U);
    s_in_completion = true;
    ++s_completion_count;
    s_last_completion_status = result->status;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_tx_completed(
                          &result->identity, result->status));
    s_in_completion = false;
}

static esp_err_t _indicate(uint16_t conn_handle, uint16_t value_handle,
                           const uint8_t *data, size_t len)
{
    TEST_ASSERT_TRUE(!s_in_completion);
    TEST_ASSERT_EQUAL(CONN_HANDLE, conn_handle);
    TEST_ASSERT_EQUAL(TX_HANDLE, value_handle);
    TEST_ASSERT_TRUE(s_frame_count < FRAME_CAPACITY);
    TEST_ASSERT_TRUE(len > 0U && len <= DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES);
    captured_frame_t *frame = &s_frames[s_frame_count++];

    memcpy(frame->value, data, len);
    frame->length = len;
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_get_in_flight_identity(
                          &frame->identity));
    TEST_ASSERT_EQUAL(s_generation, frame->identity.generation);
    TEST_ASSERT_EQUAL(CONN_HANDLE, frame->identity.conn_handle);
    TEST_ASSERT_EQUAL(BLE_LINK_OPERATION_TX_INDICATE, frame->identity.kind);
    TEST_ASSERT_TRUE(frame->identity.flow_id != 0U);
    TEST_ASSERT_TRUE(frame->identity.token != 0U);
    const ble_port_event_t sent =
    {
        .type = BLE_PORT_EVENT_NOTIFY_TX,
        .identity = frame->identity,
        .conn_handle = conn_handle,
        .attr_handle = value_handle,
        .indication = true,
        .tx_result = BLE_PORT_TX_SENT,
    };
    const unsigned completions = s_completion_count;

    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_handle_notify_tx(&sent));
    TEST_ASSERT_EQUAL(completions, s_completion_count);
    return ESP_OK;
}

static esp_err_t _unexpected_notify(uint16_t conn_handle, uint16_t value_handle,
                                    const uint8_t *data, size_t len)
{
    (void)conn_handle;
    (void)value_handle;
    (void)data;
    (void)len;
    /* Both port operations are mandatory; this profile must only indicate. */
    TEST_ASSERT_TRUE(false);
    return ESP_ERR_INVALID_STATE;
}

static const ble_port_ops_t s_port_ops =
{
    .notify = _unexpected_notify,
    .indicate = _indicate,
};
static const ble_tx_scheduler_config_t s_scheduler_config =
{
    .queue_depth = 8U,
    .max_frame_bytes = DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES,
    .ops = &s_port_ops,
    .completed = _completed,
};

static device_link_v1_status_t _submit(
    device_link_v1_operation_t operation,
    const device_link_v1_credentials_t *credentials,
    uint32_t operation_id, void *arg)
{
    (void)credentials;
    (void)arg;
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_OPERATION_SCAN, operation);
    TEST_ASSERT_TRUE(operation_id != 0U);
    s_operation_id = operation_id;
    ++s_backend_submits;
    return DEVICE_LINK_V1_STATUS_ACCEPTED;
}

static const ble_link_v1_owner_ops_t s_owner_ops =
{
    .submit_operation = _submit,
};

static ble_link_operation_identity_t _connection_identity(
    uint32_t generation, ble_link_operation_kind_t kind)
{
    const ble_link_operation_identity_t identity =
    {
        .generation = generation,
        .conn_handle = CONN_HANDLE,
        .kind = kind,
    };
    return identity;
}

static void _subscribe(uint32_t generation, bool indicate)
{
    const ble_gap_manager_event_t event =
    {
        .type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE,
        .identity = _connection_identity(generation, BLE_LINK_OPERATION_SUBSCRIBE),
        .conn_handle = CONN_HANDLE,
        .attr_handle = TX_HANDLE,
        .subscribed = indicate,
        .indicate = indicate,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&event));
    ble_link_service_set_transport_admitted(generation, CONN_HANDLE, indicate);
}

static void _connect(void)
{
    const ble_gap_manager_event_t connect =
    {
        .type = BLE_GAP_MANAGER_EVENT_CONNECT,
        .conn_handle = CONN_HANDLE,
    };
    ble_gap_manager_snapshot_t gap;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&connect));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&gap));
    TEST_ASSERT_TRUE(gap.connected);
    s_generation = gap.generation;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          s_generation, BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_set_connection(
                          s_generation, CONN_HANDLE, 1U, s_peer_addr));
    uint32_t mtu = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
    TEST_ASSERT_EQUAL(23U, mtu);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_set_att_mtu(
                          s_generation, CONN_HANDLE,
                          DEVICE_LINK_V1_REQUIRED_ATT_MTU));
    const ble_gap_manager_event_t mtu_event =
    {
        .type = BLE_GAP_MANAGER_EVENT_MTU,
        .identity = _connection_identity(s_generation, BLE_LINK_OPERATION_MTU),
        .conn_handle = CONN_HANDLE,
        .mtu = DEVICE_LINK_V1_REQUIRED_ATT_MTU,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&mtu_event));
    _subscribe(s_generation, true);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          s_generation, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          s_generation, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(
                          s_generation, true));
    const ble_gap_manager_event_t encrypted =
    {
        .type = BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE,
        .identity = _connection_identity(
            s_generation, BLE_LINK_OPERATION_ENCRYPT_CHANGE),
        .conn_handle = CONN_HANDLE,
        .encrypted = true,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&encrypted));
    TEST_ASSERT_TRUE((ble_link_session_get_state_flags() &
                      BLE_LINK_STATE_FLAG_AUTHENTICATED) != 0U);
}

static void _reset(void)
{
    ble_tx_scheduler_deinit();
    ble_link_gatt_reset();
    ble_gap_manager_init();
    memset(s_frames, 0, sizeof(s_frames));
    s_frame_count = 0U;
    s_completion_count = 0U;
    s_wake_count = 0U;
    s_owner_pending = false;
    s_in_completion = false;
    s_operation_id = 0U;
    s_backend_submits = 0U;
    s_last_completion_status = ESP_OK;
    const ble_link_gatt_config_t config =
    {
        .boot_id = BOOT_ID,
        .att_mtu = 23U,
        .tx_queue_depth = 8U,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_init(&s_scheduler_config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          s_command_rx_uuid, RX_HANDLE));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          s_server_tx_uuid, TX_HANDLE));
    ble_link_gatt_update_handles();
    ble_link_gatt_set_work_submit(NULL, NULL);
    ble_link_service_set_v1_ops(&s_owner_ops, NULL);
    ble_link_service_set_worker_wake(_wake, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(true, 0U));
    _connect();
}

static void _pump(void)
{
    TEST_ASSERT_TRUE(!s_in_completion);
    s_owner_pending = false;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
}

static const captured_frame_t *_last_frame(void)
{
    TEST_ASSERT_TRUE(s_frame_count != 0U);
    return &s_frames[s_frame_count - 1U];
}

static void _assert_in_flight(const ble_link_operation_identity_t *expected)
{
    ble_link_operation_identity_t current;

    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_get_in_flight_identity(&current));
    TEST_ASSERT_TRUE(ble_link_operation_identity_equal(expected, &current));
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
}

static void _pump_busy_100(void)
{
    const unsigned count = s_frame_count;
    const ble_link_operation_identity_t identity = _last_frame()->identity;
    const unsigned completions = s_completion_count;

    for (unsigned i = 0U; i < 100U; ++i)
    {
        ble_link_service_wake_owner();
        TEST_ASSERT_TRUE(s_owner_pending);
        _pump();
        TEST_ASSERT_EQUAL(count, s_frame_count);
        TEST_ASSERT_EQUAL(completions, s_completion_count);
        _assert_in_flight(&identity);
    }
}

static void _pump_idle_100(void)
{
    const unsigned count = s_frame_count;

    for (unsigned i = 0U; i < 100U; ++i)
    {
        ble_link_service_wake_owner();
        _pump();
        TEST_ASSERT_EQUAL(count, s_frame_count);
        TEST_ASSERT_TRUE(!ble_tx_scheduler_is_busy());
        TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    }
}

static esp_err_t _tx_event(const ble_link_operation_identity_t *identity,
                           ble_port_tx_result_t result)
{
    const ble_port_event_t event =
    {
        .type = BLE_PORT_EVENT_NOTIFY_TX,
        .identity = *identity,
        .conn_handle = CONN_HANDLE,
        .attr_handle = TX_HANDLE,
        .indication = true,
        .tx_result = result,
    };
    return ble_tx_scheduler_handle_notify_tx(&event);
}

static void _finish(ble_port_tx_result_t result)
{
    const ble_link_operation_identity_t identity = _last_frame()->identity;
    const unsigned completions = s_completion_count;
    const unsigned frames = s_frame_count;
    const unsigned wakes = s_wake_count;

    s_owner_pending = false;
    TEST_ASSERT_EQUAL(ESP_OK, _tx_event(&identity, result));
    TEST_ASSERT_EQUAL(completions + 1U, s_completion_count);
    TEST_ASSERT_EQUAL(result == BLE_PORT_TX_CONFIRMED ? ESP_OK : ESP_FAIL,
                      s_last_completion_status);
    TEST_ASSERT_EQUAL(wakes + 1U, s_wake_count);
    TEST_ASSERT_TRUE(s_owner_pending);
    TEST_ASSERT_EQUAL(frames, s_frame_count);
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_TRUE(!ble_tx_scheduler_is_busy());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, _tx_event(&identity, result));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&identity, ESP_OK));
    TEST_ASSERT_EQUAL(completions + 1U, s_completion_count);
    TEST_ASSERT_EQUAL(wakes + 1U, s_wake_count);
}

static void _timeout(void)
{
    const ble_link_operation_identity_t identity = _last_frame()->identity;
    const unsigned completions = s_completion_count;
    const unsigned frames = s_frame_count;
    const unsigned wakes = s_wake_count;

    s_owner_pending = false;
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_tx_scheduler_handle_indication_timeout(identity.token));
    TEST_ASSERT_EQUAL(completions + 1U, s_completion_count);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_last_completion_status);
    TEST_ASSERT_EQUAL(wakes + 1U, s_wake_count);
    TEST_ASSERT_TRUE(s_owner_pending);
    TEST_ASSERT_EQUAL(frames, s_frame_count);
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_TRUE(!ble_tx_scheduler_is_busy());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_tx_scheduler_handle_indication_timeout(identity.token));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&identity, ESP_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL(completions + 1U, s_completion_count);
    TEST_ASSERT_EQUAL(wakes + 1U, s_wake_count);
}

static int _write(const uint8_t *request, size_t length)
{
    const ble_gatt_registry_characteristic_t *characteristic = NULL;
    ble_gatt_registry_access_context_t context =
    {
        .op = BLE_GATT_REGISTRY_OP_WRITE_CHR,
        .write_data = request,
        .write_len = (uint16_t)length,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_handle(
                          RX_HANDLE, &characteristic));
    return characteristic->access_cb(CONN_HANDLE, RX_HANDLE,
                                     &context, characteristic->arg);
}

static void _command(uint8_t opcode, uint8_t request_id)
{
    const uint8_t request[] = { opcode, request_id };
    const unsigned frames = s_frame_count;

    TEST_ASSERT_EQUAL(0, _write(request, sizeof(request)));
    TEST_ASSERT_EQUAL(frames + 1U, s_frame_count);
    const captured_frame_t *frame = _last_frame();

    TEST_ASSERT_TRUE(frame->length >= 3U);
    TEST_ASSERT_EQUAL(opcode | DEVICE_LINK_V1_RESPONSE_MASK, frame->value[0]);
    TEST_ASSERT_EQUAL(request_id, frame->value[1]);
}

static uint32_t _u32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8U) |
           ((uint32_t)value[2] << 16U) | ((uint32_t)value[3] << 24U);
}

static void _start_scan(void)
{
    _command(DEVICE_LINK_V1_SCAN, 1U);
    const captured_frame_t *frame = _last_frame();

    TEST_ASSERT_EQUAL(7U, frame->length);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_ACCEPTED, frame->value[2]);
    TEST_ASSERT_EQUAL(1U, s_backend_submits);
    TEST_ASSERT_EQUAL(s_operation_id, _u32(&frame->value[3]));
    _pump_busy_100();
}

static void _observe_scanning(void)
{
    const device_link_v1_snapshot_t snapshot =
    {
        .state = DEVICE_LINK_V1_WIFI_SCANNING,
    };

    ble_link_service_observe_snapshot(&snapshot);
    TEST_ASSERT_TRUE(s_owner_pending);
    _pump();
}

static void _complete_backend(void)
{
    const device_link_v1_snapshot_t idle =
    {
        .state = DEVICE_LINK_V1_WIFI_IDLE,
    };
    const device_link_v1_network_t network =
    {
        .ssid = { 'L', 'a', 'b' },
        .ssid_length = 3U,
        .security = DEVICE_LINK_V1_WIFI_OPEN,
        .rssi_dbm = -40,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_complete_operation(
                          s_operation_id, DEVICE_LINK_V1_WIFI_FAILURE_NONE,
                          &network, 1U, &idle));
    TEST_ASSERT_TRUE(s_owner_pending);
}

static void _assert_status(device_link_v1_wifi_state_t state)
{
    const captured_frame_t *frame = _last_frame();

    TEST_ASSERT_EQUAL(5U, frame->length);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_EVENT_MARKER, frame->value[0]);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_WIFI_STATUS, frame->value[1]);
    TEST_ASSERT_EQUAL(state, frame->value[2]);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_WIFI_FAILURE_NONE, frame->value[3]);
    TEST_ASSERT_EQUAL(0U, frame->value[4]);
}

static void _assert_lab(const captured_frame_t *frame, size_t offset)
{
    TEST_ASSERT_EQUAL(offset + 6U, frame->length);
    TEST_ASSERT_EQUAL(3U, frame->value[offset]);
    TEST_ASSERT_TRUE(memcmp(&frame->value[offset + 1U], "Lab", 3U) == 0);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_WIFI_OPEN, frame->value[offset + 4U]);
    TEST_ASSERT_EQUAL(-40, (int8_t)frame->value[offset + 5U]);
}

static void _assert_terminal(void)
{
    const captured_frame_t *frame = _last_frame();

    TEST_ASSERT_TRUE(frame->length >= 8U);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_EVENT_MARKER, frame->value[0]);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_SCAN_COMPLETE, frame->value[1]);
    TEST_ASSERT_EQUAL(s_operation_id, _u32(&frame->value[2]));
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_WIFI_FAILURE_NONE, frame->value[6]);
    TEST_ASSERT_EQUAL(1U, frame->value[7]);
    _assert_lab(frame, 8U);
}

static void _assert_record(void)
{
    const captured_frame_t *frame = _last_frame();

    TEST_ASSERT_TRUE(frame->length >= 11U);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_OK, frame->value[2]);
    TEST_ASSERT_EQUAL(s_operation_id, _u32(&frame->value[3]));
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_OPERATION_SCAN, frame->value[7]);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_OPERATION_SUCCEEDED, frame->value[8]);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_WIFI_FAILURE_NONE, frame->value[9]);
    TEST_ASSERT_EQUAL(1U, frame->value[10]);
    _assert_lab(frame, 11U);
}

static void _ack(void)
{
    const uint8_t request[] =
    {
        DEVICE_LINK_V1_ACK_OPERATION, 4U,
        (uint8_t)s_operation_id, (uint8_t)(s_operation_id >> 8U),
        (uint8_t)(s_operation_id >> 16U), (uint8_t)(s_operation_id >> 24U),
    };
    const unsigned frames = s_frame_count;

    TEST_ASSERT_EQUAL(0, _write(request, sizeof(request)));
    TEST_ASSERT_EQUAL(frames + 1U, s_frame_count);
    TEST_ASSERT_EQUAL(3U, _last_frame()->length);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_ACK_OPERATION | DEVICE_LINK_V1_RESPONSE_MASK,
                      _last_frame()->value[0]);
    TEST_ASSERT_EQUAL(4U, _last_frame()->value[1]);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_OK, _last_frame()->value[2]);
}

static void _query_and_ack(bool fail_first_ack)
{
    _command(DEVICE_LINK_V1_GET_OPERATION, 2U);
    _assert_record();
    _finish(BLE_PORT_TX_CONFIRMED);
    _ack();
    const uint8_t get[] = { DEVICE_LINK_V1_GET_OPERATION, 3U };

    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_ATT_TX_INDICATION_PENDING,
                      _write(get, sizeof(get)));
    if (fail_first_ack)
    {
        _finish(BLE_PORT_TX_ERROR);
        _command(DEVICE_LINK_V1_GET_OPERATION, 3U);
        _assert_record();
        _finish(BLE_PORT_TX_CONFIRMED);
        _ack();
    }
    _finish(BLE_PORT_TX_CONFIRMED);
    _command(DEVICE_LINK_V1_GET_OPERATION, 5U);
    TEST_ASSERT_EQUAL(3U, _last_frame()->length);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_NOT_FOUND, _last_frame()->value[2]);
    _finish(BLE_PORT_TX_CONFIRMED);
    _pump_idle_100();
}

/* Leave exactly the terminal in flight, with SCANNING selected before the
 * backend replaces pending ordinary state with IDLE. */
static void _select_terminal(void)
{
    _start_scan();
    _finish(BLE_PORT_TX_CONFIRMED);
    _observe_scanning();
    _assert_status(DEVICE_LINK_V1_WIFI_SCANNING);
    _pump_busy_100();
    const unsigned frames = s_frame_count;
    const ble_link_operation_identity_t scanning = _last_frame()->identity;

    _complete_backend();
    _pump_busy_100();
    TEST_ASSERT_EQUAL(frames, s_frame_count);
    _assert_in_flight(&scanning);
    const uint8_t query[] = { DEVICE_LINK_V1_GET_OPERATION, 2U };

    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_ATT_TX_INDICATION_PENDING,
                      _write(query, sizeof(query)));
    _finish(BLE_PORT_TX_CONFIRMED);
    _pump();
    _assert_status(DEVICE_LINK_V1_WIFI_IDLE);
    _pump_busy_100();
    _finish(BLE_PORT_TX_CONFIRMED);
    _pump();
    _assert_terminal();
    _pump_busy_100();
    TEST_ASSERT_EQUAL(4U, s_frame_count);
}

static void test_scan_delivery(void)
{
    _reset();
    _select_terminal();
    _finish(BLE_PORT_TX_CONFIRMED);
    _pump_idle_100();
    unsigned scanning = 0U;
    unsigned idle = 0U;
    unsigned terminal = 0U;

    for (unsigned i = 0U; i < s_frame_count; ++i)
    {
        if (s_frames[i].value[0] == DEVICE_LINK_V1_EVENT_MARKER)
        {
            if (s_frames[i].value[1] == DEVICE_LINK_V1_SCAN_COMPLETE)
            {
                ++terminal;
            }
            else if (s_frames[i].value[2] == DEVICE_LINK_V1_WIFI_SCANNING)
            {
                ++scanning;
            }
            else if (s_frames[i].value[2] == DEVICE_LINK_V1_WIFI_IDLE)
            {
                ++idle;
            }
        }
    }
    TEST_ASSERT_EQUAL(1U, scanning);
    TEST_ASSERT_EQUAL(1U, idle);
    TEST_ASSERT_EQUAL(1U, terminal);
    _query_and_ack(false);
    printf("delivery: SCANNING=1 IDLE=1 SCAN_COMPLETE=1 operation_id=%lu "
           "count=1 Lab/OPEN/-40 idle_extra=0 GET_OPERATION/ACK=ok\n",
           (unsigned long)s_operation_id);
}

static void test_terminal_failure(bool timeout)
{
    _reset();
    _select_terminal();
    if (timeout)
    {
        _timeout();
    }
    else
    {
        _finish(BLE_PORT_TX_ERROR);
        TEST_ASSERT_EQUAL(ESP_FAIL, s_last_completion_status);
    }
    _pump_idle_100();
    TEST_ASSERT_EQUAL(4U, s_frame_count);
    _query_and_ack(true);
    printf("delivery: terminal_%s retired_once recovery=ok idle_extra=0\n",
           timeout ? "timeout" : "TX_ERROR");
}

static void test_lost_accepted(void)
{
    _reset();
    _start_scan();
    _timeout();
    _observe_scanning();
    _assert_status(DEVICE_LINK_V1_WIFI_SCANNING);
    _complete_backend();
    _pump_busy_100();
    _finish(BLE_PORT_TX_CONFIRMED);
    _pump();
    _assert_status(DEVICE_LINK_V1_WIFI_IDLE);
    _finish(BLE_PORT_TX_CONFIRMED);
    _pump_idle_100();
    TEST_ASSERT_EQUAL(3U, s_frame_count);
    _query_and_ack(false);
    printf("delivery: lost_ACCEPTED unsolicited_terminal=0 recovery=ok\n");
}

static void _disconnect(void)
{
    const ble_link_operation_identity_t identity = _connection_identity(
            s_generation, BLE_LINK_OPERATION_DISCONNECT);
    const ble_gap_manager_event_t disconnect =
    {
        .type = BLE_GAP_MANAGER_EVENT_DISCONNECT,
        .identity = identity,
        .conn_handle = CONN_HANDLE,
        .reason = 0x13U,
    };
    const unsigned completions = s_completion_count;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&disconnect));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          s_generation, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    /* Match production ordering: scheduler retires before qualified teardown. */
    ble_tx_scheduler_reset();
    TEST_ASSERT_EQUAL(completions + 1U, s_completion_count);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, s_last_completion_status);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_clear_session_state_if_current(&identity));
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_TRUE((ble_link_session_get_state_flags() &
                      BLE_LINK_STATE_FLAG_BOUND) != 0U);
}

static void test_disconnect_isolation(bool terminal_in_flight)
{
    _reset();
    if (terminal_in_flight)
    {
        _select_terminal();
    }
    else
    {
        _start_scan();
        _finish(BLE_PORT_TX_CONFIRMED);
        _observe_scanning();
        _assert_status(DEVICE_LINK_V1_WIFI_SCANNING);
        _pump_busy_100();
    }
    const ble_link_operation_identity_t old_tx = _last_frame()->identity;
    const uint32_t old_generation = s_generation;

    _disconnect();
    if (!terminal_in_flight)
    {
        _complete_backend();
    }
    _connect();
    TEST_ASSERT_TRUE(s_generation > old_generation);
    _pump_idle_100();
    _command(DEVICE_LINK_V1_GET_OPERATION, 6U);
    _assert_record();
    const ble_link_operation_identity_t current = _last_frame()->identity;
    const unsigned frames = s_frame_count;
    const unsigned completions = s_completion_count;
    const unsigned wakes = s_wake_count;

    TEST_ASSERT_TRUE(current.flow_id != old_tx.flow_id);
    TEST_ASSERT_TRUE(current.token != old_tx.token);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, _tx_event(&old_tx, BLE_PORT_TX_CONFIRMED));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, _tx_event(&old_tx, BLE_PORT_TX_ERROR));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&old_tx, ESP_OK));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&old_tx, ESP_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_tx_scheduler_handle_indication_timeout(old_tx.token));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_link_gatt_set_att_mtu(
                          old_generation, CONN_HANDLE, 23U));
    _subscribe(old_generation, false);
    const ble_link_operation_identity_t old_disconnect = _connection_identity(
            old_generation, BLE_LINK_OPERATION_DISCONNECT);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(
                          &old_disconnect));
    uint32_t mtu = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_REQUIRED_ATT_MTU, mtu);
    TEST_ASSERT_TRUE(ble_gap_manager_is_subscribed_kind(
                         CONN_HANDLE, TX_HANDLE, false));
    TEST_ASSERT_EQUAL(wakes, s_wake_count);
    TEST_ASSERT_EQUAL(completions, s_completion_count);
    TEST_ASSERT_EQUAL(frames, s_frame_count);
    _assert_in_flight(&current);
    _pump_busy_100();
    _finish(BLE_PORT_TX_CONFIRMED);
    _pump_idle_100();
    _query_and_ack(false);
    printf("delivery: disconnect_during_%s reused_handle=%u stale_TX/MTU/CCCD "
           "unchanged_current_TX=ok old_event_replay=0\n",
           terminal_in_flight ? "terminal" : "ordinary", CONN_HANDLE);
}

int main(void)
{
    ble_gatt_registry_init();
    test_scan_delivery();
    test_terminal_failure(false);
    test_terminal_failure(true);
    test_lost_accepted();
    test_disconnect_isolation(false);
    test_disconnect_isolation(true);
    ble_tx_scheduler_deinit();
    return 0;
}
