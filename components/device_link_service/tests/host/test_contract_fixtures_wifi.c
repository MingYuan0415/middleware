#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ble_link_service.h"
#include "connectivity_manager.h"
#include "device_link_protocol.h"
#include "device_link_tlv.h"
#include "device_link_wifi_adapter.h"

/* Generated from contracts/provisioning/fixtures/domains/wifi/v1 (see
 * contract_fixtures_generate.py --kind wifi). */
#include "contract_fixtures_wifi.inc"

#define WIFI_METHOD_GET_STATUS 1U
#define WIFI_METHOD_START_SCAN 2U
#define WIFI_METHOD_GET_SCAN_RESULTS 3U
#define WIFI_METHOD_SET_CREDENTIALS 4U
#define WIFI_METHOD_DISCONNECT 5U
#define WIFI_METHOD_RECONNECT_SAVED 6U
#define WIFI_METHOD_FORGET_SAVED 7U
#define WIFI_METHOD_SET_AUTO_CONNECT 8U

/* invalid.json case kinds (mirrors the generator). */
#define WIFI_INVALID_NON_MINIMAL_AUTO_CONNECT 1
#define WIFI_INVALID_OPEN_WITH_PASSWORD 2
#define WIFI_INVALID_PERSONAL_EMPTY_PASSWORD 3
#define WIFI_INVALID_UNSUPPORTED_SECURITY 4
#define WIFI_INVALID_SSID_NUL 5
#define WIFI_INVALID_PERSONAL_SHORT 6
#define WIFI_INVALID_PERSONAL_NON_HEX_PSK 7

/* Test-only fake hooks (fakes/connectivity_manager.c, fakes/event_bus.c,
 * fakes/ble_link_service_fake.c). */
extern void ble_link_service_fake_reset(void);
extern void connectivity_manager_fake_set_request_result(esp_err_t result);
extern void event_bus_fake_publish(event_bus_msg_id_t msg_id,
                                   uint32_t sub_type, const void *payload,
                                   size_t payload_size);
extern unsigned ble_link_service_fake_update_count(void);
extern uint64_t ble_link_service_fake_last_owner(void);
extern device_link_operation_state_t ble_link_service_fake_last_state(void);
extern device_link_status_t ble_link_service_fake_last_status(void);
extern size_t ble_link_service_fake_last_result_len(void);

static device_link_status_t _call_method(
    uint8_t method_id, const uint8_t *request, size_t request_len)
{
    const device_link_domain_descriptor_t *descriptor = NULL;
    uint8_t response[256];
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

static void _test_invalid_wifi_credentials(void)
{
    /* wifi.v1 invalid.json: the adapter is the first line of defense for
     * the cross-field credential rules. OPEN requires an empty password,
     * PERSONAL requires an 8..63-byte passphrase or 64 ASCII hex PSK,
     * SSIDs and secrets reject NUL, UNSUPPORTED is rejected, and a
     * non-minimal varint is rejected by the schema decoder. */
    for (size_t i = 0U; i < s_wifi_invalid_count; ++i)
    {
        if (s_wifi_invalid_kind[i] == WIFI_INVALID_NON_MINIMAL_AUTO_CONNECT)
        {
            assert(_call_method(WIFI_METHOD_SET_AUTO_CONNECT,
                                s_wifi_invalid[i],
                                s_wifi_invalid_len[i]) ==
                   DEVICE_LINK_STATUS_INVALID_ARGUMENT);
            continue;
        }
        /* Wrap the frozen WifiCredentials wire bytes as field 1 of a
         * SetCredentialsRequest (sync_id and auto_connect are required
         * fields of the schema). */
        uint8_t request[160];
        device_link_tlv_writer_t writer;
        size_t request_len = 0U;

        device_link_tlv_writer_init(&writer, request, sizeof(request));
        assert(device_link_tlv_put_bytes(
                   &writer, 1U, s_wifi_invalid[i],
                   s_wifi_invalid_len[i]) == ESP_OK);
        assert(device_link_tlv_put_fixed64(&writer, 2U, 1U) == ESP_OK);
        assert(device_link_tlv_put_bool(&writer, 3U, false) == ESP_OK);
        assert(device_link_tlv_writer_finish(&writer, &request_len) ==
               ESP_OK);
        assert(_call_method(WIFI_METHOD_SET_CREDENTIALS,
                            request, request_len) ==
               DEVICE_LINK_STATUS_INVALID_ARGUMENT);
    }
    printf("wifi invalid: %zu cross-field cases rejected\n",
           s_wifi_invalid_count);
}

static void _test_wifi_operation_result_payloads(void)
{
    /* wifi.v1 operation_results.json: the WifiStatus result payload is
     * frozen per case; the adapter encoder must reproduce it byte for
     * byte. Cases with an empty payload freeze the Empty / non-success
     * no-payload rule (their OperationStatus bodies are validated
     * against the fixture in the ble_runtime suite). */
    for (size_t i = 0U; i < s_wifi_result_count; ++i)
    {
        if (s_wifi_result_len[i] == 0U)
        {
            continue;
        }
        connectivity_manager_status_snapshot_t snapshot;
        uint8_t encoded[256];
        size_t encoded_len = 0U;

        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.generation = s_wifi_result_generation[i];
        snapshot.state = (connectivity_manager_state_t)
                         s_wifi_result_wifi_state[i];
        snapshot.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        snapshot.ipv4_address = s_wifi_result_has_ipv4[i] ? 1U : 0U;
        snapshot.saved_profile = s_wifi_result_saved_profile[i];
        snapshot.profile_persisted = s_wifi_result_persisted[i];
        snapshot.auto_connect = s_wifi_result_auto_connect[i];
        snapshot.manual_hold = s_wifi_result_manual_hold[i];
        snapshot.profile_revision = s_wifi_result_revision[i];
        snapshot.applied_client_sync_id =
            (connectivity_manager_client_sync_id_t)s_wifi_result_sync_id[i];
        memcpy(snapshot.ssid, s_wifi_result_ssid[i],
               s_wifi_result_ssid_len[i]);
        assert(device_link_wifi_adapter_encode_operation_result(
                   &snapshot, encoded, sizeof(encoded), &encoded_len) ==
               ESP_OK);
        assert(encoded_len == s_wifi_result_len[i]);
        assert(memcmp(encoded, s_wifi_result[i], encoded_len) == 0);
    }
    printf("wifi operation_results: %zu payload goldens validated\n",
           s_wifi_result_count);
}

static void _set_terminal_failure(
    int error, connectivity_manager_status_snapshot_t *snapshot)
{
    snapshot->last_error = ESP_FAIL;
    switch ((device_link_status_t)error)
    {
    case DEVICE_LINK_STATUS_PERMISSION_DENIED:
        snapshot->failure = CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION;
        break;
    case DEVICE_LINK_STATUS_NOT_FOUND:
        snapshot->failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        snapshot->last_error = ESP_ERR_NOT_FOUND;
        break;
    case DEVICE_LINK_STATUS_CONFLICT:
        snapshot->failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        snapshot->operation_conflict = true;
        break;
    case DEVICE_LINK_STATUS_UNAVAILABLE:
        snapshot->failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        snapshot->last_error = ESP_ERR_TIMEOUT;
        break;
    case DEVICE_LINK_STATUS_STORAGE:
        snapshot->failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
        break;
    case DEVICE_LINK_STATUS_INTERNAL:
        snapshot->failure = CONNECTIVITY_MANAGER_FAILURE_INTERNAL;
        break;
    default:
        assert(false);
    }
}

static void _test_wifi_terminal_fixtures(void)
{
    unsigned expected_updates = 0U;

    ble_link_service_fake_reset();
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    for (size_t i = 0U; i < s_wifi_result_count; ++i)
    {
        const device_link_operation_state_t expected_state =
            (device_link_operation_state_t)s_wifi_result_state[i];
        const device_link_status_t expected_error =
            (device_link_status_t)s_wifi_result_error[i];

        if (expected_state == DEVICE_LINK_OPERATION_SUCCEEDED)
        {
            assert(expected_error == DEVICE_LINK_STATUS_OK);
            continue;
        }
        assert(s_wifi_result_len[i] == 0U);
        assert(expected_state == DEVICE_LINK_OPERATION_FAILED ||
               expected_state == DEVICE_LINK_OPERATION_CANCELED);
        assert((expected_state == DEVICE_LINK_OPERATION_CANCELED) ==
               (expected_error == DEVICE_LINK_STATUS_OK));

        if (s_wifi_result_method[i] == WIFI_METHOD_START_SCAN)
        {
            connectivity_manager_scan_snapshot_t snapshot;

            memset(&snapshot, 0, sizeof(snapshot));
            snapshot.operation_id = (uint64_t)i + 1U;
            snapshot.operation_canceled =
                expected_state == DEVICE_LINK_OPERATION_CANCELED;
            if (!snapshot.operation_canceled)
            {
                snapshot.last_error =
                    expected_error == DEVICE_LINK_STATUS_UNAVAILABLE ?
                    ESP_ERR_TIMEOUT : ESP_FAIL;
            }
            event_bus_fake_publish(
                EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                &snapshot, sizeof(snapshot));
        }
        else
        {
            connectivity_manager_status_snapshot_t snapshot;

            memset(&snapshot, 0, sizeof(snapshot));
            snapshot.operation_id = (uint64_t)i + 1U;
            snapshot.operation_complete = true;
            snapshot.operation_canceled =
                expected_state == DEVICE_LINK_OPERATION_CANCELED;
            if (!snapshot.operation_canceled)
            {
                _set_terminal_failure(expected_error, &snapshot);
            }
            event_bus_fake_publish(
                EVENT_BUS_ID(CONNECTIVITY_MANAGER_MSG),
                CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                &snapshot, sizeof(snapshot));
        }
        expected_updates++;
        assert(ble_link_service_fake_update_count() == expected_updates);
        assert(ble_link_service_fake_last_owner() == (uint64_t)i + 1U);
        assert(ble_link_service_fake_last_state() == expected_state);
        assert(ble_link_service_fake_last_status() == expected_error);
        assert(ble_link_service_fake_last_result_len() == 0U);
    }
    device_link_wifi_adapter_bridge_stop();
    assert(expected_updates != 0U);
    printf("wifi terminal fixtures: %u failure/cancel pairs validated\n",
           expected_updates);
}

static void _test_wifi_golden_request(void)
{
    /* wifi.v1 golden.json: the canonical SetCredentialsRequest (PERSONAL,
     * sync id 1, auto_connect) must be admitted by the adapter end to
     * end. */
    ble_link_service_fake_reset();
    connectivity_manager_fake_set_request_result(ESP_OK);
    assert(_call_method(WIFI_METHOD_SET_CREDENTIALS,
                        s_wifi_golden, s_wifi_golden_len) ==
           DEVICE_LINK_STATUS_OK);
    puts("wifi golden: canonical SetCredentialsRequest admitted");
}

int main(void)
{
    _test_invalid_wifi_credentials();
    _test_wifi_operation_result_payloads();
    _test_wifi_terminal_fixtures();
    _test_wifi_golden_request();
    puts("contract_fixtures_wifi: all wifi fixture vectors passed");
    return 0;
}
