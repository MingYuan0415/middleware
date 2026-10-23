#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_link_service.h"
#include "ble_link_session.h"
#include "ble_runtime_test_hooks.h"
#include "ble_gap_manager.h"
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
static ble_link_operation_identity_t s_last_identity;
static bool s_complete_in_output;
static bool s_pump_in_output;
static esp_err_t s_inline_status;
static unsigned s_wake_calls;

static void _wake(void *arg)
{
    (void)arg;
    ++s_wake_calls;
}

static esp_err_t _output(const uint8_t *value, size_t len,
                         ble_link_service_tx_channel_t channel, bool is_last,
                         const ble_link_operation_identity_t *identity, void *arg)
{
    (void)channel;
    (void)is_last;
    (void)arg;
    TEST_ASSERT_TRUE(identity != NULL);
    TEST_ASSERT_EQUAL(0U, identity->token);
    TEST_ASSERT_EQUAL(BLE_LINK_OPERATION_TX_INDICATE, identity->kind);
    TEST_ASSERT_TRUE(identity->generation != 0U && identity->flow_id != 0U);
    s_last_identity = *identity;
    s_last_identity.token = 101U; /* Scheduler-owned completion token. */
    TEST_ASSERT_TRUE(len <= sizeof(s_tx));
    memcpy(s_tx, value, len);
    s_tx_len = len;
    s_tx_calls++;
    if (s_complete_in_output)
    {
        const ble_link_operation_identity_t submitted = *identity;
        const esp_err_t result = s_output_result;

        s_complete_in_output = false;
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_tx_completed(
                              &s_last_identity, s_inline_status));
        if (s_pump_in_output)
        {
            device_link_v1_snapshot_t snapshot = { 0 };

            s_pump_in_output = false;
            s_output_result = ESP_OK;
            snapshot.state = DEVICE_LINK_V1_WIFI_SCANNING;
            ble_link_service_observe_snapshot(&snapshot);
            TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
        }
        TEST_ASSERT_TRUE(ble_link_operation_identity_equal(&submitted, identity));
        return result;
    }
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
    facts.secure_connections_bond_verified = true;
    facts.identity_known = true;
    facts.secure_connections_bond_verified = true;
    facts.pairing_window_open = true;
    return facts;
}

static uint32_t s_session_generation;

static void _connect(uint32_t generation, uint16_t conn_handle)
{
    if (generation != s_session_generation)
    {
        if (s_session_generation != 0U)
        {
            TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                                  s_session_generation,
                                  BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
        }
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                              generation, BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
        s_session_generation = generation;
    }
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_on_connect(generation, conn_handle));
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
    memset(&s_last_identity, 0, sizeof(s_last_identity));
    s_complete_in_output = false;
    s_pump_in_output = false;
    s_inline_status = ESP_OK;
    s_wake_calls = 0U;
    ble_link_session_init(BOOT_ID);
    s_session_generation = 0U;
    ble_link_service_init(BOOT_ID, _output, NULL, NULL, 0U);
    ble_link_service_set_v1_ops(&ops, NULL);
    ble_link_service_set_worker_wake(_wake, NULL);
    ble_link_service_set_pairing_window(true);
    _connect(1U, 7U);
    ble_link_service_set_transport_admitted(1U, 7U, true);
}

static void _confirm_tx(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_tx_completed(
                          &s_last_identity, ESP_OK));
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
                      ble_link_service_allocate_confirmation_token(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_service_publish_numeric_comparison(123456U, 0U));
    TEST_ASSERT_EQUAL(0U, s_wake_calls);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_allocate_confirmation_token(&offered_token));
    TEST_ASSERT_EQUAL(0U, s_wake_calls);
    TEST_ASSERT_TRUE(!ble_link_service_get_confirmation().pending);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_publish_numeric_comparison(123456U, offered_token));
    TEST_ASSERT_EQUAL(1U, s_wake_calls);
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(confirmation.pending);
    TEST_ASSERT_EQUAL(offered_token, confirmation.token);
    TEST_ASSERT_EQUAL(123456U, confirmation.numeric_comparison);
    const uint64_t first_token = confirmation.token;

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_allocate_confirmation_token(&offered_token));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_publish_numeric_comparison(654321U, offered_token));
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
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_allocate_confirmation_token(&offered_token));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_publish_numeric_comparison(123456U, offered_token));
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
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_allocate_confirmation_token(&offered_token));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_publish_numeric_comparison(123456U, offered_token));
    TEST_ASSERT_EQUAL(1U, s_wake_calls);
    ble_link_service_reset();
    TEST_ASSERT_EQUAL(2U, s_wake_calls);
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(!confirmation.pending);

    _connect(2U, 9U);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_allocate_confirmation_token(&offered_token));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_publish_numeric_comparison(654321U, offered_token));
    TEST_ASSERT_EQUAL(3U, s_wake_calls);
    _connect(3U, 10U);
    TEST_ASSERT_EQUAL(4U, s_wake_calls);
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(!confirmation.pending);
    TEST_ASSERT_EQUAL(0U, confirmation.token);
    TEST_ASSERT_EQUAL(0U, confirmation.numeric_comparison);
}

static void test_confirmation_tokens_survive_init_and_exhaust_without_reuse(void)
{
    uint64_t first;
    uint64_t next;

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_allocate_confirmation_token(&first));
    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_allocate_confirmation_token(&next));
    TEST_ASSERT_TRUE(next > first);
    ble_link_service_test_set_confirmation_counter(UINT64_MAX - 1U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_allocate_confirmation_token(&next));
    TEST_ASSERT_TRUE(next == UINT64_MAX);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_publish_numeric_comparison(123456U, next));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_service_allocate_confirmation_token(&next));
    TEST_ASSERT_TRUE(next == 0U);
    const ble_link_confirmation_snapshot_t confirmation =
        ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(confirmation.pending && confirmation.token == UINT64_MAX);
    TEST_ASSERT_EQUAL(123456U, confirmation.numeric_comparison);
    _reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_service_allocate_confirmation_token(&next));
    TEST_ASSERT_TRUE(next == 0U);
    puts("confirmation tokens: reinit preserves identity; exhaustion retains prompt");
}

static void test_scan_without_owner_returns_internal(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    const uint8_t again[] = { DEVICE_LINK_V1_SCAN, 0x03U };

    memset(s_tx, 0, sizeof(s_tx));
    s_tx_len = 0U;
    s_tx_calls = 0U;
    s_output_result = ESP_OK;
    ble_link_session_init(BOOT_ID);
    s_session_generation = 0U;
    ble_link_service_init(BOOT_ID, _output, NULL, NULL, 0U);
    ble_link_service_set_pairing_window(true);
    _connect(1U, 7U);
    ble_link_service_set_transport_admitted(1U, 7U, true);
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

static void test_execute_rechecks_transport_admission(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    ble_link_work_t *work = NULL;
    const ble_link_service_facts_t facts = _facts();

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, scan,
                          sizeof(scan), &work));
    ble_link_service_set_transport_admitted(1U, 7U, false);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_service_execute(work));
    TEST_ASSERT_EQUAL(0U, s_submit_calls);
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    ble_link_service_release_work(work);
}

static void test_stale_transport_update_is_ignored(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    ble_link_work_t *work = NULL;
    const ble_link_service_facts_t facts = _facts();

    _reset();
    ble_link_service_set_transport_admitted(2U, 7U, false);
    ble_link_service_set_transport_admitted(1U, 8U, false);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, scan,
                          sizeof(scan), &work));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    TEST_ASSERT_EQUAL(1U, s_submit_calls);
    ble_link_service_release_work(work);
}

static void test_session_teardown_rejects_stale_identity(void)
{
    const ble_link_operation_identity_t stale =
    {
        .generation = 99U,
        .conn_handle = 7U,
    };
    const ble_link_operation_identity_t current =
    {
        .generation = 1U,
        .conn_handle = 7U,
    };

    _reset();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(&stale));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_clear_session_state_if_current(&current));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_clear_session_state_if_current(&current));
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
    _connect(2U, 9U);
    ble_link_service_set_transport_admitted(2U, 9U, true);
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
    _connect(2U, 7U);
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
    _confirm_tx();
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
    first_flow = s_last_identity.flow_id;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_execute(work));
    TEST_ASSERT_EQUAL(first_opcode, s_tx[0]);
    TEST_ASSERT_EQUAL(first_calls, s_tx_calls);
    TEST_ASSERT_EQUAL(first_flow, s_last_identity.flow_id);
    ble_link_service_release_work(work);
}

static void test_restart_drops_old_connection_work(void)
{
    _reset();
    ble_gap_manager_init();
    ble_gap_manager_test_set_generation(0U);
    const ble_gap_manager_event_t connect =
    {
        .type = BLE_GAP_MANAGER_EVENT_CONNECT,
        .conn_handle = 7U,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&connect));
    ble_link_work_t *work = NULL;
    const uint8_t request[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    const ble_link_service_facts_t facts = _facts();

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION,
                          request, sizeof(request), &work));
    ble_link_service_reset();
    ble_gap_manager_init();
    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_handle_event(&connect));
    ble_gap_manager_snapshot_t snapshot;

    TEST_ASSERT_EQUAL(ESP_OK, ble_gap_manager_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(2U, snapshot.generation);
    _connect(snapshot.generation, snapshot.conn_handle);
    ble_link_service_set_transport_admitted(
        snapshot.generation, snapshot.conn_handle, true);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    TEST_ASSERT_EQUAL(0U, s_submit_calls);
    ble_link_service_release_work(work);
}

static esp_err_t _write_unconfirmed(
    const ble_link_service_facts_t *facts, const uint8_t *value, size_t len)
{
    ble_link_work_t *work = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          facts, BLE_LINK_SERVICE_RX_SESSION, value, len, &work));
    const esp_err_t result = ble_link_service_execute(work);

    ble_link_service_release_work(work);
    return result;
}

static void _assert_pump_idle(unsigned calls,
                              const ble_link_operation_identity_t *identity)
{
    for (unsigned i = 0U; i < 100U; ++i)
    {
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
        TEST_ASSERT_EQUAL(calls, s_tx_calls);
        TEST_ASSERT_TRUE(ble_link_operation_identity_equal(
                             identity, &s_last_identity));
    }
}

static void test_pump_preserves_each_in_flight_identity(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    const ble_link_service_facts_t facts = _facts();
    device_link_v1_snapshot_t snapshot = { 0 };

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, scan, sizeof(scan)));
    ble_link_operation_identity_t identity = s_last_identity;

    _assert_pump_idle(1U, &identity);
    _confirm_tx();
    snapshot.state = DEVICE_LINK_V1_WIFI_SCANNING;
    ble_link_service_observe_snapshot(&snapshot);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    identity = s_last_identity;
    _assert_pump_idle(2U, &identity);
    _confirm_tx();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_complete_operation(
                          s_submitted_id, DEVICE_LINK_V1_WIFI_FAILURE_NONE,
                          NULL, 0U, &snapshot));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_WIFI_STATUS, s_tx[1]);
    identity = s_last_identity;
    _assert_pump_idle(3U, &identity);
    _confirm_tx();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_SCAN_COMPLETE, s_tx[1]);
    identity = s_last_identity;
    _assert_pump_idle(4U, &identity);
    _confirm_tx();
    _assert_pump_idle(4U, &identity);
}

static void test_completion_validates_identity_without_side_effects(void)
{
    const uint8_t info[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    const ble_link_service_facts_t facts = _facts();

    _reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_service_tx_completed(NULL, ESP_OK));
    TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, info, sizeof(info)));
    const ble_link_operation_identity_t current = s_last_identity;
    ble_link_operation_identity_t invalid = current;

    invalid.generation = 0U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_service_tx_completed(&invalid, ESP_OK));
    invalid = current;
    invalid.flow_id = 0U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_service_tx_completed(&invalid, ESP_OK));
    invalid = current;
    invalid.token = 0U;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_service_tx_completed(&invalid, ESP_OK));
    invalid = current;
    invalid.kind = BLE_LINK_OPERATION_TX_NOTIFY;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_service_tx_completed(&invalid, ESP_OK));
    invalid = current;
    ++invalid.generation;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&invalid, ESP_FAIL));
    invalid = current;
    ++invalid.conn_handle;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&invalid, ESP_FAIL));
    invalid = current;
    ++invalid.flow_id;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&invalid, ESP_FAIL));
    TEST_ASSERT_EQUAL(0U, s_wake_calls);
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    _assert_pump_idle(1U, &current);
    _confirm_tx();
    TEST_ASSERT_EQUAL(1U, s_wake_calls);
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&current, ESP_FAIL));
    TEST_ASSERT_EQUAL(1U, s_wake_calls);

    ble_link_work_t *work = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, info,
                          sizeof(info), &work));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&current, ESP_OK));
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&current, ESP_FAIL));
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(1U, s_wake_calls);
    _confirm_tx();
    TEST_ASSERT_EQUAL(2U, s_wake_calls);
}

static void test_completion_does_not_match_reused_handle(void)
{
    const uint8_t info[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    ble_link_service_facts_t facts = _facts();

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, info, sizeof(info)));
    const ble_link_operation_identity_t old = s_last_identity;

    /* Replacing the connection must retire TX without recycling its flow. */
    _connect(2U, 7U);
    ble_link_service_set_transport_admitted(2U, 7U, true);
    facts.connection_generation = 2U;
    TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, info, sizeof(info)));
    const ble_link_operation_identity_t current = s_last_identity;

    TEST_ASSERT_TRUE(current.flow_id > old.flow_id);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&old, ESP_OK));
    _assert_pump_idle(2U, &current);
    TEST_ASSERT_EQUAL(0U, s_wake_calls);
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    _confirm_tx();
}

static void test_terminal_status_retires_once(void)
{
    const esp_err_t statuses[] = { ESP_FAIL, ESP_ERR_TIMEOUT };
    const uint8_t info[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    const ble_link_service_facts_t facts = _facts();

    for (size_t i = 0U; i < sizeof(statuses) / sizeof(statuses[0]); ++i)
    {
        _reset();
        TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, info, sizeof(info)));
        const ble_link_operation_identity_t failed = s_last_identity;

        TEST_ASSERT_EQUAL(ESP_OK,
                          ble_link_service_tx_completed(&failed, statuses[i]));
        TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
        TEST_ASSERT_EQUAL(1U, s_wake_calls);
        TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                          ble_link_service_tx_completed(&failed, ESP_OK));
        TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                          ble_link_service_tx_completed(&failed, statuses[i]));
        TEST_ASSERT_EQUAL(1U, s_wake_calls);
        _assert_pump_idle(1U, &failed);
        _execute_write(info, sizeof(info));
    }
}

static void test_flow_exhaustion_never_recycles_identity(void)
{
    const uint8_t info[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    const uint8_t query[] = { DEVICE_LINK_V1_GET_OPERATION, 0x03U };
    const ble_link_service_facts_t facts = _facts();
    device_link_v1_snapshot_t snapshot = { 0 };

    _reset();
    ble_link_service_test_set_flow_counter(UINT32_MAX - 1U);
    TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, info, sizeof(info)));
    TEST_ASSERT_EQUAL(UINT32_MAX, s_last_identity.flow_id);
    _confirm_tx();
    const ble_link_operation_identity_t last = s_last_identity;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      _write_unconfirmed(&facts, info, sizeof(info)));
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(1U, s_tx_calls);
    snapshot.state = DEVICE_LINK_V1_WIFI_SCANNING;
    ble_link_service_observe_snapshot(&snapshot);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_pump_tx());
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    _assert_pump_idle(1U, &last);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      _write_unconfirmed(&facts, query, sizeof(query)));
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    ble_link_service_reset();
    _connect(1U, 7U);
    ble_link_service_set_transport_admitted(1U, 7U, true);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      _write_unconfirmed(&facts, query, sizeof(query)));
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(1U, s_tx_calls);
}

static void test_preoutput_failure_rejects_selected_event(void)
{
    const uint8_t info[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    const uint8_t status[] = { DEVICE_LINK_V1_GET_STATUS, 0x02U };
    const ble_link_service_facts_t facts = _facts();
    device_link_v1_snapshot_t snapshot = { 0 };

    _reset();
    ble_link_service_init(BOOT_ID, NULL, NULL, NULL, 0U);
    _connect(1U, 7U);
    ble_link_service_set_transport_admitted(1U, 7U, true);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      _write_unconfirmed(&facts, info, sizeof(info)));
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    snapshot.state = DEVICE_LINK_V1_WIFI_SCANNING;
    ble_link_service_observe_snapshot(&snapshot);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_pump_tx());
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(0U, s_tx_calls);

    _reset();
    snapshot.state = DEVICE_LINK_V1_WIFI_CONNECTED;
    memset(snapshot.profile_ssid, 'a', sizeof(snapshot.profile_ssid));
    snapshot.profile_ssid_length = sizeof(snapshot.profile_ssid);
    ble_link_service_observe_snapshot(&snapshot);
    /* Default MTU 23 cannot carry the full snapshot. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ble_link_service_pump_tx());
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(0U, s_tx_calls);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    ble_link_work_t *work = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, status,
                          sizeof(status), &work));
    /* Admission captured MTU 498; shrink the live transport before execution
     * so GET_STATUS still encodes the full, legitimate 32-byte SSID response. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_set_att_mtu(
                          1U, 7U, DEVICE_LINK_V1_MINIMUM_ATT_MTU));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(0U, s_tx_calls);
    _execute_write(info, sizeof(info));
    TEST_ASSERT_EQUAL(1U, s_tx_calls);
}

static void test_reentrant_completion_preserves_confirmed_accepted_and_ack(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    const uint8_t query[] = { DEVICE_LINK_V1_GET_OPERATION, 0x03U };
    uint8_t ack[] = { DEVICE_LINK_V1_ACK_OPERATION, 0x04U, 0U, 0U, 0U, 0U };
    const ble_link_service_facts_t facts = _facts();
    device_link_v1_snapshot_t snapshot = { 0 };

    _reset();
    s_complete_in_output = true;
    s_output_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, _write_unconfirmed(&facts, scan, sizeof(scan)));
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    snapshot.state = DEVICE_LINK_V1_WIFI_IDLE;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_complete_operation(
                          s_submitted_id, DEVICE_LINK_V1_WIFI_FAILURE_NONE,
                          NULL, 0U, &snapshot));
    s_output_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_SCAN_COMPLETE, s_tx[1]);
    _confirm_tx();
    ack[2] = (uint8_t)s_submitted_id;
    ack[3] = (uint8_t)(s_submitted_id >> 8);
    ack[4] = (uint8_t)(s_submitted_id >> 16);
    ack[5] = (uint8_t)(s_submitted_id >> 24);
    s_complete_in_output = true;
    s_output_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, _write_unconfirmed(&facts, ack, sizeof(ack)));
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    s_output_result = ESP_OK;
    _execute_write(query, sizeof(query));
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_NOT_FOUND, s_tx[2]);
}

static void test_reentrant_completion_does_not_reject_new_flow(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    const ble_link_service_facts_t facts = _facts();

    _reset();
    s_complete_in_output = true;
    s_pump_in_output = true;
    s_output_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, _write_unconfirmed(&facts, scan, sizeof(scan)));
    TEST_ASSERT_EQUAL(2U, s_tx_calls);
    TEST_ASSERT_EQUAL(2U, s_last_identity.flow_id);
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_WIFI_STATUS, s_tx[1]);
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(2U, s_wake_calls); /* Completion plus snapshot observation. */
    const ble_link_operation_identity_t current = s_last_identity;

    _assert_pump_idle(2U, &current);
    _confirm_tx();
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(3U, s_wake_calls);
}

static void test_session_retirement_ignores_late_completion(void)
{
    const uint8_t info[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    const ble_link_service_facts_t facts = _facts();

    for (unsigned mode = 0U; mode < 3U; ++mode)
    {
        _reset();
        TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, info, sizeof(info)));
        const ble_link_operation_identity_t old = s_last_identity;

        if (mode == 0U)
        {
            ble_link_service_reset();
        }
        else if (mode == 1U)
        {
            TEST_ASSERT_EQUAL(ESP_OK,
                              ble_link_service_clear_session_state_if_current(&old));
        }
        else
        {
            ble_link_service_abort_transactions();
        }
        TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
        TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                          ble_link_service_tx_completed(&old, ESP_OK));
        TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                          ble_link_service_tx_completed(&old, ESP_FAIL));
        TEST_ASSERT_EQUAL(0U, s_wake_calls);
    }
}

static void test_synchronous_terminal_error_retires_only_once(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    const uint8_t query[] = { DEVICE_LINK_V1_GET_OPERATION, 0x03U };
    const ble_link_service_facts_t facts = _facts();
    device_link_v1_snapshot_t snapshot = { 0 };

    _reset();
    s_complete_in_output = true;
    s_inline_status = ESP_FAIL;
    s_output_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, _write_unconfirmed(&facts, scan, sizeof(scan)));
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(1U, s_wake_calls);
    const ble_link_operation_identity_t failed = s_last_identity;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&failed, ESP_OK));
    snapshot.state = DEVICE_LINK_V1_WIFI_IDLE;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_complete_operation(
                          s_submitted_id, DEVICE_LINK_V1_WIFI_FAILURE_NONE,
                          NULL, 0U, &snapshot));
    /* Failed ACCEPTED must not make an unsolicited terminal eligible. */
    _assert_pump_idle(1U, &failed);
    s_output_result = ESP_OK;
    _execute_write(query, sizeof(query));
    TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_OK, s_tx[2]);
}

static void test_failed_terminal_and_ack_preserve_operation_record(void)
{
    const uint8_t scan[] = { DEVICE_LINK_V1_SCAN, 0x02U };
    const uint8_t query[] = { DEVICE_LINK_V1_GET_OPERATION, 0x03U };
    uint8_t ack[] = { DEVICE_LINK_V1_ACK_OPERATION, 0x04U, 0U, 0U, 0U, 0U };
    const ble_link_service_facts_t facts = _facts();
    const esp_err_t statuses[] = { ESP_FAIL, ESP_ERR_TIMEOUT };
    device_link_v1_snapshot_t snapshot = { 0 };

    for (size_t i = 0U; i < sizeof(statuses) / sizeof(statuses[0]); ++i)
    {
        _reset();
        _execute_write(scan, sizeof(scan));
        snapshot.state = DEVICE_LINK_V1_WIFI_IDLE;
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_complete_operation(
                              s_submitted_id, DEVICE_LINK_V1_WIFI_FAILURE_NONE,
                              NULL, 0U, &snapshot));
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
        TEST_ASSERT_EQUAL(DEVICE_LINK_V1_SCAN_COMPLETE, s_tx[1]);
        const ble_link_operation_identity_t terminal = s_last_identity;

        TEST_ASSERT_EQUAL(ESP_OK,
                          ble_link_service_tx_completed(&terminal, statuses[i]));
        TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
        _assert_pump_idle(2U, &terminal);
        _execute_write(query, sizeof(query));
        TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_OK, s_tx[2]);
        ack[2] = (uint8_t)s_submitted_id;
        ack[3] = (uint8_t)(s_submitted_id >> 8);
        ack[4] = (uint8_t)(s_submitted_id >> 16);
        ack[5] = (uint8_t)(s_submitted_id >> 24);
        TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, ack, sizeof(ack)));
        TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_tx_completed(
                              &s_last_identity, statuses[i]));
        TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
        _execute_write(query, sizeof(query));
        TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_OK, s_tx[2]);
        _execute_write(ack, sizeof(ack));
        _execute_write(query, sizeof(query));
        TEST_ASSERT_EQUAL(DEVICE_LINK_V1_STATUS_NOT_FOUND, s_tx[2]);
    }
}

static void test_pump_output_rejection_does_not_retry_selected_status(void)
{
    device_link_v1_snapshot_t snapshot = { 0 };

    _reset();
    snapshot.state = DEVICE_LINK_V1_WIFI_SCANNING;
    ble_link_service_observe_snapshot(&snapshot);
    s_output_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, ble_link_service_pump_tx());
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    const ble_link_operation_identity_t failed = s_last_identity;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&failed, ESP_OK));
    _assert_pump_idle(1U, &failed);
    snapshot.state = DEVICE_LINK_V1_WIFI_IDLE;
    ble_link_service_observe_snapshot(&snapshot);
    s_output_result = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(2U, s_tx_calls);
    TEST_ASSERT_TRUE(s_last_identity.flow_id > failed.flow_id);
    _confirm_tx();
}

static device_link_v1_snapshot_t _large_snapshot(void)
{
    device_link_v1_snapshot_t snapshot = { 0 };

    snapshot.state = DEVICE_LINK_V1_WIFI_CONNECTED;
    memset(snapshot.profile_ssid, 'a', sizeof(snapshot.profile_ssid));
    snapshot.profile_ssid_length = sizeof(snapshot.profile_ssid);
    return snapshot;
}

static void test_duplicate_connect_preserves_transport_and_active_tx(void)
{
    const uint8_t info[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    const ble_link_service_facts_t facts = _facts();
    device_link_v1_snapshot_t snapshot = _large_snapshot();

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, info, sizeof(info)));
    const ble_link_operation_identity_t current = s_last_identity;

    ble_link_service_observe_snapshot(&snapshot);
    const unsigned wakes = s_wake_calls;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_on_connect(1U, 7U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_on_connect(1U, 7U));
    TEST_ASSERT_EQUAL(wakes, s_wake_calls);
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    _assert_pump_idle(1U, &current);
    _confirm_tx();
    /* No new write can refresh MTU here: the duplicate retained MTU 498. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(2U, s_tx_calls);
    TEST_ASSERT_TRUE(s_tx_len > DEVICE_LINK_V1_MINIMUM_ATT_MTU - 3U);
    _confirm_tx();
    /* The original indicate admission also survived both duplicates. */
    _execute_write(info, sizeof(info));
    TEST_ASSERT_EQUAL(3U, s_tx_calls);
}

static void test_connect_requires_active_session_and_rejects_stale_identity(void)
{
    const uint8_t info[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    ble_link_service_facts_t facts = _facts();
    ble_link_work_t *work = NULL;

    _reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_service_on_connect(0U, 7U));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_link_service_on_connect(2U, 7U));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_link_service_on_connect(1U, 8U));
    /* Session is active but deliberately neither encrypted nor BOUND. */
    ble_link_dispatcher_facts_t session_facts;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_get_facts(1U, &session_facts));
    TEST_ASSERT_TRUE(!session_facts.encrypted);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_on_connect(1U, 7U));
    _connect(2U, 7U);
    ble_link_service_set_transport_admitted(2U, 7U, true);
    facts.connection_generation = 2U;
    TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, info, sizeof(info)));
    const ble_link_operation_identity_t current = s_last_identity;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_link_service_on_connect(1U, 7U));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_link_service_on_connect(2U, 8U));
    _assert_pump_idle(1U, &current);
    TEST_ASSERT_TRUE(ble_link_service_write_blocked());
    /* Even an independently active older session cannot replace service G2. */
    ble_link_session_reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          1U, BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_link_service_on_connect(1U, 7U));
    _assert_pump_idle(1U, &current);
    _confirm_tx();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          1U, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          2U, BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          2U, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_clear_session_state_if_current(&current));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ble_link_service_on_connect(2U, 7U));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_set_att_mtu(2U, 7U, 498U));
    ble_link_service_set_transport_admitted(2U, 7U, true);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, info,
                          sizeof(info), &work));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_EQUAL(1U, s_tx_calls);
}

static void test_qualified_mtu_ignores_stale_updates_and_clamps_minimum(void)
{
    device_link_v1_snapshot_t snapshot = _large_snapshot();

    _reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_service_set_att_mtu(0U, 7U, 498U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_set_att_mtu(1U, 7U, 498U));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_set_att_mtu(2U, 7U, 23U));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_set_att_mtu(1U, 8U, 23U));
    ble_link_service_observe_snapshot(&snapshot);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_TRUE(s_tx_len > DEVICE_LINK_V1_MINIMUM_ATT_MTU - 3U);
    _confirm_tx();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_set_att_mtu(1U, 7U, 0U));
    snapshot.profile_ssid[0] = 'b';
    ble_link_service_observe_snapshot(&snapshot);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ble_link_service_pump_tx());
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(1U, s_tx_calls);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.state = DEVICE_LINK_V1_WIFI_IDLE;
    ble_link_service_observe_snapshot(&snapshot);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(2U, s_tx_calls);
    _confirm_tx();
}

static void test_new_connection_resets_transport_and_does_not_replay(void)
{
    const uint8_t info[] = { DEVICE_LINK_V1_GET_INFO, 0x01U };
    ble_link_service_facts_t facts = _facts();
    device_link_v1_snapshot_t snapshot = _large_snapshot();
    ble_link_work_t *work = NULL;

    _reset();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_set_att_mtu(1U, 7U, 498U));
    ble_link_service_observe_snapshot(&snapshot);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_pump_tx());
    const ble_link_operation_identity_t old = s_last_identity;

    snapshot.profile_ssid[0] = 'b';
    ble_link_service_observe_snapshot(&snapshot);
    _connect(2U, 7U);
    TEST_ASSERT_TRUE(!ble_link_service_write_blocked());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_tx_completed(&old, ESP_OK));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      ble_link_service_set_att_mtu(1U, 7U, 498U));
    ble_link_service_set_transport_admitted(1U, 7U, true);
    _assert_pump_idle(1U, &old);
    /* Fresh G2 observation exceeds its reset MTU 23; no old MTU survived. */
    snapshot.profile_ssid[0] = 'c';
    ble_link_service_observe_snapshot(&snapshot);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ble_link_service_pump_tx());
    TEST_ASSERT_EQUAL(1U, s_tx_calls);
    facts.connection_generation = 2U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_accept(
                          &facts, BLE_LINK_SERVICE_RX_SESSION, info,
                          sizeof(info), &work));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_service_execute(work));
    ble_link_service_release_work(work);
    TEST_ASSERT_EQUAL(1U, s_tx_calls);
    ble_link_service_set_transport_admitted(2U, 7U, true);
    TEST_ASSERT_EQUAL(ESP_OK, _write_unconfirmed(&facts, info, sizeof(info)));
    TEST_ASSERT_EQUAL(2U, s_tx_calls);
    _confirm_tx();
}

int main(void)
{
    test_restart_drops_old_connection_work();
    test_get_info_response();
    test_scan_occupies_slot_until_ack();
    test_numeric_comparison_offer();
    test_numeric_comparison_disconnect_wakes_owner();
    test_numeric_comparison_reset_and_new_connection_wake_owner();
    test_scan_without_owner_returns_internal();
    test_execute_output_failure_unblocks_writes();
    test_execute_rechecks_transport_admission();
    test_stale_transport_update_is_ignored();
    test_session_teardown_rejects_stale_identity();
    test_submit_failure_returns_internal_without_slot();
    test_operation_timeout_remaining_tracks_deadline();
    test_stale_generation_is_dropped();
    test_stale_work_does_not_release_new_reservation();
    test_accept_blocks_second_write_until_confirmed();
    test_release_without_execute_unblocks_writes();
    test_pump_skips_event_while_write_reserved();
    test_execute_does_not_overwrite_in_flight_tx();
    test_pump_preserves_each_in_flight_identity();
    test_completion_validates_identity_without_side_effects();
    test_completion_does_not_match_reused_handle();
    test_terminal_status_retires_once();
    test_flow_exhaustion_never_recycles_identity();
    test_preoutput_failure_rejects_selected_event();
    test_reentrant_completion_preserves_confirmed_accepted_and_ack();
    test_reentrant_completion_does_not_reject_new_flow();
    test_session_retirement_ignores_late_completion();
    test_synchronous_terminal_error_retires_only_once();
    test_failed_terminal_and_ack_preserve_operation_record();
    test_pump_output_rejection_does_not_retry_selected_status();
    test_duplicate_connect_preserves_transport_and_active_tx();
    test_connect_requires_active_session_and_rejects_stale_identity();
    test_qualified_mtu_ignores_stale_updates_and_clamps_minimum();
    test_new_connection_resets_transport_and_does_not_replay();
    test_confirmation_tokens_survive_init_and_exhaust_without_reuse();
    return 0;
}
