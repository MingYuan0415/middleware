#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_service.h"
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
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%lld != %lld)\n", \
                    __LINE__, #expected, #actual, expected_value, \
                    actual_value); \
            abort(); \
        } \
    } while (0)

#define BOOT_ID 72623859790382856ULL

static uint8_t s_tx[DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES];
static size_t s_tx_len;
static unsigned s_tx_calls;
static uint32_t s_submitted_id;
static device_link_v1_operation_t s_submitted_op;
static unsigned s_submit_calls;
static esp_err_t s_output_result = ESP_OK;

static esp_err_t _output(const uint8_t *value, size_t len,
                         ble_link_service_tx_channel_t channel, bool is_last,
                         uint32_t flow_id, void *arg)
{
    (void)channel;
    (void)is_last;
    (void)flow_id;
    (void)arg;
    TEST_ASSERT_TRUE(len <= sizeof(s_tx));
    memcpy(s_tx, value, len);
    s_tx_len = len;
    s_tx_calls++;
    return s_output_result;
}

static void _fill_info(device_link_v1_info_t *info, void *arg)
{
    (void)arg;
    info->firmware_major = 1U;
    info->firmware_minor = 2U;
    info->firmware_patch = 3U;
}

static device_link_v1_status_t _submit(
    device_link_v1_operation_t operation,
    const device_link_v1_credentials_t *credentials,
    uint32_t operation_id, void *arg)
{
    (void)credentials;
    (void)arg;
    s_submitted_op = operation;
    s_submitted_id = operation_id;
    s_submit_calls++;
    return DEVICE_LINK_V1_STATUS_ACCEPTED;
}

static ble_link_service_facts_t _facts(void)
{
    ble_link_service_facts_t facts;

    memset(&facts, 0, sizeof(facts));
    facts.active_boot_id = BOOT_ID;
    facts.connection_generation = 1U;
    facts.preferred_att_mtu = DEVICE_LINK_V1_REQUIRED_ATT_MTU;
    facts.conn_handle = 7U;
    facts.encrypted = true;
    facts.session_authenticated = true;
    facts.authorized = true;
    facts.identity_known = true;
    facts.secure_connections_bond_verified = true;
    facts.pairing_window_open = true;
    return facts;
}

static void _reset(void)
{
    static const ble_link_v1_owner_ops_t ops =
    {
        .fill_info = _fill_info,
        .submit_operation = _submit,
    };

    memset(s_tx, 0, sizeof(s_tx));
    s_tx_len = 0U;
    s_tx_calls = 0U;
    s_submitted_id = 0U;
    s_submitted_op = 0;
    s_submit_calls = 0U;
    s_output_result = ESP_OK;
    ble_link_service_init(BOOT_ID, _output, NULL, NULL, 0U);
    ble_link_service_set_v1_ops(&ops, NULL);
    ble_link_service_set_pairing_window(true);
    ble_link_service_on_connect(1U, 7U);
}

static void _execute_write(const uint8_t *value, size_t len)
{
    ble_link_work_t *work = NULL;
    const ble_link_service_facts_t facts = _facts();

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, value, len,
                          &work));
    TEST_ASSERT_TRUE(work != NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
}

static void test_get_info_response(void)
{
    const uint8_t request[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };

    _reset();
    _execute_write(request, sizeof(request));
    TEST_ASSERT_TRUE(s_tx_calls >= 1U);
    TEST_ASSERT_TRUE(s_tx_len >= 2U);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_GET_INFO | DEVICE_LINK_V1_RESPONSE_MASK,
                      s_tx[0]);
    TEST_ASSERT_EQUAL(0x01U, s_tx[1]);
}

static void test_scan_occupies_slot_until_ack(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    uint8_t ack[6];
    device_link_v1_snapshot_t snapshot;

    _reset();
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.state = DEVICE_LINK_V1_WIFI_IDLE;
    ble_link_service_observe_snapshot(&snapshot);
    _execute_write(scan, sizeof(scan));
    TEST_ASSERT_EQUAL(1U, s_submit_calls);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_OPERATION_SCAN, s_submitted_op);
    TEST_ASSERT_TRUE(s_submitted_id != 0U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_complete_operation(
                          s_submitted_id, DEVICE_LINK_V1_WIFI_FAILURE_NONE,
                          NULL, 0U, &snapshot));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    ack[0] = DEVICE_LINK_V1_ACK_OPERATION;
    ack[1] = 0x03U;
    ack[2] = (uint8_t)s_submitted_id;
    ack[3] = (uint8_t)(s_submitted_id >> 8);
    ack[4] = (uint8_t)(s_submitted_id >> 16);
    ack[5] = (uint8_t)(s_submitted_id >> 24);
    _execute_write(ack, sizeof(ack));
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_ACK_OPERATION |
                      DEVICE_LINK_V1_RESPONSE_MASK, s_tx[0]);
}

static void test_numeric_comparison_offer(void)
{
    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_offer_numeric_comparison(
                          123456U));
    TEST_ASSERT_TRUE(ble_link_service_pending_confirmation());
    TEST_ASSERT_EQUAL(123456U, ble_link_service_numeric_comparison_value());
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_confirm_binding(
                          ble_link_service_confirmation_token(), true));
    TEST_ASSERT_TRUE(!ble_link_service_pending_confirmation());
}

static void test_scan_without_owner_returns_internal(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    const uint8_t again[] = { DEVICE_LINK_V1_SCAN, 0x03U };

    memset(s_tx, 0, sizeof(s_tx));
    s_tx_len = 0U;
    s_tx_calls = 0U;
    s_output_result = ESP_OK;
    ble_link_service_init(BOOT_ID, _output, NULL, NULL, 0U);
    ble_link_service_set_pairing_window(true);
    ble_link_service_on_connect(1U, 7U);
    _execute_write(scan, sizeof(scan));
    TEST_ASSERT_TRUE(s_tx_len >= 3U);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_SCAN | DEVICE_LINK_V1_RESPONSE_MASK,
                      s_tx[0]);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_INTERNAL, s_tx[2]);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_response_completed(1U, true));
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    _execute_write(again, sizeof(again));
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_INTERNAL, s_tx[2]);
}

static void test_execute_output_failure_unblocks_writes(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    ble_link_work_t *work = NULL;
    const ble_link_service_facts_t facts = _facts();

    _reset();
    s_output_result = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, scan,
                          sizeof(scan), &work));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
}

static void test_abort_tx_rejects_stale_identity(void)
{
    ble_link_operation_identity_t stale =
    {
        .generation = 99U,
        .conn_handle = 7U,
    };
    ble_link_operation_identity_t current =
    {
        .generation = 1U,
        .conn_handle = 7U,
    };

    _reset();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_abort_tx_if_current(&stale));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_abort_tx_if_current(&current));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(&stale));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_clear_session_state_if_current(
                          &current));
}

int main(void)
{
    test_get_info_response();
    test_scan_occupies_slot_until_ack();
    test_numeric_comparison_offer();
    test_scan_without_owner_returns_internal();
    test_execute_output_failure_unblocks_writes();
    test_abort_tx_rejects_stale_identity();
    return 0;
}
