#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "connectivity_manager.h"
#include "device_link_v1.h"
#include "device_link_wifi_adapter.h"
#include "device_link_service_test_hooks.h"
#include "event_bus.h"

extern void connectivity_manager_fake_reset(void);
extern void connectivity_manager_fake_set_status(
    const connectivity_manager_status_snapshot_t *snapshot);
extern void connectivity_manager_fake_set_scan(
    const connectivity_manager_scan_snapshot_t *snapshot);
extern void connectivity_manager_fake_set_request_result(esp_err_t result);
extern void connectivity_manager_fake_set_request_hook(
    void (*hook)(connectivity_manager_operation_id_t, void *), void *arg);
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
extern device_link_v1_wifi_failure_t ble_link_service_fake_last_failure(void);
extern void ble_link_service_fake_set_complete_result(esp_err_t result);
extern uint8_t ble_link_service_fake_last_network_count(void);
extern const device_link_v1_network_t *ble_link_service_fake_last_networks(void);

typedef struct early_result
{
    pthread_barrier_t barrier;
    pthread_t publisher;
    bool blocked;
    bool joined;
    device_link_v1_operation_t operation;
    connectivity_manager_operation_id_t manager_id;
} early_result_t;

static _Thread_local early_result_t *s_lock_probe;

BaseType_t __real_xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore,
        TickType_t timeout);

BaseType_t __wrap_xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore,
        TickType_t timeout)
{
    early_result_t *probe = s_lock_probe;

    if (probe == NULL)
    {
        return __real_xSemaphoreTakeRecursive(semaphore, timeout);
    }
    s_lock_probe = NULL;
    const BaseType_t acquired = __real_xSemaphoreTakeRecursive(semaphore, 0U);

    probe->blocked = acquired != pdTRUE;
    const int result = pthread_barrier_wait(&probe->barrier);

    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    return acquired == pdTRUE ? acquired :
           __real_xSemaphoreTakeRecursive(semaphore, timeout);
}

static void _publish_terminal(device_link_v1_operation_t operation,
                              connectivity_manager_operation_id_t manager_id,
                              uint64_t generation)
{
    if (operation == DEVICE_LINK_V1_OPERATION_SCAN)
    {
        const connectivity_manager_scan_snapshot_t scan =
        {
            .generation = generation,
            .operation_id = manager_id,
        };

        event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                               CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                               &scan, sizeof(scan));
    }
    else
    {
        const connectivity_manager_status_snapshot_t status =
        {
            .generation = generation,
            .operation_id = manager_id,
            .operation_complete = true,
            .state = CONNECTIVITY_MANAGER_STATE_IDLE,
            .available = true,
            .radio_available = true,
        };

        event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                               CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                               &status, sizeof(status));
    }
}

static void *_early_publisher(void *arg)
{
    early_result_t *probe = arg;

    s_lock_probe = probe;
    _publish_terminal(probe->operation, probe->manager_id, 10U);
    return NULL;
}

static void _publish_before_request_returns(
    connectivity_manager_operation_id_t manager_id, void *arg)
{
    early_result_t *probe = arg;

    probe->manager_id = manager_id;
    assert(pthread_create(&probe->publisher, NULL, _early_publisher, probe) == 0);
    const int result = pthread_barrier_wait(&probe->barrier);

    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    if (!probe->blocked)
    {
        /* Without the submit lock, force publication to finish before the
         * request returns. This deterministically reproduces the lost ID. */
        assert(pthread_join(probe->publisher, NULL) == 0);
        probe->joined = true;
    }
}

static void test_request_mapping_is_atomic_for_all_operations(void)
{
    const device_link_v1_operation_t operations[] =
    {
        DEVICE_LINK_V1_OPERATION_SCAN,
        DEVICE_LINK_V1_OPERATION_SET_CREDENTIALS,
        DEVICE_LINK_V1_OPERATION_CONNECT,
        DEVICE_LINK_V1_OPERATION_DISCONNECT,
        DEVICE_LINK_V1_OPERATION_FORGET,
    };
    const device_link_v1_credentials_t credentials =
    {
        .ssid = "cafe",
        .ssid_length = 4U,
        .security = DEVICE_LINK_V1_WIFI_OPEN,
    };

    for (size_t i = 0U; i < sizeof(operations) / sizeof(operations[0]); ++i)
    {
        early_result_t probe = {.operation = operations[i]};

        connectivity_manager_fake_reset();
        ble_link_service_fake_reset();
        assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
        assert(pthread_barrier_init(&probe.barrier, NULL, 2U) == 0);
        connectivity_manager_fake_set_request_hook(
            _publish_before_request_returns, &probe);
        assert(device_link_wifi_adapter_submit(operations[i], &credentials,
                                               42U, NULL) == DEVICE_LINK_V1_STATUS_ACCEPTED);
        if (!probe.joined)
        {
            assert(pthread_join(probe.publisher, NULL) == 0);
        }
        assert(ble_link_service_fake_complete_count() == 1U);
        assert(ble_link_service_fake_last_complete_id() == 42U);
        assert(probe.blocked);
        connectivity_manager_fake_set_request_hook(NULL, NULL);
        _publish_terminal(operations[i], probe.manager_id, 11U);
        assert(ble_link_service_fake_complete_count() == 1U);
        assert(pthread_barrier_destroy(&probe.barrier) == 0);
        device_link_wifi_adapter_bridge_stop();
    }
}

static void test_failed_and_retired_requests_do_not_replace_mapping(void)
{
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(DEVICE_LINK_V1_OPERATION_SCAN,
                                           NULL, 31U, NULL) == DEVICE_LINK_V1_STATUS_ACCEPTED);
    connectivity_manager_fake_set_request_result(ESP_FAIL);
    assert(device_link_wifi_adapter_submit(DEVICE_LINK_V1_OPERATION_DISCONNECT,
                                           NULL, 32U, NULL) == DEVICE_LINK_V1_STATUS_INTERNAL);
    _publish_terminal(DEVICE_LINK_V1_OPERATION_DISCONNECT, 2U, 10U);
    assert(ble_link_service_fake_complete_count() == 0U);
    _publish_terminal(DEVICE_LINK_V1_OPERATION_SCAN, 1U, 10U);
    assert(ble_link_service_fake_complete_count() == 1U);
    assert(ble_link_service_fake_last_complete_id() == 31U);

    connectivity_manager_fake_set_request_result(ESP_OK);
    assert(device_link_wifi_adapter_submit(DEVICE_LINK_V1_OPERATION_SCAN,
                                           NULL, 33U, NULL) == DEVICE_LINK_V1_STATUS_ACCEPTED);
    assert(device_link_wifi_adapter_submit(DEVICE_LINK_V1_OPERATION_SCAN,
                                           NULL, 34U, NULL) == DEVICE_LINK_V1_STATUS_ACCEPTED);
    _publish_terminal(DEVICE_LINK_V1_OPERATION_SCAN, 2U, 11U);
    assert(ble_link_service_fake_complete_count() == 1U);
    _publish_terminal(DEVICE_LINK_V1_OPERATION_SCAN, 3U, 12U);
    _publish_terminal(DEVICE_LINK_V1_OPERATION_SCAN, 3U, 13U);
    assert(ble_link_service_fake_complete_count() == 2U);
    assert(ble_link_service_fake_last_complete_id() == 34U);
    device_link_wifi_adapter_bridge_stop();
}

static void test_fill_info(void)
{
    device_link_v1_info_t info;

    memset(&info, 0xff, sizeof(info));
    device_link_wifi_adapter_set_firmware(0U, 1U, 0U);
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

    /* A failed delivery retains the first terminal payload, even if a later
     * backend snapshot reports another outcome for the same operation. */
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(DEVICE_LINK_V1_OPERATION_SCAN,
                                           NULL, 12U, NULL) == DEVICE_LINK_V1_STATUS_ACCEPTED);
    scan.operation_id = 2U;
    scan.generation = 3U;
    ble_link_service_fake_set_complete_result(ESP_FAIL);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT, &scan, sizeof(scan));
    ble_link_service_fake_set_complete_result(ESP_OK);
    scan.generation = 4U;
    scan.last_error = ESP_ERR_TIMEOUT;
    memcpy(scan.records[0].ssid, "evil", 4U);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT, &scan, sizeof(scan));
    assert(ble_link_service_fake_last_complete_id() == 12U);
    assert(ble_link_service_fake_last_failure() == DEVICE_LINK_V1_WIFI_FAILURE_NONE);
    assert(ble_link_service_fake_last_network_count() == 1U);
    const device_link_v1_network_t *network = ble_link_service_fake_last_networks();
    assert(network[0].ssid_length == 4U && memcmp(network[0].ssid, "cafe", 4U) == 0);
    device_link_wifi_adapter_bridge_stop();
}

static void test_status_event_completes_save(void)
{
    device_link_v1_credentials_t credentials;
    connectivity_manager_status_snapshot_t status;

    memset(&credentials, 0, sizeof(credentials));
    memcpy(credentials.ssid, "cafe", 4U);
    credentials.ssid_length = 4U;
    memcpy(credentials.password, "password", 8U);
    credentials.password_length = 8U;
    credentials.security = DEVICE_LINK_V1_WIFI_PERSONAL;
    memset(&status, 0, sizeof(status));
    status.generation = 5U;
    status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    status.available = true;
    status.radio_available = true;
    status.saved_profile = true;
    status.profile_persisted = true;
    status.operation_complete = true;
    status.operation_id = 1U;
    memcpy(status.ssid, "cafe", 4U);
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_SET_CREDENTIALS, &credentials, 9U,
               NULL) == DEVICE_LINK_V1_STATUS_ACCEPTED);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_complete_count() == 1U);
    assert(ble_link_service_fake_last_complete_id() == 9U);
    device_link_wifi_adapter_bridge_stop();
}

static void test_canceled_connect_completes_timeout(void)
{
    connectivity_manager_status_snapshot_t status;

    memset(&status, 0, sizeof(status));
    status.generation = 6U;
    status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    status.available = true;
    status.radio_available = true;
    status.saved_profile = true;
    status.operation_complete = true;
    status.operation_canceled = true;
    status.operation_id = 1U;
    status.last_error = ESP_ERR_NOT_FINISHED;
    memcpy(status.ssid, "cafe", 4U);
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_CONNECT, NULL, 21U, NULL) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_complete_count() == 1U);
    assert(ble_link_service_fake_last_failure() ==
           DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT);
    device_link_wifi_adapter_bridge_stop();
}

static void test_scan_timeout_maps_timeout(void)
{
    connectivity_manager_scan_snapshot_t scan;

    memset(&scan, 0, sizeof(scan));
    scan.generation = 3U;
    scan.operation_id = 1U;
    scan.last_error = ESP_ERR_TIMEOUT;
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    connectivity_manager_fake_set_scan(&scan);
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_SCAN, NULL, 22U, NULL) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                           &scan, sizeof(scan));
    assert(ble_link_service_fake_complete_count() == 1U);
    assert(ble_link_service_fake_last_failure() ==
           DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT);
    device_link_wifi_adapter_bridge_stop();
}

static void test_complete_failure_keeps_bridge_id(void)
{
    connectivity_manager_status_snapshot_t status;

    memset(&status, 0, sizeof(status));
    status.generation = 7U;
    status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    status.available = true;
    status.radio_available = true;
    status.saved_profile = true;
    status.operation_complete = true;
    status.operation_id = 1U;
    memcpy(status.ssid, "cafe", 4U);
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    ble_link_service_fake_set_complete_result(ESP_ERR_INVALID_ARG);
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_DISCONNECT, NULL, 23U, NULL) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_complete_count() == 1U);
    ble_link_service_fake_set_complete_result(ESP_OK);
    status.generation = 8U;
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_complete_count() == 2U);
    assert(ble_link_service_fake_last_complete_id() == 23U);
    device_link_wifi_adapter_bridge_stop();
}

static void test_terminal_completion_retries_on_owner_tick(void)
{
    connectivity_manager_status_snapshot_t status;

    memset(&status, 0, sizeof(status));
    status.generation = 7U;
    status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    status.available = true;
    status.radio_available = true;
    status.operation_complete = true;
    status.operation_id = 1U;
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    ble_link_service_fake_set_complete_result(ESP_ERR_INVALID_STATE);
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_DISCONNECT, NULL, 24U, NULL) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_complete_count() == 1U);
    ble_link_service_fake_set_complete_result(ESP_OK);
    device_link_wifi_adapter_tick();
    assert(ble_link_service_fake_complete_count() == 2U);
    assert(ble_link_service_fake_last_complete_id() == 24U);
    device_link_wifi_adapter_tick();
    assert(ble_link_service_fake_complete_count() == 2U);
    device_link_wifi_adapter_bridge_stop();
}

static void test_nonterminal_status_keeps_pending_terminal(void)
{
    connectivity_manager_status_snapshot_t status;

    memset(&status, 0, sizeof(status));
    status.generation = 7U;
    status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    status.available = true;
    status.radio_available = true;
    status.operation_complete = true;
    status.operation_id = 1U;
    status.operation_canceled = true;
    status.last_error = ESP_ERR_NOT_FINISHED;
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    ble_link_service_fake_set_complete_result(ESP_ERR_INVALID_STATE);
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_CONNECT, NULL, 25U, NULL) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_complete_count() == 1U);

    status.generation = 8U;
    status.operation_complete = false;
    status.operation_canceled = false;
    status.last_error = ESP_OK;
    status.state = CONNECTIVITY_MANAGER_STATE_CONNECTING;
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_complete_count() == 2U);
    assert(ble_link_service_fake_last_failure() ==
           DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT);
    status.generation = 9U;
    status.operation_complete = true;
    status.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_complete_count() == 3U);
    assert(ble_link_service_fake_last_failure() == DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT);
    assert(ble_link_service_fake_last_snapshot()->state == DEVICE_LINK_V1_WIFI_IDLE);
    ble_link_service_fake_set_complete_result(ESP_OK);
    device_link_wifi_adapter_tick();
    assert(ble_link_service_fake_complete_count() == 4U);
    assert(ble_link_service_fake_last_failure() ==
           DEVICE_LINK_V1_WIFI_FAILURE_TIMEOUT);
    device_link_wifi_adapter_bridge_stop();
}

static void test_bridge_stop_drops_terminal_retry(void)
{
    connectivity_manager_status_snapshot_t status;

    memset(&status, 0, sizeof(status));
    status.generation = 7U;
    status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    status.available = true;
    status.radio_available = true;
    status.operation_complete = true;
    status.operation_id = 1U;
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    ble_link_service_fake_set_complete_result(ESP_ERR_INVALID_STATE);
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_DISCONNECT, NULL, 26U, NULL) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_complete_count() == 1U);
    device_link_wifi_adapter_bridge_stop();
    ble_link_service_fake_set_complete_result(ESP_OK);
    device_link_wifi_adapter_tick();
    assert(ble_link_service_fake_complete_count() == 1U);
    device_link_wifi_adapter_bridge_stop();
}

static void test_missing_engine_operation_drops_terminal(void)
{
    connectivity_manager_status_snapshot_t status;

    memset(&status, 0, sizeof(status));
    status.generation = 7U;
    status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    status.available = true;
    status.radio_available = true;
    status.operation_complete = true;
    status.operation_id = 1U;
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    ble_link_service_fake_set_complete_result(ESP_ERR_NOT_FOUND);
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(device_link_wifi_adapter_submit(
               DEVICE_LINK_V1_OPERATION_DISCONNECT, NULL, 27U, NULL) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_complete_count() == 1U);
    ble_link_service_fake_set_complete_result(ESP_OK);
    device_link_wifi_adapter_tick();
    assert(ble_link_service_fake_complete_count() == 1U);
    device_link_wifi_adapter_bridge_stop();
}

static void test_bridge_start_seeds_saved_profile(void)
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
    connectivity_manager_fake_set_status(&status);
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(ble_link_service_fake_observe_count() >= 1U);
    assert(ble_link_service_fake_last_snapshot()->state ==
           DEVICE_LINK_V1_WIFI_CONNECTED);
    assert(ble_link_service_fake_last_snapshot()->profile_ssid_length == 4U);
    assert(memcmp(ble_link_service_fake_last_snapshot()->profile_ssid,
                  "cafe", 4U) == 0);
    device_link_wifi_adapter_bridge_stop();
}

static void test_bridge_start_seeds_generation_zero(void)
{
    connectivity_manager_status_snapshot_t status;
    unsigned observe_count;

    memset(&status, 0, sizeof(status));
    status.generation = 0U;
    status.state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
    status.failure = CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
    status.available = false;
    status.radio_available = false;
    status.saved_profile = true;
    status.profile_persisted = true;
    memcpy(status.ssid, "cafe", 4U);
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    connectivity_manager_fake_set_status(&status);
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    observe_count = ble_link_service_fake_observe_count();
    assert(observe_count >= 1U);
    assert(ble_link_service_fake_last_snapshot()->state ==
           DEVICE_LINK_V1_WIFI_UNAVAILABLE);
    assert(ble_link_service_fake_last_snapshot()->failure ==
           DEVICE_LINK_V1_WIFI_FAILURE_RADIO);
    assert(ble_link_service_fake_last_snapshot()->profile_ssid_length == 4U);
    assert(memcmp(ble_link_service_fake_last_snapshot()->profile_ssid,
                  "cafe", 4U) == 0);
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_observe_count() == observe_count);
    status.generation = 1U;
    status.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    status.available = true;
    status.radio_available = true;
    event_bus_fake_publish(CONNECTIVITY_MANAGER_MSG,
                           CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           &status, sizeof(status));
    assert(ble_link_service_fake_last_snapshot()->state ==
           DEVICE_LINK_V1_WIFI_CONNECTED);
    device_link_wifi_adapter_bridge_stop();
}

static void test_radio_unavailable_keeps_saved_profile(void)
{
    connectivity_manager_status_snapshot_t status;

    memset(&status, 0, sizeof(status));
    status.generation = 5U;
    status.state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
    status.failure = CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
    status.available = true;
    status.radio_available = false;
    status.saved_profile = true;
    status.profile_persisted = true;
    memcpy(status.ssid, "cafe", 4U);
    connectivity_manager_fake_reset();
    ble_link_service_fake_reset();
    connectivity_manager_fake_set_status(&status);
    assert(device_link_wifi_adapter_bridge_start() == ESP_OK);
    assert(ble_link_service_fake_last_snapshot()->state ==
           DEVICE_LINK_V1_WIFI_UNAVAILABLE);
    assert(ble_link_service_fake_last_snapshot()->failure ==
           DEVICE_LINK_V1_WIFI_FAILURE_RADIO);
    assert(ble_link_service_fake_last_snapshot()->profile_ssid_length == 4U);
    assert(memcmp(ble_link_service_fake_last_snapshot()->profile_ssid,
                  "cafe", 4U) == 0);
    device_link_wifi_adapter_bridge_stop();
}

int main(void)
{
    test_request_mapping_is_atomic_for_all_operations();
    test_failed_and_retired_requests_do_not_replace_mapping();
    test_fill_info();
    test_submit_scan();
    test_submit_save_credentials();
    test_status_event_observes_snapshot();
    test_scan_event_completes_operation();
    test_status_event_completes_save();
    test_canceled_connect_completes_timeout();
    test_scan_timeout_maps_timeout();
    test_complete_failure_keeps_bridge_id();
    test_terminal_completion_retries_on_owner_tick();
    test_nonterminal_status_keeps_pending_terminal();
    test_bridge_stop_drops_terminal_retry();
    test_missing_engine_operation_drops_terminal();
    test_bridge_start_seeds_saved_profile();
    test_bridge_start_seeds_generation_zero();
    test_radio_unavailable_keeps_saved_profile();
    puts("adapter recovery: immutable status/scan retry, new-slot isolation, bridge retirement passed");
    return 0;
}
