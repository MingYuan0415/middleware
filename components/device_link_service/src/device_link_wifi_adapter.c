#include <string.h>

#include "esp_err.h"
#ifndef UNIT_TEST_HOST
    #include "freertos/FreeRTOS.h"
    #include "freertos/semphr.h"
#endif

#include "ble_link_service.h"
#include "connectivity_manager.h"
#include "device_link_wifi_adapter.h"
#include "event_bus.h"

#define DBG_TAG "dl_wifi"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

typedef struct device_link_wifi_bridge
{
    bool started;
    uint32_t ble_operation_id;
    connectivity_manager_operation_id_t manager_operation_id;
    device_link_v1_operation_t operation;
    uint64_t last_status_generation;
    uint64_t last_scan_generation;
    event_bus_sub_handle_t subscription;
} device_link_wifi_bridge_t;

static device_link_wifi_bridge_t s_bridge;
#ifndef UNIT_TEST_HOST
    static SemaphoreHandle_t s_bridge_lock;
    static StaticSemaphore_t s_bridge_lock_control;
#endif

static void _device_link_wifi_lock(void)
{
#ifndef UNIT_TEST_HOST
    if (s_bridge_lock != NULL)
    {
        (void)xSemaphoreTakeRecursive(s_bridge_lock, portMAX_DELAY);
    }
#endif
}

static void _device_link_wifi_unlock(void)
{
#ifndef UNIT_TEST_HOST
    if (s_bridge_lock != NULL)
    {
        (void)xSemaphoreGiveRecursive(s_bridge_lock);
    }
#endif
}

static device_link_v1_wifi_failure_t _device_link_wifi_map_failure(
    connectivity_manager_failure_t failure)
{
    switch (failure)
    {
    case CONNECTIVITY_MANAGER_FAILURE_NONE:
        return DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    case CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION:
        return DEVICE_LINK_V1_WIFI_FAILURE_AUTHENTICATION;
    case CONNECTIVITY_MANAGER_FAILURE_AP_NOT_FOUND:
        return DEVICE_LINK_V1_WIFI_FAILURE_AP_NOT_FOUND;
    case CONNECTIVITY_MANAGER_FAILURE_ASSOCIATION_TIMEOUT:
    case CONNECTIVITY_MANAGER_FAILURE_DHCP_TIMEOUT:
        return DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT;
    case CONNECTIVITY_MANAGER_FAILURE_LINK_LOST:
        return DEVICE_LINK_V1_WIFI_FAILURE_LINK_LOST;
    case CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE:
        return DEVICE_LINK_V1_WIFI_FAILURE_RADIO;
    case CONNECTIVITY_MANAGER_FAILURE_STORAGE:
        return DEVICE_LINK_V1_WIFI_FAILURE_STORAGE;
    case CONNECTIVITY_MANAGER_FAILURE_CONFLICT:
    case CONNECTIVITY_MANAGER_FAILURE_INTERNAL:
    default:
        return DEVICE_LINK_V1_WIFI_FAILURE_INTERNAL;
    }
}

static void _device_link_wifi_map_snapshot(
    const connectivity_manager_status_snapshot_t *status,
    device_link_v1_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!status->available || !status->radio_available)
    {
        out->state = DEVICE_LINK_V1_WIFI_UNAVAILABLE;
        out->failure = DEVICE_LINK_V1_WIFI_FAILURE_RADIO;
        return;
    }
    if (status->state == CONNECTIVITY_MANAGER_STATE_SCANNING)
    {
        out->state = DEVICE_LINK_V1_WIFI_SCANNING;
        out->failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    }
    else if (status->state == CONNECTIVITY_MANAGER_STATE_CONNECTING ||
             status->state == CONNECTIVITY_MANAGER_STATE_WAITING_IP ||
             status->state == CONNECTIVITY_MANAGER_STATE_RETRY_WAIT)
    {
        out->state = DEVICE_LINK_V1_WIFI_CONNECTING;
        out->failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    }
    else if (status->state == CONNECTIVITY_MANAGER_STATE_IP_READY)
    {
        out->state = DEVICE_LINK_V1_WIFI_CONNECTED;
        out->failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    }
    else if (status->failure != CONNECTIVITY_MANAGER_FAILURE_NONE &&
             status->failure != CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE)
    {
        out->state = DEVICE_LINK_V1_WIFI_ERROR;
        out->failure = _device_link_wifi_map_failure(status->failure);
    }
    else
    {
        out->state = DEVICE_LINK_V1_WIFI_IDLE;
        out->failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    }
    if (status->saved_profile)
    {
        size_t length = strnlen(status->ssid, DEVICE_LINK_V1_MAX_SSID_BYTES);

        memcpy(out->profile_ssid, status->ssid, length);
        out->profile_ssid_length = (uint8_t)length;
    }
    if (!device_link_v1_snapshot_valid(out) &&
            out->state == DEVICE_LINK_V1_WIFI_CONNECTING)
    {
        out->state = DEVICE_LINK_V1_WIFI_IDLE;
        out->failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    }
}

static uint32_t _device_link_wifi_authmode(
    connectivity_manager_security_t security)
{
    if (security == CONNECTIVITY_MANAGER_SECURITY_OPEN)
    {
        return DEVICE_LINK_V1_WIFI_AUTH_OPEN;
    }
    if (security == CONNECTIVITY_MANAGER_SECURITY_PERSONAL)
    {
        return DEVICE_LINK_V1_WIFI_AUTH_WPA2_PSK;
    }
    return 1U;
}

static void _device_link_wifi_on_status(
    const connectivity_manager_status_snapshot_t *status)
{
    device_link_v1_snapshot_t snapshot;

    uint32_t ble_operation_id;
    connectivity_manager_operation_id_t manager_operation_id;
    device_link_v1_operation_t operation;

    _device_link_wifi_lock();
    if (status->generation <= s_bridge.last_status_generation)
    {
        _device_link_wifi_unlock();
        return;
    }
    s_bridge.last_status_generation = status->generation;
    ble_operation_id = s_bridge.ble_operation_id;
    manager_operation_id = s_bridge.manager_operation_id;
    operation = s_bridge.operation;
    _device_link_wifi_unlock();
    _device_link_wifi_map_snapshot(status, &snapshot);
    ble_link_service_observe_snapshot(&snapshot);
    if (!status->operation_complete ||
            status->operation_id != manager_operation_id ||
            ble_operation_id == 0U)
    {
        return;
    }
    if (operation == DEVICE_LINK_V1_OPERATION_SCAN)
    {
        return;
    }
    device_link_v1_wifi_failure_t failure =
        _device_link_wifi_map_failure(status->failure);

    if (status->operation_canceled ||
            status->last_error == ESP_ERR_NOT_FINISHED)
    {
        failure = DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT;
    }
    else if (operation == DEVICE_LINK_V1_OPERATION_CONNECT &&
             failure == DEVICE_LINK_V1_WIFI_FAILURE_STORAGE)
    {
        failure = DEVICE_LINK_V1_WIFI_FAILURE_INTERNAL;
    }
    if (ble_link_service_complete_operation(
                ble_operation_id, failure, NULL, 0U, &snapshot) != ESP_OK)
    {
        return;
    }
    _device_link_wifi_lock();
    if (s_bridge.ble_operation_id == ble_operation_id)
    {
        s_bridge.ble_operation_id = 0U;
        s_bridge.manager_operation_id = 0U;
    }
    _device_link_wifi_unlock();
}

static void _device_link_wifi_on_scan(
    const connectivity_manager_scan_snapshot_t *scan)
{
    device_link_v1_scan_source_t source[CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS];
    device_link_v1_network_t networks[DEVICE_LINK_V1_MAX_SCAN_NETWORKS];
    connectivity_manager_status_snapshot_t status;
    device_link_v1_snapshot_t snapshot;
    uint8_t count = 0U;

    uint32_t ble_operation_id;
    device_link_v1_wifi_failure_t failure;

    _device_link_wifi_lock();
    if (scan->generation <= s_bridge.last_scan_generation ||
            s_bridge.operation != DEVICE_LINK_V1_OPERATION_SCAN ||
            scan->operation_id != s_bridge.manager_operation_id ||
            s_bridge.ble_operation_id == 0U)
    {
        _device_link_wifi_unlock();
        return;
    }
    if (scan->running)
    {
        _device_link_wifi_unlock();
        return;
    }
    s_bridge.last_scan_generation = scan->generation;
    ble_operation_id = s_bridge.ble_operation_id;
    _device_link_wifi_unlock();
    memset(source, 0, sizeof(source));
    for (uint8_t i = 0U; i < scan->record_count &&
            i < CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS; ++i)
    {
        const size_t ssid_length = strnlen(scan->records[i].ssid,
                                           DEVICE_LINK_V1_MAX_SSID_BYTES);

        memcpy(source[i].ssid, scan->records[i].ssid, ssid_length);
        source[i].ssid_length = (uint8_t)ssid_length;
        source[i].authmode = _device_link_wifi_authmode(
                                 scan->records[i].security);
        source[i].rssi_dbm = scan->records[i].rssi;
    }
    count = device_link_v1_filter_scan_networks(
                source, scan->record_count, networks,
                DEVICE_LINK_V1_MAX_SCAN_NETWORKS);
    memset(&status, 0, sizeof(status));
    (void)connectivity_manager_get_status(&status);
    _device_link_wifi_map_snapshot(&status, &snapshot);
    if (scan->operation_canceled ||
            scan->last_error == ESP_ERR_NOT_FINISHED ||
            scan->last_error == ESP_ERR_TIMEOUT)
    {
        failure = DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT;
        count = 0U;
    }
    else if (scan->last_error != ESP_OK)
    {
        failure = DEVICE_LINK_V1_WIFI_FAILURE_RADIO;
        count = 0U;
    }
    else
    {
        failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    }
    if (ble_link_service_complete_operation(
                ble_operation_id, failure, networks, count, &snapshot) != ESP_OK)
    {
        return;
    }
    _device_link_wifi_lock();
    if (s_bridge.ble_operation_id == ble_operation_id)
    {
        s_bridge.ble_operation_id = 0U;
        s_bridge.manager_operation_id = 0U;
    }
    _device_link_wifi_unlock();
}

static void _device_link_wifi_event(
    event_bus_msg_id_t message_id, uint32_t subtype,
    const void *payload, size_t payload_size, void *user_data)
{
    (void)user_data;
    if (message_id != CONNECTIVITY_MANAGER_MSG || payload == NULL)
    {
        return;
    }
    if (subtype == CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT &&
            payload_size == sizeof(connectivity_manager_status_snapshot_t))
    {
        _device_link_wifi_on_status(payload);
    }
    else if (subtype == CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT &&
             payload_size == sizeof(connectivity_manager_scan_snapshot_t))
    {
        _device_link_wifi_on_scan(payload);
    }
}

void device_link_wifi_adapter_fill_info(device_link_v1_info_t *info, void *arg)
{
    (void)arg;
    if (info == NULL)
    {
        return;
    }
    info->firmware_major = 0U;
    info->firmware_minor = 1U;
    info->firmware_patch = 0U;
}

device_link_v1_status_t device_link_wifi_adapter_submit(
    device_link_v1_operation_t operation,
    const device_link_v1_credentials_t *credentials,
    uint32_t operation_id, void *arg)
{
    connectivity_manager_operation_id_t manager_id = 0U;
    esp_err_t result = ESP_ERR_INVALID_ARG;

    (void)arg;
    switch (operation)
    {
    case DEVICE_LINK_V1_OPERATION_SCAN:
        result = connectivity_manager_request_scan(&manager_id);
        break;
    case DEVICE_LINK_V1_OPERATION_SET_CREDENTIALS:
        if (credentials == NULL)
        {
            return DEVICE_LINK_V1_STATUS_INVALID_ARGUMENT;
        }
        {
            const connectivity_manager_credentials_t saved =
            {
                .ssid = (const char *)credentials->ssid,
                .ssid_length = credentials->ssid_length,
                .password = (const char *)credentials->password,
                .password_length = credentials->password_length,
                .security = credentials->security == DEVICE_LINK_V1_WIFI_OPEN ?
                CONNECTIVITY_MANAGER_SECURITY_OPEN :
                CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
            };

            result = connectivity_manager_request_save_profile(&saved,
                     &manager_id);
        }
        break;
    case DEVICE_LINK_V1_OPERATION_CONNECT:
        result = connectivity_manager_request_reconnect_saved(&manager_id);
        break;
    case DEVICE_LINK_V1_OPERATION_DISCONNECT:
        result = connectivity_manager_request_disconnect(&manager_id);
        break;
    case DEVICE_LINK_V1_OPERATION_FORGET:
        result = connectivity_manager_request_forget(&manager_id);
        break;
    default:
        return DEVICE_LINK_V1_STATUS_INVALID_ARGUMENT;
    }
    if (result != ESP_OK)
    {
        return DEVICE_LINK_V1_STATUS_INTERNAL;
    }
    _device_link_wifi_lock();
    s_bridge.ble_operation_id = operation_id;
    s_bridge.manager_operation_id = manager_id;
    s_bridge.operation = operation;
    _device_link_wifi_unlock();
    return DEVICE_LINK_V1_STATUS_ACCEPTED;
}

#ifdef UNIT_TEST_HOST
void device_link_wifi_adapter_test_set_descriptor_result(esp_err_t result)
{
    (void)result;
}
#endif

esp_err_t device_link_wifi_adapter_get_descriptor(const void **descriptor)
{
    if (descriptor != NULL)
    {
        *descriptor = NULL;
    }
    return ESP_OK;
}

esp_err_t device_link_wifi_adapter_bridge_start(void)
{
    static const ble_link_v1_owner_ops_t ops =
    {
        .fill_info = device_link_wifi_adapter_fill_info,
        .submit_operation = device_link_wifi_adapter_submit,
    };

    if (s_bridge.started)
    {
        return ESP_OK;
    }
#ifndef UNIT_TEST_HOST
    if (s_bridge_lock == NULL)
    {
        s_bridge_lock = xSemaphoreCreateRecursiveMutexStatic(
                            &s_bridge_lock_control);
        if (s_bridge_lock == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
#endif
    ble_link_service_set_v1_ops(&ops, NULL);
    const esp_err_t result = event_bus_subscribe(
                                 CONNECTIVITY_MANAGER_MSG, EVENT_BUS_SUB_TYPE_ANY,
                                 _device_link_wifi_event, NULL, EVENT_BUS_DISPATCH_PUBLISHER,
                                 &s_bridge.subscription);

    if (result != ESP_OK)
    {
        return result;
    }
    s_bridge.started = true;
    return ESP_OK;
}

void device_link_wifi_adapter_bridge_stop(void)
{
    if (!s_bridge.started)
    {
        return;
    }
    (void)event_bus_unsubscribe(s_bridge.subscription);
    ble_link_service_set_v1_ops(NULL, NULL);
    _device_link_wifi_lock();
    memset(&s_bridge, 0, sizeof(s_bridge));
    _device_link_wifi_unlock();
}
