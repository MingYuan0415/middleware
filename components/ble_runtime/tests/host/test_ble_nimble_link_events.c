#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_adv_manager.h"
#include "ble_gap_manager.h"
#include "ble_gatt_registry.h"
#include "ble_link_gatt.h"
#include "ble_link_service.h"
#include "ble_link_session.h"
#include "ble_nimble_link_events.h"
#include "ble_tx_scheduler.h"
#include "device_link_v1.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); abort(); \
} } while (0)
#define EQ(expected, actual) do { \
    const long long e_ = (long long)(expected); \
    const long long a_ = (long long)(actual); \
    if (e_ != a_) { fprintf(stderr, "line %d: %s = %lld, expected %lld\n", \
        __LINE__, #actual, a_, e_); abort(); } \
} while (0)

#define BOOT_ID UINT64_C(72623859790382856)
#define HANDLE 7U
#define RX_HANDLE 0x10U
#define TX_HANDLE 0x12U

static const uint8_t s_rx_uuid[16] =
{
    0x31, 0x6a, 0x7b, 0x2f, 0x4c, 0x9c, 0x04, 0x9c,
    0x44, 0x4f, 0xf6, 0x65, 0x11, 0x8c, 0x2a, 0x8f,
};
static const uint8_t s_tx_uuid[16] =
{
    0x31, 0x6a, 0x7b, 0x2f, 0x4c, 0x9c, 0x04, 0x9c,
    0x44, 0x4f, 0xf6, 0x65, 0x12, 0x8c, 0x2a, 0x8f,
};
static const uint8_t s_peer[6] = { 1U, 2U, 3U, 4U, 5U, 6U };
static pthread_mutex_t s_port_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool s_holds_port;
static atomic_uint s_wakes;
static unsigned s_submit_calls;
static unsigned s_indications;
static uint8_t s_value[DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES];
static size_t s_length;
static ble_link_work_t *s_queued_work;
static bool s_check_wake_state;
static ble_adv_manager_state_t s_expected_wake_state;
static unsigned s_adv_starts;
static unsigned s_adv_stops;
static uint32_t s_start_generation;
static uint32_t s_stop_generation;

/* This is SDK/port bookkeeping only. Applying GATT/service state belongs to
 * ble_nimble_link_events_handle_update(), never to this fixture. */
typedef struct handoff_fixture
{
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool abba;
    bool output_entered;
    bool prepared;
    bool output_wants_port;
    bool output_got_port;
    bool pause_after_unlock;
    bool unlocked;
    bool resume;
    unsigned lock_calls;
    unsigned prepare_calls;
    unsigned unlock_calls;
    esp_err_t lock_result;
    esp_err_t prepare_result;
    ble_port_event_t event;
    esp_err_t result;
} handoff_fixture_t;

static handoff_fixture_t *s_blocking_port;
static void _block_port_output(handoff_fixture_t *f);

static void _fixture_init(handoff_fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    EQ(0, pthread_mutex_init(&f->mutex, NULL));
    EQ(0, pthread_cond_init(&f->changed, NULL));
}

static void _fixture_destroy(handoff_fixture_t *f)
{
    EQ(0, pthread_cond_destroy(&f->changed));
    EQ(0, pthread_mutex_destroy(&f->mutex));
}

static void _wake(void *arg)
{
    (void)arg;
    if (s_check_wake_state)
    {
        EQ(s_expected_wake_state, ble_adv_manager_get_state());
    }
    atomic_fetch_add_explicit(&s_wakes, 1U, memory_order_relaxed);
}

static esp_err_t _port_lock(void *arg)
{
    handoff_fixture_t *f = arg;
    ++f->lock_calls;
    if (f->lock_result != ESP_OK)
    {
        return f->lock_result;
    }
    EQ(0, pthread_mutex_lock(&s_port_lock));
    CHECK(!s_holds_port);
    s_holds_port = true;
    return ESP_OK;
}

static void _port_unlock(void *arg)
{
    handoff_fixture_t *f = arg;
    CHECK(s_holds_port);
    s_holds_port = false;
    EQ(0, pthread_mutex_unlock(&s_port_lock));
    EQ(0, pthread_mutex_lock(&f->mutex));
    ++f->unlock_calls;
    f->unlocked = true;
    EQ(0, pthread_cond_broadcast(&f->changed));
    while (f->pause_after_unlock && !f->resume)
    {
        EQ(0, pthread_cond_wait(&f->changed, &f->mutex));
    }
    EQ(0, pthread_mutex_unlock(&f->mutex));
}

static ble_link_operation_kind_t _event_kind(ble_port_event_type_t type)
{
    switch (type)
    {
    case BLE_PORT_EVENT_CONNECT:
        return BLE_LINK_OPERATION_CONNECT;
    case BLE_PORT_EVENT_MTU:
        return BLE_LINK_OPERATION_MTU;
    case BLE_PORT_EVENT_SUBSCRIBE:
        return BLE_LINK_OPERATION_SUBSCRIBE;
    case BLE_PORT_EVENT_DISCONNECT:
        return BLE_LINK_OPERATION_DISCONNECT;
    default:
        return BLE_LINK_OPERATION_INVALID;
    }
}

static esp_err_t _prepare(const ble_port_event_t *event,
                          ble_nimble_link_update_t *update, void *arg)
{
    handoff_fixture_t *f = arg;
    ble_gap_manager_snapshot_t gap;
    CHECK(s_holds_port);
    ++f->prepare_calls;
    if (f->prepare_result != ESP_OK)
    {
        return f->prepare_result;
    }
    EQ(ESP_OK, ble_gap_manager_get_snapshot(&gap));
    if (!gap.connected || event->identity.generation != gap.generation ||
            event->conn_handle != gap.conn_handle ||
            event->identity.conn_handle != gap.conn_handle ||
            event->identity.kind != _event_kind(event->type) ||
            event->identity.flow_id != 0U || event->identity.token != 0U ||
            (event->type == BLE_PORT_EVENT_CONNECT &&
             (!event->accepted || event->status != 0)) ||
            (event->type == BLE_PORT_EVENT_SUBSCRIBE &&
             event->attr_handle != TX_HANDLE))
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (event->type == BLE_PORT_EVENT_CONNECT)
    {
        EQ(ESP_OK, ble_link_session_handle_event(gap.generation,
                BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
        EQ(ESP_OK, ble_link_session_set_connection_pairing_window(
               gap.generation, true));
    }
    *update = (ble_nimble_link_update_t)
    {
        .type = event->type,
        .identity = event->identity,
        .mtu = event->mtu,
        .indicate = event->indicate,
        .peer_addr_type = 1U,
    };
    memcpy(update->peer_addr, s_peer, sizeof(s_peer));
    if (f->abba)
    {
        EQ(0, pthread_mutex_lock(&f->mutex));
        f->prepared = true;
        EQ(0, pthread_cond_broadcast(&f->changed));
        while (!f->output_wants_port)
        {
            EQ(0, pthread_cond_wait(&f->changed, &f->mutex));
        }
        EQ(0, pthread_mutex_unlock(&f->mutex));
    }
    return ESP_OK;
}

static ble_nimble_link_events_ops_t _ops(handoff_fixture_t *f)
{
    const ble_nimble_link_events_ops_t ops =
    {
        .lock = _port_lock, .unlock = _port_unlock,
        .prepare = _prepare, .arg = f,
    };
    return ops;
}

static esp_err_t _handle(ble_port_event_t event)
{
    handoff_fixture_t f;
    _fixture_init(&f);
    const ble_nimble_link_events_ops_t ops = _ops(&f);
    const esp_err_t result = ble_nimble_link_events_handle_update(&event, &ops);
    EQ(1U, f.lock_calls);
    EQ(1U, f.prepare_calls);
    EQ(1U, f.unlock_calls);
    CHECK(f.unlocked && !s_holds_port);
    _fixture_destroy(&f);
    return result;
}

static ble_port_event_t _event(ble_port_event_type_t type, uint32_t generation)
{
    const ble_port_event_t event =
    {
        .type = type, .conn_handle = HANDLE, .attr_handle = TX_HANDLE,
        .accepted = true, .mtu = 498U, .indicate = true, .subscribed = true,
        .identity = {
            .generation = generation, .conn_handle = HANDLE,
            .kind = _event_kind(type)
        },
    };
    return event;
}

static esp_err_t _indicate(uint16_t conn, uint16_t attr,
                           const uint8_t *value, size_t length)
{
    EQ(HANDLE, conn);
    EQ(TX_HANDLE, attr);
    CHECK(length <= sizeof(s_value));
    memcpy(s_value, value, length);
    s_length = length;
    ++s_indications;
    if (s_blocking_port != NULL)
    {
        _block_port_output(s_blocking_port);
    }
    return ESP_OK;
}

static esp_err_t _adv_start(const ble_port_adv_config_t *config)
{
    ++s_adv_starts;
    s_start_generation = config->generation;
    return ESP_OK;
}

static esp_err_t _adv_stop(uint32_t generation)
{
    ++s_adv_stops;
    s_stop_generation = generation;
    return ESP_OK;
}

static esp_err_t _notify(uint16_t conn, uint16_t attr,
                         const uint8_t *value, size_t length)
{
    (void)conn;
    (void)attr;
    (void)value;
    (void)length;
    CHECK(false);
    return ESP_FAIL;
}

static const ble_port_ops_t s_port_ops =
{
    .notify = _notify, .indicate = _indicate,
    .adv_start = _adv_start, .adv_stop = _adv_stop,
};

static void _completed(const ble_tx_scheduler_result_t *result, void *arg)
{
    (void)arg;
    const esp_err_t status = ble_link_service_tx_completed(
                                 &result->identity, result->status);
    CHECK(status == ESP_OK || status == ESP_ERR_NOT_FOUND);
}

static device_link_v1_status_t _submit(device_link_v1_operation_t operation,
                                       const device_link_v1_credentials_t *credentials, uint32_t id, void *arg)
{
    (void)credentials;
    (void)arg;
    EQ(DEVICE_LINK_V1_OPERATION_SCAN, operation);
    CHECK(id != 0U);
    ++s_submit_calls;
    return DEVICE_LINK_V1_STATUS_ACCEPTED;
}

static const ble_link_v1_owner_ops_t s_owner_ops =
{
    .submit_operation = _submit,
};

static esp_err_t _queue_work(ble_link_work_t *work, void *arg)
{
    (void)arg;
    CHECK(s_queued_work == NULL);
    s_queued_work = work;
    return ESP_OK;
}

static void _reset(void)
{
    ble_tx_scheduler_deinit();
    ble_link_gatt_reset();
    ble_gap_manager_init();
    s_submit_calls = 0U;
    s_indications = 0U;
    s_length = 0U;
    s_queued_work = NULL;
    s_check_wake_state = false;
    atomic_store(&s_wakes, 0U);
    static const ble_tx_scheduler_config_t scheduler =
    {
        .queue_depth = 8U, .max_frame_bytes = 495U,
        .ops = &s_port_ops, .completed = _completed,
    };
    EQ(ESP_OK, ble_tx_scheduler_init(&scheduler));
    const ble_link_gatt_config_t config =
    {
        .boot_id = BOOT_ID, .att_mtu = 23U, .tx_queue_depth = 8U,
    };
    EQ(ESP_OK, ble_link_gatt_init(&config));
    EQ(ESP_OK, ble_gatt_registry_assign_handle(s_rx_uuid, RX_HANDLE));
    EQ(ESP_OK, ble_gatt_registry_assign_handle(s_tx_uuid, TX_HANDLE));
    ble_link_gatt_update_handles();
    ble_link_gatt_set_work_submit(NULL, NULL);
    ble_link_service_set_v1_ops(&s_owner_ops, NULL);
    ble_link_service_set_worker_wake(_wake, NULL);
    ble_link_service_set_pairing_window(true);
    ble_link_session_set_pairing_window(true);
}

static uint32_t _admit_acl(void)
{
    const ble_gap_manager_event_t connect =
    {
        .type = BLE_GAP_MANAGER_EVENT_CONNECT, .conn_handle = HANDLE,
    };
    ble_gap_manager_snapshot_t gap;
    EQ(ESP_OK, ble_gap_manager_handle_event(&connect));
    EQ(ESP_OK, ble_gap_manager_get_snapshot(&gap));
    CHECK(gap.connected && gap.generation != 0U);
    return gap.generation;
}

static uint32_t _connect(void)
{
    const uint32_t generation = _admit_acl();
    EQ(ESP_OK, _handle(_event(BLE_PORT_EVENT_CONNECT, generation)));
    return generation;
}

static void _security(uint32_t generation)
{
    EQ(ESP_OK, ble_link_session_handle_event(generation,
            BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    EQ(ESP_OK, ble_link_session_handle_event(generation,
            BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    EQ(ESP_OK, ble_link_session_set_identity_known(generation, true));
    EQ(ESP_OK, ble_link_session_set_authorization(true, 0U));
}

static void _transport(uint32_t generation)
{
    ble_port_event_t subscribe = _event(BLE_PORT_EVENT_SUBSCRIBE, generation);
    const ble_gap_manager_event_t gap_subscribe =
    {
        .type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE,
        .identity = subscribe.identity, .conn_handle = HANDLE,
        .attr_handle = TX_HANDLE, .indicate = true, .subscribed = true,
    };
    EQ(ESP_OK, _handle(_event(BLE_PORT_EVENT_MTU, generation)));
    EQ(ESP_OK, ble_gap_manager_handle_event(&gap_subscribe));
    EQ(ESP_OK, _handle(subscribe));
}

static int _write(uint8_t opcode)
{
    const uint8_t bytes[2] = { opcode, 42U };
    const ble_gatt_registry_characteristic_t *characteristic = NULL;
    ble_gatt_registry_access_context_t context =
    {
        .op = BLE_GATT_REGISTRY_OP_WRITE_CHR,
        .write_data = bytes, .write_len = sizeof(bytes),
    };
    EQ(ESP_OK, ble_gatt_registry_lookup_by_handle(RX_HANDLE, &characteristic));
    return characteristic->access_cb(HANDLE, RX_HANDLE, &context,
                                     characteristic->arg);
}

static void _confirm(void)
{
    ble_port_event_t event =
    {
        .type = BLE_PORT_EVENT_NOTIFY_TX, .conn_handle = HANDLE,
        .attr_handle = TX_HANDLE, .indication = true,
        .tx_result = BLE_PORT_TX_CONFIRMED,
    };
    EQ(ESP_OK, ble_tx_scheduler_get_in_flight_identity(&event.identity));
    EQ(ESP_OK, ble_tx_scheduler_handle_notify_tx(&event));
}

static void _retire(uint32_t generation)
{
    ble_port_event_t event = _event(BLE_PORT_EVENT_DISCONNECT, generation);
    const ble_gap_manager_event_t gap =
    {
        .type = BLE_GAP_MANAGER_EVENT_DISCONNECT,
        .identity = event.identity, .conn_handle = HANDLE,
    };
    /* The inactive session fence precedes qualified service teardown. */
    EQ(ESP_OK, ble_link_session_handle_event(generation,
            BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    ble_tx_scheduler_reset();
    EQ(ESP_OK, ble_link_service_clear_session_state_if_current(&event.identity));
    EQ(ESP_OK, ble_gap_manager_handle_event(&gap));
}

static void test_validation_and_failure_release(void)
{
    handoff_fixture_t f;
    _reset();
    const uint32_t gen = _connect();
    _fixture_init(&f);
    ble_nimble_link_events_ops_t ops = _ops(&f);
    ble_port_event_t event = _event(BLE_PORT_EVENT_MTU, gen);
    EQ(ESP_ERR_INVALID_ARG, ble_nimble_link_events_handle_update(NULL, &ops));
    EQ(ESP_ERR_INVALID_ARG, ble_nimble_link_events_handle_update(&event, NULL));
    ble_nimble_link_events_ops_t incomplete = ops;
    incomplete.prepare = NULL;
    EQ(ESP_ERR_INVALID_ARG, ble_nimble_link_events_handle_update(&event, &incomplete));
    incomplete = ops;
    incomplete.lock = NULL;
    EQ(ESP_ERR_INVALID_ARG, ble_nimble_link_events_handle_update(&event, &incomplete));
    incomplete = ops;
    incomplete.unlock = NULL;
    EQ(ESP_ERR_INVALID_ARG, ble_nimble_link_events_handle_update(&event, &incomplete));
    event.type = BLE_PORT_EVENT_ENC_CHANGE;
    EQ(ESP_ERR_NOT_FOUND, ble_nimble_link_events_handle_update(&event, &ops));
    EQ(0U, f.lock_calls);
    event.type = BLE_PORT_EVENT_MTU;
    f.lock_result = ESP_ERR_INVALID_STATE;
    EQ(ESP_ERR_INVALID_STATE, ble_nimble_link_events_handle_update(&event, &ops));
    EQ(0U, f.prepare_calls);
    EQ(0U, f.unlock_calls);
    f.lock_result = ESP_OK;
    f.prepare_result = ESP_FAIL;
    EQ(ESP_FAIL, ble_nimble_link_events_handle_update(&event, &ops));
    EQ(1U, f.prepare_calls);
    EQ(1U, f.unlock_calls);
    uint32_t mtu;
    EQ(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
    EQ(23U, mtu);
    _fixture_destroy(&f);
    ble_port_event_t stale = _event(BLE_PORT_EVENT_MTU, gen);
    stale.identity.generation = gen + 1U;
    EQ(ESP_ERR_NOT_FOUND, _handle(stale));
    stale = _event(BLE_PORT_EVENT_SUBSCRIBE, gen);
    stale.attr_handle = RX_HANDLE;
    EQ(ESP_ERR_NOT_FOUND, _handle(stale));
    stale = _event(BLE_PORT_EVENT_CONNECT, gen);
    stale.accepted = false;
    EQ(ESP_ERR_NOT_FOUND, _handle(stale));
}

static void test_early_transport_and_queued_unsubscribe(void)
{
    _reset();
    const uint32_t gen = _connect();
    ble_link_dispatcher_facts_t facts;
    EQ(ESP_OK, ble_link_session_get_facts(gen, &facts));
    CHECK(!facts.encrypted);
    CHECK(!(ble_link_session_get_state_flags() & BLE_LINK_STATE_FLAG_BOUND));
    _transport(gen);
    uint32_t mtu;
    EQ(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
    EQ(498U, mtu);
    _security(gen);
    EQ(0, _write(DEVICE_LINK_V1_SCAN));
    EQ(1U, s_submit_calls);
    EQ(1U, s_indications);
    CHECK(s_length >= 3U);
    EQ(DEVICE_LINK_V1_SCAN | DEVICE_LINK_V1_RESPONSE_MASK, s_value[0]);
    EQ(DEVICE_LINK_V1_STATUS_ACCEPTED, s_value[2]);
    _confirm();

    _reset();
    const uint32_t queued_gen = _connect();
    _transport(queued_gen);
    _security(queued_gen);
    ble_link_gatt_set_work_submit(_queue_work, NULL);
    EQ(0, _write(DEVICE_LINK_V1_SCAN));
    CHECK(s_queued_work != NULL);
    EQ(0U, s_submit_calls);
    ble_port_event_t unsubscribe = _event(BLE_PORT_EVENT_SUBSCRIBE, queued_gen);
    unsubscribe.indicate = false;
    unsubscribe.subscribed = false;
    EQ(ESP_OK, _handle(unsubscribe));
    /* Leave GAP's CCCD true: only the real helper-to-service handoff can
     * reject work that already passed the GATT admission boundary. */
    EQ(ESP_ERR_INVALID_STATE, ble_link_service_execute(s_queued_work));
    ble_link_service_release_work(s_queued_work);
    s_queued_work = NULL;
    EQ(0U, s_submit_calls);
    EQ(0U, s_indications);
    CHECK(!ble_link_service_write_blocked());
    puts("link events: early MTU/CCCD -> SCAN ACCEPTED; queued unsubscribe submits=0");
}

static void _block_port_output(handoff_fixture_t *f)
{
    /* Actual GATT output reached scheduler -> port while service holds L. */
    EQ(0, pthread_mutex_lock(&f->mutex));
    f->output_entered = true;
    EQ(0, pthread_cond_broadcast(&f->changed));
    while (!f->prepared)
    {
        EQ(0, pthread_cond_wait(&f->changed, &f->mutex));
    }
    f->output_wants_port = true;
    EQ(0, pthread_cond_broadcast(&f->changed));
    EQ(0, pthread_mutex_unlock(&f->mutex));
    EQ(0, pthread_mutex_lock(&s_port_lock));
    EQ(0, pthread_mutex_lock(&f->mutex));
    while (!f->unlocked)
    {
        EQ(0, pthread_cond_wait(&f->changed, &f->mutex));
    }
    f->output_got_port = true;
    EQ(0, pthread_cond_broadcast(&f->changed));
    EQ(0, pthread_mutex_unlock(&f->mutex));
    EQ(0, pthread_mutex_unlock(&s_port_lock));
}

static void *_output_thread(void *arg)
{
    (void)arg;
    EQ(0, _write(DEVICE_LINK_V1_GET_INFO));
    return NULL;
}

static void *_update_thread(void *arg)
{
    handoff_fixture_t *f = arg;
    const ble_nimble_link_events_ops_t ops = _ops(f);
    f->result = ble_nimble_link_events_handle_update(&f->event, &ops);
    return NULL;
}


static void test_abba_boundary(ble_port_event_type_t type)
{
    handoff_fixture_t f;
    pthread_t a, b;
    _reset();
    const uint32_t gen = _connect();
    _fixture_init(&f);
    f.abba = true;
    f.event = _event(type, gen);
    _transport(gen);
    _security(gen);
    s_blocking_port = &f;
    EQ(0, pthread_create(&a, NULL, _output_thread, NULL));
    EQ(0, pthread_mutex_lock(&f.mutex));
    while (!f.output_entered)
    {
        EQ(0, pthread_cond_wait(&f.changed, &f.mutex));
    }
    EQ(0, pthread_mutex_unlock(&f.mutex));
    EQ(0, pthread_create(&b, NULL, _update_thread, &f));
    EQ(0, pthread_join(a, NULL));
    EQ(0, pthread_join(b, NULL));
    s_blocking_port = NULL;
    EQ(ESP_OK, f.result);
    CHECK(f.output_wants_port && f.output_got_port && f.unlocked);
    EQ(1U, f.unlock_calls);
    /* Duplicate CONNECT must not retire the indication selected under L. */
    CHECK(ble_link_service_response_in_flight());
    _confirm();
    _fixture_destroy(&f);
}

static void _wait_unlocked(handoff_fixture_t *f)
{
    EQ(0, pthread_mutex_lock(&f->mutex));
    while (!f->unlocked)
    {
        EQ(0, pthread_cond_wait(&f->changed, &f->mutex));
    }
    EQ(0, pthread_mutex_unlock(&f->mutex));
}

static void _resume(handoff_fixture_t *f)
{
    EQ(0, pthread_mutex_lock(&f->mutex));
    f->resume = true;
    EQ(0, pthread_cond_broadcast(&f->changed));
    EQ(0, pthread_mutex_unlock(&f->mutex));
}

static void _assert_current_identity(const ble_link_operation_identity_t *expected)
{
    ble_link_operation_identity_t current;
    EQ(ESP_OK, ble_tx_scheduler_get_in_flight_identity(&current));
    CHECK(ble_link_operation_identity_equal(expected, &current));
    CHECK(ble_link_service_response_in_flight());
}

static void _assert_large_status(void)
{
    device_link_v1_snapshot_t snapshot =
    {
        .state = DEVICE_LINK_V1_WIFI_IDLE, .profile_ssid_length = 32U,
    };
    memset(snapshot.profile_ssid, 'x', sizeof(snapshot.profile_ssid));
    ble_link_service_observe_snapshot(&snapshot);
    const unsigned indications = s_indications;
    EQ(ESP_OK, ble_link_service_pump_tx());
    EQ(indications + 1U, s_indications);
    CHECK(s_length > 20U);
    EQ(DEVICE_LINK_V1_EVENT_MARKER, s_value[0]);
    EQ(DEVICE_LINK_V1_WIFI_STATUS, s_value[1]);
    _confirm();
}

static void test_reconnect_defaults(void)
{
    _reset();
    const uint32_t g1 = _connect();
    _transport(g1);
    _security(g1);
    _retire(g1);
    const uint32_t g2 = _connect();
    CHECK(g2 > g1);
    uint32_t mtu;
    EQ(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
    EQ(23U, mtu);
    /* Install only the GAP CCCD: helper admission remains closed. */
    const ble_port_event_t subscribe = _event(BLE_PORT_EVENT_SUBSCRIBE, g2);
    const ble_gap_manager_event_t gap =
    {
        .type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE,
        .identity = subscribe.identity, .conn_handle = HANDLE,
        .attr_handle = TX_HANDLE, .indicate = true, .subscribed = true,
    };
    EQ(ESP_OK, ble_gap_manager_handle_event(&gap));
    _security(g2);
    /* GATT can admit GET_INFO at MTU23; execution must still reject CCCD. */
    CHECK(_write(DEVICE_LINK_V1_GET_INFO) != 0);
    EQ(0U, s_indications);
    EQ(ESP_OK, _handle(subscribe));
    device_link_v1_snapshot_t snapshot =
    {
        .state = DEVICE_LINK_V1_WIFI_IDLE, .profile_ssid_length = 32U,
    };
    memset(snapshot.profile_ssid, 'y', sizeof(snapshot.profile_ssid));
    ble_link_service_observe_snapshot(&snapshot);
    EQ(ESP_ERR_INVALID_SIZE, ble_link_service_pump_tx());
    EQ(0U, s_indications);
    CHECK(!ble_link_service_write_blocked());
    EQ(ESP_OK, _handle(_event(BLE_PORT_EVENT_MTU, g2)));
    _assert_large_status();
}


static void test_delayed_old_action(ble_port_event_type_t type, bool reconnect)
{
    handoff_fixture_t delayed;
    pthread_t thread;
    _reset();
    const uint32_t g1 = _connect();
    _transport(g1);
    _security(g1);
    _fixture_init(&delayed);
    delayed.pause_after_unlock = true;
    delayed.event = _event(type, g1);
    delayed.event.mtu = 23U;
    delayed.event.indicate = false;
    delayed.event.subscribed = false;
    EQ(0, pthread_create(&thread, NULL, _update_thread, &delayed));
    _wait_unlocked(&delayed);
    _retire(g1);
    ble_link_operation_identity_t current = { 0 };
    uint32_t g2 = 0U;
    if (reconnect)
    {
        g2 = _connect();
        CHECK(g2 > g1);
        uint32_t mtu;
        EQ(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
        EQ(23U, mtu);
        _transport(g2);
        _security(g2);
        EQ(0, _write(DEVICE_LINK_V1_GET_INFO));
        EQ(ESP_OK, ble_tx_scheduler_get_in_flight_identity(&current));
    }
    _resume(&delayed);
    EQ(0, pthread_join(thread, NULL));
    EQ(type == BLE_PORT_EVENT_SUBSCRIBE ? ESP_OK : ESP_ERR_NOT_FOUND,
       delayed.result);
    if (reconnect)
    {
        uint32_t mtu;
        EQ(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
        EQ(498U, mtu);
        _assert_current_identity(&current);
        _confirm();
        _assert_large_status();
        EQ(0, _write(DEVICE_LINK_V1_SCAN));
        EQ(1U, s_submit_calls);
        _confirm();
        EQ(ESP_ERR_NOT_FOUND, ble_link_service_on_connect(g1, HANDLE));
        EQ(ESP_ERR_NOT_FOUND, ble_link_gatt_set_att_mtu(g1, HANDLE, 23U));
        EQ(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
        EQ(498U, mtu);
    }
    else
    {
        ble_link_dispatcher_facts_t facts;
        EQ(ESP_ERR_INVALID_STATE, ble_link_session_get_facts(g1, &facts));
        EQ(ESP_ERR_NOT_FOUND, ble_link_service_on_connect(g1, HANDLE));
        CHECK(!ble_link_service_response_in_flight());
        CHECK(_write(DEVICE_LINK_V1_SCAN) != 0);
        EQ(0U, s_submit_calls);
    }
    _fixture_destroy(&delayed);
}

static void test_duplicate_connect_and_identity_refresh(void)
{
    _reset();
    const uint32_t gen = _connect();
    _transport(gen);
    _security(gen);
    EQ(0, _write(DEVICE_LINK_V1_GET_INFO));
    ble_link_operation_identity_t before;
    EQ(ESP_OK, ble_tx_scheduler_get_in_flight_identity(&before));
    EQ(ESP_OK, _handle(_event(BLE_PORT_EVENT_CONNECT, gen)));
    EQ(ESP_OK, ble_link_gatt_update_identity(gen, HANDLE, 1U, s_peer));
    uint32_t mtu;
    EQ(ESP_OK, ble_link_gatt_get_att_mtu(&mtu));
    EQ(498U, mtu);
    _assert_current_identity(&before);
    _confirm();
    _assert_large_status();
    EQ(0, _write(DEVICE_LINK_V1_SCAN));
    EQ(1U, s_submit_calls);
    _confirm();
}

static uint32_t _now(void)
{
    return 100000U;
}

static void _adv_init(ble_nimble_adv_event_state_t *state)
{
    static const uint8_t name[] = "MT";
    static const ble_adv_manager_config_t config =
    {
        .fast_interval_ms = 100U, .slow_interval_ms = 700U,
        .fast_window_ms = 30000U, .short_name = name,
        .short_name_len = sizeof(name) - 1U, .service_uuid = s_tx_uuid,
        .now_ms = _now, .ops = &s_port_ops,
    };
    memset(state, 0, sizeof(*state));
    ble_adv_manager_init(&config);
    ble_event_router_init();
    EQ(ESP_OK, ble_event_router_register(
           ble_nimble_link_events_adv_consumer, state));
    s_adv_starts = 0U;
    s_adv_stops = 0U;
    s_start_generation = 0U;
    s_stop_generation = 0U;
}

static void _dispatch_expect(ble_port_event_t event, unsigned wake_delta,
                             ble_adv_manager_state_t state)
{
    const unsigned wakes = atomic_load(&s_wakes);
    s_check_wake_state = true;
    s_expected_wake_state = state;
    EQ(ESP_OK, ble_event_router_dispatch(&event));
    s_check_wake_state = false;
    EQ(wakes + wake_delta, atomic_load(&s_wakes));
    EQ(state, ble_adv_manager_get_state());
}

static void test_advertising_event_whitelist(void)
{
    ble_nimble_adv_event_state_t state;
    _reset();
    _adv_init(&state);
    const ble_port_event_type_t ignored[] =
    {
        BLE_PORT_EVENT_MTU, BLE_PORT_EVENT_ENC_CHANGE, BLE_PORT_EVENT_SUBSCRIBE,
    };
    for (size_t i = 0U; i < sizeof(ignored) / sizeof(ignored[0]); ++i)
    {
        _dispatch_expect(_event(ignored[i], 1U), 0U, BLE_ADV_MANAGER_STATE_STOPPED);
    }
    const ble_port_tx_result_t outcomes[] =
    {
        BLE_PORT_TX_SENT, BLE_PORT_TX_CONFIRMED, BLE_PORT_TX_ERROR, BLE_PORT_TX_TIMEOUT,
    };
    for (size_t i = 0U; i < sizeof(outcomes) / sizeof(outcomes[0]); ++i)
    {
        ble_port_event_t tx = _event(BLE_PORT_EVENT_NOTIFY_TX, 1U);
        tx.tx_result = outcomes[i];
        tx.indication = true;
        _dispatch_expect(tx, 0U, BLE_ADV_MANAGER_STATE_STOPPED);
    }
    const ble_port_event_type_t allowed[] =
    {
        BLE_PORT_EVENT_CONNECT, BLE_PORT_EVENT_DISCONNECT,
        BLE_PORT_EVENT_SYNC, BLE_PORT_EVENT_RESET,
        BLE_PORT_EVENT_ADV_STARTED, BLE_PORT_EVENT_ADV_STOPPED,
        BLE_PORT_EVENT_ADV_COMPLETE,
    };
    for (size_t i = 0U; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
    {
        _dispatch_expect(_event(allowed[i], 1U), 1U, BLE_ADV_MANAGER_STATE_STOPPED);
    }
    ble_adv_manager_deinit();
}

static void test_advertising_identity_and_live_lease(void)
{
    ble_nimble_adv_event_state_t state;
    ble_adv_lease_t lease;
    _reset();
    _adv_init(&state);
    _dispatch_expect((ble_port_event_t)
    {
        .type = BLE_PORT_EVENT_RESET,
    }, 1U, BLE_ADV_MANAGER_STATE_STOPPED);
    EQ(ESP_OK, ble_adv_manager_acquire_lease(&lease, BLE_ADV_MANAGER_MODE_SLOW, false));
    EQ(0U, s_adv_starts);
    _dispatch_expect((ble_port_event_t)
    {
        .type = BLE_PORT_EVENT_SYNC
    },
    1U, BLE_ADV_MANAGER_STATE_STARTING);
    EQ(1U, s_adv_starts);
    _dispatch_expect((ble_port_event_t)
    {
        .type = BLE_PORT_EVENT_ADV_STARTED, .generation = s_start_generation,
    }, 1U, BLE_ADV_MANAGER_STATE_SLOW);

    ble_port_event_t connect = _event(BLE_PORT_EVENT_CONNECT, 22U);
    _dispatch_expect(connect, 1U, BLE_ADV_MANAGER_STATE_STOPPING);
    EQ(1U, s_adv_stops);
    _dispatch_expect((ble_port_event_t)
    {
        .type = BLE_PORT_EVENT_ADV_STOPPED, .generation = s_stop_generation,
    }, 1U, BLE_ADV_MANAGER_STATE_STOPPED);
    EQ(22U, state.generation);
    EQ(HANDLE, state.conn_handle);

    ble_port_event_t rejected = _event(BLE_PORT_EVENT_CONNECT, 23U);
    rejected.accepted = false;
    _dispatch_expect(rejected, 1U, BLE_ADV_MANAGER_STATE_STOPPED);
    EQ(22U, state.generation);
    const unsigned starts = s_adv_starts;
    ble_port_event_t stale = _event(BLE_PORT_EVENT_DISCONNECT, 21U);
    _dispatch_expect(stale, 0U, BLE_ADV_MANAGER_STATE_STOPPED);
    stale = _event(BLE_PORT_EVENT_DISCONNECT, 22U);
    stale.identity.token = 1U;
    _dispatch_expect(stale, 0U, BLE_ADV_MANAGER_STATE_STOPPED);
    stale.identity.token = 0U;
    stale.identity.flow_id = 1U;
    _dispatch_expect(stale, 0U, BLE_ADV_MANAGER_STATE_STOPPED);
    stale.identity.flow_id = 0U;
    stale.identity.kind = BLE_LINK_OPERATION_MTU;
    _dispatch_expect(stale, 0U, BLE_ADV_MANAGER_STATE_STOPPED);
    stale = _event(BLE_PORT_EVENT_DISCONNECT, 22U);
    stale.conn_handle = HANDLE + 1U;
    _dispatch_expect(stale, 0U, BLE_ADV_MANAGER_STATE_STOPPED);
    EQ(starts, s_adv_starts);
    EQ(22U, state.generation);

    _dispatch_expect(_event(BLE_PORT_EVENT_DISCONNECT, 22U),
                     1U, BLE_ADV_MANAGER_STATE_STARTING);
    EQ(starts + 1U, s_adv_starts);
    EQ(0U, state.generation);
    _dispatch_expect((ble_port_event_t)
    {
        .type = BLE_PORT_EVENT_ADV_STARTED, .generation = s_start_generation,
    }, 1U, BLE_ADV_MANAGER_STATE_SLOW);
    const unsigned before_reset = s_adv_starts;
    _dispatch_expect((ble_port_event_t)
    {
        .type = BLE_PORT_EVENT_ADV_COMPLETE, .host_reset_pending = true,
    }, 1U, BLE_ADV_MANAGER_STATE_SLOW);
    EQ(before_reset, s_adv_starts);
    _dispatch_expect((ble_port_event_t)
    {
        .type = BLE_PORT_EVENT_RESET
    },
    1U, BLE_ADV_MANAGER_STATE_STOPPED);
    EQ(before_reset, s_adv_starts);
    _dispatch_expect((ble_port_event_t)
    {
        .type = BLE_PORT_EVENT_SYNC
    },
    1U, BLE_ADV_MANAGER_STATE_STARTING);
    EQ(before_reset + 1U, s_adv_starts);
    ble_adv_manager_deinit();
    puts("advertising: non-adv wakes=0; stale disconnect ignored; live slow lease restarts");
}

int main(void)
{
    ble_gatt_registry_init();
    test_validation_and_failure_release();
    test_early_transport_and_queued_unsubscribe();
    test_abba_boundary(BLE_PORT_EVENT_MTU);
    test_abba_boundary(BLE_PORT_EVENT_SUBSCRIBE);
    test_abba_boundary(BLE_PORT_EVENT_CONNECT);
    puts("link events: CONNECT/MTU/SUBSCRIBE unlock-before-L; both ABBA threads complete");
    test_delayed_old_action(BLE_PORT_EVENT_CONNECT, false);
    test_delayed_old_action(BLE_PORT_EVENT_MTU, false);
    test_delayed_old_action(BLE_PORT_EVENT_SUBSCRIBE, false);
    test_delayed_old_action(BLE_PORT_EVENT_CONNECT, true);
    test_delayed_old_action(BLE_PORT_EVENT_MTU, true);
    test_delayed_old_action(BLE_PORT_EVENT_SUBSCRIBE, true);
    test_duplicate_connect_and_identity_refresh();
    test_reconnect_defaults();
    puts("link events: inactive G1 fenced; reused-handle G2 MTU/CCCD/TX preserved");
    test_advertising_event_whitelist();
    test_advertising_identity_and_live_lease();
    ble_tx_scheduler_deinit();
    ble_link_gatt_reset();
    return 0;
}
