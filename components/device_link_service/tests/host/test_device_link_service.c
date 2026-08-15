#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#ifndef DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS
    #define DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS 8U
#endif
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host_freertos.h"

#include "ble_adv_manager.h"
#include "ble_gatt_registry.h"
#include "ble_link_codec.h"
#include "ble_link_gatt.h"
#include "ble_link_service.h"
#include "ble_link_session.h"
#include "ble_port_ops.h"
#include "ble_runtime.h"
#include "ble_tx_scheduler.h"
#include "connectivity_manager.h"
#include "device_link_service.h"
#include "device_link_security.h"
#include "device_link_security_auth.h"
#include "nv_storage.h"
#include "event_bus.h"

#define TEST_WINDOW_MS 200U
#define TEST_FAST_INTERVAL_MS 100U
#define TEST_SLOW_INTERVAL_MS 700U
#define TEST_FAST_WINDOW_MS 30000U
#define TEST_BOOT_ID 72623859790382856ULL
#define TEST_CONN_HANDLE 7U
#define TEST_SESSION_RX_HANDLE 0x60U
#define TEST_SESSION_TX_HANDLE 0x61U
#define TEST_PROTOCOL_ATT_MTU 498U
#define TEST_TX_CAPTURE_MAX 16U
#define TEST_TX_CAPTURE_BYTES 512U

static const uint8_t s_test_service_uuid[16] =
{
    0x8b, 0x03, 0xdc, 0x36, 0xd0, 0x63, 0x05, 0x8d,
    0x30, 0x42, 0x10, 0xc5, 0x8c, 0xe4, 0x77, 0x2c,
};

static const uint8_t s_test_session_rx_uuid[16] =
{
    0xf4, 0xeb, 0x8f, 0x50, 0x48, 0xee, 0x19, 0x83,
    0xfe, 0x48, 0xf5, 0x60, 0xdb, 0xae, 0xbf, 0x1b,
};

static const uint8_t s_test_session_tx_uuid[16] =
{
    0xec, 0x3d, 0x69, 0x58, 0xa5, 0xc1, 0xa2, 0x83,
    0x5a, 0x4f, 0x1b, 0x57, 0x38, 0x5d, 0xc6, 0x2c,
};

static const uint8_t s_test_public_instance_id[3] =
{
    0x12U, 0x34U, 0x56U,
};

static void _set_test_grants(device_link_security_auth_record_t *record)
{
    record->granted_permission_count = 3U;
    record->granted_permissions[0] = DEVICE_LINK_PERMISSION_CORE_READ;
    record->granted_permissions[1] = DEVICE_LINK_PERMISSION_CORE_BIND;
    record->granted_permissions[2] = DEVICE_LINK_PERMISSION_CORE_OPERATE;
}

typedef struct protocol_tx_capture
{
    ble_link_operation_identity_t identity;
    uint16_t conn_handle;
    uint16_t value_handle;
    size_t len;
    uint8_t data[TEST_TX_CAPTURE_BYTES];
} protocol_tx_capture_t;

static pthread_mutex_t s_adv_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_adv_started;
static bool s_adv_stopped;
static uint16_t s_adv_interval_ms;
static uint32_t s_adv_generation;
static uint32_t s_adv_stop_generation;
static uint8_t s_adv_service_data[5];
static size_t s_adv_service_data_len;
static unsigned s_adv_start_count;
static unsigned s_adv_stop_count;
static atomic_bool s_fail_adv_start = ATOMIC_VAR_INIT(false);
static atomic_bool s_fail_adv_stop = ATOMIC_VAR_INIT(false);
static atomic_bool s_fail_port_init = ATOMIC_VAR_INIT(false);
static atomic_bool s_fail_peer_store_reset = ATOMIC_VAR_INIT(false);
static atomic_bool s_fail_pairing_gate_close = ATOMIC_VAR_INIT(false);
static atomic_bool s_fail_disconnect = ATOMIC_VAR_INIT(false);
static atomic_bool s_cleanup_manages_pause = ATOMIC_VAR_INIT(false);
static atomic_bool s_cleanup_handoff_to_port = ATOMIC_VAR_INIT(false);
static atomic_bool s_port_cleanup_pending = ATOMIC_VAR_INIT(false);
static atomic_bool s_cleanup_barrier_inject_replacement =
    ATOMIC_VAR_INIT(false);
static atomic_bool s_cleanup_barrier_injected = ATOMIC_VAR_INIT(false);
static atomic_bool s_cleanup_barrier_retain_acl = ATOMIC_VAR_INIT(false);
static atomic_bool s_cleanup_barrier_acl_retained = ATOMIC_VAR_INIT(false);
static atomic_uint s_cleanup_barrier_count = ATOMIC_VAR_INIT(0U);
static atomic_uint s_pairing_gate_close_count = ATOMIC_VAR_INIT(0U);
static atomic_uint s_revoke_count = ATOMIC_VAR_INIT(0U);
static atomic_uint s_disconnect_count = ATOMIC_VAR_INIT(0U);
static atomic_uint s_peer_store_reset_count = ATOMIC_VAR_INIT(0U);
static atomic_bool s_pairing_gate_open = ATOMIC_VAR_INIT(false);
static atomic_bool s_emit_connect_during_start = ATOMIC_VAR_INIT(false);
static pthread_mutex_t s_tx_lock = PTHREAD_MUTEX_INITIALIZER;
static protocol_tx_capture_t s_tx_capture[TEST_TX_CAPTURE_MAX];
static size_t s_tx_capture_count;
static bool s_protocol_registry_initialized;
static bool s_protocol_initialized;
static uint16_t s_protocol_frame_id;
static atomic_uint s_link_state_publish_count = ATOMIC_VAR_INIT(0U);
static atomic_uint s_cleanup_call_count = ATOMIC_VAR_INIT(0U);
static atomic_uint s_replacement_call_count = ATOMIC_VAR_INIT(0U);
static atomic_int s_cleanup_result = ATOMIC_VAR_INIT(ESP_OK);
static atomic_int s_replacement_result = ATOMIC_VAR_INIT(ESP_OK);
static const ble_link_security_ops_t *s_protocol_security_ops;

static void _adv_converge(void);

static void _fake_security_close(void)
{
}

static esp_err_t _fake_discard_provisional(
    const ble_link_operation_identity_t *identity, bool terminate_conn)
{
    assert(identity != NULL);
    (void)terminate_conn;
    atomic_fetch_add_explicit(&s_cleanup_call_count, 1U,
                              memory_order_acq_rel);
    const esp_err_t result = (esp_err_t)atomic_load_explicit(
                                 &s_cleanup_result, memory_order_acquire);

    if (atomic_load_explicit(&s_cleanup_manages_pause,
                             memory_order_acquire))
    {
        (void)ble_adv_manager_set_pause_reason(
            BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP, true);
        if (result == ESP_OK)
        {
            (void)ble_adv_manager_set_pause_reason(
                BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP, false);
        }
    }
    return result;
}

static esp_err_t _fake_promote_provisional(
    const ble_link_operation_identity_t *identity)
{
    assert(identity != NULL);
    atomic_fetch_add_explicit(&s_cleanup_call_count, 1U,
                              memory_order_acq_rel);
    return (esp_err_t)atomic_load_explicit(&s_cleanup_result,
                                           memory_order_acquire);
}

static esp_err_t _fake_replace_authorization(
    const ble_link_operation_identity_t *identity)
{
    assert(identity != NULL);
    atomic_fetch_add_explicit(&s_replacement_call_count, 1U,
                              memory_order_acq_rel);
    const esp_err_t result = (esp_err_t)atomic_load_explicit(
                                 &s_replacement_result,
                                 memory_order_acquire);

    if (result == ESP_OK &&
            atomic_load_explicit(&s_cleanup_handoff_to_port,
                                 memory_order_acquire))
    {
        atomic_store_explicit(&s_port_cleanup_pending, true,
                              memory_order_release);
    }
    return result;
}

static const ble_link_security_ops_t s_cleanup_security_ops =
{
    .close_session = _fake_security_close,
    .discard_provisional_bond = _fake_discard_provisional,
    .promote_provisional_bond = _fake_promote_provisional,
    .replace_authorization = _fake_replace_authorization,
};

/* Asynchronous revoke stub modeling the real host-core owner: the first
 * call (the worker's enqueue) records the request and marks the revoke
 * in flight; every further call is de-duplicated like the real port. The
 * journal is cleared only by the test-side completion helper, mirroring
 * the owner clearing it after the store is verified empty. */
static atomic_int s_revoke_in_flight = ATOMIC_VAR_INIT(0);

esp_err_t ble_nimble_port_revoke_binding(void)
{
    if (atomic_exchange_explicit(&s_revoke_in_flight, 1,
                                 memory_order_acq_rel))
    {
        /* A revoke is already queued/executing: de-duplicated. */
        return ESP_OK;
    }
    atomic_fetch_add_explicit(&s_revoke_count, 1U, memory_order_acq_rel);
    return ESP_OK;
}

static void _revoke_stub_complete(void)
{
    (void)device_link_security_end_revoke();
    atomic_store_explicit(&s_revoke_in_flight, 0, memory_order_release);
}

static void _revoke_stub_fail(void)
{
    atomic_store_explicit(&s_revoke_in_flight, 0, memory_order_release);
}

/* Host stub for the host-core ACL termination: records the request. */
esp_err_t ble_nimble_port_request_disconnect(void)
{
    atomic_fetch_add_explicit(&s_disconnect_count, 1U, memory_order_acq_rel);
    return atomic_load_explicit(&s_fail_disconnect, memory_order_acquire) ?
           ESP_ERR_NO_MEM : ESP_OK;
}

bool ble_nimble_port_cleanup_pending(void)
{
    return atomic_load_explicit(&s_port_cleanup_pending,
                                memory_order_acquire);
}

esp_err_t ble_nimble_port_begin_cleanup_drain(void)
{
    _adv_converge();
    if (ble_adv_manager_get_state() != BLE_ADV_MANAGER_STATE_STOPPED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const unsigned int barrier = atomic_fetch_add_explicit(
                                     &s_cleanup_barrier_count, 1U,
                                     memory_order_acq_rel) + 1U;

    if (atomic_load_explicit(&s_cleanup_barrier_retain_acl,
                             memory_order_acquire) &&
            !atomic_exchange_explicit(&s_cleanup_barrier_acl_retained, true,
                                      memory_order_acq_rel))
    {
        /* Model the production host barrier retaining the accepted ACL until
         * its exact terminal callback, independently of HCI submission. */
        atomic_fetch_add_explicit(&s_disconnect_count, 1U,
                                  memory_order_acq_rel);
        atomic_store_explicit(&s_port_cleanup_pending, true,
                              memory_order_release);
    }
    if (barrier >= 2U &&
            atomic_load_explicit(&s_cleanup_barrier_inject_replacement,
                                 memory_order_acquire) &&
            !atomic_exchange_explicit(&s_cleanup_barrier_injected, true,
                                      memory_order_acq_rel))
    {
        const ble_link_operation_identity_t replacement =
        {
            .generation = 1U,
            .security_epoch = 1U,
            .token = 123U,
            .kind = BLE_LINK_OPERATION_REMOTE_REPLACEMENT,
            .conn_handle = TEST_CONN_HANDLE,
        };

        if (ble_link_service_register_remote_replacement(&replacement) !=
                ESP_OK)
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

esp_err_t ble_nimble_port_set_pairing_window(bool open)
{
    (void)open;
    return ESP_OK;
}

bool ble_gap_manager_is_subscribed_kind(
    uint16_t conn_handle, uint16_t attr_handle, bool notify)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)notify;
    return true;
}
static bool s_port_started;
static bool s_port_stopped;
static ble_adv_manager_state_t s_adv_state_before_deinit;
static void _adv_converge(void);
static void _protocol_port_init(void);

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
    if (atomic_load_explicit(&s_fail_adv_start, memory_order_acquire))
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

static esp_err_t _fake_adv_stop(uint32_t generation)
{
    s_adv_stop_generation = generation;
    s_adv_started = false;
    s_adv_stopped = true;
    s_adv_stop_count++;
    return atomic_load_explicit(&s_fail_adv_stop, memory_order_acquire) ?
           ESP_FAIL : ESP_OK;
}

static esp_err_t _fake_notify(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len)
{
    (void)conn_handle;
    (void)value_handle;
    (void)data;
    (void)len;
    return ESP_OK;
}

static esp_err_t _fake_indicate(
    uint16_t conn_handle, uint16_t value_handle,
    const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U || len > TEST_TX_CAPTURE_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_tx_lock);
    if (s_tx_capture_count >= TEST_TX_CAPTURE_MAX)
    {
        (void)pthread_mutex_unlock(&s_tx_lock);
        return ESP_ERR_NO_MEM;
    }
    protocol_tx_capture_t *capture = &s_tx_capture[s_tx_capture_count];

    assert(ble_tx_scheduler_get_in_flight_identity(
               &capture->identity) == ESP_OK);
    capture->conn_handle = conn_handle;
    capture->value_handle = value_handle;
    capture->len = len;
    memcpy(capture->data, data, len);
    s_tx_capture_count++;
    (void)pthread_mutex_unlock(&s_tx_lock);
    return ESP_OK;
}

static void _fake_tx_completed(
    const ble_tx_scheduler_result_t *result, void *arg)
{
    (void)arg;
    if (result != NULL && result->status == ESP_OK &&
            result->flow_id != 0U)
    {
        (void)ble_link_service_response_completed(
            result->flow_id, result->is_last);
    }
}

static esp_err_t _fake_publish_link_state(
    const uint8_t *value, size_t len, void *arg)
{
    (void)arg;
    assert(value != NULL && len > 0U);
    /* The host subscription fake always reports the link_state CCCD enabled. */
    atomic_fetch_add_explicit(&s_link_state_publish_count, 1U,
                              memory_order_acq_rel);
    return ESP_OK;
}

static const ble_port_ops_t s_test_ops =
{
    .adv_start = _fake_adv_start,
    .adv_stop = _fake_adv_stop,
    .notify = _fake_notify,
    .indicate = _fake_indicate,
};

static ble_adv_manager_config_t s_adv_config;
static uint8_t s_adv_payload_buffer[5];
static const ble_runtime_host_port_t s_test_port;

static esp_err_t _fake_port_init(void)
{
    if (s_fail_port_init)
    {
        return ESP_FAIL;
    }
    memset(&s_adv_config, 0, sizeof(s_adv_config));
    s_adv_config.fast_interval_ms = TEST_FAST_INTERVAL_MS;
    s_adv_config.slow_interval_ms = TEST_SLOW_INTERVAL_MS;
    s_adv_config.fast_window_ms = TEST_FAST_WINDOW_MS;
    s_adv_config.short_name = (const uint8_t *)"MT";
    s_adv_config.short_name_len = 2U;
    s_adv_config.service_uuid = s_test_service_uuid;
    s_adv_config.adv_version = 2U;
    s_adv_config.public_instance_id = s_test_public_instance_id;
    s_adv_config.now_ms = _adv_now_ms;
    s_adv_config.arm_timer = NULL;
    s_adv_config.timer_arg = NULL;
    s_adv_config.ops = &s_test_ops;
    s_adv_config.lock = _adv_lock_cb;
    s_adv_config.unlock = _adv_unlock_cb;
    s_adv_config.lock_arg = NULL;
    s_adv_payload_buffer[0] = 2U;
    s_adv_payload_buffer[1] = 0U;
    memcpy(&s_adv_payload_buffer[2], s_test_public_instance_id,
           sizeof(s_test_public_instance_id));
    ble_adv_manager_init(&s_adv_config);
    _protocol_port_init();
    return ESP_OK;
}

static esp_err_t _fake_port_start(void)
{
    s_port_started = true;
    if (atomic_load_explicit(&s_emit_connect_during_start,
                             memory_order_acquire))
    {
        ble_port_event_t event;

        memset(&event, 0, sizeof(event));
        event.type = BLE_PORT_EVENT_CONNECT;
        event.conn_handle = TEST_CONN_HANDLE;
        event.status = 0;
        event.accepted = true;
        event.identity.generation = 1U;
        event.identity.kind = BLE_LINK_OPERATION_CONNECT;
        event.identity.conn_handle = event.conn_handle;
        assert(ble_event_router_dispatch(&event) == ESP_OK);
    }
    return ESP_OK;
}

static esp_err_t _fake_set_pairing_gate(bool open)
{
    const esp_err_t pause_result = ble_adv_manager_set_pause_reason(
                                       BLE_ADV_MANAGER_PAUSE_REASON_WINDOW_TRANSITION,
                                       true);

    if (pause_result != ESP_OK)
    {
        return pause_result;
    }
    _adv_converge();
    if (ble_adv_manager_get_state() != BLE_ADV_MANAGER_STATE_STOPPED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!open)
    {
        atomic_fetch_add_explicit(&s_pairing_gate_close_count, 1U,
                                  memory_order_acq_rel);
        if (atomic_load_explicit(&s_fail_pairing_gate_close,
                                 memory_order_acquire))
        {
            return ESP_FAIL;
        }
    }
    atomic_store_explicit(&s_pairing_gate_open, open, memory_order_release);
    return ESP_OK;
}

static esp_err_t _fake_reset_peer_store(void)
{
    atomic_fetch_add_explicit(&s_peer_store_reset_count, 1U,
                              memory_order_acq_rel);
    if (atomic_load_explicit(&s_fail_peer_store_reset,
                             memory_order_acquire))
    {
        return ESP_FAIL;
    }
    bool pending = false;
    const esp_err_t result = device_link_security_revoke_pending(&pending);

    if (result != ESP_OK || !pending)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return device_link_security_end_revoke();
}

static esp_err_t _fake_port_stop(void)
{
    s_port_stopped = true;
    s_port_started = false;
    return ESP_OK;
}

static esp_err_t _fake_port_deinit(void)
{
    s_adv_state_before_deinit = ble_adv_manager_get_state();
    if (s_protocol_initialized)
    {
        ble_link_gatt_reset();
        ble_link_session_reset();
        ble_tx_scheduler_deinit();
        s_protocol_initialized = false;
    }
    ble_adv_manager_deinit();
    return ESP_OK;
}

static esp_err_t s_public_id_getter_result;
static uint8_t s_public_id_bytes[DEVICE_LINK_SECURITY_PUBLIC_INSTANCE_BYTES];

static unsigned s_public_id_getter_calls;

static esp_err_t _fake_get_public_instance_id(uint8_t out_instance_id[3])
{
    memcpy(out_instance_id, s_public_id_bytes,
           DEVICE_LINK_SECURITY_PUBLIC_INSTANCE_BYTES);
    s_public_id_getter_calls++;
    return s_public_id_getter_result;
}

static const ble_runtime_host_port_t s_test_port =
{
    .init = _fake_port_init,
    .start = _fake_port_start,
    .set_pairing_gate = _fake_set_pairing_gate,
    .reset_peer_store = _fake_reset_peer_store,
    .stop = _fake_port_stop,
    .deinit = _fake_port_deinit,
};

/* Port with a working public-instance-id getter. */
static const ble_runtime_host_port_t s_public_test_port =
{
    .init = _fake_port_init,
    .start = _fake_port_start,
    .set_pairing_gate = _fake_set_pairing_gate,
    .reset_peer_store = _fake_reset_peer_store,
    .get_public_instance_id = _fake_get_public_instance_id,
    .stop = _fake_port_stop,
    .deinit = _fake_port_deinit,
};

static device_link_service_config_t s_config;
static uint8_t s_random_counter;

static pthread_mutex_t s_publish_lock = PTHREAD_MUTEX_INITIALIZER;
static device_link_service_snapshot_t s_last_published;
static bool s_published;
static unsigned s_publish_count_value;

/* Single-slot event-bus fake for the completion bridge: one connectivity
 * subscriber at a time, delivered synchronously in publisher context. */
static event_bus_cb_t s_connectivity_callback = NULL;
static void *s_connectivity_callback_arg = NULL;
static event_bus_sub_handle_t s_connectivity_handle =
    EVENT_BUS_SUB_HANDLE_INVALID;

EVENT_BUS_DEFINE_ID(CONNECTIVITY_MANAGER_MSG);

static void _test_dispatch_connectivity(
    uint32_t sub_type, const void *payload, size_t payload_size)
{
    if (s_connectivity_callback != NULL)
    {
        s_connectivity_callback(EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                                sub_type, payload, payload_size,
                                s_connectivity_callback_arg);
    }
}

esp_err_t event_bus_publish(event_bus_msg_id_t msg_id, uint32_t sub_type,
                            const void *payload, size_t payload_size,
                            uint32_t flags)
{
    (void)flags;
    if (msg_id == EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG))
    {
        _test_dispatch_connectivity(sub_type, payload, payload_size);
        return ESP_OK;
    }
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

esp_err_t event_bus_subscribe(event_bus_msg_id_t msg_id, uint32_t sub_type,
                              event_bus_cb_t callback, void *user_data,
                              event_bus_dispatch_context_t context,
                              event_bus_sub_handle_t *out_handle)
{
    assert(msg_id == EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG));
    assert(callback != NULL);
    assert(context == EVENT_BUS_DISPATCH_PUBLISHER);
    assert(out_handle != NULL);
    if (s_connectivity_callback != NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    s_connectivity_callback = callback;
    s_connectivity_callback_arg = user_data;
    s_connectivity_handle = 1U;
    *out_handle = 1U;
    (void)sub_type;
    return ESP_OK;
}

esp_err_t event_bus_unsubscribe(event_bus_sub_handle_t handle)
{
    assert(handle == 1U);
    if (s_connectivity_callback == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    s_connectivity_callback = NULL;
    s_connectivity_callback_arg = NULL;
    s_connectivity_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    return ESP_OK;
}

/* Test hook: publish one terminal connectivity snapshot through the fake
 * bus, as the connectivity worker would. */
static void _test_publish_connectivity_status(
    const connectivity_manager_status_snapshot_t *snapshot)
{
    (void)event_bus_publish(EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                            CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                            snapshot, sizeof(*snapshot), 0U);
}

static void _test_publish_connectivity_scan(
    const connectivity_manager_scan_snapshot_t *snapshot)
{
    (void)event_bus_publish(EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                            CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                            snapshot, sizeof(*snapshot), 0U);
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

uint32_t esp_random(void)
{
    /* Incrementing LCG; the byte stream stays deterministic. */
    s_random_counter += 0x9e3779b9U;
    return s_random_counter;
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *bytes = buf;

    for (size_t i = 0U; i < len; ++i)
    {
        bytes[i] = (uint8_t)(s_random_counter++);
    }
}

static void _reset_host(void)
{
    assert(!s_protocol_initialized);
    host_freertos_reset_controls();
    s_random_counter = 0U;
    s_adv_started = false;
    s_adv_stopped = false;
    s_adv_interval_ms = 0U;
    s_adv_generation = 0U;
    s_adv_stop_generation = 0U;
    s_adv_service_data_len = 0U;
    s_adv_start_count = 0U;
    s_adv_stop_count = 0U;
    atomic_store_explicit(&s_fail_adv_start, false, memory_order_release);
    atomic_store_explicit(&s_fail_adv_stop, false, memory_order_release);
    atomic_store_explicit(&s_fail_port_init, false, memory_order_release);
    atomic_store_explicit(&s_fail_peer_store_reset, false,
                          memory_order_release);
    atomic_store_explicit(&s_fail_pairing_gate_close, false,
                          memory_order_release);
    atomic_store_explicit(&s_fail_disconnect, false, memory_order_release);
    atomic_store_explicit(&s_cleanup_manages_pause, false,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_handoff_to_port, false,
                          memory_order_release);
    atomic_store_explicit(&s_port_cleanup_pending, false,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_barrier_inject_replacement, false,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_barrier_injected, false,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_barrier_retain_acl, false,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_barrier_acl_retained, false,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_barrier_count, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_pairing_gate_close_count, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_peer_store_reset_count, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_revoke_count, 0U, memory_order_release);
    atomic_store_explicit(&s_disconnect_count, 0U, memory_order_release);
    atomic_store_explicit(&s_revoke_in_flight, 0, memory_order_release);
    atomic_store_explicit(&s_pairing_gate_open, false, memory_order_release);
    atomic_store_explicit(&s_emit_connect_during_start, false,
                          memory_order_release);
    s_port_started = false;
    s_port_stopped = false;
    s_adv_state_before_deinit = BLE_ADV_MANAGER_STATE_STOPPED;
    s_protocol_frame_id = 1U;
    atomic_store_explicit(&s_link_state_publish_count, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_call_count, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_replacement_call_count, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_result, ESP_OK,
                          memory_order_release);
    atomic_store_explicit(&s_replacement_result, ESP_OK,
                          memory_order_release);
    s_protocol_security_ops = NULL;
    (void)pthread_mutex_lock(&s_tx_lock);
    memset(s_tx_capture, 0, sizeof(s_tx_capture));
    s_tx_capture_count = 0U;
    (void)pthread_mutex_unlock(&s_tx_lock);
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
    event.generation = s_adv_stop_generation;
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
        ble_adv_manager_poll();
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

static bool _wait_atomic_at_least(
    const atomic_uint *value, unsigned int expected, uint32_t timeout_ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 2000000L,
    };

    for (uint32_t elapsed = 0U; elapsed < timeout_ms; elapsed += 2U)
    {
        if (atomic_load_explicit(value, memory_order_acquire) >= expected)
        {
            return true;
        }
        (void)nanosleep(&delay, NULL);
    }
    return atomic_load_explicit(value, memory_order_acquire) >= expected;
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

static bool _status_error_set(void);

static bool _revoke_pending(void)
{
    bool pending = false;

    return device_link_security_revoke_pending(&pending) == ESP_OK &&
           pending;
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

static bool _slow_advertising_started(void)
{
    (void)pthread_mutex_lock(&s_adv_lock);
    const bool started = s_adv_started && !s_adv_stopped &&
                         s_adv_interval_ms == TEST_SLOW_INTERVAL_MS;

    (void)pthread_mutex_unlock(&s_adv_lock);
    return started;
}

static bool _pairing_gate_close_attempted(void)
{
    return atomic_load_explicit(&s_pairing_gate_close_count,
                                memory_order_acquire) > 0U;
}

static bool _pairing_gate_closed(void)
{
    return !atomic_load_explicit(&s_pairing_gate_open,
                                 memory_order_acquire);
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
        ble_adv_manager_poll();
        (void)nanosleep(&delay, NULL);
    }
}

typedef struct protocol_response
{
    uint64_t request_id;
    uint32_t error;
    ble_link_codec_response_tag_t body;
    size_t body_len;
    uint8_t body_data[128];
} protocol_response_t;

static void _protocol_clear_tx_capture(void)
{
    (void)pthread_mutex_lock(&s_tx_lock);
    memset(s_tx_capture, 0, sizeof(s_tx_capture));
    s_tx_capture_count = 0U;
    (void)pthread_mutex_unlock(&s_tx_lock);
}

static bool _protocol_wait_tx_capture(
    protocol_tx_capture_t *capture, uint32_t timeout_ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 2000000L,
    };

    for (uint32_t elapsed = 0U; elapsed < timeout_ms; elapsed += 2U)
    {
        bool ready = false;

        (void)pthread_mutex_lock(&s_tx_lock);
        if (s_tx_capture_count > 0U)
        {
            *capture = s_tx_capture[0];
            ready = true;
        }
        (void)pthread_mutex_unlock(&s_tx_lock);
        if (ready)
        {
            return true;
        }
        ble_adv_manager_poll();
        (void)nanosleep(&delay, NULL);
    }
    return false;
}

static bool _protocol_response_idle(void)
{
    return !ble_tx_scheduler_is_busy() &&
           !ble_link_service_response_in_flight();
}

static bool _protocol_wait_confirmation(
    bool pending, uint64_t token,
    device_link_service_status_t *out, uint32_t timeout_ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 2000000L,
    };

    for (uint32_t elapsed = 0U; elapsed < timeout_ms; elapsed += 2U)
    {
        device_link_service_status_t status;

        if (device_link_service_get_status(&status) == ESP_OK &&
                status.pending_confirmation == pending &&
                (!pending || status.confirmation_token == token ||
                 token == 0U))
        {
            if (out != NULL)
            {
                *out = status;
            }
            return true;
        }
        (void)nanosleep(&delay, NULL);
    }
    return false;
}

static bool _protocol_wait_status_error_after(
    uint64_t generation, int32_t error,
    device_link_service_status_t *out, uint32_t timeout_ms)
{
    const struct timespec delay =
    {
        .tv_sec = 0,
        .tv_nsec = 2000000L,
    };

    for (uint32_t elapsed = 0U; elapsed < timeout_ms; elapsed += 2U)
    {
        device_link_service_status_t status;

        if (device_link_service_get_status(&status) == ESP_OK &&
                status.generation > generation &&
                status.last_error == error)
        {
            if (out != NULL)
            {
                *out = status;
            }
            return true;
        }
        (void)nanosleep(&delay, NULL);
    }
    return false;
}

static void _protocol_port_init(void)
{
    static const ble_tx_scheduler_config_t scheduler_config =
    {
        .queue_depth = 16U,
        .max_frame_bytes = TEST_TX_CAPTURE_BYTES,
        .ops = &s_test_ops,
        .completed = _fake_tx_completed,
        .completed_arg = NULL,
        .lock = _adv_lock_cb,
        .unlock = _adv_unlock_cb,
        .lock_arg = NULL,
    };
    ble_link_gatt_config_t gatt_config;

    assert(!s_protocol_initialized);
    assert(ble_tx_scheduler_init(&scheduler_config) == ESP_OK);
    if (!s_protocol_registry_initialized)
    {
        ble_gatt_registry_init();
        s_protocol_registry_initialized = true;
    }
    memset(&gatt_config, 0, sizeof(gatt_config));
    gatt_config.boot_id = TEST_BOOT_ID;
    gatt_config.att_mtu = TEST_PROTOCOL_ATT_MTU;
    gatt_config.tx_queue_depth = scheduler_config.queue_depth;
    gatt_config.publish_link_state = _fake_publish_link_state;
    gatt_config.security_ops = s_protocol_security_ops;
    assert(ble_link_gatt_init(&gatt_config) == ESP_OK);
    assert(ble_gatt_registry_assign_handle(
               s_test_session_rx_uuid, TEST_SESSION_RX_HANDLE) == ESP_OK);
    assert(ble_gatt_registry_assign_handle(
               s_test_session_tx_uuid, TEST_SESSION_TX_HANDLE) == ESP_OK);
    ble_link_gatt_update_handles();
    s_protocol_initialized = true;
}

static void _protocol_establish_bootstrap_session(uint32_t generation)
{
    static const uint8_t peer_addr[6] =
    {
        0x11U, 0U, 0U, 0U, 0U, 0xeaU,
    };
    uint32_t security_epoch = 0U;

    ble_link_gatt_set_connection(
        generation, TEST_CONN_HANDLE, 1U, peer_addr);
    ble_link_gatt_set_att_mtu(TEST_PROTOCOL_ATT_MTU);
    assert(ble_link_session_handle_event(
               generation,
               BLE_LINK_SESSION_EVENT_ACL_CONNECTED) == ESP_OK);
    assert(ble_link_session_handle_event(
               generation,
               BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED) == ESP_OK);
    assert(ble_link_session_handle_event(
               generation,
               BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED) == ESP_OK);
    assert(ble_link_session_set_identity_known(generation, true) == ESP_OK);
    assert(ble_link_session_set_connection_pairing_window(
               generation, true) == ESP_OK);
    assert(ble_link_session_security2_open(
               generation, &security_epoch) == ESP_OK);
    assert(security_epoch != 0U);
}

static void _protocol_establish_session(uint32_t generation)
{
    static const uint8_t peer_addr[6] =
    {
        0x11U, 0U, 0U, 0U, 0U, 0xeaU,
    };
    uint32_t security_epoch = 0U;

    ble_link_gatt_set_connection(
        generation, TEST_CONN_HANDLE, 1U, peer_addr);
    ble_link_gatt_set_att_mtu(TEST_PROTOCOL_ATT_MTU);
    assert(ble_link_session_handle_event(
               generation,
               BLE_LINK_SESSION_EVENT_ACL_CONNECTED) == ESP_OK);
    assert(ble_link_session_handle_event(
               generation,
               BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED) == ESP_OK);
    assert(ble_link_session_handle_event(
               generation,
               BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED) == ESP_OK);
    assert(ble_link_session_set_identity_known(generation, true) == ESP_OK);
    assert(ble_link_session_set_connection_pairing_window(
               generation, true) == ESP_OK);
    assert(ble_link_session_security2_open(
               generation, &security_epoch) == ESP_OK);
    assert(security_epoch != 0U);
    assert(ble_link_session_set_authorization(true, 1U) == ESP_OK);
    assert(ble_link_session_report_session_match_current(
               generation, 1U) == ESP_OK);
}

static void _protocol_reopen_session(uint32_t generation)
{
    uint32_t security_epoch = 0U;

    assert(ble_link_session_security2_open(
               generation, &security_epoch) == ESP_OK);
    assert(security_epoch != 0U);
    assert(ble_link_session_report_session_match_current(
               generation, 1U) == ESP_OK);
}

static void _protocol_disconnect_session(uint32_t generation)
{
    assert(_wait_for(_protocol_response_idle, 500U));
    ble_link_service_clear_session_state();
    assert(ble_link_session_handle_event(
               generation,
               BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED) == ESP_OK);
    ble_tx_scheduler_reset();
}

static int _protocol_write_session_request(
    const uint8_t *request, size_t request_len)
{
    uint8_t framed[TEST_TX_CAPTURE_BYTES];
    const size_t total_len = request_len + 1U;
    const ble_gatt_registry_characteristic_t *characteristic = NULL;
    ble_gatt_registry_access_context_t context;

    assert(request != NULL);
    assert(8U + total_len <= sizeof(framed));
    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = (uint8_t)(s_protocol_frame_id & 0xffU);
    framed[3] = (uint8_t)(s_protocol_frame_id >> 8U);
    framed[4] = (uint8_t)(total_len & 0xffU);
    framed[5] = (uint8_t)(total_len >> 8U);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, request_len);
    s_protocol_frame_id++;
    if (s_protocol_frame_id == 0U)
    {
        s_protocol_frame_id = 1U;
    }
    memset(&context, 0, sizeof(context));
    context.op = BLE_GATT_REGISTRY_OP_WRITE_CHR;
    context.write_data = framed;
    context.write_len = (uint16_t)(8U + total_len);
    assert(ble_gatt_registry_lookup_by_handle(
               TEST_SESSION_RX_HANDLE, &characteristic) == ESP_OK);
    assert(characteristic != NULL && characteristic->access_cb != NULL);
    return characteristic->access_cb(
               TEST_CONN_HANDLE, TEST_SESSION_RX_HANDLE,
               &context, characteristic->arg);
}

static void _protocol_decode_response(
    const protocol_tx_capture_t *capture, protocol_response_t *out)
{
    ble_link_codec_envelope_t envelope;
    ble_link_codec_response_t response;

    assert(capture != NULL && out != NULL);
    assert(capture->conn_handle == TEST_CONN_HANDLE);
    assert(capture->value_handle == TEST_SESSION_TX_HANDLE);
    assert(capture->len >= 9U);
    assert(capture->data[0] == 1U);
    assert(capture->data[1] == 3U);
    const size_t framed_payload_len =
        (size_t)capture->data[4] | ((size_t)capture->data[5] << 8U);

    assert(framed_payload_len + 8U == capture->len);
    assert(capture->data[8] == BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED);
    assert(ble_link_codec_decode_envelope(
               &capture->data[9], capture->len - 9U, &envelope) == ESP_OK);
    assert(envelope.body == BLE_LINK_CODEC_BODY_RESPONSE);
    assert(ble_link_codec_decode_response(
               envelope.body_data, envelope.body_len, &response) == ESP_OK);
    assert(response.body_len <= sizeof(out->body_data));
    memset(out, 0, sizeof(*out));
    out->request_id = response.request_id;
    out->error = response.error;
    out->body = response.body;
    out->body_len = response.body_len;
    if (response.body_len > 0U)
    {
        memcpy(out->body_data, response.body_data, response.body_len);
    }
}

static void _protocol_confirm_response(
    const protocol_tx_capture_t *capture)
{
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_NOTIFY_TX;
    event.identity = capture->identity;
    event.conn_handle = capture->conn_handle;
    event.attr_handle = capture->value_handle;
    event.indication = true;
    event.tx_result = BLE_PORT_TX_CONFIRMED;
    assert(ble_tx_scheduler_handle_notify_tx(&event) == ESP_OK);
    assert(_wait_for(_protocol_response_idle, 500U));
}

static void _protocol_exchange(
    const uint8_t *request, size_t request_len,
    protocol_response_t *response)
{
    protocol_tx_capture_t capture;

    assert(_protocol_response_idle());
    _protocol_clear_tx_capture();
    assert(_protocol_write_session_request(request, request_len) == 0);
    assert(_protocol_wait_tx_capture(&capture, 500U));
    _protocol_decode_response(&capture, response);
    _protocol_confirm_response(&capture);
}

static const uint8_t s_protocol_prepare_request[] =
{
    0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
};

static void _protocol_capture_prepare(
    const protocol_response_t *response,
    uint8_t txn_id[8], uint8_t credential[16])
{
    assert(response->error == BLE_LINK_ERROR_OK);
    assert(response->body == BLE_LINK_CODEC_RESPONSE_AUTHORIZE_PREPARE);
    assert(response->body_len >= 27U);
    assert(response->body_data[0] == 0x09U);
    assert(response->body_data[9] == 0x12U);
    assert(response->body_data[10] == 0x10U);
    memcpy(txn_id, &response->body_data[1], 8U);
    memcpy(credential, &response->body_data[11], 16U);
}

static size_t _protocol_build_commit(
    uint8_t *out, size_t capacity, uint64_t request_id,
    const uint8_t txn_id[8], const uint8_t credential[16])
{
    uint8_t commit_body[27];
    uint8_t request[48];
    const size_t request_len = 11U + sizeof(commit_body);
    const size_t output_len = 13U + request_len;

    commit_body[0] = 0x09U;
    memcpy(&commit_body[1], txn_id, 8U);
    commit_body[9] = 0x12U;
    commit_body[10] = 0x10U;
    memcpy(&commit_body[11], credential, 16U);
    request[0] = 0x09U;
    for (size_t i = 0U; i < 8U; ++i)
    {
        request[1U + i] = (uint8_t)(request_id >> (8U * i));
    }
    request[9] = 0x6aU;
    request[10] = (uint8_t)sizeof(commit_body);
    memcpy(&request[11], commit_body, sizeof(commit_body));
    assert(output_len <= capacity);
    out[0] = 0x08U;
    out[1] = 0x02U;
    out[2] = 0x19U;
    for (size_t i = 0U; i < 8U; ++i)
    {
        out[3U + i] = (uint8_t)(TEST_BOOT_ID >> (8U * i));
    }
    out[11] = 0x52U;
    out[12] = (uint8_t)request_len;
    memcpy(&out[13], request, request_len);
    return output_len;
}

static size_t _protocol_build_recovery_query(
    uint8_t *out, size_t capacity, uint64_t request_id,
    const uint8_t credential[16])
{
    size_t pos = 0U;

    assert(capacity >= 64U);
    out[pos++] = 0x08U;
    out[pos++] = 0x02U;
    out[pos++] = 0x19U;
    for (size_t i = 0U; i < 8U; ++i)
    {
        out[pos++] = (uint8_t)(TEST_BOOT_ID >> (8U * i));
    }
    out[pos++] = 0x20U;
    out[pos++] = 0x01U;
    out[pos++] = 0x52U;
    const size_t request_len_pos = pos++;

    out[pos++] = 0x09U;
    for (size_t i = 0U; i < 8U; ++i)
    {
        out[pos++] = (uint8_t)(request_id >> (8U * i));
    }
    out[pos++] = 0x72U;
    out[pos++] = 18U;
    out[pos++] = 0x0aU;
    out[pos++] = 16U;
    memcpy(&out[pos], credential, 16U);
    pos += 16U;
    out[request_len_pos] = (uint8_t)(pos - request_len_pos - 1U);
    return pos;
}

static void _protocol_save_auth_record(uint8_t credential[16])
{
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    _set_test_grants(&record);
    for (size_t i = 0U; i < sizeof(record.credential_id); ++i)
    {
        record.credential_id[i] = (uint8_t)(i + 1U);
        record.device_auth_id[i] = (uint8_t)(0x40U + i);
        record.salt[i] = (uint8_t)(0x80U + i);
    }
    for (size_t i = 0U; i < sizeof(record.verifier); ++i)
    {
        record.verifier[i] = (uint8_t)((i & 0x7fU) + 1U);
    }
    record.peer_addr_type = 1U;
    record.peer_addr[0] = 0x11U;
    record.peer_addr[5] = 0xeaU;
    memcpy(credential, record.credential_id, sizeof(record.credential_id));
    assert(device_link_security_save_auth_record(&record) == ESP_OK);
}

static esp_err_t _sec_stub_request(
    const uint8_t *request, size_t request_len,
    uint8_t **response, size_t *response_len, void *arg)
{
    (void)request;
    (void)request_len;
    (void)arg;
    *response = NULL;
    *response_len = 0U;
    return ESP_ERR_NOT_SUPPORTED;
}

static const ble_runtime_host_port_t *s_runtime_port = &s_test_port;

static void _init_service(void)
{
    const device_link_security_config_t security_config =
    {
        .username = "microtech",
        .session_id = 1U,
        .request_cb = _sec_stub_request,
        .request_arg = NULL,
    };

    assert(device_link_security_init(&security_config) == ESP_OK);
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = s_runtime_port;
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
    assert(s_adv_service_data[0] == 2U);
    assert(s_adv_service_data[1] == 0U);
    assert(memcmp(&s_adv_service_data[2], s_test_public_instance_id,
                  sizeof(s_test_public_instance_id)) == 0);
}

static void _deinit_service(void)
{
    assert(device_link_service_deinit(DEVICE_LINK_SERVICE_WAIT_FOREVER) ==
           ESP_OK);
    assert(!s_port_started);
    assert(s_port_stopped);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_tasks() == 0U);
    /* Security, link-session, link-service, and GATT delivery state keep
     * boot-lifetime mutexes once the protocol fixture has been used. */
    assert(host_freertos_live_semaphores() <= 4U);
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

static void _test_rollback_port_init_failure(void)
{
    _reset_host();
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    /* The fake port init runs inside ble_runtime_start, so this exercises
     * an initialized runtime whose port init fails: the rollback must
     * tear the runtime down and leave a deinit/re-init viable lifecycle. */
    s_fail_port_init = true;
    assert(device_link_service_init(&s_config) == ESP_FAIL);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_tasks() == 0U);
    /* Security, link-session, and link-service mutexes are boot-lifetime. */
    assert(host_freertos_live_semaphores() <= 4U);
    assert(device_link_service_deinit(DEVICE_LINK_SERVICE_WAIT_FOREVER) ==
           ESP_OK);
    s_fail_port_init = false;
    _init_service();
    _deinit_service();
}

static void _test_rollback_runtime_never_initialized(void)
{
    _reset_host();
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    /* A resource creation failure before ble_runtime_init reaches the
     * runtime-never-initialized teardown branch: the rollback must not
     * call ble_runtime_deinit on an uninitialized runtime and must leave
     * a deinit/re-init viable lifecycle. */
    host_freertos_fail_next_queue_creates(1U);
    assert(device_link_service_init(&s_config) == ESP_ERR_NO_MEM);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_tasks() == 0U);
    /* Security, link-session, and link-service mutexes are boot-lifetime. */
    assert(host_freertos_live_semaphores() <= 4U);
    assert(device_link_service_deinit(DEVICE_LINK_SERVICE_WAIT_FOREVER) ==
           ESP_OK);
    _init_service();
    _deinit_service();
}

static void _test_rollback_lease_failure_retryable(void)
{
    _reset_host();
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    /* The slow lease installs, then the convergence restart fails, so the
     * init rollback must release the lease and tear down the runtime. */
    s_fail_adv_start = true;
    assert(device_link_service_init(&s_config) == ESP_FAIL);
    /* The installed slow lease must have been released: in a fresh
     * manager the release leaves STOPPED with no port operation. The
     * state is captured by the fake port right before the manager is
     * deinitialized, so a leaked lease would surface as FAULTED. */
    assert(s_adv_state_before_deinit == BLE_ADV_MANAGER_STATE_STOPPED);
    assert(!s_port_started);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_tasks() == 0U);
    /* Security, link-session, and link-service mutexes are boot-lifetime. */
    assert(host_freertos_live_semaphores() <= 4U);
    assert(device_link_service_deinit(DEVICE_LINK_SERVICE_WAIT_FOREVER) ==
           ESP_OK);
    s_fail_adv_start = false;
    _init_service();
    _deinit_service();
}

static void _test_rollback_deinit_enqueue_failure_retryable(void)
{
    _reset_host();
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    /* Fail after the worker exists, then reject rollback's DEINIT enqueue.
     * STOPPING must retain an explicit unsent command obligation: a later
     * deinit retries the enqueue instead of waiting for an exit signal that
     * no worker can produce. */
    atomic_store_explicit(&s_fail_adv_start, true, memory_order_release);
    host_freertos_fail_next_queue_sends(1U);
    assert(device_link_service_init(&s_config) == ESP_ERR_TIMEOUT);
    assert(host_freertos_live_queues() == 1U);
    assert(host_freertos_live_tasks() == 1U);

    atomic_store_explicit(&s_fail_adv_start, false, memory_order_release);
    assert(device_link_service_deinit(DEVICE_LINK_SERVICE_WAIT_FOREVER) ==
           ESP_OK);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_tasks() == 0U);
    _init_service();
    _deinit_service();
}

static void _test_factory_reset_startup_gate(void)
{
    const device_link_security_config_t security_config =
    {
        .username = "microtech",
        .session_id = 1U,
        .request_cb = _sec_stub_request,
        .request_arg = NULL,
    };

    nv_storage_fake_reset();
    _reset_host();
    assert(device_link_security_init(&security_config) == ESP_OK);
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    s_config.startup_mode =
        DEVICE_LINK_SERVICE_STARTUP_FACTORY_RESET_GATED;
    atomic_store_explicit(&s_fail_adv_start, true, memory_order_release);
    assert(device_link_service_init(&s_config) == ESP_OK);
    assert(_wait_for(_status_available, 500U));
    assert(s_port_started);
    assert(!s_adv_started);
    assert(ble_adv_manager_get_state() == BLE_ADV_MANAGER_STATE_STOPPED);
    assert(atomic_load_explicit(&s_peer_store_reset_count,
                                memory_order_acquire) == 1U);
    assert(!_revoke_pending());

    /* The global reset owner clears its journal before releasing this
     * gate. Init already installed the persistent slow lease while paused,
     * so release is only the visibility commit. A physical START failure is
     * retained by the ADV owner rather than reopening a storage transaction. */
    assert(device_link_service_release_startup_gate() == ESP_OK);
    assert(!s_adv_started);
    assert(ble_adv_manager_get_state() == BLE_ADV_MANAGER_STATE_FAULTED);
    assert(ble_adv_manager_get_retry_remaining_ms() <= 100U);
    atomic_store_explicit(&s_fail_adv_start, false, memory_order_release);
    assert(_wait_for(_slow_advertising_started, 1000U));
    _adv_converge();
    assert(ble_adv_manager_get_state() == BLE_ADV_MANAGER_STATE_SLOW);
    assert(s_adv_interval_ms == TEST_SLOW_INTERVAL_MS);
    assert(device_link_service_release_startup_gate() == ESP_OK);
    _deinit_service();

    nv_storage_fake_reset();
    _reset_host();
    assert(device_link_security_init(&security_config) == ESP_OK);
    atomic_store_explicit(&s_fail_peer_store_reset, true,
                          memory_order_release);
    assert(device_link_service_init(&s_config) == ESP_FAIL);
    assert(!s_adv_started);
    assert(!s_port_started);
    assert(_revoke_pending());
    assert(device_link_service_deinit(DEVICE_LINK_SERVICE_WAIT_FOREVER) ==
           ESP_OK);
    assert(device_link_security_end_revoke() == ESP_OK);
}

static void _test_binding_confirmation(void)
{
    _reset_host();
    _init_service();

    /* No transaction: nothing pending, confirmation is a no-op. */
    assert(!device_link_service_pending_confirmation());
    assert(device_link_service_confirm_binding(1U, true) == ESP_OK);
    assert(!device_link_service_pending_confirmation());
    assert(device_link_service_confirm_binding(1U, false) == ESP_OK);
    assert(!device_link_service_pending_confirmation());
    _deinit_service();
}

static bool _revoke_not_pending(void)
{
    bool pending = false;

    return device_link_security_revoke_pending(&pending) == ESP_OK &&
           !pending;
}

static bool _revoke_requested(void)
{
    return atomic_load_explicit(&s_revoke_count, memory_order_acquire) > 0U;
}

static void _test_revoke_async_retry(void)
{
    /* The host-core owner is busy: the journal stays, the worker keeps
     * polling, and the port stub de-duplicates the retries; the revoke
     * completes only when the owner clears the journal. */
    device_link_security_auth_record_t record;

    nv_storage_fake_reset();
    memset(&record, 0, sizeof(record));
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    _set_test_grants(&record);
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES; ++i)
    {
        record.credential_id[i] = (uint8_t)(i + 1U);
        record.device_auth_id[i] = (uint8_t)(0x40U + i);
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_SALT_BYTES; ++i)
    {
        record.salt[i] = (uint8_t)(0x30U + i);
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES; ++i)
    {
        record.verifier[i] = (uint8_t)(i & 0x7fU);
    }
    record.peer_addr_type = 1U;
    record.peer_addr[0] = 0x11U;
    record.peer_addr[5] = 0xeaU;
    assert(device_link_security_save_auth_record(&record) == ESP_OK);

    _reset_host();
    _init_service();
    atomic_store_explicit(&s_revoke_count, 0U, memory_order_release);
    atomic_store_explicit(&s_revoke_in_flight, 0, memory_order_release);
    assert(device_link_service_revoke_binding() == ESP_OK);
    /* The worker issued the deletion request; the journal stays and the
     * retry polls without re-issuing (in-flight dedup). */
    assert(_wait_for(_revoke_requested, 500U));
    _pump_ms(300U);
    assert(atomic_load_explicit(&s_revoke_count, memory_order_acquire) == 1U);
    assert(_revoke_pending());
    /* While the revoke is in flight, a new binding window must not open:
     * the worker rejects the OPEN with a stable error, never a window. */
    assert(device_link_service_open_window() == ESP_OK);
    _pump_ms(300U);
    {
        device_link_service_status_t status;

        assert(device_link_service_get_status(&status) == ESP_OK);
        assert(!status.active);
        assert(status.state != DEVICE_LINK_SERVICE_STATE_WINDOW);
        assert(status.last_error == ESP_ERR_INVALID_STATE);
    }
    /* The owner completes the deletion: the journal disappears without a
     * second enqueue, and the revoke converges. */
    _revoke_stub_complete();
    assert(_wait_for(_revoke_not_pending, 500U));
    assert(device_link_security_load_auth_record(&record) ==
           ESP_ERR_NOT_FOUND);
    assert(atomic_load_explicit(&s_revoke_count, memory_order_acquire) == 1U);
    _deinit_service();
}

static void _test_revoke_fail_retries(void)
{
    /* The host-core deletion fails: the journal stays, the in-flight flag
     * is released, and the worker re-enqueues until it succeeds. */
    device_link_security_auth_record_t record;

    nv_storage_fake_reset();
    memset(&record, 0, sizeof(record));
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    _set_test_grants(&record);
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES; ++i)
    {
        record.credential_id[i] = (uint8_t)(i + 1U);
        record.device_auth_id[i] = (uint8_t)(0x40U + i);
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_SALT_BYTES; ++i)
    {
        record.salt[i] = (uint8_t)(0x30U + i);
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES; ++i)
    {
        record.verifier[i] = (uint8_t)(i & 0x7fU);
    }
    record.peer_addr_type = 1U;
    record.peer_addr[0] = 0x11U;
    record.peer_addr[5] = 0xeaU;
    assert(device_link_security_save_auth_record(&record) == ESP_OK);

    _reset_host();
    _init_service();
    atomic_store_explicit(&s_revoke_count, 0U, memory_order_release);
    atomic_store_explicit(&s_revoke_in_flight, 0, memory_order_release);
    assert(device_link_service_revoke_binding() == ESP_OK);
    assert(_wait_for(_revoke_requested, 500U));
    assert(_revoke_pending());
    /* The owner reports failure: the worker retries the enqueue. */
    _revoke_stub_fail();
    _pump_ms(300U);
    assert(atomic_load_explicit(&s_revoke_count, memory_order_acquire) > 1U);
    assert(_revoke_pending());
    /* The retry succeeds: the journal clears and the revoke converges. */
    _revoke_stub_complete();
    assert(_wait_for(_revoke_not_pending, 500U));
    assert(device_link_security_load_auth_record(&record) ==
           ESP_ERR_NOT_FOUND);
    _deinit_service();
}

typedef struct suspend_thread_arg
{
    esp_err_t result;
} suspend_thread_arg_t;

typedef struct deinit_thread_arg
{
    uint32_t timeout_ms;
    atomic_bool entered;
    esp_err_t result;
} deinit_thread_arg_t;

typedef struct api_acquire_barrier
{
    atomic_bool entered;
    atomic_bool release;
} api_acquire_barrier_t;

typedef struct get_status_thread_arg
{
    device_link_service_status_t status;
    esp_err_t result;
} get_status_thread_arg_t;

static void *_suspend_thread(void *arg)
{
    suspend_thread_arg_t *thread_arg = arg;

    thread_arg->result = device_link_service_suspend(5000U);
    return NULL;
}

static void *_suspend_forever_thread(void *arg)
{
    suspend_thread_arg_t *thread_arg = arg;

    thread_arg->result = device_link_service_suspend(
                             DEVICE_LINK_SERVICE_WAIT_FOREVER);
    return NULL;
}

static void *_deinit_thread(void *arg)
{
    deinit_thread_arg_t *thread_arg = arg;

    atomic_store_explicit(&thread_arg->entered, true, memory_order_release);
    thread_arg->result = device_link_service_deinit(thread_arg->timeout_ms);
    return NULL;
}

static void _api_acquire_barrier(void *arg)
{
    api_acquire_barrier_t *barrier = arg;

    if (!atomic_exchange_explicit(&barrier->entered, true,
                                  memory_order_acq_rel))
    {
        while (!atomic_load_explicit(&barrier->release,
                                     memory_order_acquire))
        {
            vTaskDelay(1U);
        }
    }
}

static void *_get_status_thread(void *arg)
{
    get_status_thread_arg_t *thread_arg = arg;

    thread_arg->result = device_link_service_get_status(&thread_arg->status);
    return NULL;
}


static void _test_suspend_multiple_pending_and_cancel(void)
{
    /* Two consecutive suspends both fail their close: both stay pending
     * and are confirmed together when the close recovers. */
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

    atomic_store_explicit(&s_fail_adv_stop, true, memory_order_release);
    pthread_t first;
    pthread_t second;
    suspend_thread_arg_t first_arg = {.result = ESP_FAIL};
    suspend_thread_arg_t second_arg = {.result = ESP_FAIL};

    assert(pthread_create(&first, NULL, _suspend_thread, &first_arg) == 0);
    _pump_ms(300U);
    assert(pthread_create(&second, NULL, _suspend_thread, &second_arg) == 0);
    _pump_ms(300U);
    /* Neither was confirmed while the close could not converge. */
    atomic_store_explicit(&s_fail_adv_stop, false, memory_order_release);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);
    assert(first_arg.result == ESP_OK);
    assert(second_arg.result == ESP_OK);
    assert(_wait_for(_status_suspended, 500U));
    _deinit_service();
}

static void _test_suspend_waits_forever_cancelled_by_resume(void)
{
    /* A WAIT_FOREVER suspend whose close cannot converge is cancelled by
     * a RESUME: the waiter returns TIMEOUT instead of blocking forever
     * (and deinit is never starved by the held API user count). */
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

    atomic_store_explicit(&s_fail_adv_stop, true, memory_order_release);
    pthread_t thread;
    suspend_thread_arg_t thread_arg = {.result = ESP_FAIL};

    assert(pthread_create(&thread, NULL, _suspend_forever_thread,
                          &thread_arg) == 0);
    _pump_ms(400U);
    assert(device_link_service_resume(1000U) == ESP_OK);
    assert(pthread_join(thread, NULL) == 0);
    assert(thread_arg.result == ESP_ERR_TIMEOUT);
    {
        device_link_service_status_t status;

        assert(device_link_service_get_status(&status) == ESP_OK);
        assert(status.state != DEVICE_LINK_SERVICE_STATE_SUSPENDED);
    }
    atomic_store_explicit(&s_fail_adv_stop, false, memory_order_release);
    _deinit_service();
}

static void _test_concurrent_deinit_before_command_admission(void)
{
    /* A WAIT_FOREVER suspend keeps one API user admitted while its physical
     * close fails. The first deinit therefore remains in its pre-command
     * drain phase. A concurrent deinit must fail as a lifecycle collision;
     * treating STOPPING as proof that DEINIT was already queued would make
     * the second caller wait for an exit signal that nobody arranged. */
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

    atomic_store_explicit(&s_fail_adv_stop, true, memory_order_release);
    pthread_t suspend_thread;
    suspend_thread_arg_t suspend_arg = {.result = ESP_FAIL};

    assert(pthread_create(&suspend_thread, NULL, _suspend_forever_thread,
                          &suspend_arg) == 0);
    _pump_ms(200U);

    pthread_t deinit_thread;
    deinit_thread_arg_t deinit_arg =
    {
        .timeout_ms = 300U,
        .entered = ATOMIC_VAR_INIT(false),
        .result = ESP_FAIL,
    };

    assert(pthread_create(&deinit_thread, NULL, _deinit_thread,
                          &deinit_arg) == 0);
    while (!atomic_load_explicit(&deinit_arg.entered, memory_order_acquire))
    {
        vTaskDelay(1U);
    }
    device_link_service_status_t status;

    for (unsigned int attempt = 0U; attempt < 100U; ++attempt)
    {
        if (device_link_service_get_status(&status) == ESP_ERR_INVALID_STATE)
        {
            break;
        }
        vTaskDelay(1U);
    }
    assert(device_link_service_get_status(&status) == ESP_ERR_INVALID_STATE);
    assert(device_link_service_deinit(50U) == ESP_ERR_INVALID_STATE);
    assert(pthread_join(deinit_thread, NULL) == 0);
    assert(deinit_arg.result == ESP_ERR_TIMEOUT);

    /* The timed-out coordinator restored RUNNING. Cancel the retained
     * suspend, recover the port, and prove normal teardown remains viable. */
    assert(device_link_service_resume(1000U) == ESP_OK);
    assert(pthread_join(suspend_thread, NULL) == 0);
    assert(suspend_arg.result == ESP_ERR_TIMEOUT);
    atomic_store_explicit(&s_fail_adv_stop, false, memory_order_release);
    _deinit_service();
}

static void _test_api_admission_rejects_retired_instance(void)
{
    /* Pause an API after it sampled RUNNING but before its admission CAS.
     * A full deinit/reinit returns the lifecycle bits to RUNNING; only the
     * packed instance generation distinguishes that ABA and prevents the old
     * call from entering the new queue/mutex instance. */
    _reset_host();
    _init_service();
    api_acquire_barrier_t barrier =
    {
        .entered = ATOMIC_VAR_INIT(false),
        .release = ATOMIC_VAR_INIT(false),
    };
    get_status_thread_arg_t status_arg = {.result = ESP_FAIL};
    pthread_t status_thread;

    device_link_service_test_set_api_acquire_hook(
        _api_acquire_barrier, &barrier);
    assert(pthread_create(&status_thread, NULL, _get_status_thread,
                          &status_arg) == 0);
    while (!atomic_load_explicit(&barrier.entered, memory_order_acquire))
    {
        vTaskDelay(1U);
    }
    assert(device_link_service_deinit(DEVICE_LINK_SERVICE_WAIT_FOREVER) ==
           ESP_OK);
    _reset_host();
    _init_service();
    atomic_store_explicit(&barrier.release, true, memory_order_release);
    assert(pthread_join(status_thread, NULL) == 0);
    assert(status_arg.result == ESP_ERR_INVALID_STATE);
    device_link_service_test_set_api_acquire_hook(NULL, NULL);
    _deinit_service();
}


static void _test_suspend_cancel_then_fail_again(void)
{
    /* Fill the whole pending set with failing suspends, cancel them ALL
     * with RESUME, then verify a new WAIT_FOREVER suspend is admitted and
     * cancellable again: the bounded pending set and result slots are
     * fully reusable after cancellation. */
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

    atomic_store_explicit(&s_fail_adv_stop, true, memory_order_release);
    pthread_t threads[DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];
    suspend_thread_arg_t args[DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];

    for (unsigned int i = 0U; i < DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS;
            ++i)
    {
        args[i].result = ESP_FAIL;
        assert(pthread_create(&threads[i], NULL, _suspend_thread,
                              &args[i]) == 0);
    }
    _pump_ms(600U);
    assert(device_link_service_resume(1000U) == ESP_OK);
    for (unsigned int i = 0U; i < DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS;
            ++i)
    {
        assert(pthread_join(threads[i], NULL) == 0);
        TEST_ASSERT_TRUE(args[i].result == ESP_ERR_TIMEOUT);
    }
    /* The capacity is restored: a new WAIT_FOREVER suspend is admitted
     * and can be cancelled again. */
    pthread_t thread;
    suspend_thread_arg_t thread_arg = {.result = ESP_FAIL};

    assert(pthread_create(&thread, NULL, _suspend_forever_thread,
                          &thread_arg) == 0);
    _pump_ms(400U);
    assert(device_link_service_resume(1000U) == ESP_OK);
    assert(pthread_join(thread, NULL) == 0);
    assert(thread_arg.result == ESP_ERR_TIMEOUT);
    atomic_store_explicit(&s_fail_adv_stop, false, memory_order_release);
    _deinit_service();
}

static void _test_suspend_outstanding_cap_enforced(void)
{
    /* With the window open and the close persistently failing, SLOTS
     * suspends stay outstanding (their result slots occupied); the next
     * suspend is refused at allocation with a definite INVALID_STATE, and
     * once the close recovers every admitted waiter completes. */
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

    atomic_store_explicit(&s_fail_adv_stop, true, memory_order_release);
    pthread_t threads[DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];
    suspend_thread_arg_t args[DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS];
    unsigned int created = 0U;

    for (unsigned int i = 0U; i < DEVICE_LINK_SERVICE_SUSPEND_RESULT_SLOTS;
            ++i)
    {
        args[i].result = ESP_FAIL;
        assert(pthread_create(&threads[i], NULL, _suspend_thread,
                              &args[i]) == 0);
        created++;
    }
    /* Give every waiter time to be admitted and to fail its close. */
    _pump_ms(600U);
    /* The next suspend cannot be admitted: definite refusal, no hang. */
    TEST_ASSERT_TRUE(device_link_service_suspend(5000U) ==
                     ESP_ERR_INVALID_STATE);
    /* Recover: every admitted waiter completes. */
    atomic_store_explicit(&s_fail_adv_stop, false, memory_order_release);
    for (unsigned int i = 0U; i < created; ++i)
    {
        assert(pthread_join(threads[i], NULL) == 0);
        TEST_ASSERT_TRUE(args[i].result == ESP_OK);
    }
    _deinit_service();
}

static void _test_suspend_blocked_by_failed_close(void)
{
    /* A suspend whose window close cannot converge (the advertising stop
     * fails, so the lease release fails) must NOT be acknowledged; once
     * the close retried by the worker succeeds, the SAME suspend call
     * returns ESP_OK (no second suspend is submitted). */
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
    /* Fault the stop path so the close's lease release cannot converge. */
    atomic_store_explicit(&s_fail_adv_stop, true, memory_order_release);
    pthread_t thread;
    suspend_thread_arg_t thread_arg = {.result = ESP_FAIL};

    assert(pthread_create(&thread, NULL, _suspend_thread, &thread_arg) == 0);
    _pump_ms(400U);
    {
        device_link_service_status_t status;

        assert(device_link_service_get_status(&status) == ESP_OK);
        assert(status.active);
        assert(status.state == DEVICE_LINK_SERVICE_STATE_WINDOW);
    }
    /* Recover: the worker-tick close retry succeeds and confirms the
     * pending suspend sequence; the waiting caller returns ESP_OK. */
    atomic_store_explicit(&s_fail_adv_stop, false, memory_order_release);
    assert(pthread_join(thread, NULL) == 0);
    assert(thread_arg.result == ESP_OK);
    assert(_wait_for(_status_suspended, 500U));
    _deinit_service();
}

static void _test_revoke_binding(void)
{
    /* A committed authorization record exists, then the local revoke
     * journals the intent and erases the authorization in the worker;
     * the port stub records the bond-deletion request. */
    device_link_security_auth_record_t record;

    nv_storage_fake_reset();
    memset(&record, 0, sizeof(record));
    record.magic = DEVICE_LINK_SECURITY_AUTH_MAGIC;
    record.schema_version = DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION;
    _set_test_grants(&record);
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES; ++i)
    {
        record.credential_id[i] = (uint8_t)(i + 1U);
        record.device_auth_id[i] = (uint8_t)(0x40U + i);
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_SALT_BYTES; ++i)
    {
        record.salt[i] = (uint8_t)(0x30U + i);
    }
    for (size_t i = 0U; i < DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES; ++i)
    {
        record.verifier[i] = (uint8_t)(i & 0x7fU);
    }
    record.peer_addr_type = 1U;
    record.peer_addr[0] = 0x11U;
    record.peer_addr[5] = 0xeaU;
    assert(device_link_security_save_auth_record(&record) == ESP_OK);

    _reset_host();
    _init_service();
    atomic_store_explicit(&s_revoke_count, 0U, memory_order_release);
    atomic_store_explicit(&s_revoke_in_flight, 0, memory_order_release);
    assert(device_link_service_revoke_binding() == ESP_OK);
    /* The worker issued the deletion request; the test-side owner then
     * completes it (journal cleared), and the revoke converges with
     * exactly one enqueue. */
    assert(_wait_for(_revoke_requested, 500U));
    _revoke_stub_complete();
    assert(_wait_for(_revoke_not_pending, 500U));
    assert(!_revoke_pending());
    assert(device_link_security_load_auth_record(&record) ==
           ESP_ERR_NOT_FOUND);
    assert(atomic_load_explicit(&s_revoke_count, memory_order_acquire) == 1U);
    _deinit_service();
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
    assert(strcmp(field, "link-v2") == 0);
    assert(_qr_field(qr, "name", field, sizeof(field)) != NULL);
    assert(strcmp(field, "MT") == 0);
    assert(_qr_field(qr, "service", field, sizeof(field)) != NULL);
    assert(strcmp(field,
                  "2c77e48c-c510-4230-8d05-63d036dc038b") == 0);
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

static void _test_close_window_retains_connected_peer_termination(void)
{
    _reset_host();
    _init_service();
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    _adv_converge();

    /* Model an already-bound peer connected during the window. There is no
     * provisional cleanup to trigger termination, so close must retain the
     * ACL obligation independently. */
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_CONNECT;
    event.status = 0;
    event.accepted = true;
    event.conn_handle = TEST_CONN_HANDLE;
    event.identity.generation = 1U;
    event.identity.kind = BLE_LINK_OPERATION_CONNECT;
    event.identity.conn_handle = TEST_CONN_HANDLE;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    assert(_wait_for(_status_connected_now, 500U));

    atomic_store_explicit(&s_fail_disconnect, true, memory_order_release);
    assert(device_link_service_close_window() == ESP_OK);
    assert(_wait_for(_status_error_set, 500U));
    assert(atomic_load_explicit(&s_disconnect_count,
                                memory_order_acquire) >= 1U);
    _adv_converge();
    assert(ble_adv_manager_get_state() == BLE_ADV_MANAGER_STATE_STOPPED);

    const unsigned int failed_attempts = atomic_load_explicit(
            &s_disconnect_count,
            memory_order_acquire);

    atomic_store_explicit(&s_fail_disconnect, false, memory_order_release);
    host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
    assert(_wait_atomic_at_least(&s_disconnect_count,
                                 failed_attempts + 1U, 500U));

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_DISCONNECT;
    event.conn_handle = TEST_CONN_HANDLE;
    event.identity.generation = 1U;
    event.identity.kind = BLE_LINK_OPERATION_DISCONNECT;
    event.identity.conn_handle = TEST_CONN_HANDLE;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    assert(_wait_for(_status_disconnected, 500U));
    _adv_converge();
    assert(_slow_advertising_started());
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
    event.accepted = true;
    event.identity.generation = 1U;
    event.identity.kind = BLE_LINK_OPERATION_CONNECT;
    event.identity.conn_handle = event.conn_handle;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    assert(_wait_for(_status_connected_now, 500U));
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.client_connected);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_CONNECTED);
    assert(device_link_service_is_busy());

    /* A rejected ACL must never be tracked as the client. */
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_CONNECT;
    event.conn_handle = 2U;
    event.status = 0;
    event.accepted = false;
    event.identity.generation = 2U;
    event.identity.kind = BLE_LINK_OPERATION_CONNECT;
    event.identity.conn_handle = event.conn_handle;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    _pump_ms(20U);
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.client_connected);

    /* A late disconnect for a retired handle must not clear the live
     * connection. */
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_DISCONNECT;
    event.conn_handle = 7U;
    event.identity.generation = 1U;
    event.identity.kind = BLE_LINK_OPERATION_DISCONNECT;
    event.identity.conn_handle = event.conn_handle;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    _pump_ms(20U);
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.client_connected);

    /* Retire generation 1, then reuse the same numeric handle. */
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_DISCONNECT;
    event.conn_handle = 1U;
    event.identity.generation = 1U;
    event.identity.kind = BLE_LINK_OPERATION_DISCONNECT;
    event.identity.conn_handle = event.conn_handle;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    assert(_wait_for(_status_disconnected, 500U));

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_CONNECT;
    event.conn_handle = 1U;
    event.status = 0;
    event.accepted = true;
    event.identity.generation = 2U;
    event.identity.kind = BLE_LINK_OPERATION_CONNECT;
    event.identity.conn_handle = event.conn_handle;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    assert(_wait_for(_status_connected_now, 500U));

    /* A retired generation with the reused handle must not clear the new
     * connection. */
    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_DISCONNECT;
    event.conn_handle = 1U;
    event.identity.generation = 1U;
    event.identity.kind = BLE_LINK_OPERATION_DISCONNECT;
    event.identity.conn_handle = event.conn_handle;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    _pump_ms(20U);
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.client_connected);

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_DISCONNECT;
    event.conn_handle = 1U;
    event.identity.generation = 2U;
    event.identity.kind = BLE_LINK_OPERATION_DISCONNECT;
    event.identity.conn_handle = event.conn_handle;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    assert(_wait_for(_status_disconnected, 500U));
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(!status.client_connected);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_ADVERTISING);
    assert(!device_link_service_is_busy());
    _deinit_service();
}

static void _test_starting_callback_before_worker_publication(void)
{
    _reset_host();
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    atomic_store_explicit(&s_emit_connect_during_start, true,
                          memory_order_release);

    assert(device_link_service_init(&s_config) == ESP_OK);
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.client_connected);
    assert(status.state == DEVICE_LINK_SERVICE_STATE_CONNECTED);
    /* The callback ran while the atomic worker handle was NULL. A later
     * disconnect proves the published handle and catch-up wake path are live. */
    ble_port_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_PORT_EVENT_DISCONNECT;
    event.conn_handle = TEST_CONN_HANDLE;
    event.identity.generation = 1U;
    event.identity.kind = BLE_LINK_OPERATION_DISCONNECT;
    event.identity.conn_handle = event.conn_handle;
    assert(ble_event_router_dispatch(&event) == ESP_OK);
    assert(_wait_for(_status_disconnected, 500U));
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

static void _test_open_and_close_fault_recovery(void)
{
    _reset_host();
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = 60000U;
    assert(device_link_service_init(&s_config) == ESP_OK);
    _adv_converge();
    assert(_wait_for(_status_available, 500U));

    /* First open reaches the final bindable START and that START fails. The
     * visible window stays inactive while the worker retains the physical
     * lease, gate, verifier, and resume cleanup obligations. */
    s_fail_adv_start = true;
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_error_set, 500U));
    assert(_wait_for(_status_not_active, 500U));
    s_fail_adv_start = false;
    /* No external OPEN is needed to repair the failed resume. The worker
     * retained the lease/release and resume obligations and restores the
     * slow advertisement once the port recovers. */
    assert(_wait_for(_slow_advertising_started, 1000U));
    _adv_converge();
    assert(ble_adv_manager_get_state() == BLE_ADV_MANAGER_STATE_SLOW);
    assert(!atomic_load_explicit(&s_pairing_gate_open,
                                 memory_order_acquire));
    assert(!(ble_link_session_get_state_flags() &
             BLE_LINK_STATE_FLAG_BINDABLE));
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    _adv_converge();

    /* A close whose physical STOP cannot converge keeps the window and
     * lease, marks the close pending, and retries without closing the SMP
     * gate ahead of the live bindable advertisement. */
    atomic_store_explicit(&s_fail_adv_stop, true, memory_order_release);
    assert(device_link_service_close_window() == ESP_OK);
    assert(_wait_for(_status_error_set, 500U));
    {
        device_link_service_status_t status;

        assert(device_link_service_get_status(&status) == ESP_OK);
        assert(status.active);
    }
    atomic_store_explicit(&s_fail_adv_stop, false, memory_order_release);
    assert(_wait_for(_status_not_active, 500U));
    assert(!(ble_link_session_get_state_flags() &
             BLE_LINK_STATE_FLAG_BINDABLE));

    /* A subsequent open converges again (through a stop for the payload
     * change) and the window opens. */
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    s_fail_adv_start = false;
    _deinit_service();
}

static void _test_open_cleanup_gate_retry(void)
{
    _reset_host();
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = &s_test_port;
    s_config.task_priority = 4U;
    s_config.window_ms = 60000U;
    assert(device_link_service_init(&s_config) == ESP_OK);
    _adv_converge();
    assert(_wait_for(_status_available, 500U));

    /* The bindable START fails after the lease and gate were acquired. The
     * first rollback also cannot close the gate. Both obligations must stay
     * with the worker instead of being dropped with this OPEN command. */
    atomic_store_explicit(&s_fail_adv_start, true, memory_order_release);
    atomic_store_explicit(&s_fail_pairing_gate_close, true,
                          memory_order_release);
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_error_set, 500U));
    assert(_wait_for(_status_not_active, 500U));
    assert(atomic_load_explicit(&s_pairing_gate_open,
                                memory_order_acquire));

    /* Let the ADV owner recover while the gate operation keeps failing.
     * Repeated attempts prove the cleanup duty is owner state, not a queue
     * item that can be consumed once and lost. */
    atomic_store_explicit(&s_fail_adv_start, false, memory_order_release);
    assert(_wait_for(_pairing_gate_close_attempted, 1000U));
    const unsigned int first_attempts = atomic_load_explicit(
                                            &s_pairing_gate_close_count,
                                            memory_order_acquire);

    _pump_ms(300U);
    assert(atomic_load_explicit(&s_pairing_gate_close_count,
                                memory_order_acquire) > first_attempts);
    assert(atomic_load_explicit(&s_pairing_gate_open,
                                memory_order_acquire));

    atomic_store_explicit(&s_fail_pairing_gate_close, false,
                          memory_order_release);
    assert(_wait_for(_pairing_gate_closed, 1000U));
    assert(_wait_for(_slow_advertising_started, 1000U));
    _adv_converge();
    assert(ble_adv_manager_get_state() == BLE_ADV_MANAGER_STATE_SLOW);
    assert(!(ble_link_session_get_state_flags() &
             BLE_LINK_STATE_FLAG_BINDABLE));

    /* A fresh window proves the retained lease identity was released and
     * did not block or alias the next owner lease. */
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
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
    /* Security, link-session, link-service, and GATT mutexes are boot-lifetime. */
    assert(host_freertos_live_semaphores() <= 4U);
    assert((ble_link_session_get_state_flags() &
            BLE_LINK_STATE_FLAG_BINDABLE) == 0U);
    device_link_service_status_t status;

    assert(device_link_service_get_status(&status) == ESP_ERR_INVALID_STATE);
    assert(!device_link_service_pending_confirmation());
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

static uint64_t _protocol_probe_authorization(
    uint64_t commit_request_id,
    uint8_t txn_id[8], uint8_t credential[16])
{
    protocol_response_t response;
    uint8_t commit[64];

    _protocol_exchange(
        s_protocol_prepare_request, sizeof(s_protocol_prepare_request),
        &response);
    assert(response.request_id == 3U);
    _protocol_capture_prepare(&response, txn_id, credential);
    const size_t commit_len = _protocol_build_commit(
                                  commit, sizeof(commit), commit_request_id,
                                  txn_id, credential);

    _protocol_exchange(commit, commit_len, &response);
    assert(response.request_id == commit_request_id);
    assert(response.error == BLE_LINK_ERROR_CONFIRMATION_REQUIRED);
    assert(response.body == BLE_LINK_CODEC_RESPONSE_NONE);
    device_link_service_status_t status;

    assert(_protocol_wait_confirmation(true, 0U, &status, 500U));
    assert(status.confirmation_token != 0U);
    return status.confirmation_token;
}

static void _test_protocol_commit_probe_confirmation(void)
{
    uint8_t txn_id[8];
    uint8_t credential[16];
    uint8_t commit[64];
    protocol_response_t response;
    device_link_service_status_t status;

    nv_storage_fake_reset();
    _reset_host();
    _init_service();
    _protocol_establish_bootstrap_session(1U);

    const uint64_t token = _protocol_probe_authorization(
                               5U, txn_id, credential);

    assert(token != 0U);
    assert(device_link_service_confirm_binding(token, true) == ESP_OK);
    assert(_protocol_wait_confirmation(false, 0U, &status, 500U));
    assert(status.confirmation_token == 0U);
    assert(status.last_error == ESP_OK);
    const unsigned publish_before_commit = atomic_load_explicit(
            &s_link_state_publish_count, memory_order_acquire);

    const size_t commit_len = _protocol_build_commit(
                                  commit, sizeof(commit), 6U,
                                  txn_id, credential);

    _protocol_exchange(commit, commit_len, &response);
    assert(response.request_id == 6U);
    assert(response.error == BLE_LINK_ERROR_OK);
    assert(response.body == BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT);
    assert((ble_link_session_get_state_flags() &
            BLE_LINK_STATE_FLAG_BOUND) != 0U);
    assert(atomic_load_explicit(&s_link_state_publish_count,
                                memory_order_acquire) >
           publish_before_commit);
    device_link_security_auth_record_t record;

    memset(&record, 0, sizeof(record));
    assert(device_link_security_load_auth_record(&record) == ESP_OK);
    assert(memcmp(record.credential_id, credential, sizeof(credential)) == 0);
    _deinit_service();
    nv_storage_fake_reset();
}

static void _test_protocol_stale_confirmation_token(void)
{
    uint8_t txn_id[8];
    uint8_t credential[16];
    device_link_service_status_t status;

    nv_storage_fake_reset();
    _reset_host();
    _init_service();
    _protocol_establish_session(1U);

    const uint64_t stale_token = _protocol_probe_authorization(
                                     5U, txn_id, credential);

    assert(device_link_service_confirm_binding(stale_token, false) == ESP_OK);
    assert(_protocol_wait_confirmation(false, 0U, NULL, 500U));
    _protocol_reopen_session(1U);
    const uint64_t current_token = _protocol_probe_authorization(
                                       6U, txn_id, credential);

    assert(current_token != stale_token);
    assert(device_link_service_get_status(&status) == ESP_OK);
    const uint64_t generation_before = status.generation;

    assert(device_link_service_confirm_binding(stale_token, true) == ESP_OK);
    assert(_protocol_wait_status_error_after(
               generation_before, ESP_ERR_INVALID_STATE,
               &status, 500U));
    assert(status.pending_confirmation);
    assert(status.confirmation_token == current_token);

    assert(device_link_service_confirm_binding(current_token, false) == ESP_OK);
    assert(_protocol_wait_confirmation(false, 0U, NULL, 500U));
    _deinit_service();
    nv_storage_fake_reset();
}

static void _test_protocol_stale_generation_confirmation(void)
{
    uint8_t txn_id[8];
    uint8_t credential[16];
    device_link_service_status_t status;

    nv_storage_fake_reset();
    _reset_host();
    _init_service();
    _protocol_establish_session(1U);

    const uint64_t retired_token = _protocol_probe_authorization(
                                       5U, txn_id, credential);

    _protocol_disconnect_session(1U);
    _protocol_establish_session(2U);
    const uint64_t current_token = _protocol_probe_authorization(
                                       6U, txn_id, credential);

    assert(current_token != retired_token);
    assert(device_link_service_get_status(&status) == ESP_OK);
    const uint64_t generation_before = status.generation;

    assert(device_link_service_confirm_binding(retired_token, true) == ESP_OK);
    assert(_protocol_wait_status_error_after(
               generation_before, ESP_ERR_INVALID_STATE,
               &status, 500U));
    assert(status.pending_confirmation);
    assert(status.confirmation_token == current_token);

    assert(device_link_service_confirm_binding(current_token, false) == ESP_OK);
    assert(_protocol_wait_confirmation(false, 0U, NULL, 500U));
    _deinit_service();
    nv_storage_fake_reset();
}

static void _test_protocol_recovery_storage_errors(void)
{
    uint8_t request[64];
    uint8_t credential[16];
    protocol_response_t response;

    for (size_t i = 0U; i < sizeof(credential); ++i)
    {
        credential[i] = (uint8_t)(i + 1U);
    }
    nv_storage_fake_reset();
    _reset_host();
    _init_service();
    _protocol_establish_session(1U);

    size_t request_len = _protocol_build_recovery_query(
                             request, sizeof(request), 10U, credential);

    _protocol_exchange(request, request_len, &response);
    assert(response.request_id == 10U);
    assert(response.error == BLE_LINK_ERROR_NOT_FOUND);
    assert(response.body == BLE_LINK_CODEC_RESPONSE_NONE);

    _protocol_save_auth_record(credential);
    request_len = _protocol_build_recovery_query(
                      request, sizeof(request), 11U, credential);
    _protocol_exchange(request, request_len, &response);
    assert(response.request_id == 11U);
    assert(response.error == BLE_LINK_ERROR_OK);
    assert(response.body == BLE_LINK_CODEC_RESPONSE_AUTHORIZATION_RESULT);

    nv_storage_fake_fail_next_get(ESP_FAIL);
    request_len = _protocol_build_recovery_query(
                      request, sizeof(request), 12U, credential);
    _protocol_exchange(request, request_len, &response);
    assert(response.request_id == 12U);
    assert(response.error == BLE_LINK_ERROR_STORAGE);
    assert(response.body == BLE_LINK_CODEC_RESPONSE_NONE);

    device_link_security_auth_record_t malformed;

    memset(&malformed, 0, sizeof(malformed));
    assert(nv_storage_set_blob(
               "dls.auth", &malformed, sizeof(malformed)) == ESP_OK);
    request_len = _protocol_build_recovery_query(
                      request, sizeof(request), 13U, credential);
    _protocol_exchange(request, request_len, &response);
    assert(response.request_id == 13U);
    assert(response.error == BLE_LINK_ERROR_INTERNAL);
    assert(response.body == BLE_LINK_CODEC_RESPONSE_NONE);

    _protocol_save_auth_record(credential);
    uint8_t wrong_credential[16] = {0U};

    request_len = _protocol_build_recovery_query(
                      request, sizeof(request), 14U, wrong_credential);
    _protocol_exchange(request, request_len, &response);
    assert(response.request_id == 14U);
    assert(response.error == BLE_LINK_ERROR_NOT_FOUND);
    assert(response.body == BLE_LINK_CODEC_RESPONSE_NONE);

    _deinit_service();
    nv_storage_fake_reset();
}

static bool _retained_cleanup_cleared(void)
{
    return !ble_link_service_retained_cleanup_pending();
}

static void _test_retained_provisional_cleanup_worker_backoff(void)
{
    protocol_response_t response;

    nv_storage_fake_reset();
    _reset_host();
    s_protocol_security_ops = &s_cleanup_security_ops;
    atomic_store_explicit(&s_cleanup_result, ESP_ERR_NO_MEM,
                          memory_order_release);
    _init_service();
    _protocol_establish_bootstrap_session(1U);
    _protocol_exchange(
        s_protocol_prepare_request, sizeof(s_protocol_prepare_request),
        &response);
    assert(response.request_id == 3U);

    /* Model the NimBLE terminal path retaining a provisional delete while
     * the real Device Link worker owns every subsequent retry. */
    ble_link_service_clear_session_state();
    assert(atomic_load_explicit(&s_cleanup_call_count,
                                memory_order_acquire) == 1U);
    assert(ble_link_service_retained_cleanup_pending());
    assert(ble_link_service_retained_retry_remaining_ms() > 0U);
    assert(ble_link_service_retained_retry_remaining_ms() <= 100U);

    for (unsigned int notification = 0U; notification < 64U; ++notification)
    {
        ble_link_service_wake_owner();
    }
    _pump_ms(20U);
    assert(atomic_load_explicit(&s_cleanup_call_count,
                                memory_order_acquire) == 1U);

    host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
    assert(_wait_atomic_at_least(&s_cleanup_call_count, 2U, 500U));
    assert(ble_link_service_retained_retry_remaining_ms() > 0U);
    assert(ble_link_service_retained_retry_remaining_ms() <= 200U);
    for (unsigned int notification = 0U; notification < 64U; ++notification)
    {
        ble_link_service_wake_owner();
    }
    _pump_ms(20U);
    assert(atomic_load_explicit(&s_cleanup_call_count,
                                memory_order_acquire) == 2U);

    atomic_store_explicit(&s_cleanup_result, ESP_OK,
                          memory_order_release);
    host_freertos_advance_ticks(pdMS_TO_TICKS(200U));
    assert(_wait_atomic_at_least(&s_cleanup_call_count, 3U, 500U));
    assert(_wait_for(_retained_cleanup_cleared, 500U));
    assert(!ble_link_service_retained_cleanup_pending());
    assert(ble_link_service_retained_retry_remaining_ms() == UINT32_MAX);
    _deinit_service();
    nv_storage_fake_reset();
}

static void _test_window_close_cleanup_pause_outlives_window_owner(void)
{
    protocol_response_t response;

    nv_storage_fake_reset();
    _reset_host();
    s_protocol_security_ops = &s_cleanup_security_ops;
    atomic_store_explicit(&s_cleanup_manages_pause, true,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_result, ESP_ERR_NO_MEM,
                          memory_order_release);
    _init_service();
    assert(device_link_service_open_window() == ESP_OK);
    assert(_wait_for(_status_active, 500U));
    _adv_converge();
    _protocol_establish_bootstrap_session(1U);
    _protocol_exchange(
        s_protocol_prepare_request, sizeof(s_protocol_prepare_request),
        &response);
    assert(response.request_id == 3U);

    const unsigned int starts_before_close = s_adv_start_count;

    assert(device_link_service_close_window() == ESP_OK);
    assert(_wait_for(_status_not_active, 500U));
    _adv_converge();
    assert(ble_link_service_retained_cleanup_pending());
    assert(ble_adv_manager_get_state() == BLE_ADV_MANAGER_STATE_STOPPED);

    /* Releasing WINDOW_TRANSITION did not release PEER_CLEANUP. Notification
     * storms also cannot bypass the retained cleanup backoff or expose slow
     * advertising while deletion still fails. */
    for (unsigned int notification = 0U; notification < 64U; ++notification)
    {
        ble_link_service_wake_owner();
    }
    _pump_ms(20U);
    _adv_converge();
    assert(s_adv_start_count == starts_before_close);
    assert(ble_adv_manager_get_state() == BLE_ADV_MANAGER_STATE_STOPPED);

    atomic_store_explicit(&s_cleanup_result, ESP_OK,
                          memory_order_release);
    host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
    assert(_wait_for(_retained_cleanup_cleared, 500U));
    _adv_converge();
    assert(_slow_advertising_started());
    _deinit_service();
    nv_storage_fake_reset();
}

static void _test_retained_replacement_worker_backoff(void)
{
    const ble_link_operation_identity_t replacement =
    {
        .generation = 1U,
        .security_epoch = 1U,
        .token = 77U,
        .kind = BLE_LINK_OPERATION_REMOTE_REPLACEMENT,
        .conn_handle = TEST_CONN_HANDLE,
    };

    nv_storage_fake_reset();
    _reset_host();
    s_protocol_security_ops = &s_cleanup_security_ops;
    atomic_store_explicit(&s_replacement_result, ESP_FAIL,
                          memory_order_release);
    _init_service();
    assert(ble_link_service_register_remote_replacement(&replacement) ==
           ESP_OK);
    assert(_wait_atomic_at_least(&s_replacement_call_count, 1U, 500U));
    assert(ble_link_service_retained_cleanup_pending());
    for (unsigned int notification = 0U; notification < 64U; ++notification)
    {
        ble_link_service_wake_owner();
    }
    _pump_ms(20U);
    assert(atomic_load_explicit(&s_replacement_call_count,
                                memory_order_acquire) == 1U);

    host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
    assert(_wait_atomic_at_least(&s_replacement_call_count, 2U, 500U));
    for (unsigned int notification = 0U; notification < 64U; ++notification)
    {
        ble_link_service_wake_owner();
    }
    _pump_ms(20U);
    assert(atomic_load_explicit(&s_replacement_call_count,
                                memory_order_acquire) == 2U);

    atomic_store_explicit(&s_replacement_result, ESP_OK,
                          memory_order_release);
    host_freertos_advance_ticks(pdMS_TO_TICKS(200U));
    assert(_wait_for(_retained_cleanup_cleared, 500U));
    assert(atomic_load_explicit(&s_replacement_call_count,
                                memory_order_acquire) == 3U);
    assert(ble_link_service_retained_retry_remaining_ms() == UINT32_MAX);
    _deinit_service();
    nv_storage_fake_reset();
}

static bool _port_cleanup_retained(void)
{
    return ble_nimble_port_cleanup_pending();
}

static void _test_deinit_drains_replacement_before_runtime_stop(void)
{
    const ble_link_operation_identity_t replacement =
    {
        .generation = 1U,
        .security_epoch = 1U,
        .token = 99U,
        .kind = BLE_LINK_OPERATION_REMOTE_REPLACEMENT,
        .conn_handle = TEST_CONN_HANDLE,
    };

    nv_storage_fake_reset();
    _reset_host();
    s_protocol_security_ops = &s_cleanup_security_ops;
    atomic_store_explicit(&s_replacement_result, ESP_FAIL,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_handoff_to_port, true,
                          memory_order_release);
    _init_service();
    assert(ble_link_service_register_remote_replacement(&replacement) ==
           ESP_OK);
    assert(_wait_atomic_at_least(&s_replacement_call_count, 1U, 500U));
    assert(ble_link_service_retained_cleanup_pending());

    /* DEINIT lands during the first 100 ms retry cooldown. A bounded caller
     * times out, but the worker and runtime remain alive with the immutable
     * replacement obligation intact. */
    assert(device_link_service_deinit(20U) == ESP_ERR_TIMEOUT);
    assert(s_port_started);
    assert(!s_port_stopped);
    assert(ble_link_service_retained_cleanup_pending());

    /* The retry can now be handed to the port. Service ownership clears, but
     * shutdown still cannot advance while physical store deletion is pending. */
    atomic_store_explicit(&s_replacement_result, ESP_OK,
                          memory_order_release);
    host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
    assert(_wait_for(_port_cleanup_retained, 500U));
    assert(!ble_link_service_retained_cleanup_pending());
    assert(device_link_service_deinit(20U) == ESP_ERR_TIMEOUT);
    assert(s_port_started);
    assert(!s_port_stopped);

    atomic_store_explicit(&s_port_cleanup_pending, false,
                          memory_order_release);
    assert(device_link_service_deinit(1000U) == ESP_OK);
    assert(s_port_stopped);
    nv_storage_fake_reset();
}

static void _test_deinit_rechecks_after_final_host_barrier(void)
{
    nv_storage_fake_reset();
    _reset_host();
    s_protocol_security_ops = &s_cleanup_security_ops;
    atomic_store_explicit(&s_replacement_result, ESP_FAIL,
                          memory_order_release);
    atomic_store_explicit(&s_cleanup_barrier_inject_replacement, true,
                          memory_order_release);
    _init_service();

    /* The first empty observation is not sufficient: the final host barrier
     * injects a previously admitted replacement producer. Its retained
     * failure must be pumped and prevent runtime teardown. */
    assert(device_link_service_deinit(20U) == ESP_ERR_TIMEOUT);
    assert(atomic_load_explicit(&s_cleanup_barrier_injected,
                                memory_order_acquire));
    assert(atomic_load_explicit(&s_cleanup_barrier_count,
                                memory_order_acquire) >= 2U);
    assert(ble_link_service_retained_cleanup_pending());
    assert(s_port_started);
    assert(!s_port_stopped);

    atomic_store_explicit(&s_replacement_result, ESP_OK,
                          memory_order_release);
    host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
    assert(device_link_service_deinit(1000U) == ESP_OK);
    assert(!ble_link_service_retained_cleanup_pending());
    assert(s_port_stopped);
    nv_storage_fake_reset();
}

static void _test_deinit_drains_journaled_revoke_after_acl_terminal(void)
{
    uint8_t credential[DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES];

    nv_storage_fake_reset();
    _reset_host();
    atomic_store_explicit(&s_cleanup_barrier_retain_acl, true,
                          memory_order_release);
    _init_service();
    _protocol_save_auth_record(credential);
    assert(device_link_service_revoke_binding() == ESP_OK);
    assert(_wait_for(_revoke_requested, 500U));
    assert(_revoke_pending());

    /* Shutdown's host barrier retains the live ACL. Neither HCI submission
     * nor an in-flight revoke is enough to report a clean deinit. */
    assert(device_link_service_deinit(20U) == ESP_ERR_TIMEOUT);
    assert(atomic_load_explicit(&s_cleanup_barrier_acl_retained,
                                memory_order_acquire));
    assert(ble_nimble_port_cleanup_pending());
    assert(_revoke_pending());
    assert(!s_port_stopped);

    /* Model the exact DISCONNECT followed by verified peer-store deletion
     * and journal clearing. Only then may the same deinit converge. */
    atomic_store_explicit(&s_port_cleanup_pending, false,
                          memory_order_release);
    _revoke_stub_complete();
    assert(device_link_service_deinit(1000U) == ESP_OK);
    assert(!_revoke_pending());
    assert(s_port_stopped);
    nv_storage_fake_reset();
}


static void _test_operation_bridge_completes_table(void)
{
    _reset_host();
    _init_service();
    /* Admit one wifi operation through the public async API; the domain is
     * capability-gated off, but the boot-available table is not. */
    uint64_t table_id = 0U;

    assert(ble_link_service_async_operation_start(
               DEVICE_LINK_DOMAIN_WIFI, 4U, 42U, NULL, NULL,
               &table_id) == ESP_OK);
    assert(table_id != 0U);
    /* The terminal snapshot published by the connectivity worker is
     * bridged into the table: SUCCEEDED with the WifiStatus payload. */
    connectivity_manager_status_snapshot_t terminal;

    memset(&terminal, 0, sizeof(terminal));
    terminal.generation = 1U;
    terminal.operation_id = 42U;
    terminal.operation_complete = true;
    terminal.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    terminal.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    terminal.last_error = ESP_OK;
    terminal.profile_revision = CONNECTIVITY_MANAGER_PROFILE_REVISION_INITIAL;
    terminal.auto_connect = true;
    _test_publish_connectivity_status(&terminal);
    /* The record is terminal: updating the same owner is rejected. */
    assert(ble_link_service_async_operation_update(
               42U, DEVICE_LINK_OPERATION_RUNNING, DEVICE_LINK_STATUS_OK,
               NULL, 0U) == ESP_ERR_NOT_FOUND);
    /* Unknown owners (manager operations this service never admitted) are
     * ignored without affecting the table. */
    terminal.operation_id = 99U;
    terminal.operation_complete = true;
    terminal.last_error = ESP_ERR_INVALID_STATE;
    terminal.failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
    _test_publish_connectivity_status(&terminal);
    assert(ble_link_service_async_operation_update(
               99U, DEVICE_LINK_OPERATION_RUNNING, DEVICE_LINK_STATUS_OK,
               NULL, 0U) == ESP_ERR_NOT_FOUND);
    /* A failed terminal maps the classified failure to FAILED without a
     * result payload. */
    assert(ble_link_service_async_operation_start(
               DEVICE_LINK_DOMAIN_WIFI, 4U, 43U, NULL, NULL,
               &table_id) == ESP_OK);
    terminal.operation_id = 43U;
    terminal.operation_complete = true;
    terminal.last_error = ESP_ERR_INVALID_STATE;
    terminal.failure = CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION;
    _test_publish_connectivity_status(&terminal);
    assert(ble_link_service_async_operation_update(
               43U, DEVICE_LINK_OPERATION_RUNNING, DEVICE_LINK_STATUS_OK,
               NULL, 0U) == ESP_ERR_NOT_FOUND);
    /* A scan terminal completes a start_scan record with no payload. */
    connectivity_manager_scan_snapshot_t scan;

    assert(ble_link_service_async_operation_start(
               DEVICE_LINK_DOMAIN_WIFI, 2U, 44U, NULL, NULL,
               &table_id) == ESP_OK);
    memset(&scan, 0, sizeof(scan));
    scan.generation = 1U;
    scan.operation_id = 44U;
    scan.running = false;
    scan.last_error = ESP_OK;
    _test_publish_connectivity_scan(&scan);
    assert(ble_link_service_async_operation_update(
               44U, DEVICE_LINK_OPERATION_RUNNING, DEVICE_LINK_STATUS_OK,
               NULL, 0U) == ESP_ERR_NOT_FOUND);
    /* The bridge subscription dies with the service. */
    assert(s_connectivity_callback != NULL);
    _deinit_service();
    assert(s_connectivity_callback == NULL);
}

static void _test_bridge_survives_disabled_boot_enable(void)
{
    /* Persisted Bluetooth-disabled policy blob (little-endian host):
     * {magic 0x444c4254, version 1, enabled 0, reserved 0}. */
    static const uint8_t policy_disabled[8] =
    {
        0x54U, 0x42U, 0x4cU, 0x44U, 0x01U, 0x00U, 0x00U, 0x00U,
    };
    device_link_service_status_t status;

    _reset_host();
    nv_storage_fake_reset();
    assert(nv_storage_set_blob("dl_bt_policy", policy_disabled,
                               sizeof(policy_disabled)) == ESP_OK);
    const device_link_security_config_t security_config =
    {
        .username = "microtech",
        .session_id = 1U,
        .request_cb = _sec_stub_request,
        .request_arg = NULL,
    };

    assert(device_link_security_init(&security_config) == ESP_OK);
    memset(&s_config, 0, sizeof(s_config));
    s_config.runtime_port = s_runtime_port;
    s_config.task_priority = 4U;
    s_config.window_ms = TEST_WINDOW_MS;
    assert(device_link_service_init(&s_config) == ESP_OK);
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(!status.enabled);
    assert(!s_port_started);
    /* The completion bridge is installed at init even though BLE is off:
     * the Wi-Fi domain registers when the runtime is enabled later, and
     * operation completions must not be lost. */
    assert(s_connectivity_callback != NULL);
    assert(device_link_service_set_enabled(true,
                                           DEVICE_LINK_SERVICE_WAIT_FOREVER) == ESP_OK);
    assert(device_link_service_get_status(&status) == ESP_OK);
    assert(status.enabled);
    assert(s_port_started);
    assert(s_connectivity_callback != NULL);
    /* End-to-end after enable: admit, publish terminal, table completes. */
    uint64_t table_id = 0U;

    assert(ble_link_service_async_operation_start(
               DEVICE_LINK_DOMAIN_WIFI, 4U, 71U, NULL, NULL,
               &table_id) == ESP_OK);
    assert(table_id != 0U);
    connectivity_manager_status_snapshot_t terminal;

    memset(&terminal, 0, sizeof(terminal));
    terminal.generation = 1U;
    terminal.operation_id = 71U;
    terminal.operation_complete = true;
    terminal.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    terminal.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    terminal.last_error = ESP_OK;
    terminal.profile_revision = CONNECTIVITY_MANAGER_PROFILE_REVISION_INITIAL;
    terminal.auto_connect = true;
    _test_publish_connectivity_status(&terminal);
    assert(ble_link_service_async_operation_update(
               71U, DEVICE_LINK_OPERATION_RUNNING, DEVICE_LINK_STATUS_OK,
               NULL, 0U) == ESP_ERR_NOT_FOUND);
    _deinit_service();
    assert(s_connectivity_callback == NULL);
}

static void test_public_verifier_getter_fail_closed(void)
{
    /* The public verifier is installed only from a definite, non-zero
     * instance id. A failing getter must never install a verifier from
     * uninitialized stack data. */
    static const uint8_t peer_addr[6] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xe5};

    s_runtime_port = &s_public_test_port;
    s_public_id_getter_calls = 0U;
    s_public_id_getter_result = ESP_FAIL;
    memset(s_public_id_bytes, 0x5a, sizeof(s_public_id_bytes));
    _init_service();
    /* Window closed, public getter failed: no verifier may be selected. */
    assert(device_link_security_select_verifier(
               1U, peer_addr, sizeof(peer_addr), false) == ESP_OK);
    assert(device_link_security_selected_verifier() ==
           DEVICE_LINK_SECURITY_VERIFIER_NONE);
    _deinit_service();

    /* Success path: a definite instance id installs the public verifier. */
    s_public_id_getter_result = ESP_OK;
    s_public_id_bytes[0] = 0x12U;
    s_public_id_bytes[1] = 0x34U;
    s_public_id_bytes[2] = 0x56U;
    _init_service();
    assert(device_link_security_select_verifier(
               1U, peer_addr, sizeof(peer_addr), false) == ESP_OK);
    assert(device_link_security_selected_verifier() ==
           DEVICE_LINK_SECURITY_VERIFIER_PUBLIC);
    _deinit_service();

    /* An all-zero instance id is equally fail-closed. */
    s_public_id_getter_result = ESP_OK;
    memset(s_public_id_bytes, 0, sizeof(s_public_id_bytes));
    _init_service();
    assert(device_link_security_select_verifier(
               1U, peer_addr, sizeof(peer_addr), false) == ESP_OK);
    assert(device_link_security_selected_verifier() ==
           DEVICE_LINK_SECURITY_VERIFIER_NONE);
    _deinit_service();
    s_runtime_port = &s_test_port;
}

int main(void)
{
    _test_bad_configuration();
    _test_start_failure_rolls_back();
    _test_rollback_port_init_failure();
    _test_rollback_runtime_never_initialized();
    _test_rollback_lease_failure_retryable();
    _test_rollback_deinit_enqueue_failure_retryable();
    _test_factory_reset_startup_gate();
    _test_binding_confirmation();
    _test_revoke_binding();
    _test_revoke_async_retry();
    _test_revoke_fail_retries();
    _test_suspend_blocked_by_failed_close();
    _test_suspend_multiple_pending_and_cancel();
    _test_suspend_waits_forever_cancelled_by_resume();
    _test_concurrent_deinit_before_command_admission();
    _test_api_admission_rejects_retired_instance();
    _test_suspend_outstanding_cap_enforced();
    _test_suspend_cancel_then_fail_again();
    _test_window_lifecycle();
    _test_close_window();
    _test_close_window_retains_connected_peer_termination();
    _test_connect_events();
    _test_starting_callback_before_worker_publication();
    _test_suspend_resume();
    _test_close_after_pending_open();
    _test_remaining_time_publishes_periodically();
    _test_open_and_close_fault_recovery();
    _test_open_cleanup_gate_retry();
    _test_suspend_waits_for_its_own_command();
    _test_deinit_while_window_open();
    _test_reinit_after_deinit();
    test_public_verifier_getter_fail_closed();
    _test_protocol_commit_probe_confirmation();
    _test_protocol_stale_confirmation_token();
    _test_protocol_stale_generation_confirmation();
    _test_protocol_recovery_storage_errors();
    _test_retained_provisional_cleanup_worker_backoff();
    _test_window_close_cleanup_pause_outlives_window_owner();
    _test_retained_replacement_worker_backoff();
    _test_deinit_drains_replacement_before_runtime_stop();
    _test_deinit_rechecks_after_final_host_barrier();
    _test_deinit_drains_journaled_revoke_after_acl_terminal();
    _test_operation_bridge_completes_table();
    _test_bridge_survives_disabled_boot_enable();
    puts("device_link_service host tests passed");
    return 0;
}
