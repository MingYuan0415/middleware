#include "provisioning_protocol.h"

#include "microtech/provisioning/v1/provisioning.pb-c.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef Microtech__Provisioning__V1__ProvisioningRequest pb_request_t;
typedef Microtech__Provisioning__V1__ProvisioningResponse pb_response_t;

typedef enum fake_request_kind
{
    FAKE_REQUEST_NONE = 0,
    FAKE_REQUEST_SCAN,
    FAKE_REQUEST_CONNECT,
    FAKE_REQUEST_DISCONNECT,
    FAKE_REQUEST_RECONNECT,
    FAKE_REQUEST_FORGET,
    FAKE_REQUEST_AUTO_CONNECT,
} fake_request_kind_t;

static connectivity_manager_status_snapshot_t s_status;
static connectivity_manager_operation_id_t s_next_operation = 1U;
static connectivity_manager_operation_id_t s_canceled_operation;
static esp_err_t s_admission_result = ESP_OK;
static fake_request_kind_t s_last_request;
static bool s_auto_connect_value;
static uint8_t s_copied_ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES];
static uint8_t s_copied_password[CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES];
static size_t s_copied_ssid_length;
static size_t s_copied_password_length;

static esp_err_t _admit(connectivity_manager_operation_id_t *operation_id)
{
    assert(operation_id != NULL);
    if (s_admission_result != ESP_OK)
    {
        return s_admission_result;
    }
    *operation_id = s_next_operation++;
    return ESP_OK;
}

esp_err_t connectivity_manager_request_scan(
    connectivity_manager_operation_id_t *operation_id)
{
    s_last_request = FAKE_REQUEST_SCAN;
    return _admit(operation_id);
}

esp_err_t connectivity_manager_request_connect(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_operation_id_t *operation_id)
{
    assert(credentials != NULL);
    s_last_request = FAKE_REQUEST_CONNECT;
    assert(credentials->ssid_length <= sizeof(s_copied_ssid));
    assert(credentials->password_length <= sizeof(s_copied_password));
    memset(s_copied_ssid, 0, sizeof(s_copied_ssid));
    memset(s_copied_password, 0, sizeof(s_copied_password));
    memcpy(s_copied_ssid, credentials->ssid, credentials->ssid_length);
    memcpy(s_copied_password, credentials->password,
           credentials->password_length);
    s_copied_ssid_length = credentials->ssid_length;
    s_copied_password_length = credentials->password_length;
    return _admit(operation_id);
}

esp_err_t connectivity_manager_request_disconnect(
    connectivity_manager_operation_id_t *operation_id)
{
    s_last_request = FAKE_REQUEST_DISCONNECT;
    return _admit(operation_id);
}

esp_err_t connectivity_manager_request_reconnect_saved(
    connectivity_manager_operation_id_t *operation_id)
{
    s_last_request = FAKE_REQUEST_RECONNECT;
    return _admit(operation_id);
}

esp_err_t connectivity_manager_request_forget(
    connectivity_manager_operation_id_t *operation_id)
{
    s_last_request = FAKE_REQUEST_FORGET;
    return _admit(operation_id);
}

esp_err_t connectivity_manager_set_auto_connect(
    bool enabled, connectivity_manager_operation_id_t *operation_id)
{
    s_last_request = FAKE_REQUEST_AUTO_CONNECT;
    s_auto_connect_value = enabled;
    return _admit(operation_id);
}

esp_err_t connectivity_manager_cancel(
    connectivity_manager_operation_id_t operation_id)
{
    s_canceled_operation = operation_id;
    return ESP_OK;
}

esp_err_t connectivity_manager_get_status(
    connectivity_manager_status_snapshot_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *status = s_status;
    return ESP_OK;
}

static void _reset(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.available = true;
    s_status.radio_available = true;
    s_status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    s_status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    s_status.last_error = ESP_OK;
    s_next_operation = 1U;
    s_canceled_operation = 0U;
    s_admission_result = ESP_OK;
    s_last_request = FAKE_REQUEST_NONE;
    s_auto_connect_value = false;
    memset(s_copied_ssid, 0, sizeof(s_copied_ssid));
    memset(s_copied_password, 0, sizeof(s_copied_password));
    s_copied_ssid_length = 0U;
    s_copied_password_length = 0U;
}

static pb_response_t *_handle(provisioning_protocol_t *protocol,
                              const pb_request_t *request,
                              provisioning_protocol_result_t *result,
                              uint8_t **wire, size_t *wire_length)
{
    const size_t request_length =
        microtech__provisioning__v1__provisioning_request__get_packed_size(
            request);
    uint8_t request_wire[256];
    assert(request_length <= sizeof(request_wire));
    memset(request_wire, 0xA5, sizeof(request_wire));
    assert(microtech__provisioning__v1__provisioning_request__pack(
               request, request_wire) == request_length);
    assert(provisioning_protocol_handle(
               protocol, request_wire, request_length, wire, wire_length,
               result) == ESP_OK);
    for (size_t index = 0U; index < request_length; ++index)
    {
        assert(request_wire[index] == 0U);
    }
    pb_response_t *response =
        microtech__provisioning__v1__provisioning_response__unpack(
            NULL, *wire_length, *wire);
    assert(response != NULL);
    return response;
}

static pb_response_t *_handle_wire(provisioning_protocol_t *protocol,
                                   uint8_t *request_wire,
                                   size_t request_length,
                                   provisioning_protocol_result_t *result,
                                   uint8_t **wire, size_t *wire_length)
{
    assert(provisioning_protocol_handle(
               protocol, request_wire, request_length, wire, wire_length,
               result) == ESP_OK);
    for (size_t index = 0U; index < request_length; ++index)
    {
        assert(request_wire[index] == 0U);
    }
    pb_response_t *response =
        microtech__provisioning__v1__provisioning_response__unpack(
            NULL, *wire_length, *wire);
    assert(response != NULL);
    return response;
}

static void _free_response(pb_response_t *response, uint8_t *wire)
{
    microtech__provisioning__v1__provisioning_response__free_unpacked(
        response, NULL);
    free(wire);
}

static void _assert_error_response(provisioning_protocol_t *protocol,
                                   const pb_request_t *request,
                                   Microtech__Provisioning__V1__ResponseCode code)
{
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(protocol, request, &result,
                                      &wire, &wire_length);
    assert(response->request_id == request->request_id);
    assert(response->code == code);
    assert(response->body_case ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY__NOT_SET);
    const Microtech__Provisioning__V1__FailureReason expected_failure =
        code ==
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_RADIO_UNAVAILABLE ?
        MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_RADIO_UNAVAILABLE :
        MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_NONE;
    assert(response->failure == expected_failure);
    assert(!result.operation_admitted);
    assert(!result.finish_session);
    _free_response(response, wire);
}

static void _test_capabilities_and_malformed(void)
{
    provisioning_protocol_t protocol;
    assert(provisioning_protocol_init(&protocol, "A1B2C3", "1.2.3",
                                      &s_status) == ESP_OK);
    Microtech__Provisioning__V1__GetCapabilitiesRequest body =
        MICROTECH__PROVISIONING__V1__GET_CAPABILITIES_REQUEST__INIT;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 1U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_CAPABILITIES;
    request.get_capabilities = &body;
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(&protocol, &request, &result,
                                      &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->failure ==
           MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_NONE);
    assert(response->body_case ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_CAPABILITIES);
    assert(response->capabilities->protocol_version->major == 1U);
    assert(response->capabilities->protocol_version->minor == 0U);
    assert(response->capabilities->n_features == 4U);
    assert(response->capabilities->features[0] ==
           MICROTECH__PROVISIONING__V1__FEATURE__FEATURE_WIFI_SCAN);
    assert(response->capabilities->features[1] ==
           MICROTECH__PROVISIONING__V1__FEATURE__FEATURE_HIDDEN_NETWORK);
    assert(response->capabilities->features[2] ==
           MICROTECH__PROVISIONING__V1__FEATURE__FEATURE_SAVED_NETWORK_MANAGEMENT);
    assert(response->capabilities->features[3] ==
           MICROTECH__PROVISIONING__V1__FEATURE__FEATURE_AUTO_CONNECT_POLICY);
    assert(strcmp(response->capabilities->device_id, "A1B2C3") == 0);
    assert(strcmp(response->capabilities->firmware_version, "1.2.3") == 0);
    assert(response->capabilities->max_ssid_bytes == 32U);
    assert(response->capabilities->max_password_bytes == 63U);
    assert(response->capabilities->max_scan_records == 5U);
    assert(response->capabilities->minimum_event_mtu == 185U);
    assert(response->capabilities->maximum_event_frame_bytes == 160U);
    _free_response(response, wire);

    uint8_t golden[] =
    {
        0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x10, 0x01, 0x52, 0x00,
    };
    wire = NULL;
    wire_length = 0U;
    response = _handle_wire(&protocol, golden, sizeof(golden), &result,
                            &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->body_case ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_CAPABILITIES);
    _free_response(response, wire);

    uint8_t unknown_field[] =
    {
        0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x10, 0x01, 0x52, 0x00, 0xA0, 0x06, 0x07,
    };
    wire = NULL;
    wire_length = 0U;
    response = _handle_wire(&protocol, unknown_field, sizeof(unknown_field),
                            &result, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    _free_response(response, wire);

    const uint8_t invalid_cases[][3] =
    {
        {0x09, 0x01, 0x00},
        {0x52, 0x02, 0x00},
        {0x0F, 0x00, 0x00},
    };
    const size_t invalid_lengths[] = {2U, 3U, 1U};
    for (size_t case_index = 0U;
            case_index < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]);
            ++case_index)
    {
        uint8_t malformed[3];
        memcpy(malformed, invalid_cases[case_index], sizeof(malformed));
        wire = NULL;
        wire_length = 0U;
        assert(provisioning_protocol_handle(
                   &protocol, malformed, invalid_lengths[case_index],
                   &wire, &wire_length, &result) == ESP_ERR_INVALID_ARG);
        assert(wire == NULL);
        assert(wire_length == 0U);
        for (size_t index = 0U; index < invalid_lengths[case_index]; ++index)
        {
            assert(malformed[index] == 0U);
        }
    }

    uint8_t oversized[PROVISIONING_PROTOCOL_MAX_PLAINTEXT_REQUEST_BYTES + 1U];
    memset(oversized, 0xA5, sizeof(oversized));
    assert(provisioning_protocol_handle(
               &protocol, oversized, sizeof(oversized), &wire, &wire_length,
               &result) == ESP_ERR_INVALID_SIZE);
    for (size_t index = 0U; index < sizeof(oversized); ++index)
    {
        assert(oversized[index] == 0U);
    }
}

static uint64_t _set_credentials(provisioning_protocol_t *protocol)
{
    static uint8_t ssid[] = "MT-Test";
    static uint8_t password[] = "password1";
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
    request.request_id = 2U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_CREDENTIALS;
    request.set_credentials = &body;
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(protocol, &request, &result,
                                      &wire, &wire_length);
    assert(result.operation_admitted);
    assert(response->body_case ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_OPERATION_ACCEPTED);
    assert(response->operation_accepted->operation->state ==
           MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_PENDING);
    assert(s_copied_ssid_length == sizeof(ssid) - 1U);
    assert(s_copied_password_length == sizeof(password) - 1U);
    assert(memcmp(s_copied_ssid, ssid, sizeof(ssid) - 1U) == 0);
    assert(memcmp(s_copied_password, password,
                  sizeof(password) - 1U) == 0);
    const uint64_t operation_id =
        response->operation_accepted->operation->operation_id;
    _free_response(response, wire);
    return operation_id;
}

static void _test_snapshot_validation_and_admission(void)
{
    provisioning_protocol_t protocol;
    s_status.generation = 9U;
    s_status.saved_profile = true;
    s_status.profile_persisted = true;
    s_status.auto_connect = true;
    s_status.manual_hold = true;
    memcpy(s_status.ssid, "Saved", sizeof("Saved"));
    assert(provisioning_protocol_init(&protocol, "A1B2C3", "1.2.3",
                                      &s_status) == ESP_OK);

    Microtech__Provisioning__V1__GetSnapshotRequest snapshot_body =
        MICROTECH__PROVISIONING__V1__GET_SNAPSHOT_REQUEST__INIT;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 10U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_SNAPSHOT;
    request.get_snapshot = &snapshot_body;
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(&protocol, &request, &result,
                                      &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->snapshot->generation == 9U);
    assert(response->snapshot->state ==
           MICROTECH__PROVISIONING__V1__WIFI_STATE__WIFI_STATE_IDLE);
    assert(response->snapshot->failure ==
           MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_NONE);
    assert(response->snapshot->ssid.len == strlen("Saved"));
    assert(response->snapshot->saved_profile);
    assert(response->snapshot->profile_persisted);
    assert(response->snapshot->auto_connect);
    assert(response->snapshot->manual_hold);
    _free_response(response, wire);

    request.request_id = 0U;
    _assert_error_response(
        &protocol, &request,
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT);
    request.request_id = 11U;
    request.protocol_major = 2U;
    _assert_error_response(
        &protocol, &request,
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_UNSUPPORTED_VERSION);
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY__NOT_SET;
    _assert_error_response(
        &protocol, &request,
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT);

    Microtech__Provisioning__V1__SubscribeEventsRequest subscribe =
        MICROTECH__PROVISIONING__V1__SUBSCRIBE_EVENTS_REQUEST__INIT;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SUBSCRIBE_EVENTS;
    request.subscribe_events = &subscribe;
    _assert_error_response(
        &protocol, &request,
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_UNSUPPORTED_OPERATION);

    uint8_t ssid[] = {'X'};
    Microtech__Provisioning__V1__WifiCredentials credentials =
        MICROTECH__PROVISIONING__V1__WIFI_CREDENTIALS__INIT;
    credentials.ssid.data = ssid;
    credentials.ssid.len = sizeof(ssid);
    credentials.security = (Microtech__Provisioning__V1__WifiSecurity)99;
    Microtech__Provisioning__V1__SetCredentialsRequest credentials_body =
        MICROTECH__PROVISIONING__V1__SET_CREDENTIALS_REQUEST__INIT;
    credentials_body.credentials = &credentials;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_CREDENTIALS;
    request.set_credentials = &credentials_body;
    _assert_error_response(
        &protocol, &request,
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT);

    uint8_t nul_ssid[] = {'M', 'T', '\0', 'X'};
    credentials.ssid.data = nul_ssid;
    credentials.ssid.len = sizeof(nul_ssid);
    credentials.security =
        MICROTECH__PROVISIONING__V1__WIFI_SECURITY__WIFI_SECURITY_OPEN;
    _assert_error_response(
        &protocol, &request,
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT);

    provisioning_protocol_t unavailable;
    connectivity_manager_status_snapshot_t unavailable_status = s_status;
    unavailable_status.radio_available = false;
    assert(provisioning_protocol_init(&unavailable, "A1B2C3", "1.2.3",
                                      &unavailable_status) == ESP_OK);
    Microtech__Provisioning__V1__StartScanRequest scan =
        MICROTECH__PROVISIONING__V1__START_SCAN_REQUEST__INIT;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_START_SCAN;
    request.start_scan = &scan;
    _assert_error_response(
        &unavailable, &request,
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_RADIO_UNAVAILABLE);

    provisioning_protocol_t no_profile;
    connectivity_manager_status_snapshot_t no_profile_status = s_status;
    no_profile_status.saved_profile = false;
    no_profile_status.profile_persisted = false;
    no_profile_status.auto_connect = false;
    assert(provisioning_protocol_init(&no_profile, "A1B2C3", "1.2.3",
                                      &no_profile_status) == ESP_OK);
    Microtech__Provisioning__V1__ReconnectSavedRequest reconnect =
        MICROTECH__PROVISIONING__V1__RECONNECT_SAVED_REQUEST__INIT;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_RECONNECT_SAVED;
    request.reconnect_saved = &reconnect;
    _assert_error_response(
        &no_profile, &request,
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND);

    s_admission_result = ESP_ERR_NO_MEM;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_START_SCAN;
    request.start_scan = &scan;
    _assert_error_response(
        &protocol, &request,
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_BUSY);
    s_admission_result = ESP_OK;
}

static void _test_operation_polling_and_cancel(void)
{
    provisioning_protocol_t protocol;
    assert(provisioning_protocol_init(&protocol, "A1B2C3", "1.2.3",
                                      &s_status) == ESP_OK);
    const uint64_t operation_id = _set_credentials(&protocol);
    assert(operation_id != 0U);

    Microtech__Provisioning__V1__StartScanRequest scan_body =
        MICROTECH__PROVISIONING__V1__START_SCAN_REQUEST__INIT;
    pb_request_t scan_request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    scan_request.request_id = 3U;
    scan_request.protocol_major = 1U;
    scan_request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_START_SCAN;
    scan_request.start_scan = &scan_body;
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(&protocol, &scan_request, &result,
                                      &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_BUSY);
    assert(!result.operation_admitted);
    _free_response(response, wire);

    Microtech__Provisioning__V1__CancelOperationRequest cancel_body =
        MICROTECH__PROVISIONING__V1__CANCEL_OPERATION_REQUEST__INIT;
    cancel_body.operation_id = operation_id;
    pb_request_t cancel_request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    cancel_request.request_id = 4U;
    cancel_request.protocol_major = 1U;
    cancel_request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_CANCEL_OPERATION;
    cancel_request.cancel_operation = &cancel_body;
    response = _handle(&protocol, &cancel_request, &result,
                       &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(s_canceled_operation == operation_id);
    assert(response->operation->state ==
           MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_PENDING);
    _free_response(response, wire);

    connectivity_manager_status_snapshot_t terminal = s_status;
    terminal.generation = s_status.generation + 1U;
    terminal.operation_id = operation_id;
    terminal.operation_complete = true;
    terminal.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    terminal.ipv4_address = 1U;
    terminal.saved_profile = true;
    terminal.profile_persisted = true;
    terminal.auto_connect = true;
    terminal.last_error = ESP_OK;
    memcpy(terminal.ssid, "MT-Test", sizeof("MT-Test"));
    assert(provisioning_protocol_ingest_status(&protocol, &terminal));
    assert(provisioning_protocol_active_operation(&protocol) == 0U);

    Microtech__Provisioning__V1__GetOperationRequest get_body =
        MICROTECH__PROVISIONING__V1__GET_OPERATION_REQUEST__INIT;
    get_body.operation_id = operation_id;
    pb_request_t get_request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    get_request.request_id = 5U;
    get_request.protocol_major = 1U;
    get_request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_OPERATION;
    get_request.get_operation = &get_body;
    response = _handle(&protocol, &get_request, &result,
                       &wire, &wire_length);
    assert(response->operation->state ==
           MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_SUCCEEDED);
    assert(response->operation->failure ==
           MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_NONE);
    _free_response(response, wire);

    response = _handle(&protocol, &cancel_request, &result,
                       &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->operation->state ==
           MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_SUCCEEDED);
    _free_response(response, wire);

    cancel_body.operation_id = operation_id + 100U;
    _assert_error_response(
        &protocol, &cancel_request,
        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND);
}

static void _test_saved_mutation(
    Microtech__Provisioning__V1__ProvisioningRequest__BodyCase body_case,
    Microtech__Provisioning__V1__OperationType expected_type,
    fake_request_kind_t expected_request)
{
    provisioning_protocol_t protocol;
    s_status.saved_profile = true;
    s_status.profile_persisted = true;
    assert(provisioning_protocol_init(&protocol, "A1B2C3", "1.2.3",
                                      &s_status) == ESP_OK);
    Microtech__Provisioning__V1__DisconnectRequest disconnect =
        MICROTECH__PROVISIONING__V1__DISCONNECT_REQUEST__INIT;
    Microtech__Provisioning__V1__ReconnectSavedRequest reconnect =
        MICROTECH__PROVISIONING__V1__RECONNECT_SAVED_REQUEST__INIT;
    Microtech__Provisioning__V1__ForgetSavedRequest forget =
        MICROTECH__PROVISIONING__V1__FORGET_SAVED_REQUEST__INIT;
    Microtech__Provisioning__V1__SetAutoConnectRequest set_auto =
        MICROTECH__PROVISIONING__V1__SET_AUTO_CONNECT_REQUEST__INIT;
    set_auto.enabled = true;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 20U;
    request.protocol_major = 1U;
    request.body_case = body_case;
    switch (body_case)
    {
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_DISCONNECT:
        request.disconnect = &disconnect;
        break;
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_RECONNECT_SAVED:
        request.reconnect_saved = &reconnect;
        break;
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_FORGET_SAVED:
        request.forget_saved = &forget;
        break;
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_AUTO_CONNECT:
        request.set_auto_connect = &set_auto;
        break;
    default:
        assert(false);
        return;
    }
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(&protocol, &request, &result,
                                      &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->body_case ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_OPERATION_ACCEPTED);
    assert(response->operation_accepted->operation->operation_id != 0U);
    assert(response->operation_accepted->operation->type == expected_type);
    assert(response->operation_accepted->operation->state ==
           MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_PENDING);
    assert(response->operation_accepted->operation->failure ==
           MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_NONE);
    assert(result.operation_admitted);
    assert(s_last_request == expected_request);
    if (expected_request == FAKE_REQUEST_AUTO_CONNECT)
    {
        assert(s_auto_connect_value);
    }
    _free_response(response, wire);
}

static void _test_saved_mutation_matrix(void)
{
    _test_saved_mutation(
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_DISCONNECT,
        MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_DISCONNECT,
        FAKE_REQUEST_DISCONNECT);
    _test_saved_mutation(
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_RECONNECT_SAVED,
        MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_RECONNECT_SAVED,
        FAKE_REQUEST_RECONNECT);
    _test_saved_mutation(
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_FORGET_SAVED,
        MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_FORGET_SAVED,
        FAKE_REQUEST_FORGET);
    _test_saved_mutation(
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_AUTO_CONNECT,
        MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_SET_AUTO_CONNECT,
        FAKE_REQUEST_AUTO_CONNECT);
}

static uint64_t _start_scan(provisioning_protocol_t *protocol)
{
    Microtech__Provisioning__V1__StartScanRequest body =
        MICROTECH__PROVISIONING__V1__START_SCAN_REQUEST__INIT;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 30U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_START_SCAN;
    request.start_scan = &body;
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(protocol, &request, &result,
                                      &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->operation_accepted->operation->type ==
           MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_SCAN);
    assert(result.operation_admitted);
    assert(s_last_request == FAKE_REQUEST_SCAN);
    const uint64_t operation_id =
        response->operation_accepted->operation->operation_id;
    _free_response(response, wire);
    return operation_id;
}

static pb_response_t *_get_scan_results(
    provisioning_protocol_t *protocol, uint64_t generation,
    uint8_t **wire, size_t *wire_length)
{
    Microtech__Provisioning__V1__GetScanResultsRequest body =
        MICROTECH__PROVISIONING__V1__GET_SCAN_RESULTS_REQUEST__INIT;
    body.generation = generation;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 31U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_SCAN_RESULTS;
    request.get_scan_results = &body;
    provisioning_protocol_result_t result;
    return _handle(protocol, &request, &result, wire, wire_length);
}

static pb_response_t *_get_operation(
    provisioning_protocol_t *protocol, uint64_t operation_id,
    uint8_t **wire, size_t *wire_length)
{
    Microtech__Provisioning__V1__GetOperationRequest body =
        MICROTECH__PROVISIONING__V1__GET_OPERATION_REQUEST__INIT;
    body.operation_id = operation_id;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 32U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_OPERATION;
    request.get_operation = &body;
    provisioning_protocol_result_t result;
    return _handle(protocol, &request, &result, wire, wire_length);
}

static void _test_scan_polling_and_limits(void)
{
    provisioning_protocol_t protocol;
    assert(provisioning_protocol_init(&protocol, "A1B2C3", "1.2.3",
                                      &s_status) == ESP_OK);
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _get_scan_results(
                                  &protocol, 0U, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND);
    _free_response(response, wire);

    const uint64_t operation_id = _start_scan(&protocol);
    connectivity_manager_scan_snapshot_t running;
    memset(&running, 0, sizeof(running));
    running.generation = 40U;
    running.operation_id = operation_id;
    running.running = true;
    provisioning_protocol_ingest_scan(&protocol, &running);

    connectivity_manager_scan_snapshot_t complete;
    memset(&complete, 0, sizeof(complete));
    complete.generation = 41U;
    complete.operation_id = operation_id;
    complete.record_count = CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS;
    complete.truncated = true;
    for (size_t index = 0U; index < complete.record_count; ++index)
    {
        memset(complete.records[index].ssid, (int)('A' + index),
               CONNECTIVITY_MANAGER_SSID_MAX_BYTES);
        complete.records[index].rssi = (int8_t)(-20 - (int)index);
        complete.records[index].channel = (uint8_t)(index + 1U);
        complete.records[index].security = index == 0U ?
                                           CONNECTIVITY_MANAGER_SECURITY_OPEN :
                                           CONNECTIVITY_MANAGER_SECURITY_PERSONAL;
        complete.records[index].saved = index == 0U;
    }
    provisioning_protocol_ingest_scan(&protocol, &complete);
    assert(provisioning_protocol_active_operation(&protocol) == 0U);

    response = _get_scan_results(&protocol, 0U, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->scan_results->generation == 41U);
    assert(response->scan_results->n_networks ==
           CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS);
    assert(response->scan_results->truncated);
    assert(response->scan_results->networks[0]->ssid.len ==
           CONNECTIVITY_MANAGER_SSID_MAX_BYTES);
    assert(response->scan_results->networks[0]->saved);
    assert(wire_length + 16U <= 500U);
    _free_response(response, wire);

    response = _get_scan_results(&protocol, 41U, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    _free_response(response, wire);
    response = _get_scan_results(&protocol, 40U, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND);
    _free_response(response, wire);

    const uint64_t failed_operation = _start_scan(&protocol);
    connectivity_manager_scan_snapshot_t failed;
    memset(&failed, 0, sizeof(failed));
    failed.generation = 42U;
    failed.operation_id = failed_operation;
    failed.last_error = ESP_FAIL;
    provisioning_protocol_ingest_scan(&protocol, &failed);
    response = _get_operation(&protocol, failed_operation,
                              &wire, &wire_length);
    assert(response->operation->state ==
           MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_FAILED);
    assert(response->operation->failure ==
           MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_INTERNAL);
    _free_response(response, wire);
    response = _get_scan_results(&protocol, 0U, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->scan_results->generation == 41U);
    _free_response(response, wire);

    const uint64_t canceled_operation = _start_scan(&protocol);
    connectivity_manager_scan_snapshot_t canceled;
    memset(&canceled, 0, sizeof(canceled));
    canceled.generation = 43U;
    canceled.operation_id = canceled_operation;
    canceled.last_error = ESP_ERR_NOT_FINISHED;
    provisioning_protocol_ingest_scan(&protocol, &canceled);
    response = _get_operation(&protocol, canceled_operation,
                              &wire, &wire_length);
    assert(response->operation->state ==
           MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_CANCELED);
    assert(response->operation->failure ==
           MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_CANCELED);
    _free_response(response, wire);
}

static void _test_scan_terminal_releases_stale_busy_status(void)
{
    provisioning_protocol_t protocol;
    assert(provisioning_protocol_init(&protocol, "A1B2C3", "1.2.3",
                                      &s_status) == ESP_OK);
    const uint64_t scan_operation = _start_scan(&protocol);

    connectivity_manager_status_snapshot_t stale = s_status;
    stale.generation = 1U;
    stale.operation_id = scan_operation;
    stale.operation_complete = false;
    assert(!provisioning_protocol_ingest_status(&protocol, &stale));
    assert(protocol.connectivity.operation_id == scan_operation);

    connectivity_manager_scan_snapshot_t terminal;
    memset(&terminal, 0, sizeof(terminal));
    terminal.generation = 1U;
    terminal.operation_id = scan_operation;
    provisioning_protocol_ingest_scan(&protocol, &terminal);
    assert(provisioning_protocol_active_operation(&protocol) == 0U);
    assert(protocol.connectivity.operation_id == 0U);

    const uint64_t connect_operation = _set_credentials(&protocol);
    assert(connect_operation != 0U);
    assert(s_last_request == FAKE_REQUEST_CONNECT);
}

static void _test_rejections_and_finish(void)
{
    provisioning_protocol_t protocol;
    assert(provisioning_protocol_init(&protocol, "A1B2C3", "1.2.3",
                                      &s_status) == ESP_OK);
    Microtech__Provisioning__V1__SubscribeEventsRequest subscribe =
        MICROTECH__PROVISIONING__V1__SUBSCRIBE_EVENTS_REQUEST__INIT;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 6U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SUBSCRIBE_EVENTS;
    request.subscribe_events = &subscribe;
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(&protocol, &request, &result,
                                      &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_UNSUPPORTED_OPERATION);
    _free_response(response, wire);

    Microtech__Provisioning__V1__FinishSessionRequest finish =
        MICROTECH__PROVISIONING__V1__FINISH_SESSION_REQUEST__INIT;
    request.request_id = 7U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_FINISH_SESSION;
    request.finish_session = &finish;
    response = _handle(&protocol, &request, &result,
                       &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->body_case ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY__NOT_SET);
    assert(result.finish_session);
    _free_response(response, wire);
}

static void _test_scan_results_and_generation_zero(void)
{
    provisioning_protocol_t protocol;
    assert(provisioning_protocol_init(&protocol, "A1B2C3", "1.2.3",
                                      &s_status) == ESP_OK);
    Microtech__Provisioning__V1__StartScanRequest start =
        MICROTECH__PROVISIONING__V1__START_SCAN_REQUEST__INIT;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 8U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_START_SCAN;
    request.start_scan = &start;
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(&protocol, &request, &result,
                                      &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    const uint64_t operation_id =
        response->operation_accepted->operation->operation_id;
    assert(operation_id != 0U);
    _free_response(response, wire);

    connectivity_manager_scan_snapshot_t scan =
    {
        .generation = 44U,
        .operation_id = operation_id,
        .last_error = ESP_OK,
        .record_count = 2U,
        .truncated = true,
        .running = false,
    };
    memcpy(scan.records[0].ssid, "Strong AP", sizeof("Strong AP"));
    scan.records[0].rssi = -40;
    scan.records[0].channel = 6U;
    scan.records[0].security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL;
    memcpy(scan.records[1].ssid, "Open AP", sizeof("Open AP"));
    scan.records[1].rssi = -55;
    scan.records[1].channel = 11U;
    scan.records[1].security = CONNECTIVITY_MANAGER_SECURITY_OPEN;
    provisioning_protocol_ingest_scan(&protocol, &scan);
    assert(provisioning_protocol_active_operation(&protocol) == 0U);

    Microtech__Provisioning__V1__GetScanResultsRequest get =
        MICROTECH__PROVISIONING__V1__GET_SCAN_RESULTS_REQUEST__INIT;
    request.request_id = 9U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_SCAN_RESULTS;
    request.get_scan_results = &get;
    response = _handle(&protocol, &request, &result, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->scan_results->generation == 44U);
    assert(response->scan_results->n_networks == 2U);
    assert(response->scan_results->truncated);
    assert(response->scan_results->networks[0]->ssid.len ==
           sizeof("Strong AP") - 1U);
    assert(memcmp(response->scan_results->networks[0]->ssid.data,
                  "Strong AP", sizeof("Strong AP") - 1U) == 0);
    _free_response(response, wire);

    get.generation = 45U;
    request.request_id = 10U;
    response = _handle(&protocol, &request, &result, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND);
    assert(response->failure ==
           MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_NONE);
    _free_response(response, wire);
}

static void _test_storage_failure_cancel_idempotence(void)
{
    provisioning_protocol_t protocol;
    assert(provisioning_protocol_init(&protocol, "A1B2C3", "1.2.3",
                                      &s_status) == ESP_OK);
    const uint64_t operation_id = _set_credentials(&protocol);
    connectivity_manager_status_snapshot_t terminal = s_status;
    terminal.generation = s_status.generation + 1U;
    terminal.operation_id = operation_id;
    terminal.operation_complete = true;
    terminal.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    terminal.failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
    terminal.last_error = ESP_FAIL;
    terminal.ipv4_address = 1U;
    terminal.saved_profile = true;
    terminal.profile_persisted = false;
    memcpy(terminal.ssid, "MT-Test", sizeof("MT-Test"));
    assert(!provisioning_protocol_ingest_status(&protocol, &terminal));

    Microtech__Provisioning__V1__CancelOperationRequest cancel =
        MICROTECH__PROVISIONING__V1__CANCEL_OPERATION_REQUEST__INIT;
    cancel.operation_id = operation_id;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 11U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_CANCEL_OPERATION;
    request.cancel_operation = &cancel;
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(&protocol, &request, &result,
                                      &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK);
    assert(response->operation->operation_id == operation_id);
    assert(response->operation->state ==
           MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_FAILED);
    assert(response->operation->failure ==
           MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_STORAGE);
    _free_response(response, wire);

    cancel.operation_id = operation_id + 100U;
    request.request_id = 12U;
    response = _handle(&protocol, &request, &result, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND);
    _free_response(response, wire);
}

static void _test_invalid_credentials_and_requests(void)
{
    provisioning_protocol_t protocol;
    assert(provisioning_protocol_init(&protocol, "A1B2C3", "1.2.3",
                                      &s_status) == ESP_OK);
    uint8_t ssid[] = {'M', 'T', '\0', 'X'};
    uint8_t password[] = "password1";
    Microtech__Provisioning__V1__WifiCredentials credentials =
        MICROTECH__PROVISIONING__V1__WIFI_CREDENTIALS__INIT;
    credentials.ssid.data = ssid;
    credentials.ssid.len = sizeof(ssid);
    credentials.password.data = password;
    credentials.password.len = sizeof(password) - 1U;
    credentials.security =
        MICROTECH__PROVISIONING__V1__WIFI_SECURITY__WIFI_SECURITY_PERSONAL;
    Microtech__Provisioning__V1__SetCredentialsRequest set =
        MICROTECH__PROVISIONING__V1__SET_CREDENTIALS_REQUEST__INIT;
    set.credentials = &credentials;
    pb_request_t request =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__INIT;
    request.request_id = 13U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_CREDENTIALS;
    request.set_credentials = &set;
    provisioning_protocol_result_t result;
    uint8_t *wire = NULL;
    size_t wire_length = 0U;
    pb_response_t *response = _handle(&protocol, &request, &result,
                                      &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT);
    assert(!result.operation_admitted);
    _free_response(response, wire);

    request.request_id = 14U;
    request.protocol_major = 2U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_CAPABILITIES;
    Microtech__Provisioning__V1__GetCapabilitiesRequest capabilities =
        MICROTECH__PROVISIONING__V1__GET_CAPABILITIES_REQUEST__INIT;
    request.get_capabilities = &capabilities;
    response = _handle(&protocol, &request, &result, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_UNSUPPORTED_VERSION);
    _free_response(response, wire);

    request.request_id = 15U;
    request.protocol_major = 1U;
    request.body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY__NOT_SET;
    response = _handle(&protocol, &request, &result, &wire, &wire_length);
    assert(response->code ==
           MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT);
    _free_response(response, wire);
}

int main(void)
{
    _reset();
    _test_capabilities_and_malformed();
    _reset();
    _test_snapshot_validation_and_admission();
    _reset();
    _test_operation_polling_and_cancel();
    _reset();
    _test_saved_mutation_matrix();
    _reset();
    _test_scan_polling_and_limits();
    _reset();
    _test_scan_terminal_releases_stale_busy_status();
    _reset();
    _test_rejections_and_finish();
    _reset();
    _test_scan_results_and_generation_zero();
    _reset();
    _test_storage_failure_cancel_idempotence();
    _reset();
    _test_invalid_credentials_and_requests();
    puts("provisioning protocol tests passed");
    return 0;
}
