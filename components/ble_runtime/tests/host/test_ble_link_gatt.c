#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_gap_manager.h"
#include "ble_gatt_registry.h"
#include "ble_link_events.h"
#include "ble_link_gatt.h"
#include "ble_link_session.h"
#include "ble_tx_scheduler.h"
#include "host_freertos.h"

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
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%lld != %lld)\n", \
                    __LINE__, #expected, #actual, expected_value, actual_value); \
            abort(); \
        } \
    } while (0)

#define BOOT_ID 72623859790382856ULL
#define GEN 1U

static uint8_t s_published[BLE_LINK_STATE_MAX_ENCODED_BYTES];
static size_t s_published_len;
static unsigned int s_publish_calls;
static pthread_barrier_t *s_publish_enter_barrier;
static pthread_barrier_t *s_publish_exit_barrier;

static ble_link_gatt_config_t s_config;

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

static const ble_port_ops_t s_fake_port_ops =
{
    .adv_start = NULL,
    .adv_stop = NULL,
    .notify = _fake_notify,
    .indicate = _fake_indicate,
};

static const ble_tx_scheduler_config_t s_scheduler_config =
{
    .queue_depth = 32U,
    .max_frame_bytes = 512U,
    .ops = &s_fake_port_ops,
};

static esp_err_t _publish(const uint8_t *value, size_t len, void *arg)
{
    (void)arg;
    memcpy(s_published, value, len);
    s_published_len = len;
    s_publish_calls++;
    if (s_publish_enter_barrier != NULL)
    {
        (void)pthread_barrier_wait(s_publish_enter_barrier);
        (void)pthread_barrier_wait(s_publish_exit_barrier);
    }
    return ESP_OK;
}


static void _subscribe_tx_kind(uint16_t attr_handle, bool notify,
                               bool indicate)
{
    static bool connected = false;
    ble_gap_manager_event_t event;
    ble_gap_manager_snapshot_t snapshot;

    if (!connected)
    {
        ble_gap_manager_init();
        memset(&event, 0, sizeof(event));
        event.type = BLE_GAP_MANAGER_EVENT_CONNECT;
        event.conn_handle = 7U;
        event.status = 0;
        TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&event));
        connected = true;
    }
    memset(&event, 0, sizeof(event));
    event.type = BLE_GAP_MANAGER_EVENT_SUBSCRIBE;
    event.conn_handle = 7U;
    event.attr_handle = attr_handle;
    event.notify = notify;
    event.indicate = indicate;
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    event.identity.generation = snapshot.generation;
    event.identity.kind = BLE_LINK_OPERATION_SUBSCRIBE;
    event.identity.conn_handle = event.conn_handle;
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&event));
}

static void _establish_session(void)
{
    ble_link_session_init(BOOT_ID);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN, BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    uint32_t epoch = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          GEN, &epoch));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(
                          GEN, true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, 1U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN, 1U, epoch));
}

static void _reset(void)
{
    memset(&s_published, 0, sizeof(s_published));
    s_published_len = 0U;
    s_publish_calls = 0U;
    s_publish_enter_barrier = NULL;
    s_publish_exit_barrier = NULL;
    ble_link_gatt_reset();
    ble_link_events_reset();
    ble_tx_scheduler_deinit();
    memset(&s_config, 0, sizeof(s_config));
    s_config.boot_id = BOOT_ID;
    s_config.connection_generation = GEN;
    s_config.conn_handle = 7U;
    s_config.att_mtu = 23U;
    s_config.tx_queue_depth = 32U;
    s_config.publish_link_state = _publish;
    TEST_ASSERT_EQUAL(ESP_OK, ble_tx_scheduler_init(&s_scheduler_config));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_init(&s_config));
}

static void *_refresh_link_state_thread(void *arg)
{
    esp_err_t *result = arg;

    *result = ble_link_gatt_refresh_link_state();
    return NULL;
}

static void test_refresh_retains_concurrent_epoch_dirty(void)
{
    pthread_barrier_t entered;
    pthread_barrier_t release;
    pthread_t refresh_thread;
    esp_err_t refresh_result = ESP_FAIL;

    _reset();
    _establish_session();
    TEST_ASSERT_EQUAL(0, pthread_barrier_init(&entered, NULL, 2U));
    TEST_ASSERT_EQUAL(0, pthread_barrier_init(&release, NULL, 2U));
    s_publish_enter_barrier = &entered;
    s_publish_exit_barrier = &release;
    TEST_ASSERT_EQUAL(0, pthread_create(
                          &refresh_thread, NULL,
                          _refresh_link_state_thread, &refresh_result));
    (void)pthread_barrier_wait(&entered);

    /* Host callbacks may advance both epochs or mark a failed notification
     * while the owner is inside the transport submit. None of those facts may
     * be overwritten when the older submit returns. */
    ble_link_gatt_authentication_epoch_advance();
    ble_link_gatt_cccd_epoch_advance();
    ble_link_gatt_mark_link_state_dirty();
    (void)pthread_barrier_wait(&release);
    TEST_ASSERT_EQUAL(0, pthread_join(refresh_thread, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, refresh_result);
    TEST_ASSERT_EQUAL(1U, s_publish_calls);

    s_publish_enter_barrier = NULL;
    s_publish_exit_barrier = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED,
                      ble_link_gatt_refresh_link_state());
    host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_refresh_link_state());
    TEST_ASSERT_EQUAL(2U, s_publish_calls);
    TEST_ASSERT_EQUAL(0, pthread_barrier_destroy(&entered));
    TEST_ASSERT_EQUAL(0, pthread_barrier_destroy(&release));
}

static void test_async_failure_retry_is_cooled_down(void)
{
    _reset();
    _establish_session();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_refresh_link_state());
    const unsigned int initial_calls = s_publish_calls;

    for (unsigned int attempt = 0U; attempt < 3U; ++attempt)
    {
        /* Model an asynchronous terminal notification failure. Its wake may
         * run the owner immediately, but the retained submit is ineligible
         * until the absolute 100 ms retry boundary. */
        ble_link_gatt_mark_link_state_dirty();
        TEST_ASSERT_TRUE(ble_link_gatt_link_state_retry_pending());
        TEST_ASSERT_TRUE(
            ble_link_gatt_link_state_retry_remaining_ms() > 0U);
        TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED,
                          ble_link_gatt_refresh_link_state());
        TEST_ASSERT_EQUAL(initial_calls + attempt, s_publish_calls);
        host_freertos_advance_ticks(pdMS_TO_TICKS(100U));
        TEST_ASSERT_EQUAL(0U,
                          ble_link_gatt_link_state_retry_remaining_ms());
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_refresh_link_state());
        TEST_ASSERT_EQUAL(initial_calls + attempt + 1U, s_publish_calls);
    }
}

static void test_att_mtu_clamped(void)
{
    uint32_t facts_mtu = 0U;

    _reset();
    /* A peer requesting more than the 498 cap is answered with the cap. */
    ble_link_gatt_set_att_mtu(517U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&facts_mtu));
    TEST_ASSERT_EQUAL(498U, facts_mtu);
    /* Values inside [23, 498] pass through. */
    ble_link_gatt_set_att_mtu(185U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&facts_mtu));
    TEST_ASSERT_EQUAL(185U, facts_mtu);
    /* Values below the mandatory floor are clamped up to 23. */
    ble_link_gatt_set_att_mtu(10U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&facts_mtu));
    TEST_ASSERT_EQUAL(23U, facts_mtu);
    const uint8_t peer_addr[6] = {1U, 2U, 3U, 4U, 5U, 6U};

    ble_link_gatt_set_att_mtu(185U);
    ble_link_gatt_set_connection(GEN + 1U, 8U, 1U, peer_addr);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&facts_mtu));
    TEST_ASSERT_EQUAL(23U, facts_mtu);
    ble_link_gatt_set_att_mtu(185U);
    ble_link_gatt_reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&facts_mtu));
    TEST_ASSERT_EQUAL(23U, facts_mtu);
}

static void test_identity_update_preserves_current_acl_mtu(void)
{
    static const uint8_t identity[6] = {1U, 2U, 3U, 4U, 5U, 6U};
    uint32_t facts_mtu = 0U;

    _reset();
    ble_link_gatt_set_att_mtu(498U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_update_identity(
                          GEN, 7U, 0U, identity));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&facts_mtu));
    TEST_ASSERT_EQUAL(498U, facts_mtu);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_link_gatt_update_identity(
                          GEN + 1U, 7U, 0U, identity));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_link_gatt_update_identity(
                          GEN, 8U, 0U, identity));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_gatt_update_identity(
                          GEN, 7U, 0U, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_get_att_mtu(&facts_mtu));
    TEST_ASSERT_EQUAL(498U, facts_mtu);
}

static void test_registered_characteristics(void)
{
    /* link_state first. */
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_uuid(
                          (const uint8_t[16])
    {
        0x35, 0xcc, 0x88, 0x36, 0x00, 0xa2, 0x14, 0x91,
              0x98, 0x4f, 0x02, 0xb2, 0xf0, 0x51, 0x1f, 0xf9,
    }, &characteristic));
    TEST_ASSERT_TRUE(characteristic != NULL);
    TEST_ASSERT_TRUE((characteristic->properties &
                      BLE_GATT_REGISTRY_PROP_READ) != 0U);
    TEST_ASSERT_TRUE((characteristic->properties &
                      BLE_GATT_REGISTRY_PROP_NOTIFY) != 0U);
    TEST_ASSERT_EQUAL(BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM,
                      characteristic->read_admission);
    TEST_ASSERT_TRUE(characteristic->access_cb != NULL);
}

static void test_registered_profile_uuids(void)
{
    static const uint8_t expected_services[1][16] =
    {
        {
            0x8b, 0x03, 0xdc, 0x36, 0xd0, 0x63, 0x05, 0x8d,
            0x30, 0x42, 0x10, 0xc5, 0x8c, 0xe4, 0x77, 0x2c,
        },
    };
    static const size_t expected_characteristic_counts[1] = {5U};
    static const uint8_t expected_characteristics[5][16] =
    {
        {
            0x35, 0xcc, 0x88, 0x36, 0x00, 0xa2, 0x14, 0x91,
            0x98, 0x4f, 0x02, 0xb2, 0xf0, 0x51, 0x1f, 0xf9,
        },
        {
            0xf4, 0xeb, 0x8f, 0x50, 0x48, 0xee, 0x19, 0x83,
            0xfe, 0x48, 0xf5, 0x60, 0xdb, 0xae, 0xbf, 0x1b,
        },
        {
            0xec, 0x3d, 0x69, 0x58, 0xa5, 0xc1, 0xa2, 0x83,
            0x5a, 0x4f, 0x1b, 0x57, 0x38, 0x5d, 0xc6, 0x2c,
        },
        {
            0x48, 0x67, 0xb4, 0xa3, 0x26, 0x5e, 0xdb, 0x85,
            0x13, 0x41, 0x1d, 0xcd, 0x56, 0xcc, 0xdf, 0x29,
        },
        {
            0x79, 0x4b, 0xc8, 0x31, 0x64, 0xb8, 0x90, 0xbe,
            0xa8, 0x47, 0x4f, 0x4f, 0x6a, 0xc0, 0x6e, 0x7c,
        },
    };

    _reset();
    for (size_t i = 0U; i < 1U; ++i)
    {
        const ble_gatt_registry_service_t *service = NULL;

        TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_get_service(i, &service));
        TEST_ASSERT_TRUE(service != NULL);
        TEST_ASSERT_TRUE(memcmp(service->uuid, expected_services[i], 16U) == 0);
        TEST_ASSERT_EQUAL(expected_characteristic_counts[i],
                          service->characteristic_count);
    }
    for (size_t i = 0U; i < 5U; ++i)
    {
        const ble_gatt_registry_characteristic_t *characteristic = NULL;

        TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_uuid(
                              expected_characteristics[i], &characteristic));
        TEST_ASSERT_TRUE(characteristic != NULL);
        TEST_ASSERT_TRUE(memcmp(characteristic->uuid,
                                expected_characteristics[i], 16U) == 0);
    }
}

static void test_write_feeds_service(void)
{
    /* A capabilities request written to control_rx produces an outbound
     * fragment through the sink (TX scheduler submit). */
    static const uint8_t request[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00,
    };
    uint8_t framed[512];
    ble_gatt_registry_access_context_t context;

    _reset();
    _establish_session();
    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x01U;
    framed[3] = 0x00U;
    framed[4] = (uint8_t)((sizeof(request) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(request) + 1U) >> 8U) & 0xffU);
    framed[6] = 0x00U;
    framed[7] = 0x00U;
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], request, sizeof(request));
    memset(&context, 0, sizeof(context));
    context.op = BLE_GATT_REGISTRY_OP_WRITE_CHR;
    context.write_data = framed;
    context.write_len = (uint16_t)(9U + sizeof(request));
    /* Assign the control_rx handle. */
    const uint16_t control_rx_handle = 0x20U;
    const uint16_t control_tx_handle = 0x21U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0x48, 0x67, 0xb4, 0xa3, 0x26, 0x5e, 0xdb, 0x85,
              0x13, 0x41, 0x1d, 0xcd, 0x56, 0xcc, 0xdf, 0x29,
    }, control_rx_handle));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0x79, 0x4b, 0xc8, 0x31, 0x64, 0xb8, 0x90, 0xbe,
              0xa8, 0x47, 0x4f, 0x4f, 0x6a, 0xc0, 0x6e, 0x7c,
    }, control_tx_handle));
    ble_link_gatt_update_handles();
    /* The control response is an indication: its CCCD must be enabled. */
    _subscribe_tx_kind(control_tx_handle, false, true);
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_handle(
                          control_rx_handle, &characteristic));
    const int cb_result = characteristic->access_cb(
                              7U, control_rx_handle, &context, NULL);

    TEST_ASSERT_EQUAL(0, cb_result);
}

static void test_link_state_read(void)
{
    uint8_t value[BLE_LINK_STATE_MAX_ENCODED_BYTES];
    uint16_t len = 0U;
    ble_gatt_registry_access_context_t context;

    _reset();
    _establish_session();
    memset(&context, 0, sizeof(context));
    context.op = BLE_GATT_REGISTRY_OP_READ_CHR;
    context.read_out = value;
    context.read_capacity = sizeof(value);
    context.read_len = &len;
    const uint16_t link_state_handle = 0x10U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0x35, 0xcc, 0x88, 0x36, 0x00, 0xa2, 0x14, 0x91,
              0x98, 0x4f, 0x02, 0xb2, 0xf0, 0x51, 0x1f, 0xf9,
    }, link_state_handle));
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_handle(
                          link_state_handle, &characteristic));
    TEST_ASSERT_EQUAL(0, characteristic->access_cb(
                          7U, link_state_handle, &context, NULL));
    TEST_ASSERT_TRUE(len > 0U);
    /* protocol_major=2, protocol_minor=0. */
    TEST_ASSERT_EQUAL(0x02U, value[0]);
    TEST_ASSERT_EQUAL(0x00U, value[1]);
    TEST_ASSERT_EQUAL(0x02U, value[2]);
    TEST_ASSERT_EQUAL(0x00U, value[3]);
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_MAX_ENCODED_BYTES, len);
}

static void test_event_sequence_boot_lifecycle(void)
{
    _reset();
    TEST_ASSERT_EQUAL(1U, ble_link_events_baseline());
    TEST_ASSERT_EQUAL(2U, ble_link_events_next());
    ble_link_gatt_reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_restart());
    TEST_ASSERT_EQUAL(2U, ble_link_events_baseline());

    ble_link_gatt_config_t next_boot = s_config;

    next_boot.boot_id = BOOT_ID + 1U;
    ble_link_gatt_reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_init(&next_boot));
    TEST_ASSERT_EQUAL(1U, ble_link_events_baseline());
    TEST_ASSERT_EQUAL(2U, ble_link_events_next());
}

static void test_refresh_publishes_once(void)
{
    _reset();
    _establish_session();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_refresh_link_state());
    TEST_ASSERT_EQUAL(1U, s_publish_calls);
    TEST_ASSERT_TRUE(s_published_len > 0U);
    /* Unchanged state: no second publish. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_refresh_link_state());
    TEST_ASSERT_EQUAL(1U, s_publish_calls);
    /* Changed flags republish. */
    ble_link_session_set_pairing_window(true);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_refresh_link_state());
    TEST_ASSERT_EQUAL(2U, s_publish_calls);
}

static void test_update_handles(void)
{
    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0xec, 0x3d, 0x69, 0x58, 0xa5, 0xc1, 0xa2, 0x83,
              0x5a, 0x4f, 0x1b, 0x57, 0x38, 0x5d, 0xc6, 0x2c,
    }, 0x30U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0x79, 0x4b, 0xc8, 0x31, 0x64, 0xb8, 0x90, 0xbe,
              0xa8, 0x47, 0x4f, 0x4f, 0x6a, 0xc0, 0x6e, 0x7c,
    }, 0x40U));
    ble_link_gatt_update_handles();
    TEST_ASSERT_EQUAL(0x30U, ble_link_gatt_session_tx_handle());
    TEST_ASSERT_EQUAL(0x40U, ble_link_gatt_control_tx_handle());
}

static void test_legacy_transfer_service_absent(void)
{
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    _reset();
    /* The v1 transfer service is not part of the v2 active profile. */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_gatt_registry_lookup_by_uuid(
                          (const uint8_t[16])
    {
        0xa1, 0xf0, 0x28, 0x86, 0x72, 0x76, 0xac, 0x97,
              0x1e, 0x47, 0xff, 0xe9, 0x4d, 0x8d, 0x59, 0xf3,
    }, &characteristic));
    TEST_ASSERT_TRUE(characteristic == NULL);
}

static void test_link_state_read_reports_att_resource(void)
{
    uint8_t value[1] = {0};
    uint16_t len = 0U;
    ble_gatt_registry_access_context_t context;
    const uint16_t link_state_handle = 0x70U;

    _reset();
    _establish_session();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0x35, 0xcc, 0x88, 0x36, 0x00, 0xa2, 0x14, 0x91,
              0x98, 0x4f, 0x02, 0xb2, 0xf0, 0x51, 0x1f, 0xf9,
    }, link_state_handle));
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_handle(
                          link_state_handle, &characteristic));
    memset(&context, 0, sizeof(context));
    context.op = BLE_GATT_REGISTRY_OP_READ_CHR;
    context.read_out = value;
    context.read_capacity = 0U;
    context.read_len = &len;
    TEST_ASSERT_EQUAL(0x11, characteristic->access_cb(
                          7U, link_state_handle, &context, NULL));
}

static void test_authorize_prepare_on_session_channel(void)
{
    /* The authorize flow is no longer gated behind a flag; the prepare
     * request runs through the session channel admission. */
    _reset();
    _establish_session();
    static const uint8_t prepare[] =
    {
        0x08, 0x02, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x52, 0x0b, 0x09, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x00,
    };
    uint8_t framed[512];
    ble_gatt_registry_access_context_t context;

    memset(framed, 0, sizeof(framed));
    framed[0] = 1U;
    framed[1] = 3U;
    framed[2] = 0x01U;
    framed[4] = (uint8_t)((sizeof(prepare) + 1U) & 0xffU);
    framed[5] = (uint8_t)(((sizeof(prepare) + 1U) >> 8U) & 0xffU);
    framed[8] = BLE_LINK_SERVICE_TRANSPORT_TYPE_PROTECTED;
    memcpy(&framed[9], prepare, sizeof(prepare));
    memset(&context, 0, sizeof(context));
    context.op = BLE_GATT_REGISTRY_OP_WRITE_CHR;
    context.write_data = framed;
    context.write_len = (uint16_t)(9U + sizeof(prepare));
    const uint16_t session_rx_handle = 0x60U;
    const uint16_t session_tx_handle = 0x61U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0xf4, 0xeb, 0x8f, 0x50, 0x48, 0xee, 0x19, 0x83,
              0xfe, 0x48, 0xf5, 0x60, 0xdb, 0xae, 0xbf, 0x1b,
    }, session_rx_handle));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0xec, 0x3d, 0x69, 0x58, 0xa5, 0xc1, 0xa2, 0x83,
              0x5a, 0x4f, 0x1b, 0x57, 0x38, 0x5d, 0xc6, 0x2c,
    }, session_tx_handle));
    ble_link_gatt_update_handles();
    /* The session response is an indication: its CCCD must be enabled. */
    _subscribe_tx_kind(session_tx_handle, false, true);
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_handle(
                          session_rx_handle, &characteristic));
    /* The feed succeeds: the prepare response is produced. */
    TEST_ASSERT_EQUAL(0, characteristic->access_cb(
                          7U, session_rx_handle, &context, NULL));
}

static void test_idle_timeout_wired(void)
{
    _reset();
    _establish_session();
    uint32_t error = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_query_admission(
                          GEN, BLE_LINK_SESSION_CHANNEL_CONTROL,
                          &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, error);
    bool partial = false;
    uint32_t ingress_epoch = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_get_reassembly_state(
                          BLE_LINK_SERVICE_RX_CONTROL,
                          &partial, &ingress_epoch));
    ble_link_gatt_on_reassembly_idle_generation(GEN, ingress_epoch);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_query_admission(
                          GEN, BLE_LINK_SESSION_CHANNEL_CONTROL,
                          &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED, error);
}

int main(void)
{
    test_att_mtu_clamped();
    test_identity_update_preserves_current_acl_mtu();
    test_registered_characteristics();
    test_registered_profile_uuids();
    test_write_feeds_service();
    test_link_state_read();
    test_event_sequence_boot_lifecycle();
    test_refresh_publishes_once();
    test_refresh_retains_concurrent_epoch_dirty();
    test_async_failure_retry_is_cooled_down();
    test_update_handles();
    test_idle_timeout_wired();
    test_legacy_transfer_service_absent();
    test_link_state_read_reports_att_resource();
    test_authorize_prepare_on_session_channel();
    printf("ble_link_gatt: all tests passed\n");
    return 0;
}
