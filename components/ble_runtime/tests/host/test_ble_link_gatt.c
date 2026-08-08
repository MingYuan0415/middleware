#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_gatt_registry.h"
#include "ble_link_gatt.h"
#include "ble_link_session.h"
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

static ble_link_gatt_config_t s_config;

static void _publish(const uint8_t *value, size_t len, void *arg)
{
    (void)arg;
    memcpy(s_published, value, len);
    s_published_len = len;
    s_publish_calls++;
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
    memset(&s_config, 0, sizeof(s_config));
    s_config.boot_id = BOOT_ID;
    s_config.connection_generation = GEN;
    s_config.conn_handle = 7U;
    s_config.att_mtu = 23U;
    s_config.tx_queue_depth = 32U;
    s_config.publish_link_state = _publish;
    ble_link_gatt_reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_gatt_init(&s_config));
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
}

static void test_registered_characteristics(void)
{
    /* link_state first. */
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_uuid(
                          (const uint8_t[16])
    {
        0xb5, 0x82, 0xef, 0xa4, 0x8a, 0x23, 0x4f, 0x86,
              0xeb, 0x44, 0x6b, 0xdc, 0x24, 0x1f, 0x78, 0xa4,
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

static void test_write_feeds_service(void)
{
    /* A capabilities request written to control_rx produces an outbound
     * fragment through the sink (TX scheduler submit). */
    static const uint8_t request[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0xc8, 0x13, 0x3d, 0x40, 0xfb, 0x3d, 0x0c, 0x8e,
              0x72, 0x47, 0x9d, 0x66, 0x62, 0x46, 0xa1, 0x81,
    }, control_rx_handle));
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_handle(
                          control_rx_handle, &characteristic));
    const int cb_result = characteristic->access_cb(
                              7U, control_rx_handle, &context, NULL);

    fprintf(stderr, "write cb result=%d\n", cb_result);
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
        0xb5, 0x82, 0xef, 0xa4, 0x8a, 0x23, 0x4f, 0x86,
              0xeb, 0x44, 0x6b, 0xdc, 0x24, 0x1f, 0x78, 0xa4,
    }, link_state_handle));
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_handle(
                          link_state_handle, &characteristic));
    TEST_ASSERT_EQUAL(0, characteristic->access_cb(
                          7U, link_state_handle, &context, NULL));
    TEST_ASSERT_TRUE(len > 0U);
    /* protocol_major=1 first byte. */
    TEST_ASSERT_EQUAL(0x08U, value[0]);
    TEST_ASSERT_EQUAL(0x01U, value[1]);
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
        0x2a, 0x05, 0xaf, 0xd2, 0x5f, 0xec, 0xa1, 0x83,
              0x2c, 0x40, 0xac, 0xbe, 0x10, 0x57, 0xe8, 0x2b,
    }, 0x30U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0x3a, 0x88, 0x03, 0x4c, 0xf6, 0xb8, 0x62, 0xb5,
              0x9c, 0x4a, 0x40, 0x1e, 0xc7, 0x5a, 0x73, 0x11,
    }, 0x40U));
    ble_link_gatt_update_handles();
    TEST_ASSERT_EQUAL(0x30U, ble_link_gatt_session_tx_handle());
    TEST_ASSERT_EQUAL(0x40U, ble_link_gatt_control_tx_handle());
}

static void test_transfer_service_rejects(void)
{
    const ble_gatt_registry_characteristic_t *characteristic = NULL;

    _reset();
    /* transfer_rx exists and rejects every access. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_lookup_by_uuid(
                          (const uint8_t[16])
    {
        0xa1, 0xf0, 0x28, 0x86, 0x72, 0x76, 0xac, 0x97,
              0x1e, 0x47, 0xff, 0xe9, 0x4d, 0x8d, 0x59, 0xf3,
    }, &characteristic));
    TEST_ASSERT_TRUE(characteristic != NULL);
    TEST_ASSERT_TRUE(characteristic->access_cb != NULL);
    /* An unauthenticated write is rejected. */
    uint8_t data[4] = {0x01, 0x02, 0x03, 0x04};
    ble_gatt_registry_access_context_t context;

    memset(&context, 0, sizeof(context));
    context.op = BLE_GATT_REGISTRY_OP_WRITE_CHR;
    context.write_data = data;
    context.write_len = sizeof(data);
    TEST_ASSERT_TRUE(characteristic->access_cb(
                         7U, 0x50U, &context, NULL) != 0);
}

static void test_authorize_prepare_on_session_channel(void)
{
    /* The authorize flow is no longer gated behind a flag; the prepare
     * request runs through the session channel admission. */
    _reset();
    _establish_session();
    static const uint8_t prepare[] =
    {
        0x08, 0x01, 0x19, 0x08, 0x07, 0x06, 0x05, 0x04,
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

    TEST_ASSERT_EQUAL(ESP_OK, ble_gatt_registry_assign_handle(
                          (const uint8_t[16])
    {
        0xa2, 0xf0, 0xcd, 0xfc, 0xe0, 0xe6, 0x5c, 0xb8,
              0xd8, 0x4d, 0xcb, 0x4c, 0x43, 0xe6, 0x01, 0x48,
    }, session_rx_handle));
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
    test_registered_characteristics();
    test_write_feeds_service();
    test_link_state_read();
    test_refresh_publishes_once();
    test_update_handles();
    test_idle_timeout_wired();
    test_transfer_service_rejects();
    test_authorize_prepare_on_session_channel();
    printf("ble_link_gatt: all tests passed\n");
    return 0;
}
