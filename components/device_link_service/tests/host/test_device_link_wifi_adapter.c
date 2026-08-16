#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "connectivity_manager.h"
#include "device_link_operation.h"
#include "device_link_protocol.h"
#include "device_link_tlv.h"
#include "device_link_wifi_adapter.h"
#include "event_bus.h"

#define WIFI_METHOD_GET_STATUS 1U
#define WIFI_METHOD_START_SCAN 2U
#define WIFI_METHOD_GET_SCAN_RESULTS 3U
#define WIFI_METHOD_SET_CREDENTIALS 4U
#define WIFI_METHOD_DISCONNECT 5U
#define WIFI_METHOD_RECONNECT_SAVED 6U
#define WIFI_METHOD_FORGET_SAVED 7U
#define WIFI_METHOD_SET_AUTO_CONNECT 8U

/* Test-only fake hooks (fakes/connectivity_manager.c, fakes/event_bus.c,
 * fakes/ble_link_service_fake.c). */
extern void connectivity_manager_fake_set_scan(
    const connectivity_manager_scan_snapshot_t *snapshot);
extern void connectivity_manager_fake_set_request_result(esp_err_t result);
extern void event_bus_fake_publish(event_bus_msg_id_t msg_id,
                                   uint32_t sub_type, const void *payload,
                                   size_t payload_size);
extern void ble_link_service_fake_reset(void);
extern void ble_link_service_fake_set_in_flight(bool in_flight);
extern void ble_link_service_fake_set_start_result(esp_err_t result);
extern unsigned ble_link_service_fake_defer_count(void);
extern unsigned ble_link_service_fake_update_count(void);
extern uint64_t ble_link_service_fake_last_owner(void);
extern device_link_operation_state_t ble_link_service_fake_last_state(void);
extern device_link_status_t ble_link_service_fake_last_status(void);
extern size_t ble_link_service_fake_last_result_len(void);

static bool contains_byte(const uint8_t *data, size_t len, uint8_t value)
{
    for (size_t i = 0U; i < len; ++i)
    {
        if (data[i] == value)
        {
            return true;
        }
    }
    return false;
}

static device_link_status_t _call_method(uint8_t method_id,
        const uint8_t *request, size_t request_len)
{
    const device_link_domain_descriptor_t *descriptor = NULL;
    uint8_t response[128];
    size_t response_len = 0U;
    const device_link_request_context_t context =
    {
        .header =
        {
            .domain_id = DEVICE_LINK_DOMAIN_WIFI,
            .domain_major = 1U,
            .method_id = method_id,
            .call_id = 1U,
            .boot_id = 1U,
        },
        .security_authenticated = true,
        .authorized = true,
    };

    assert(device_link_wifi_adapter_get_descriptor(&descriptor) == ESP_OK);
    assert(method_id >= 1U && method_id <= descriptor->method_count);
    return descriptor->methods[method_id - 1U].handler(
               &context, request, request_len, response, sizeof(response),
               &response_len, descriptor->methods[method_id - 1U].handler_arg);
}

static void _fill_scan(connectivity_manager_scan_snapshot_t *scan)
{
    memset(scan, 0, sizeof(*scan));
    scan->generation = UINT64_C(0x0102030405060708);
    scan->truncated = true;
    for (size_t i = 0U; i < CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS; ++i)
    {
        connectivity_manager_scan_record_t *record = &scan->records[i];

        snprintf(record->ssid, sizeof(record->ssid), "AP-%zu", i);
        record->rssi = -40 - (int)i;
        record->channel = (uint8_t)(1U + i);
        record->security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL;
        record->saved = i == 0U;
        scan->record_count++;
    }
}

static void _make_scan_query(
    uint64_t generation, uint64_t page, uint8_t *out, size_t *out_len)
{
    device_link_tlv_writer_t writer;

    device_link_tlv_writer_init(&writer, out, 64U);
    assert(device_link_tlv_put_fixed64(&writer, 1U, generation) == ESP_OK);
    assert(device_link_tlv_put_uint(&writer, 2U, page) == ESP_OK);
    assert(device_link_tlv_writer_finish(&writer, out_len) == ESP_OK);
}

static void _count_scan_response(
    const uint8_t *response, size_t response_len,
    uint64_t *out_page, size_t *out_networks, bool *out_has_more,
    bool *out_truncated)
{
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has = false;

    *out_page = 0U;
    *out_networks = 0U;
    *out_has_more = false;
    *out_truncated = false;
    assert(device_link_tlv_reader_init(&reader, response, response_len) ==
           ESP_OK);
    while (device_link_tlv_reader_next(&reader, &field, &has) == ESP_OK &&
            has)
    {
        if (field.id == 2U)
        {
            *out_page = field.value.unsigned_value;
        }
        else if (field.id == 3U)
        {
            (*out_networks)++;
        }
        else if (field.id == 4U)
        {
            *out_has_more = field.value.unsigned_value != 0U;
        }
        else if (field.id == 5U)
        {
            *out_truncated = field.value.unsigned_value != 0U;
        }
    }
    assert(reader.offset == reader.len);
}

static void test_scan_results_are_paged(void)
{
    const device_link_domain_descriptor_t *descriptor = NULL;
    connectivity_manager_scan_snapshot_t scan;
    uint8_t request[64];
    size_t request_len = 0U;
    uint8_t response[768];
    size_t response_len = 0U;
    const device_link_request_context_t context =
    {
        .header =
        {
            .domain_id = DEVICE_LINK_DOMAIN_WIFI,
            .domain_major = 1U,
            .method_id = 3U,
            .call_id = 1U,
            .boot_id = 1U,
        },
        .security_authenticated = true,
        .authorized = true,
    };
    uint64_t page = 0U;
    size_t networks = 0U;
    bool has_more = false;
    bool truncated = false;

    _fill_scan(&scan);
    connectivity_manager_fake_set_scan(&scan);
    assert(device_link_wifi_adapter_get_descriptor(&descriptor) == ESP_OK);
    /* Page 0 carries every record below the page size and no tail. */
    _make_scan_query(scan.generation, 0U, request, &request_len);
    assert(descriptor->methods[2].handler(
               &context, request, request_len, response, sizeof(response),
               &response_len, descriptor->methods[2].handler_arg) ==
           DEVICE_LINK_STATUS_OK);
    _count_scan_response(response, response_len, &page, &networks,
                         &has_more, &truncated);
    assert(page == 0U);
    assert(networks == scan.record_count);
    assert(!has_more);
    assert(truncated);
    /* A page past the snapshot end is empty, not an error. */
    _make_scan_query(scan.generation, 7U, request, &request_len);
    assert(descriptor->methods[2].handler(
               &context, request, request_len, response, sizeof(response),
               &response_len, descriptor->methods[2].handler_arg) ==
           DEVICE_LINK_STATUS_OK);
    _count_scan_response(response, response_len, &page, &networks,
                         &has_more, &truncated);
    assert(page == 7U);
    assert(networks == 0U);
    assert(!has_more);
    /* A stale generation is NOT_FOUND. */
    _make_scan_query(scan.generation + 1U, 0U, request, &request_len);
    assert(descriptor->methods[2].handler(
               &context, request, request_len, response, sizeof(response),
               &response_len, descriptor->methods[2].handler_arg) ==
           DEVICE_LINK_STATUS_NOT_FOUND);
    connectivity_manager_fake_set_scan(NULL);
}

static void test_descriptor_is_static_and_complete(void)
{
    const device_link_domain_descriptor_t *descriptor = NULL;

    assert(device_link_wifi_adapter_get_descriptor(&descriptor) == ESP_OK);
    assert(descriptor != NULL);
    assert(descriptor->domain_id == DEVICE_LINK_DOMAIN_WIFI);
    assert(descriptor->major == 1U);
    assert(descriptor->method_count == 8U);
    for (size_t i = 0U; i < descriptor->method_count; ++i)
    {
        assert(descriptor->methods[i].method_id == i + 1U);
        assert(descriptor->methods[i].handler != NULL);
        assert(descriptor->methods[i].request_schema != NULL);
        assert(descriptor->methods[i].response_schema != NULL);
    }
}

static void test_credentials_are_delegated_without_echo(void)
{
    const device_link_domain_descriptor_t *descriptor = NULL;
    uint8_t nested[32];
    uint8_t request[64];
    device_link_tlv_writer_t nested_writer;
    device_link_tlv_writer_t writer;
    size_t nested_len = 0U;
    size_t request_len = 0U;

    device_link_tlv_writer_init(&nested_writer, nested, sizeof(nested));
    assert(device_link_tlv_put_bytes(&nested_writer, 1U,
                                     (const uint8_t *)"AP1", 3U) == ESP_OK);
    assert(device_link_tlv_put_bytes(&nested_writer, 2U,
                                     (const uint8_t *)"p", 1U) == ESP_OK);
    assert(device_link_tlv_put_uint(&nested_writer, 3U, 1U) == ESP_OK);
    assert(device_link_tlv_writer_finish(&nested_writer, &nested_len) ==
           ESP_OK);
    device_link_tlv_writer_init(&writer, request, sizeof(request));
    assert(device_link_tlv_put_bytes(&writer, 1U, nested, nested_len) ==
           ESP_OK);
    assert(device_link_tlv_put_fixed64(&writer, 2U,
                                       UINT64_C(0x0102030405060708)) == ESP_OK);
    assert(device_link_tlv_put_bool(&writer, 3U, true) == ESP_OK);
    assert(device_link_tlv_writer_finish(&writer, &request_len) == ESP_OK);
    uint8_t response[128];
    size_t response_len = 0U;
    const device_link_request_context_t context =
    {
        .header =
        {
            .domain_id = DEVICE_LINK_DOMAIN_WIFI,
            .domain_major = 1U,
            .method_id = 4U,
            .call_id = 1U,
            .boot_id = 1U,
        },
        .security_authenticated = true,
        .authorized = true,
    };

    assert(device_link_wifi_adapter_get_descriptor(&descriptor) == ESP_OK);
    assert(descriptor->methods[3].handler(&context, request, request_len,
                                          response, sizeof(response),
                                          &response_len,
                                          descriptor->methods[3].handler_arg) ==
           DEVICE_LINK_STATUS_OK);
    assert(response_len != 0U);
    assert(!contains_byte(response, response_len, (uint8_t)'p'));
}

static void test_unauthorized_calls_are_rejected(void)
{
    const device_link_domain_descriptor_t *descriptor = NULL;
    const device_link_request_context_t context =
    {
        .header =
        {
            .domain_id = DEVICE_LINK_DOMAIN_WIFI,
            .domain_major = 1U,
            .method_id = 1U,
            .call_id = 1U,
            .boot_id = 1U,
        },
        .security_authenticated = true,
        .authorized = false,
    };
    uint8_t response[128];
    size_t response_len = 0U;

    assert(device_link_wifi_adapter_get_descriptor(&descriptor) == ESP_OK);
    assert(descriptor->methods[0].handler(&context, NULL, 0U, response,
                                          sizeof(response), &response_len,
                                          descriptor->methods[0].handler_arg) ==
           DEVICE_LINK_STATUS_PERMISSION_DENIED);
}

static void test_bridge_forwards_terminal_status(void)
{
    connectivity_manager_status_snapshot_t status;

    memset(&status, 0, sizeof(status));
    status.generation = 1U;
    status.operation_id = 7U;
    status.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    status.last_error = ESP_OK;
    status.profile_revision = CONNECTIVITY_MANAGER_PROFILE_REVISION_INITIAL;
    status.auto_connect = true;
    status.operation_complete = true;

    ble_link_service_fake_reset();
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    /* Idempotent. */
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    event_bus_fake_publish(EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_update_count() == 1U);
    assert(ble_link_service_fake_last_owner() == 7U);
    assert(ble_link_service_fake_last_state() ==
           DEVICE_LINK_OPERATION_SUCCEEDED);
    assert(ble_link_service_fake_last_status() == DEVICE_LINK_STATUS_OK);
    /* WifiStatus payload attached for a result-declaring method. */
    assert(ble_link_service_fake_last_result_len() != 0U);

    /* A failed terminal maps the classified failure and drops the payload. */
    status.operation_id = 8U;
    status.state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
    status.failure = CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION;
    status.last_error = ESP_ERR_INVALID_STATE;
    status.operation_complete = true;
    event_bus_fake_publish(EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_update_count() == 2U);
    assert(ble_link_service_fake_last_owner() == 8U);
    assert(ble_link_service_fake_last_state() == DEVICE_LINK_OPERATION_FAILED);
    assert(ble_link_service_fake_last_status() ==
           DEVICE_LINK_STATUS_PERMISSION_DENIED);
    assert(ble_link_service_fake_last_result_len() == 0U);

    /* Non-terminal snapshots are ignored. */
    status.operation_complete = false;
    event_bus_fake_publish(EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_update_count() == 2U);

    device_link_wifi_adapter_bridge_stop();
    /* Stop is idempotent and unsubscribes: no further updates. */
    device_link_wifi_adapter_bridge_stop();
    status.operation_complete = true;
    event_bus_fake_publish(EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_update_count() == 2U);
}

static void test_bridge_forwards_terminal_scan(void)
{
    connectivity_manager_scan_snapshot_t scan;

    memset(&scan, 0, sizeof(scan));
    scan.generation = 1U;
    scan.operation_id = 11U;
    scan.running = false;
    scan.last_error = ESP_OK;

    ble_link_service_fake_reset();
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    event_bus_fake_publish(EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                           &scan, sizeof(scan));
    assert(ble_link_service_fake_update_count() == 1U);
    assert(ble_link_service_fake_last_owner() == 11U);
    assert(ble_link_service_fake_last_state() ==
           DEVICE_LINK_OPERATION_SUCCEEDED);
    assert(ble_link_service_fake_last_status() == DEVICE_LINK_STATUS_OK);
    /* start_scan declares core.v2.Empty: no result payload. */
    assert(ble_link_service_fake_last_result_len() == 0U);

    scan.operation_id = 12U;
    scan.last_error = ESP_ERR_TIMEOUT;
    event_bus_fake_publish(EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                           &scan, sizeof(scan));
    assert(ble_link_service_fake_update_count() == 2U);
    assert(ble_link_service_fake_last_owner() == 12U);
    assert(ble_link_service_fake_last_state() == DEVICE_LINK_OPERATION_FAILED);
    assert(ble_link_service_fake_last_status() ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    assert(ble_link_service_fake_last_result_len() == 0U);

    /* A running scan snapshot is not terminal. */
    scan.running = true;
    event_bus_fake_publish(EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                           &scan, sizeof(scan));
    assert(ble_link_service_fake_update_count() == 2U);
    device_link_wifi_adapter_bridge_stop();
}

static void test_admit_uses_table_operation_id(void)
{
    const device_link_domain_descriptor_t *descriptor = NULL;
    uint8_t request[64];
    uint8_t response[128];
    size_t request_len = 0U;
    size_t response_len = 0U;
    device_link_tlv_writer_t writer;

    device_link_tlv_writer_init(&writer, request, sizeof(request));
    assert(device_link_tlv_writer_finish(&writer, &request_len) == ESP_OK);
    const device_link_request_context_t context =
    {
        .header =
        {
            .domain_id = DEVICE_LINK_DOMAIN_WIFI,
            .domain_major = 1U,
            .method_id = 2U,
            .call_id = 1U,
            .boot_id = 1U,
        },
        .security_authenticated = true,
        .authorized = true,
    };

    assert(device_link_wifi_adapter_get_descriptor(&descriptor) == ESP_OK);
    assert(descriptor->methods[1].handler(&context, request, request_len,
                                          response, sizeof(response),
                                          &response_len,
                                          descriptor->methods[1].handler_arg) ==
           DEVICE_LINK_STATUS_OK);
    /* The accepted operation encodes the table id, not the manager id. */
    device_link_tlv_reader_t reader;
    device_link_tlv_field_t field;
    bool has = false;
    uint64_t table_id = 0U;

    assert(device_link_tlv_reader_init(&reader, response, response_len) ==
           ESP_OK);
    while (device_link_tlv_reader_next(&reader, &field, &has) == ESP_OK &&
            has)
    {
        if (field.id == 1U)
        {
            table_id = field.value.unsigned_value;
        }
    }
    /* The accepted operation encodes the table operation id. */
    assert(table_id != 0U);
}

static void _make_empty_request(uint8_t *request, size_t *request_len)
{
    device_link_tlv_writer_t writer;

    device_link_tlv_writer_init(&writer, request, 64U);
    assert(device_link_tlv_writer_finish(&writer, request_len) == ESP_OK);
}

static void test_admission_errors_follow_allowed_statuses(void)
{
    uint8_t request[64];
    size_t request_len = 0U;

    _make_empty_request(request, &request_len);
    ble_link_service_fake_reset();
    /* Manager lifecycle unavailable (cold-start/shutdown) is UNAVAILABLE
     * for every asynchronous method, never CONFLICT. */
    connectivity_manager_fake_set_request_result(ESP_ERR_INVALID_STATE);
    assert(_call_method(WIFI_METHOD_START_SCAN, request, request_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    assert(_call_method(WIFI_METHOD_DISCONNECT, request, request_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    assert(_call_method(WIFI_METHOD_RECONNECT_SAVED, request, request_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    assert(_call_method(WIFI_METHOD_FORGET_SAVED, request, request_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    /* set_auto_connect rejects an empty request body before admission. */
    uint8_t auto_connect_request[8];
    device_link_tlv_writer_t auto_writer;

    device_link_tlv_writer_init(&auto_writer, auto_connect_request,
                                sizeof(auto_connect_request));
    assert(device_link_tlv_put_bool(&auto_writer, 1U, true) == ESP_OK);
    size_t auto_connect_len = 0U;

    assert(device_link_tlv_writer_finish(&auto_writer,
                                         &auto_connect_len) == ESP_OK);
    assert(_call_method(WIFI_METHOD_SET_AUTO_CONNECT,
                        auto_connect_request, auto_connect_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);

    /* Queue full maps to RESOURCE_EXHAUSTED only for start_scan and
     * set_credentials (their allowed sets); the other methods express it
     * as UNAVAILABLE. */
    connectivity_manager_fake_set_request_result(ESP_ERR_NO_MEM);
    assert(_call_method(WIFI_METHOD_START_SCAN, request, request_len) ==
           DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED);
    assert(_call_method(WIFI_METHOD_DISCONNECT, request, request_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    assert(_call_method(WIFI_METHOD_RECONNECT_SAVED, request, request_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    assert(_call_method(WIFI_METHOD_FORGET_SAVED, request, request_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    assert(_call_method(WIFI_METHOD_SET_AUTO_CONNECT,
                        auto_connect_request, auto_connect_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    connectivity_manager_fake_set_request_result(ESP_OK);
}

static void test_table_admission_failure_maps_per_method(void)
{
    uint8_t request[64];
    size_t request_len = 0U;

    _make_empty_request(request, &request_len);
    ble_link_service_fake_reset();
    ble_link_service_fake_set_start_result(ESP_ERR_NO_MEM);
    assert(_call_method(WIFI_METHOD_START_SCAN, request, request_len) ==
           DEVICE_LINK_STATUS_RESOURCE_EXHAUSTED);
    assert(_call_method(WIFI_METHOD_DISCONNECT, request, request_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    assert(_call_method(WIFI_METHOD_RECONNECT_SAVED, request, request_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    assert(_call_method(WIFI_METHOD_FORGET_SAVED, request, request_len) ==
           DEVICE_LINK_STATUS_UNAVAILABLE);
    ble_link_service_fake_set_start_result(ESP_OK);
}

static void test_busy_admission_for_allowed_methods(void)
{
    uint8_t request[64];
    size_t request_len = 0U;

    _make_empty_request(request, &request_len);
    ble_link_service_fake_reset();
    ble_link_service_fake_set_in_flight(true);
    /* start_scan, disconnect and reconnect_saved freeze BUSY in their
     * allowed_statuses and must reject synchronously while another Wi-Fi
     * operation is live. */
    assert(_call_method(WIFI_METHOD_START_SCAN, request, request_len) ==
           DEVICE_LINK_STATUS_BUSY);
    assert(_call_method(WIFI_METHOD_DISCONNECT, request, request_len) ==
           DEVICE_LINK_STATUS_BUSY);
    assert(_call_method(WIFI_METHOD_RECONNECT_SAVED, request, request_len) ==
           DEVICE_LINK_STATUS_BUSY);
    /* forget_saved has no BUSY in its allowed set and stays deferred by
     * the manager owner. */
    assert(_call_method(WIFI_METHOD_FORGET_SAVED, request, request_len) ==
           DEVICE_LINK_STATUS_OK);
    ble_link_service_fake_set_in_flight(false);
}

int main(void)
{
    test_descriptor_is_static_and_complete();
    test_credentials_are_delegated_without_echo();
    test_unauthorized_calls_are_rejected();
    test_scan_results_are_paged();
    test_bridge_forwards_terminal_status();
    test_bridge_forwards_terminal_scan();
    test_admit_uses_table_operation_id();
    test_admission_errors_follow_allowed_statuses();
    test_table_admission_failure_maps_per_method();
    test_busy_admission_for_allowed_methods();
    return 0;
}
