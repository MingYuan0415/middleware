#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ble_link_service.h"

static ble_link_v1_owner_ops_t s_ops;
static void *s_ops_arg;
static unsigned s_complete_count;
static uint32_t s_last_complete_id;
static device_link_v1_wifi_failure_t s_last_failure;
static unsigned s_observe_count;
static device_link_v1_snapshot_t s_last_snapshot;
static esp_err_t s_complete_result = ESP_OK;
static uint8_t s_last_network_count;
static device_link_v1_network_t s_last_networks[DEVICE_LINK_V1_MAX_SCAN_NETWORKS];

void ble_link_service_fake_reset(void)
{
    memset(&s_ops, 0, sizeof(s_ops));
    s_ops_arg = NULL;
    s_complete_count = 0U;
    s_last_complete_id = 0U;
    s_last_failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    s_observe_count = 0U;
    memset(&s_last_snapshot, 0, sizeof(s_last_snapshot));
    s_complete_result = ESP_OK;
    s_last_network_count = 0U;
    memset(s_last_networks, 0, sizeof(s_last_networks));
}

void ble_link_service_fake_set_complete_result(esp_err_t result)
{
    s_complete_result = result;
}

unsigned ble_link_service_fake_complete_count(void)
{
    return s_complete_count;
}

uint32_t ble_link_service_fake_last_complete_id(void)
{
    return s_last_complete_id;
}

device_link_v1_wifi_failure_t ble_link_service_fake_last_failure(void)
{
    return s_last_failure;
}

unsigned ble_link_service_fake_observe_count(void)
{
    return s_observe_count;
}

const device_link_v1_snapshot_t *ble_link_service_fake_last_snapshot(void)
{
    return &s_last_snapshot;
}

uint8_t ble_link_service_fake_last_network_count(void)
{
    return s_last_network_count;
}

const device_link_v1_network_t *ble_link_service_fake_last_networks(void)
{
    return s_last_networks;
}

void ble_link_service_set_v1_ops(const ble_link_v1_owner_ops_t *ops, void *arg)
{
    if (ops == NULL)
    {
        memset(&s_ops, 0, sizeof(s_ops));
        s_ops_arg = NULL;
        return;
    }
    s_ops = *ops;
    s_ops_arg = arg;
}

void ble_link_service_observe_snapshot(const device_link_v1_snapshot_t *snapshot)
{
    s_observe_count++;
    if (snapshot != NULL)
    {
        s_last_snapshot = *snapshot;
    }
}

esp_err_t ble_link_service_complete_operation(
    uint32_t operation_id, device_link_v1_wifi_failure_t failure,
    const device_link_v1_network_t *networks, uint8_t count,
    const device_link_v1_snapshot_t *snapshot)
{
    s_last_network_count = count;
    if (count > 0U)
    {
        memcpy(s_last_networks, networks, (size_t)count * sizeof(networks[0]));
    }
    s_complete_count++;
    s_last_complete_id = operation_id;
    s_last_failure = failure;
    if (snapshot != NULL)
    {
        s_last_snapshot = *snapshot;
    }
    return s_complete_result;
}

void ble_link_service_wake_owner(void)
{
}
