#include "provisioning_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef Microtech__Provisioning__V1__Capabilities proto_capabilities_t;
typedef Microtech__Provisioning__V1__FailureReason proto_failure_t;
typedef Microtech__Provisioning__V1__Feature proto_feature_t;
typedef Microtech__Provisioning__V1__OperationAccepted proto_accepted_t;
typedef Microtech__Provisioning__V1__OperationState proto_operation_state_t;
typedef Microtech__Provisioning__V1__OperationType proto_operation_type_t;
typedef Microtech__Provisioning__V1__ProtocolVersion proto_version_t;
typedef Microtech__Provisioning__V1__ProvisioningRequest proto_request_t;
typedef Microtech__Provisioning__V1__ProvisioningResponse proto_response_t;
typedef Microtech__Provisioning__V1__ResponseCode proto_response_code_t;
typedef Microtech__Provisioning__V1__ScanSnapshot proto_scan_t;
typedef Microtech__Provisioning__V1__WifiNetwork proto_network_t;
typedef Microtech__Provisioning__V1__WifiSecurity proto_security_t;
typedef Microtech__Provisioning__V1__WifiSnapshot proto_wifi_snapshot_t;
typedef Microtech__Provisioning__V1__WifiState proto_wifi_state_t;

#define PROTO_FAILURE_NONE \
    MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_NONE
#define PROTO_FAILURE_RADIO \
    MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_RADIO_UNAVAILABLE
#define PROTO_OPERATION_PENDING \
    MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_PENDING
#define PROTO_OPERATION_RUNNING \
    MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_RUNNING
#define PROTO_OPERATION_SUCCEEDED \
    MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_SUCCEEDED
#define PROTO_OPERATION_FAILED \
    MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_FAILED
#define PROTO_OPERATION_CANCELED \
    MICROTECH__PROVISIONING__V1__OPERATION_STATE__OPERATION_STATE_CANCELED
#define PROTO_RESPONSE_OK \
    MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_OK

typedef struct provisioning_protocol_response_storage
{
    proto_version_t version;
    proto_feature_t features[4];
    proto_capabilities_t capabilities;
    proto_wifi_snapshot_t wifi_snapshot;
    proto_accepted_t accepted;
    proto_scan_t scan;
    proto_network_t networks[CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS];
    proto_network_t *network_pointers[CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS];
} provisioning_protocol_response_storage_t;

typedef struct provisioning_protocol_allocation
{
    max_align_t alignment;
    size_t size;
} provisioning_protocol_allocation_t;

static void _provisioning_protocol_secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (bytes != NULL && size > 0U)
    {
        *bytes = 0U;
        ++bytes;
        --size;
    }
}

static void *_provisioning_protocol_allocate(void *allocator_data, size_t size)
{
    (void)allocator_data;
    if (size > SIZE_MAX - sizeof(provisioning_protocol_allocation_t))
    {
        return NULL;
    }
    provisioning_protocol_allocation_t *allocation =
        calloc(1U, sizeof(*allocation) + size);
    if (allocation == NULL)
    {
        return NULL;
    }
    allocation->size = size;
    return allocation + 1;
}

static void _provisioning_protocol_free(void *allocator_data, void *pointer)
{
    (void)allocator_data;
    if (pointer == NULL)
    {
        return;
    }
    provisioning_protocol_allocation_t *allocation =
        (provisioning_protocol_allocation_t *)pointer - 1;
    const size_t size = allocation->size;
    _provisioning_protocol_secure_zero(
        allocation, sizeof(*allocation) + size);
    free(allocation);
}

static proto_failure_t _provisioning_protocol_failure(
    connectivity_manager_failure_t failure)
{
    switch (failure)
    {
    case CONNECTIVITY_MANAGER_FAILURE_NONE:
        return PROTO_FAILURE_NONE;
    case CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION:
        return MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_AUTHENTICATION;
    case CONNECTIVITY_MANAGER_FAILURE_AP_NOT_FOUND:
        return MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_AP_NOT_FOUND;
    case CONNECTIVITY_MANAGER_FAILURE_ASSOCIATION_TIMEOUT:
        return MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_ASSOCIATION_TIMEOUT;
    case CONNECTIVITY_MANAGER_FAILURE_DHCP_TIMEOUT:
        return MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_DHCP_TIMEOUT;
    case CONNECTIVITY_MANAGER_FAILURE_LINK_LOST:
        return MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_LINK_LOST;
    case CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE:
        return PROTO_FAILURE_RADIO;
    case CONNECTIVITY_MANAGER_FAILURE_STORAGE:
        return MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_STORAGE;
    case CONNECTIVITY_MANAGER_FAILURE_INTERNAL:
    default:
        return MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_INTERNAL;
    }
}

static proto_wifi_state_t _provisioning_protocol_wifi_state(
    connectivity_manager_state_t state)
{
    switch (state)
    {
    case CONNECTIVITY_MANAGER_STATE_OFFLINE:
        return MICROTECH__PROVISIONING__V1__WIFI_STATE__WIFI_STATE_UNAVAILABLE;
    case CONNECTIVITY_MANAGER_STATE_IDLE:
        return MICROTECH__PROVISIONING__V1__WIFI_STATE__WIFI_STATE_IDLE;
    case CONNECTIVITY_MANAGER_STATE_SCANNING:
        return MICROTECH__PROVISIONING__V1__WIFI_STATE__WIFI_STATE_SCANNING;
    case CONNECTIVITY_MANAGER_STATE_CONNECTING:
        return MICROTECH__PROVISIONING__V1__WIFI_STATE__WIFI_STATE_CONNECTING;
    case CONNECTIVITY_MANAGER_STATE_WAITING_IP:
        return MICROTECH__PROVISIONING__V1__WIFI_STATE__WIFI_STATE_OBTAINING_IP;
    case CONNECTIVITY_MANAGER_STATE_IP_READY:
        return MICROTECH__PROVISIONING__V1__WIFI_STATE__WIFI_STATE_CONNECTED;
    case CONNECTIVITY_MANAGER_STATE_RETRY_WAIT:
        return MICROTECH__PROVISIONING__V1__WIFI_STATE__WIFI_STATE_RETRY_WAIT;
    case CONNECTIVITY_MANAGER_STATE_SUSPENDED:
        return MICROTECH__PROVISIONING__V1__WIFI_STATE__WIFI_STATE_SUSPENDED;
    default:
        return MICROTECH__PROVISIONING__V1__WIFI_STATE__WIFI_STATE_UNAVAILABLE;
    }
}

static proto_security_t _provisioning_protocol_security(
    connectivity_manager_security_t security)
{
    switch (security)
    {
    case CONNECTIVITY_MANAGER_SECURITY_OPEN:
        return MICROTECH__PROVISIONING__V1__WIFI_SECURITY__WIFI_SECURITY_OPEN;
    case CONNECTIVITY_MANAGER_SECURITY_PERSONAL:
        return MICROTECH__PROVISIONING__V1__WIFI_SECURITY__WIFI_SECURITY_PERSONAL;
    case CONNECTIVITY_MANAGER_SECURITY_UNSUPPORTED:
    default:
        return MICROTECH__PROVISIONING__V1__WIFI_SECURITY__WIFI_SECURITY_UNSUPPORTED;
    }
}

static void _provisioning_protocol_finish_operation(
    provisioning_protocol_context_t *context,
    esp_err_t result, connectivity_manager_failure_t failure)
{
    if (result == ESP_ERR_NOT_FINISHED)
    {
        context->active_operation.state = PROTO_OPERATION_CANCELED;
        context->active_operation.failure =
            MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_CANCELED;
    }
    else if (result == ESP_OK &&
             failure == CONNECTIVITY_MANAGER_FAILURE_NONE)
    {
        context->active_operation.state = PROTO_OPERATION_SUCCEEDED;
        context->active_operation.failure = PROTO_FAILURE_NONE;
    }
    else
    {
        context->active_operation.state = PROTO_OPERATION_FAILED;
        context->active_operation.failure =
            failure == CONNECTIVITY_MANAGER_FAILURE_NONE ?
            MICROTECH__PROVISIONING__V1__FAILURE_REASON__FAILURE_REASON_INTERNAL :
            _provisioning_protocol_failure(failure);
    }
    context->terminal_operation = context->active_operation;
    context->terminal_operation_valid = true;
    context->active_operation_valid = false;
}

static void _provisioning_protocol_start_operation(
    provisioning_protocol_context_t *context, uint64_t operation_id,
    proto_operation_type_t type)
{
    context->active_operation = (provisioning_protocol_operation_t)
                                MICROTECH__PROVISIONING__V1__OPERATION_STATUS__INIT;
    context->active_operation.operation_id = operation_id;
    context->active_operation.type = type;
    context->active_operation.state = PROTO_OPERATION_PENDING;
    context->active_operation.failure = PROTO_FAILURE_NONE;
    context->active_operation_valid = true;
}

static const provisioning_protocol_operation_t
*_provisioning_protocol_last_operation(
    const provisioning_protocol_context_t *context)
{
    if (context->active_operation_valid)
    {
        return &context->active_operation;
    }
    return context->terminal_operation_valid ?
           &context->terminal_operation : NULL;
}

static void _provisioning_protocol_build_wifi_snapshot(
    const provisioning_protocol_context_t *context,
    proto_wifi_snapshot_t *snapshot)
{
    *snapshot = (proto_wifi_snapshot_t)
                MICROTECH__PROVISIONING__V1__WIFI_SNAPSHOT__INIT;
    snapshot->generation = context->connectivity.generation != 0U ?
                           context->connectivity.generation : 1U;
    snapshot->state = _provisioning_protocol_wifi_state(
                          context->connectivity.state);
    snapshot->failure = _provisioning_protocol_failure(
                            context->connectivity.failure);
    snapshot->ssid.data = (uint8_t *)context->connectivity.ssid;
    snapshot->ssid.len = strnlen(context->connectivity.ssid,
                                 sizeof(context->connectivity.ssid));
    snapshot->has_ipv4 = context->connectivity.state ==
                         CONNECTIVITY_MANAGER_STATE_IP_READY &&
                         context->connectivity.ipv4_address != 0U;
    snapshot->saved_profile = context->connectivity.saved_profile;
    snapshot->profile_persisted = context->connectivity.saved_profile &&
                                  context->connectivity.profile_persisted;
    snapshot->auto_connect = context->connectivity.saved_profile &&
                             context->connectivity.auto_connect;
    snapshot->manual_hold = context->connectivity.manual_hold;
    snapshot->last_operation =
        (provisioning_protocol_operation_t *)
        _provisioning_protocol_last_operation(context);
}

static proto_response_code_t _provisioning_protocol_admission_code(
    esp_err_t result)
{
    switch (result)
    {
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT;
    case ESP_ERR_NOT_FOUND:
        return MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND;
    case ESP_ERR_NO_MEM:
        return MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_BUSY;
    case ESP_ERR_INVALID_STATE:
        return MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_RADIO_UNAVAILABLE;
    default:
        return MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INTERNAL;
    }
}

static void _provisioning_protocol_set_response_code(
    proto_response_t *response, proto_response_code_t code)
{
    response->code = code;
    response->failure = code ==
                        MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_RADIO_UNAVAILABLE ?
                        PROTO_FAILURE_RADIO : PROTO_FAILURE_NONE;
}

static bool _provisioning_protocol_radio_ready(
    const provisioning_protocol_context_t *context)
{
    return context->connectivity.available &&
           context->connectivity.radio_available &&
           context->connectivity.state !=
           CONNECTIVITY_MANAGER_STATE_SUSPENDED;
}

static bool _provisioning_protocol_busy(
    const provisioning_protocol_context_t *context)
{
    return context->active_operation_valid ||
           (context->connectivity.operation_id != 0U &&
            !context->connectivity.operation_complete);
}

static bool _provisioning_protocol_credentials_valid(
    const Microtech__Provisioning__V1__WifiCredentials *credentials)
{
    if (credentials == NULL || credentials->ssid.data == NULL ||
            credentials->ssid.len == 0U ||
            credentials->ssid.len > CONNECTIVITY_MANAGER_SSID_MAX_BYTES ||
            memchr(credentials->ssid.data, '\0', credentials->ssid.len) != NULL)
    {
        return false;
    }
    if (credentials->security ==
            MICROTECH__PROVISIONING__V1__WIFI_SECURITY__WIFI_SECURITY_OPEN)
    {
        return credentials->password.len == 0U;
    }
    return credentials->security ==
           MICROTECH__PROVISIONING__V1__WIFI_SECURITY__WIFI_SECURITY_PERSONAL &&
           credentials->password.data != NULL &&
           credentials->password.len >= 8U &&
           credentials->password.len <=
           CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES &&
           memchr(credentials->password.data, '\0',
                  credentials->password.len) == NULL;
}

static esp_err_t _provisioning_protocol_request_operation(
    provisioning_protocol_context_t *context,
    const proto_request_t *request,
    uint64_t *operation_id, proto_operation_type_t *type)
{
    *operation_id = 0U;
    switch (request->body_case)
    {
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_START_SCAN:
        *type = MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_SCAN;
        return connectivity_manager_request_scan(operation_id);
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_CREDENTIALS:
    {
        const Microtech__Provisioning__V1__WifiCredentials *credentials =
            request->set_credentials->credentials;
        const connectivity_manager_credentials_t manager_credentials =
        {
            .ssid = (const char *)credentials->ssid.data,
            .ssid_length = credentials->ssid.len,
            .password = (const char *)credentials->password.data,
            .password_length = credentials->password.len,
            .security = credentials->security ==
            MICROTECH__PROVISIONING__V1__WIFI_SECURITY__WIFI_SECURITY_PERSONAL ?
            CONNECTIVITY_MANAGER_SECURITY_PERSONAL :
            CONNECTIVITY_MANAGER_SECURITY_OPEN,
        };
        *type = MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_SET_CREDENTIALS;
        return connectivity_manager_request_connect(&manager_credentials,
                operation_id);
    }
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_DISCONNECT:
        *type = MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_DISCONNECT;
        return connectivity_manager_request_disconnect(operation_id);
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_RECONNECT_SAVED:
        *type = MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_RECONNECT_SAVED;
        return connectivity_manager_request_reconnect_saved(operation_id);
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_FORGET_SAVED:
        *type = MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_FORGET_SAVED;
        return connectivity_manager_request_forget(operation_id);
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_AUTO_CONNECT:
        *type = MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_SET_AUTO_CONNECT;
        return connectivity_manager_set_auto_connect(
                   request->set_auto_connect->enabled, operation_id);
    default:
        (void)context;
        return ESP_ERR_INVALID_ARG;
    }
}

static bool _provisioning_protocol_is_mutation(
    Microtech__Provisioning__V1__ProvisioningRequest__BodyCase body)
{
    return body ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_START_SCAN ||
           body ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_CREDENTIALS ||
           body ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_DISCONNECT ||
           body ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_RECONNECT_SAVED ||
           body ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_FORGET_SAVED ||
           body ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_AUTO_CONNECT;
}

static bool _provisioning_protocol_saved_operation(
    Microtech__Provisioning__V1__ProvisioningRequest__BodyCase body)
{
    return body ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_DISCONNECT ||
           body ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_RECONNECT_SAVED ||
           body ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_FORGET_SAVED ||
           body ==
           MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_AUTO_CONNECT;
}

static void _provisioning_protocol_handle_capabilities(
    provisioning_protocol_context_t *context, proto_response_t *response,
    provisioning_protocol_response_storage_t *storage)
{
    storage->version = (proto_version_t)
                       MICROTECH__PROVISIONING__V1__PROTOCOL_VERSION__INIT;
    storage->version.major = 1U;
    const proto_feature_t features[] =
    {
        MICROTECH__PROVISIONING__V1__FEATURE__FEATURE_WIFI_SCAN,
        MICROTECH__PROVISIONING__V1__FEATURE__FEATURE_HIDDEN_NETWORK,
        MICROTECH__PROVISIONING__V1__FEATURE__FEATURE_SAVED_NETWORK_MANAGEMENT,
        MICROTECH__PROVISIONING__V1__FEATURE__FEATURE_AUTO_CONNECT_POLICY,
    };
    memcpy(storage->features, features, sizeof(features));
    storage->capabilities = (proto_capabilities_t)
                            MICROTECH__PROVISIONING__V1__CAPABILITIES__INIT;
    storage->capabilities.protocol_version = &storage->version;
    storage->capabilities.firmware_version = context->firmware_version;
    storage->capabilities.device_id = context->device_id;
    storage->capabilities.n_features =
        sizeof(storage->features) / sizeof(storage->features[0]);
    storage->capabilities.features = storage->features;
    storage->capabilities.max_ssid_bytes =
        CONNECTIVITY_MANAGER_SSID_MAX_BYTES;
    storage->capabilities.max_password_bytes =
        CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES;
    storage->capabilities.max_scan_records =
        CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS;
    storage->capabilities.minimum_event_mtu = 185U;
    storage->capabilities.maximum_event_frame_bytes = 160U;
    response->body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_CAPABILITIES;
    response->capabilities = &storage->capabilities;
    _provisioning_protocol_set_response_code(response, PROTO_RESPONSE_OK);
}

static void _provisioning_protocol_handle_snapshot(
    provisioning_protocol_context_t *context, proto_response_t *response,
    provisioning_protocol_response_storage_t *storage)
{
    _provisioning_protocol_build_wifi_snapshot(
        context, &storage->wifi_snapshot);
    response->body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_SNAPSHOT;
    response->snapshot = &storage->wifi_snapshot;
    _provisioning_protocol_set_response_code(response, PROTO_RESPONSE_OK);
}

static void _provisioning_protocol_handle_get_operation(
    provisioning_protocol_context_t *context, const proto_request_t *request,
    proto_response_t *response)
{
    const uint64_t operation_id = request->get_operation->operation_id;
    provisioning_protocol_operation_t *operation = NULL;
    if (context->active_operation_valid &&
            context->active_operation.operation_id == operation_id)
    {
        operation = &context->active_operation;
    }
    else if (context->terminal_operation_valid &&
             context->terminal_operation.operation_id == operation_id)
    {
        operation = &context->terminal_operation;
    }
    if (operation == NULL)
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND);
        return;
    }
    response->body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_OPERATION;
    response->operation = operation;
    _provisioning_protocol_set_response_code(response, PROTO_RESPONSE_OK);
}

static void _provisioning_protocol_handle_scan_results(
    provisioning_protocol_context_t *context, const proto_request_t *request,
    proto_response_t *response,
    provisioning_protocol_response_storage_t *storage)
{
    if (!context->scan_valid ||
            (request->get_scan_results->generation != 0U &&
             request->get_scan_results->generation !=
             context->scan.generation))
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND);
        return;
    }

    storage->scan = (proto_scan_t)
                    MICROTECH__PROVISIONING__V1__SCAN_SNAPSHOT__INIT;
    storage->scan.generation = context->scan.generation;
    storage->scan.n_networks = context->scan.record_count;
    storage->scan.networks = storage->network_pointers;
    storage->scan.truncated = context->scan.truncated;
    for (size_t index = 0U; index < storage->scan.n_networks; ++index)
    {
        storage->networks[index] = (proto_network_t)
                                   MICROTECH__PROVISIONING__V1__WIFI_NETWORK__INIT;
        storage->networks[index].ssid.data =
            (uint8_t *)context->scan.records[index].ssid;
        storage->networks[index].ssid.len = strnlen(
                                                context->scan.records[index].ssid,
                                                sizeof(context->scan.records[index].ssid));
        storage->networks[index].rssi = context->scan.records[index].rssi;
        storage->networks[index].channel =
            context->scan.records[index].channel;
        storage->networks[index].security =
            _provisioning_protocol_security(
                context->scan.records[index].security);
        storage->networks[index].saved =
            context->scan.records[index].saved;
        storage->network_pointers[index] = &storage->networks[index];
    }
    response->body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_SCAN_RESULTS;
    response->scan_results = &storage->scan;
    _provisioning_protocol_set_response_code(response, PROTO_RESPONSE_OK);
}

static void _provisioning_protocol_handle_cancel(
    provisioning_protocol_context_t *context, const proto_request_t *request,
    proto_response_t *response)
{
    const uint64_t operation_id = request->cancel_operation->operation_id;
    if (context->terminal_operation_valid &&
            context->terminal_operation.operation_id == operation_id)
    {
        response->body_case =
            MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_OPERATION;
        response->operation = &context->terminal_operation;
        _provisioning_protocol_set_response_code(response, PROTO_RESPONSE_OK);
        return;
    }
    if (!context->active_operation_valid ||
            context->active_operation.operation_id != operation_id)
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND);
        return;
    }
    const esp_err_t result = connectivity_manager_cancel(operation_id);
    if (result != ESP_OK)
    {
        _provisioning_protocol_set_response_code(
            response, _provisioning_protocol_admission_code(result));
        return;
    }
    response->body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_OPERATION;
    response->operation = &context->active_operation;
    _provisioning_protocol_set_response_code(response, PROTO_RESPONSE_OK);
}

static void _provisioning_protocol_handle_mutation(
    provisioning_protocol_context_t *context, const proto_request_t *request,
    proto_response_t *response,
    provisioning_protocol_response_storage_t *storage)
{
    if (_provisioning_protocol_busy(context))
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_BUSY);
        return;
    }
    if (!_provisioning_protocol_radio_ready(context))
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_RADIO_UNAVAILABLE);
        return;
    }
    if (_provisioning_protocol_saved_operation(request->body_case) &&
            !context->connectivity.saved_profile)
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_NOT_FOUND);
        return;
    }

    uint64_t operation_id = 0U;
    proto_operation_type_t type =
        MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_UNSPECIFIED;
    const esp_err_t result = _provisioning_protocol_request_operation(
                                 context, request, &operation_id, &type);
    if (result != ESP_OK || operation_id == 0U)
    {
        _provisioning_protocol_set_response_code(
            response, result == ESP_OK ?
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INTERNAL :
            _provisioning_protocol_admission_code(result));
        return;
    }

    _provisioning_protocol_start_operation(context, operation_id, type);
    storage->accepted = (proto_accepted_t)
                        MICROTECH__PROVISIONING__V1__OPERATION_ACCEPTED__INIT;
    storage->accepted.operation = &context->active_operation;
    response->body_case =
        MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__BODY_OPERATION_ACCEPTED;
    response->operation_accepted = &storage->accepted;
    _provisioning_protocol_set_response_code(response, PROTO_RESPONSE_OK);
}

static bool _provisioning_protocol_arguments_valid(
    const proto_request_t *request)
{
    switch (request->body_case)
    {
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_OPERATION:
        return request->get_operation != NULL &&
               request->get_operation->operation_id != 0U;
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SET_CREDENTIALS:
        return request->set_credentials != NULL &&
               _provisioning_protocol_credentials_valid(
                   request->set_credentials->credentials);
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_CANCEL_OPERATION:
        return request->cancel_operation != NULL &&
               request->cancel_operation->operation_id != 0U;
    default:
        return true;
    }
}

static void _provisioning_protocol_dispatch(
    provisioning_protocol_context_t *context, const proto_request_t *request,
    proto_response_t *response,
    provisioning_protocol_response_storage_t *storage)
{
    if (request->request_id == 0U)
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT);
        return;
    }
    response->request_id = request->request_id;
    if (request->protocol_major != 1U)
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_UNSUPPORTED_VERSION);
        return;
    }
    if (request->body_case ==
            MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY__NOT_SET)
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT);
        return;
    }
    if (request->body_case ==
            MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_SUBSCRIBE_EVENTS)
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_UNSUPPORTED_OPERATION);
        return;
    }
    if (!_provisioning_protocol_arguments_valid(request))
    {
        _provisioning_protocol_set_response_code(
            response,
            MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT);
        return;
    }

    switch (request->body_case)
    {
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_CAPABILITIES:
        _provisioning_protocol_handle_capabilities(
            context, response, storage);
        break;
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_SNAPSHOT:
        _provisioning_protocol_handle_snapshot(context, response, storage);
        break;
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_OPERATION:
        _provisioning_protocol_handle_get_operation(context, request, response);
        break;
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_GET_SCAN_RESULTS:
        _provisioning_protocol_handle_scan_results(
            context, request, response, storage);
        break;
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_CANCEL_OPERATION:
        _provisioning_protocol_handle_cancel(context, request, response);
        break;
    case MICROTECH__PROVISIONING__V1__PROVISIONING_REQUEST__BODY_FINISH_SESSION:
        context->finish_requested = true;
        _provisioning_protocol_set_response_code(response, PROTO_RESPONSE_OK);
        break;
    default:
        if (_provisioning_protocol_is_mutation(request->body_case))
        {
            _provisioning_protocol_handle_mutation(
                context, request, response, storage);
        }
        else
        {
            _provisioning_protocol_set_response_code(
                response,
                MICROTECH__PROVISIONING__V1__RESPONSE_CODE__RESPONSE_CODE_INVALID_ARGUMENT);
        }
        break;
    }
}

esp_err_t provisioning_protocol_init(provisioning_protocol_context_t *context,
                                     const char *device_id,
                                     const char *firmware_version,
                                     const connectivity_manager_status_snapshot_t
                                     *status)
{
    if (context == NULL || device_id == NULL || firmware_version == NULL ||
            status == NULL ||
            strlen(device_id) != PROVISIONING_PROTOCOL_DEVICE_ID_BYTES ||
            strlen(firmware_version) >=
            PROVISIONING_PROTOCOL_FIRMWARE_VERSION_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(context, 0, sizeof(*context));
    context->connectivity = *status;
    (void)snprintf(context->device_id, sizeof(context->device_id), "%s",
                   device_id);
    (void)snprintf(context->firmware_version,
                   sizeof(context->firmware_version), "%s",
                   firmware_version);
    return ESP_OK;
}

static void _provisioning_protocol_accept_status(
    provisioning_protocol_context_t *context,
    const connectivity_manager_status_snapshot_t *status)
{
    if (context == NULL || status == NULL || status->generation == 0U ||
            status->generation <= context->connectivity.generation)
    {
        return;
    }
    context->connectivity = *status;
    if (!context->active_operation_valid ||
            context->active_operation.type ==
            MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_SCAN ||
            status->operation_id != context->active_operation.operation_id)
    {
        return;
    }
    if (!status->operation_complete)
    {
        context->active_operation.state = PROTO_OPERATION_RUNNING;
        return;
    }
    const proto_operation_type_t type = context->active_operation.type;
    _provisioning_protocol_finish_operation(context, status->last_error,
                                            status->failure);
    if (type ==
            MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_SET_CREDENTIALS &&
            context->terminal_operation.state == PROTO_OPERATION_SUCCEEDED &&
            status->profile_persisted)
    {
        context->credentials_persisted = true;
    }
}

static void _provisioning_protocol_accept_scan(
    provisioning_protocol_context_t *context,
    const connectivity_manager_scan_snapshot_t *scan)
{
    if (context == NULL || scan == NULL || scan->generation == 0U)
    {
        return;
    }
    const bool active_scan = context->active_operation_valid &&
                             context->active_operation.type ==
                             MICROTECH__PROVISIONING__V1__OPERATION_TYPE__OPERATION_TYPE_SCAN &&
                             scan->operation_id ==
                             context->active_operation.operation_id;
    if (!active_scan)
    {
        if (!scan->running && scan->last_error == ESP_OK &&
                (!context->scan_valid ||
                 scan->generation > context->scan.generation))
        {
            context->scan = *scan;
            context->scan_valid = true;
        }
        return;
    }
    if (scan->running)
    {
        context->active_operation.state = PROTO_OPERATION_RUNNING;
        return;
    }
    if (scan->last_error == ESP_OK)
    {
        if (!context->scan_valid ||
                scan->generation > context->scan.generation)
        {
            context->scan = *scan;
            context->scan_valid = true;
        }
    }
    _provisioning_protocol_finish_operation(
        context, scan->last_error,
        scan->last_error == ESP_OK ||
        scan->last_error == ESP_ERR_NOT_FINISHED ?
        CONNECTIVITY_MANAGER_FAILURE_NONE :
        CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
}

esp_err_t provisioning_protocol_handle(
    provisioning_protocol_context_t *context,
    uint8_t *input, size_t input_size,
    uint8_t **output, size_t *output_size,
    provisioning_protocol_result_t *request_result)
{
    if (input == NULL || input_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (context == NULL || output == NULL || output_size == NULL ||
            request_result == NULL)
    {
        _provisioning_protocol_secure_zero(input, input_size);
        return ESP_ERR_INVALID_ARG;
    }
    if (input_size > PROVISIONING_PROTOCOL_MAX_PLAINTEXT_REQUEST_BYTES)
    {
        _provisioning_protocol_secure_zero(input, input_size);
        return ESP_ERR_INVALID_SIZE;
    }
    *output = NULL;
    *output_size = 0U;
    memset(request_result, 0, sizeof(*request_result));
    const uint64_t operation_before =
        provisioning_protocol_active_operation(context);
    ProtobufCAllocator allocator =
    {
        .alloc = _provisioning_protocol_allocate,
        .free = _provisioning_protocol_free,
        .allocator_data = NULL,
    };
    proto_request_t *request =
        microtech__provisioning__v1__provisioning_request__unpack(
            &allocator, input_size, input);
    _provisioning_protocol_secure_zero(input, input_size);
    if (request == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    proto_response_t response =
        MICROTECH__PROVISIONING__V1__PROVISIONING_RESPONSE__INIT;
    provisioning_protocol_response_storage_t storage;
    memset(&storage, 0, sizeof(storage));
    response.failure = PROTO_FAILURE_NONE;
    _provisioning_protocol_dispatch(context, request, &response, &storage);
    request_result->finish_session = context->finish_requested;
    context->finish_requested = false;
    request_result->operation_admitted = operation_before == 0U &&
                                         provisioning_protocol_active_operation(context) != 0U;
    const size_t packed_size =
        microtech__provisioning__v1__provisioning_response__get_packed_size(
            &response);
    esp_err_t result = ESP_OK;
    if (packed_size == 0U ||
            packed_size > PROVISIONING_PROTOCOL_MAX_PLAINTEXT_RESPONSE_BYTES)
    {
        result = ESP_ERR_INVALID_SIZE;
    }
    else
    {
        *output = malloc(packed_size);
        if (*output == NULL)
        {
            result = ESP_ERR_NO_MEM;
        }
        else
        {
            *output_size =
                microtech__provisioning__v1__provisioning_response__pack(
                    &response, *output);
            if (*output_size != packed_size)
            {
                free(*output);
                *output = NULL;
                *output_size = 0U;
                result = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }

    microtech__provisioning__v1__provisioning_request__free_unpacked(
        request, &allocator);
    return result;
}

bool provisioning_protocol_ingest_status(
    provisioning_protocol_t *protocol,
    const connectivity_manager_status_snapshot_t *status)
{
    _provisioning_protocol_accept_status(protocol, status);
    const bool persisted = protocol != NULL && protocol->credentials_persisted;
    if (protocol != NULL)
    {
        protocol->credentials_persisted = false;
    }
    return persisted;
}

void provisioning_protocol_ingest_scan(
    provisioning_protocol_t *protocol,
    const connectivity_manager_scan_snapshot_t *scan)
{
    _provisioning_protocol_accept_scan(protocol, scan);
}

uint64_t provisioning_protocol_active_operation(
    const provisioning_protocol_context_t *context)
{
    return context != NULL && context->active_operation_valid ?
           context->active_operation.operation_id : 0U;
}
