#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "connectivity_manager.h"
#include "device_link_v1.h"
#include "device_link_wifi_adapter.h"
#include "event_bus.h"

extern void connectivity_manager_fake_reset(void);
extern void connectivity_manager_fake_set_scan(
    const connectivity_manager_scan_snapshot_t *snapshot);
extern void connectivity_manager_fake_set_request_result(esp_err_t result);
extern const connectivity_manager_credentials_t *
connectivity_manager_fake_last_saved(void);
extern void event_bus_fake_publish(event_bus_msg_id_t msg_id,
                                   uint32_t sub_type, const void *payload,
                                   size_t payload_size);
extern void ble_link_service_fake_reset(void);
extern unsigned ble_link_service_fake_complete_count(void);
extern uint32_t ble_link_service_fake_last_complete_id(void);
extern unsigned ble_link_service_fake_observe_count(void);
extern const device_link_v1_snapshot_t *ble_link_service_fake_last_snapshot(void);

static void test_fill_info(void)
{
    device_link_v1_info_t info;

    memset(&info, 0xff, sizeof(info));
    device_link_wifi_adapter_fill_info(&info, NULL);
    assert(info.firmware_major == 0U);
    assert(info.firmware_minor == 1U);
    assert(info.firmware_patch == 0U);
}

static void test_submit_scan(void)
{
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_SCAN, NULL, 9U, NULL) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    device_link_wifi_adapter_bridge_stop();
}

static void test_submit_save_credentials(void)
{
    device_link_v1_credentials_t credentials;

    memset(&credentials, 0, sizeof(credentials));
    memcpy(credentials.ssid, "cafe", 4U);
    credentials.ssid_length = 4U;
    memcpy(credentials.password, "password", 8U);
    credentials.password_length = 8U;
    credentials.security = DEVICE_LINK_V1_WIFI_PERSONAL;
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_SET_CREDENTIALS, &credentials, 3U,
               NULL) == DEVICE_LINK_V1_STATUS_ACCEPTED);
    {
        const connectivity_manager_credentials_t *saved =
            connectivity_manager_fake_last_saved();

        assert(saved != NULL);
        assert(saved->ssid_length == 4U);
        assert(memcmp(saved->ssid, "cafe", 4U) == 0);
        assert(saved->security == CONNECTIVITY_MANAGER_SECURITY_PERSONAL);
    }
    device_link_wifi_adapter_bridge_stop();
}

static void test_status_event_observes_snapshot(void)
{
    connectivity_manager_status_snapshot_t status;

    memset(&status, 0, sizeof(status));
    status.generation = 4U;
    status.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    status.available = true;
    status.radio_available = true;
    status.saved_profile = true;
    status.profile_persisted = true;
    memcpy(status.ssid, "cafe", 4U);
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_observe_count() >= 1U);
    assert(ble_link_service_fake_last_snapshot()->state ==
           DEVICE_LINK_V1_WIFI_CONNECTED);
    device_link_wifi_adapter_bridge_stop();
}

static void test_scan_event_completes_operation(void)
{
    connectivity_manager_scan_snapshot_t scan;

    memset(&scan, 0, sizeof(scan));
    scan.generation = 2U;
    scan.operation_id = 1U;
    scan.last_error = ESP_OK;
    scan.record_count = 1U;
    memcpy(scan.records[0].ssid, "cafe", 4U);
    scan.records[0].security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL;
    scan.records[0].rssi = -40;
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    connectivity_manager_fake_set_scan(&scan);
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_SCAN, NULL, 11U, NULL) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                           &scan, sizeof(scan));
    assert(ble_link_service_fake_complete_count() == 1U);
    assert(ble_link_service_fake_last_complete_id() == 11U);
    device_link_wifi_adapter_bridge_stop();
}

int main(void)
{
    test_fill_info();
    test_submit_scan();
    test_submit_save_credentials();
    test_status_event_observes_snapshot();
    test_scan_event_completes_operation();
    return 0;
}
