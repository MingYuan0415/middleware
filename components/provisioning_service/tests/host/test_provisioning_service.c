#include "host_freertos.h"
#include "host_provisioning_service.h"
#include "microtech/provisioning/v1/provisioning.pb-c.h"
#include "provisioning_service.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef Microtech__Provisioning__V1__ProvisioningRequest pb_request_t;
typedef Microtech__Provisioning__V1__ProvisioningResponse pb_response_t;

static const provisioning_service_config_t s_config =
{
    .task_priority = 4U,
    .window_ms = 200U,
    .success_grace_ms = 20U,
    .finish_close_delay_ms = 20U,
};

static void _sleep_ms(uint32_t milliseconds)
{
    const struct timespec delay =
    {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

static bool _wait_state(provisioning_service_state_t state,
                        uint32_t timeout_ms)
{
    for (uint32_t elapsed = 0U; elapsed <= timeout_ms; ++elapsed)
    {
        provisioning_service_status_t status;
        if (provisioning_service_get_status(&status) == ESP_OK &&
                status.state == state)
        {
            return true;
        }
        _sleep_ms(1U);
    }
    return false;
}

static pb_response_t *_request(const pb_request_t *request,
                               uint8_t **response_wire)
{
    const size_t request_length =
        microtech__provisioning__v1__provisioning_request__get_packed_size(
            request);
    uint8_t request_wire[256];
    assert(request_length <= sizeof(request_wire));
    memset(request_wire, 0xA5, sizeof(request_wire));
    assert(microtech__provisioning__v1__provisioning_request__pack(
               request, request_wire) == request_length);
    ssize_t response_length = 0;
    *response_wire = NULL;
    assert(host_provisioning_request(
               request_wire, request_length,
               response_wire, &response_length) == ESP_OK);
    for (size_t index = 0U; index < request_length; ++index)
    {
        assert(request_wire[index] == 0U);
    }
    assert(response_length > 0);
    pb_response_t *response =
        microtech__provisioning__v1__provisioning_response__unpack(
            NULL, (size_t)response_length, *response_wire);
    assert(response != NULL);
    return response;
}

static void _free_response(pb_response_t *response, uint8_t *wire)
{
    microtech__provisioning__v1__provisioning_response__free_unpacked(
        response, NULL);
    free(wire);
}

static uint64_t _set_credentials(void)
{
    uint8_t ssid[] = "Host-Network";
    uint8_t password[] = "password1";
    Microtech__Provisioning__V1__WifiCredentials credentials =
        MICROTECH__PROVISIONING__V1__WIFI_CREDENTIALS__INIT;
    credentials.ssid.data = ssid;
    credentials.ssid.len = sizeof(ssid) - 1U;
    credentials.password.data = password;
    credentials.password.len = sizeof(password) - 1U;
    credentials.security =
        MICROTECH__PROVISIONING__V1__WIFI_SECURITY__WIFI_SECURITY_PERSONAL;
    Microtech__Provisioning__V1__SetCredentialsRequest body =
        MICROTECH__PROVISIONING__V1__SET_CREDENTIALS_REQUEST__INIT;
    body.credentials = &credentials;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 1U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_CREDENTIALS;
    request.set_credentials = &body;
    uint8_t *wire = NULL;
    pb_response_t *response = _request(&request, &wire);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->operation_accepted->operation->operation_id != 0U);
    const uint64_t operation_id =
        response->operation_accepted->operation->operation_id;
    _free_response(response, wire);
    return operation_id;
}

static uint64_t _start_scan(void)
{
    Microtech__Provisioning__V1__StartScanRequest body =
        MICROTECH__PROVISIONING__V1__START_SCAN_REQUEST__INIT;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 5U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_START_SCAN;
    request.start_scan = &body;
    uint8_t *wire = NULL;
    pb_response_t *response = _request(&request, &wire);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->operation_accepted->operation->operation_id != 0U);
    const uint64_t operation_id =
        response->operation_accepted->operation->operation_id;
    _free_response(response, wire);
    return operation_id;
}

static void _finish_session(void)
{
    Microtech__Provisioning__V1__FinishSessionRequest body =
        MICROTECH__PROVISIONING__V1__FINISH_SESSION_REQUEST__INIT;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 2U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_FINISH_SESSION;
    request.finish_session = &body;
    uint8_t *wire = NULL;
    pb_response_t *response = _request(&request, &wire);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->body_case ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY__NOT_SET);
    _free_response(response, wire);
}

static pb_response_t *_get_snapshot(uint8_t **wire)
{
    Microtech__Provisioning__V1__GetSnapshotRequest body =
        MICROTECH__PROVISIONING__V1__GET_SNAPSHOT_REQUEST__INIT;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 3U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_SNAPSHOT;
    request.get_snapshot = &body;
    return _request(&request, wire);
}

static pb_response_t *_get_scan_results(uint8_t **wire)
{
    Microtech__Provisioning__V1__GetScanResultsRequest body =
        MICROTECH__PROVISIONING__V1__GET_SCAN_RESULTS_REQUEST__INIT;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 4U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_SCAN_RESULTS;
    request.get_scan_results = &body;
    return _request(&request, wire);
}

static void _publish_terminal(uint64_t operation_id, bool success)
{
    connectivity_manager_status_snapshot_t status;
    memset(&status, 0, sizeof(status));
    status.generation = operation_id + 100U;
    status.operation_id = operation_id;
    status.operation_complete = true;
    status.available = true;
    status.radio_available = true;
    status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    if (success)
    {
        status.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
        status.ipv4_address = 1U;
        status.saved_profile = true;
        status.profile_persisted = true;
        status.auto_connect = true;
        memcpy(status.ssid, "Host-Network", sizeof("Host-Network"));
    }
    else
    {
        status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        status.last_error = ESP_ERR_NOT_FINISHED;
    }
    host_provisioning_publish_status(&status);
}

static void _test_window_transport_and_suspend(void)
{
    provisioning_service_status_t status;
    assert(provisioning_service_get_status(&status) == ESP_OK);
    assert(status.available);
    assert(!status.active);
    assert(strcmp(status.device_name, "MT-A1B2C3") == 0);

    assert(provisioning_service_suspend(0U) == ESP_OK);
    assert(provisioning_service_suspend(0U) == ESP_OK);
    assert(provisioning_service_open_window() == ESP_ERR_INVALID_STATE);
    assert(provisioning_service_resume(0U) == ESP_OK);
    assert(provisioning_service_resume(0U) == ESP_OK);

    assert(provisioning_service_open_window() == ESP_OK);
    assert(provisioning_service_is_active());
    assert(host_provisioning_wait_transport(true, 500U));
    assert(host_provisioning_transport_shape_valid());
    assert(strcmp(host_provisioning_device_name(), "MT-A1B2C3") == 0);
    assert(strcmp(host_provisioning_protocol_version(),
                  "{\"prov\":{\"ver\":\"v1.0\",\"sec_ver\":2,"
                  "\"sec_patch_ver\":1,\"cap\":[\"mt-prov-v1\"]}}") == 0);
    assert(provisioning_service_open_window() == ESP_OK);
    assert(host_provisioning_transport_start_count() == 1U);

    host_provisioning_emit_ble(true);
    assert(_wait_state(PROVISIONING_SERVICE_STATE_CONNECTED, 100U));
    host_provisioning_emit_ble(false);
    assert(_wait_state(PROVISIONING_SERVICE_STATE_ADVERTISING, 100U));

    char first_qr[PROVISIONING_SERVICE_QR_MAX_BYTES];
    size_t first_length = 0U;
    assert(provisioning_service_copy_qr(
               first_qr, sizeof(first_qr), &first_length) == ESP_OK);
    assert(first_length > 0U);
    assert(strstr(first_qr, "\"device_id\":\"A1B2C3\"") != NULL);
    assert(strstr(first_qr, "\"name\":\"MT-A1B2C3\"") != NULL);
    assert(provisioning_service_close_window() == ESP_OK);
    assert(host_provisioning_wait_transport(false, 500U));
    assert(host_provisioning_application_secrets_zeroized());
    assert(!provisioning_service_is_active());
    assert(provisioning_service_copy_qr(
               first_qr, sizeof(first_qr), &first_length) == ESP_ERR_NOT_FOUND);

    assert(provisioning_service_open_window() == ESP_OK);
    assert(host_provisioning_wait_transport(true, 500U));
    char second_qr[PROVISIONING_SERVICE_QR_MAX_BYTES];
    size_t second_length = 0U;
    assert(provisioning_service_copy_qr(
               second_qr, sizeof(second_qr), &second_length) == ESP_OK);
    assert(second_length == first_length);
    assert(memcmp(first_qr, second_qr, second_length) != 0);
    memset(first_qr, 0, sizeof(first_qr));
    memset(second_qr, 0, sizeof(second_qr));

    const unsigned starts = host_provisioning_transport_start_count();
    host_provisioning_block_next_stop();
    assert(provisioning_service_close_window() == ESP_OK);
    assert(host_provisioning_wait_stop_blocked(500U));
    assert(provisioning_service_open_window() == ESP_OK);
    host_provisioning_release_stop();
    assert(host_provisioning_wait_transport(true, 500U));
    assert(host_provisioning_transport_start_count() == starts + 1U);
}

static void _test_finish_close_and_timeout(void)
{
    uint64_t operation_id = _set_credentials();
    _finish_session();
    assert(host_provisioning_wait_transport(false, 500U));
    assert(host_provisioning_cancel_count() == 0U);
    provisioning_service_status_t status;
    assert(provisioning_service_get_status(&status) == ESP_OK);
    assert(status.wifi_operation_active);
    assert(status.wifi_operation_id == operation_id);
    _publish_terminal(operation_id, false);

    assert(provisioning_service_open_window() == ESP_OK);
    assert(host_provisioning_wait_transport(true, 500U));
    operation_id = _set_credentials();
    assert(provisioning_service_close_window() == ESP_OK);
    assert(host_provisioning_wait_transport(false, 500U));
    assert(host_provisioning_canceled_operation() == operation_id);
    assert(host_provisioning_cancel_count() == 1U);
    _publish_terminal(operation_id, false);

    assert(provisioning_service_open_window() == ESP_OK);
    assert(host_provisioning_wait_transport(true, 500U));
    operation_id = _set_credentials();
    host_freertos_advance_ticks(s_config.window_ms + 1U);
    assert(host_provisioning_wait_transport(false, 500U));
    assert(host_provisioning_canceled_operation() == operation_id);
    assert(host_provisioning_cancel_count() == 2U);
    _publish_terminal(operation_id, false);
}

static void _test_success_grace(void)
{
    assert(provisioning_service_open_window() == ESP_OK);
    assert(host_provisioning_wait_transport(true, 500U));
    const uint64_t operation_id = _set_credentials();
    _publish_terminal(operation_id, true);
    assert(host_provisioning_wait_transport(false, 500U));
    assert(host_provisioning_cancel_count() == 2U);
    provisioning_service_status_t status;
    assert(provisioning_service_get_status(&status) == ESP_OK);
    assert(!status.wifi_operation_active);
}

static void _test_transport_stop_fault(void)
{
    host_provisioning_reset();
    assert(provisioning_service_init(&s_config) == ESP_OK);
    assert(provisioning_service_open_window() == ESP_OK);
    assert(host_provisioning_wait_transport(true, 500U));
    host_provisioning_fail_next_stop(ESP_FAIL);
    assert(provisioning_service_close_window() == ESP_OK);
    assert(_wait_state(PROVISIONING_SERVICE_STATE_ERROR, 500U));

    provisioning_service_status_t status;
    assert(provisioning_service_get_status(&status) == ESP_OK);
    assert(status.active);
    assert(status.last_error == ESP_FAIL);
    assert(provisioning_service_is_active());
    assert(host_provisioning_transport_stop_count() == 1U);
    assert(host_provisioning_application_secrets_zeroized());
    assert(provisioning_service_open_window() == ESP_FAIL);
    assert(provisioning_service_close_window() == ESP_FAIL);
    assert(provisioning_service_suspend(100U) == ESP_FAIL);
    assert(provisioning_service_resume(100U) == ESP_FAIL);
    assert(provisioning_service_deinit(1000U) == ESP_FAIL);
    assert(host_provisioning_transport_stop_count() == 1U);
}

static void _test_init_snapshot_refresh(void)
{
    host_provisioning_reset();
    connectivity_manager_status_snapshot_t status;
    memset(&status, 0, sizeof(status));
    status.generation = 9U;
    status.available = true;
    status.radio_available = true;
    status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    status.saved_profile = true;
    status.profile_persisted = true;
    status.auto_connect = true;
    memcpy(status.ssid, "Refreshed", sizeof("Refreshed"));
    connectivity_manager_scan_snapshot_t scan;
    memset(&scan, 0, sizeof(scan));
    scan.generation = 44U;
    scan.last_error = ESP_OK;
    scan.record_count = 1U;
    memcpy(scan.records[0].ssid, "Refresh AP", sizeof("Refresh AP"));
    scan.records[0].security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL;
    host_provisioning_stage_init_refresh(&status, &scan);

    assert(provisioning_service_init(&s_config) == ESP_OK);
    assert(provisioning_service_open_window() == ESP_OK);
    assert(host_provisioning_wait_transport(true, 500U));
    uint8_t *wire = NULL;
    pb_response_t *response = _get_snapshot(&wire);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->snapshot->generation == 9U);
    assert(response->snapshot->ssid.len == sizeof("Refreshed") - 1U);
    _free_response(response, wire);

    response = _get_scan_results(&wire);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->scan_results->generation == 44U);
    assert(response->scan_results->n_networks == 1U);
    _free_response(response, wire);
    assert(provisioning_service_close_window() == ESP_OK);
    assert(host_provisioning_wait_transport(false, 500U));
    assert(provisioning_service_deinit(1000U) == ESP_OK);
}

static void _test_async_publish_boundary(void)
{
    host_provisioning_reset();
    assert(provisioning_service_init(&s_config) == ESP_OK);
    assert(provisioning_service_open_window() == ESP_OK);
    assert(host_provisioning_wait_transport(true, 500U));
    host_freertos_pause_queue_receive(true);
    assert(host_freertos_wait_queue_receive_paused(500U));
    host_provisioning_reset_publish_observer();

    const uint64_t scan_operation = _start_scan();
    connectivity_manager_scan_snapshot_t scan;
    memset(&scan, 0, sizeof(scan));
    scan.generation = 100U;
    scan.operation_id = scan_operation;
    scan.running = true;
    host_provisioning_publish_scan(&scan);
    scan.generation = 101U;
    scan.running = false;
    scan.last_error = ESP_OK;
    scan.record_count = 1U;
    memcpy(scan.records[0].ssid, "Async AP", sizeof("Async AP"));
    scan.records[0].security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL;
    host_provisioning_publish_scan(&scan);

    uint8_t *wire = NULL;
    pb_response_t *response = _get_scan_results(&wire);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->scan_results->generation == 101U);
    assert(response->scan_results->n_networks == 1U);
    _free_response(response, wire);

    const uint64_t connect_operation = _set_credentials();
    _publish_terminal(connect_operation, false);
    provisioning_service_status_t status;
    assert(provisioning_service_get_status(&status) == ESP_OK);
    assert(!status.wifi_operation_active);
    const uint64_t expected_generation = status.generation;
    assert(host_provisioning_publish_count() == 0U);
    assert(host_provisioning_max_publish_depth() == 1U);

    host_freertos_pause_queue_receive(false);
    assert(host_provisioning_wait_publish_count(1U, 500U));
    assert(host_provisioning_publish_count() == 1U);
    assert(host_provisioning_last_publish_generation() == expected_generation);
    assert(host_provisioning_max_publish_depth() == 1U);
    assert(provisioning_service_close_window() == ESP_OK);
    assert(host_provisioning_wait_transport(false, 500U));
    assert(provisioning_service_deinit(1000U) == ESP_OK);
}

static void _test_publish_recovery_and_deinit(void)
{
    host_provisioning_reset();
    assert(provisioning_service_init(&s_config) == ESP_OK);
    assert(host_provisioning_wait_publish_count(1U, 500U));

    host_provisioning_reset_publish_observer();
    host_freertos_fail_next_queue_sends(1U);
    connectivity_manager_status_snapshot_t status;
    memset(&status, 0, sizeof(status));
    status.generation = 200U;
    status.available = true;
    status.radio_available = true;
    status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    host_provisioning_publish_status(&status);
    assert(host_provisioning_wait_publish_count(1U, 500U));
    const uint64_t queue_recovery_generation =
        host_provisioning_last_publish_generation();
    assert(queue_recovery_generation != 0U);

    host_provisioning_reset_publish_observer();
    host_provisioning_fail_next_publish(ESP_FAIL);
    status.generation = 201U;
    host_provisioning_publish_status(&status);
    assert(host_provisioning_wait_publish_count(1U, 500U));
    assert(host_provisioning_failed_publish_generation() != 0U);
    assert(host_provisioning_failed_publish_generation() ==
           host_provisioning_last_publish_generation());
    assert(host_provisioning_max_publish_depth() == 1U);

    host_freertos_pause_queue_receive(true);
    assert(host_freertos_wait_queue_receive_paused(500U));
    host_provisioning_reset_publish_observer();
    status.generation = 202U;
    host_provisioning_publish_status(&status);
    assert(provisioning_service_deinit(20U) == ESP_ERR_TIMEOUT);
    host_freertos_pause_queue_receive(false);
    assert(provisioning_service_deinit(1000U) == ESP_OK);
    assert(host_freertos_wait_no_tasks(100U));
}

int main(void)
{
    host_freertos_reset_controls();
    host_provisioning_reset();
    provisioning_service_config_t invalid = s_config;
    invalid.task_priority = 0U;
    assert(provisioning_service_init(&invalid) == ESP_ERR_INVALID_ARG);
    invalid = s_config;
    invalid.task_priority = configMAX_PRIORITIES;
    assert(provisioning_service_init(&invalid) == ESP_ERR_INVALID_ARG);
    assert(provisioning_service_init(&s_config) == ESP_OK);
    assert(provisioning_service_init(&s_config) == ESP_OK);

    _test_window_transport_and_suspend();
    _test_finish_close_and_timeout();
    _test_success_grace();

    assert(provisioning_service_deinit(1000U) == ESP_OK);
    assert(provisioning_service_deinit(1000U) == ESP_OK);
    assert(host_freertos_wait_no_tasks(100U));
    assert(host_freertos_live_tasks() == 0U);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_semaphores() == 0U);

    _test_async_publish_boundary();
    assert(host_freertos_live_tasks() == 0U);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_semaphores() == 0U);
    _test_publish_recovery_and_deinit();
    assert(host_freertos_live_tasks() == 0U);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_semaphores() == 0U);
    _test_init_snapshot_refresh();
    assert(host_freertos_live_tasks() == 0U);
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_semaphores() == 0U);
    _test_transport_stop_fault();
    puts("provisioning service lifecycle tests passed");
    return 0;
}
