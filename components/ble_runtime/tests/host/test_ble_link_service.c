#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
static device_link_v1_status_t s_submit_result = DEVICE_LINK_V1_STATUS_ACCEPTED;
static uint32_t s_last_flow_id;
static unsigned s_wake_calls;

static void _wake(void *arg)
{
    (void)arg;
    ++s_wake_calls;
}

static esp_err_t _output(const uint8_t *value, size_t len,
                         ble_link_service_tx_channel_t channel, bool is_last,
                         uint32_t flow_id, void *arg)
{
    (void)channel;
    (void)is_last;
    (void)arg;
    s_last_flow_id = flow_id;
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
    return s_submit_result;
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
    s_submit_result = DEVICE_LINK_V1_STATUS_ACCEPTED;
    s_last_flow_id = 0U;
    s_wake_calls = 0U;
    ble_link_service_init(BOOT_ID, _output, NULL, NULL, 0U);
    ble_link_service_set_v1_ops(&ops, NULL);
    ble_link_service_set_worker_wake(_wake, NULL);
    ble_link_service_set_pairing_window(true);
    ble_link_service_on_connect(1U, 7U);
}

static void _confirm_tx(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_response_completed(
                          s_last_flow_id, true));
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
    _confirm_tx();
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
    _confirm_tx();
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
    ble_link_confirmation_snapshot_t confirmation;
    uint64_t offered_token;

    _reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_service_offer_numeric_comparison(
                          123456U, NULL));
    TEST_ASSERT_EQUAL(0U, s_wake_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_offer_numeric_comparison(
                          123456U, &offered_token));
    TEST_ASSERT_EQUAL(1U, s_wake_calls);
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(confirmation.pending);
    TEST_ASSERT_EQUAL(offered_token, confirmation.token);
    TEST_ASSERT_EQUAL(123456U, confirmation.numeric_comparison);
    const uint64_t first_token = confirmation.token;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_offer_numeric_comparison(
                          654321U, &offered_token));
    TEST_ASSERT_EQUAL(2U, s_wake_calls);
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(confirmation.pending);
    TEST_ASSERT_EQUAL(offered_token, confirmation.token);
    TEST_ASSERT_TRUE(offered_token != first_token);
    TEST_ASSERT_EQUAL(654321U, confirmation.numeric_comparison);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_service_confirm_binding(first_token, true));
    TEST_ASSERT_EQUAL(2U, s_wake_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_confirm_binding(
                          confirmation.token, true));
    TEST_ASSERT_EQUAL(3U, s_wake_calls);
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(!confirmation.pending);
    TEST_ASSERT_EQUAL(0U, confirmation.token);
    TEST_ASSERT_EQUAL(0U, confirmation.numeric_comparison);

    ble_link_service_clear_session_state();
    TEST_ASSERT_EQUAL(3U, s_wake_calls);
}

static void test_numeric_comparison_disconnect_wakes_owner(void)
{
    ble_link_confirmation_snapshot_t confirmation;
    uint64_t offered_token;

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_offer_numeric_comparison(
                          123456U, &offered_token));
    TEST_ASSERT_TRUE(offered_token != 0U);
    TEST_ASSERT_EQUAL(1U, s_wake_calls);
    ble_link_service_clear_session_state();
    TEST_ASSERT_EQUAL(2U, s_wake_calls);
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(!confirmation.pending);
    TEST_ASSERT_EQUAL(0U, confirmation.token);
    TEST_ASSERT_EQUAL(0U, confirmation.numeric_comparison);
}

static void test_numeric_comparison_reset_and_new_connection_wake_owner(void)
{
    ble_link_confirmation_snapshot_t confirmation;
    uint64_t offered_token;

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_offer_numeric_comparison(
                          123456U, &offered_token));
    TEST_ASSERT_EQUAL(1U, s_wake_calls);
    ble_link_service_reset();
    TEST_ASSERT_EQUAL(2U, s_wake_calls);
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(!confirmation.pending);

    ble_link_service_on_connect(2U, 9U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_offer_numeric_comparison(
                          654321U, &offered_token));
    TEST_ASSERT_EQUAL(3U, s_wake_calls);
    ble_link_service_on_connect(3U, 10U);
    TEST_ASSERT_EQUAL(4U, s_wake_calls);
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(!confirmation.pending);
    TEST_ASSERT_EQUAL(0U, confirmation.token);
    TEST_ASSERT_EQUAL(0U, confirmation.numeric_comparison);
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

static void test_submit_failure_returns_internal_without_slot(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    const uint8_t query[] = { DEVICE_LINK_V1_GET_OPERATION, 0x03U };

    _reset();
    s_submit_result = DEVICE_LINK_V1_STATUS_INTERNAL;
    _execute_write(scan, sizeof(scan));
    TEST_ASSERT_TRUE(s_tx_len >= 3U);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_INTERNAL, s_tx[2]);
    s_submit_result = DEVICE_LINK_V1_STATUS_ACCEPTED;
    _execute_write(query, sizeof(query));
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_NOT_FOUND, s_tx[2]);
}

static void test_operation_timeout_remaining_tracks_deadline(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };

    _reset();
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_link_service_operation_timeout_remaining_ms());
    _execute_write(scan, sizeof(scan));
    TEST_ASSERT_TRUE(ble_link_service_operation_timeout_remaining_ms() <=
                     DEVICE_LINK_V1_OPERATION_TIMEOUT_MS);
    TEST_ASSERT_TRUE(ble_link_service_operation_timeout_remaining_ms() !=
                     UINT32_MAX);
    ble_link_service_tick(
        (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) +
        DEVICE_LINK_V1_OPERATION_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(UINT32_MAX,
                      ble_link_service_operation_timeout_remaining_ms());
}

static void test_stale_work_does_not_release_new_reservation(void)
{
    const uint8_t request[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    ble_link_work_t *old_work = NULL;
    ble_link_work_t *new_work = NULL;
    ble_link_work_t *third = NULL;
    ble_link_service_facts_t facts = _facts();

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, request,
                          sizeof(request), &old_work));
    ble_link_service_reset();
    ble_link_service_on_connect(2U, 9U);
    facts.connection_generation = 2U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, request,
                          sizeof(request), &new_work));
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(old_work));
    ble_link_service_release_work(old_work);
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, request,
                          sizeof(request), &third));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(new_work));
    ble_link_service_release_work(new_work);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_GET_INFO | DEVICE_LINK_V1_RESPONSE_MASK,
                      s_tx[0]);
    _confirm_tx();
}

static void test_stale_generation_is_dropped(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    ble_link_work_t *work = NULL;
    const ble_link_service_facts_t facts = _facts();

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, scan,
                          sizeof(scan), &work));
    ble_link_service_on_connect(2U, 7U);
    s_tx_calls = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_EQUAL(0U, s_tx_calls);
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
}

static void test_accept_blocks_second_write_until_confirmed(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    const uint8_t info[] = { DEVICE_LINK_V1_GET_INFO, 0x03U };
    ble_link_work_t *work = NULL;
    ble_link_work_t *blocked = NULL;
    const ble_link_service_facts_t facts = _facts();

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, scan,
                          sizeof(scan), &work));
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, info,
                          sizeof(info), &blocked));
    TEST_ASSERT_TRUE(blocked == NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_response_completed(
                          s_last_flow_id, true));
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
}

static void test_release_without_execute_unblocks_writes(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    ble_link_work_t *work = NULL;
    const ble_link_service_facts_t facts = _facts();

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, scan,
                          sizeof(scan), &work));
    ble_link_service_release_work(work);
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, scan,
                          sizeof(scan), &work));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    ble_link_service_release_work(work);
}

static void test_pump_skips_event_while_write_reserved(void)
{
    const uint8_t request[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    ble_link_work_t *work = NULL;
    const ble_link_service_facts_t facts = _facts();
    device_link_v1_snapshot_t snapshot;

    _reset();
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.state = DEVICE_LINK_V1_WIFI_CONNECTED;
    snapshot.failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    memcpy(snapshot.profile_ssid, "cafe", 4U);
    snapshot.profile_ssid_length = 4U;
    ble_link_service_observe_snapshot(&snapshot);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, request,
                          sizeof(request), &work));
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    s_tx_calls = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(0U, s_tx_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_EQUAL(1U, s_tx_calls);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_GET_INFO | DEVICE_LINK_V1_RESPONSE_MASK,
                      s_tx[0]);
    TEST_ASSERT_EQUAL(0x01U, s_tx[1]);
    _confirm_tx();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(2U, s_tx_calls);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_EVENT_MARKER, s_tx[0]);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_WIFI_STATUS, s_tx[1]);
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    _confirm_tx();
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, request,
                          sizeof(request), &work));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    _confirm_tx();
}

static void test_execute_does_not_overwrite_in_flight_tx(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    ble_link_work_t *work = NULL;
    const ble_link_service_facts_t facts = _facts();
    uint8_t first_opcode;
    unsigned first_calls;
    uint32_t first_flow;

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, scan,
                          sizeof(scan), &work));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    first_opcode = s_tx[0];
    first_calls = s_tx_calls;
    first_flow = s_last_flow_id;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_execute(work));
    TEST_ASSERT_EQUAL(first_opcode, s_tx[0]);
    TEST_ASSERT_EQUAL(first_calls, s_tx_calls);
    TEST_ASSERT_EQUAL(first_flow, s_last_flow_id);
    ble_link_service_release_work(work);
}

int main(void)
{
    test_get_info_response();
    test_scan_occupies_slot_until_ack();
    test_numeric_comparison_offer();
    test_numeric_comparison_disconnect_wakes_owner();
    test_numeric_comparison_reset_and_new_connection_wake_owner();
    test_scan_without_owner_returns_internal();
    test_execute_output_failure_unblocks_writes();
    test_abort_tx_rejects_stale_identity();
    test_submit_failure_returns_internal_without_slot();
    test_operation_timeout_remaining_tracks_deadline();
    test_stale_generation_is_dropped();
    test_stale_work_does_not_release_new_reservation();
    test_accept_blocks_second_write_until_confirmed();
    test_release_without_execute_unblocks_writes();
    test_pump_skips_event_while_write_reserved();
    test_execute_does_not_overwrite_in_flight_tx();
    return 0;
}
