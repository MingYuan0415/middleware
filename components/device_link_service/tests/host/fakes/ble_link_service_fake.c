#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_link_service.h"

static uint64_t s_next_operation_id = 1U;
static unsigned s_update_count = 0U;
static unsigned s_defer_count = 0U;
static uint64_t s_last_owner_id = 0U;
static device_link_operation_state_t s_last_state;
static device_link_status_t s_last_status;
static size_t s_last_result_len = 0U;
static bool s_in_flight = false;
static esp_err_t s_start_result = ESP_OK;

esp_err_t ble_link_service_async_operation_start(
    uint8_t domain_id, uint8_t method_id, uint64_t owner_id,
    device_link_operation_cancel_t cancel, void *cancel_arg,
    uint64_t *out_operation_id)
{
    (void)domain_id;
    (void)method_id;
    (void)cancel;
    (void)cancel_arg;
    if (out_operation_id == NULL || owner_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_start_result != ESP_OK)
    {
        return s_start_result;
    }
    *out_operation_id = s_next_operation_id++;
    return ESP_OK;
}

esp_err_t ble_link_service_async_operation_update(
    uint64_t owner_id, device_link_operation_state_t state,
    device_link_status_t status, const uint8_t *result, size_t result_len)
{
    (void)result;
    if (owner_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_update_count++;
    s_last_owner_id = owner_id;
    s_last_state = state;
    s_last_status = status;
    s_last_result_len = result_len;
    return ESP_OK;
}

esp_err_t ble_link_service_async_operation_defer_update(
    uint64_t owner_id, device_link_operation_state_t state,
    device_link_status_t status, const uint8_t *result, size_t result_len)
{
    (void)owner_id;
    (void)state;
    (void)status;
    (void)result;
    (void)result_len;
    s_defer_count++;
    return ESP_OK;
}

bool ble_link_service_async_operation_in_flight(uint8_t domain_id)
{
    (void)domain_id;
    return s_in_flight;
}

/* Test hooks. */
void ble_link_service_fake_reset(void)
{
    s_update_count = 0U;
    s_defer_count = 0U;
    s_last_owner_id = 0U;
    s_last_result_len = 0U;
    s_in_flight = false;
    s_start_result = ESP_OK;
}

void ble_link_service_fake_set_in_flight(bool in_flight)
{
    s_in_flight = in_flight;
}

void ble_link_service_fake_set_start_result(esp_err_t result)
{
    s_start_result = result;
}

unsigned ble_link_service_fake_defer_count(void)
{
    return s_defer_count;
}

unsigned ble_link_service_fake_update_count(void)
{
    return s_update_count;
}

uint64_t ble_link_service_fake_last_owner(void)
{
    return s_last_owner_id;
}

device_link_operation_state_t ble_link_service_fake_last_state(void)
{
    return s_last_state;
}

device_link_status_t ble_link_service_fake_last_status(void)
{
    return s_last_status;
}

size_t ble_link_service_fake_last_result_len(void)
{
    return s_last_result_len;
}
