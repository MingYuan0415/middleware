#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_link_service.h"

static uint64_t s_next_operation_id = 1U;
static unsigned s_update_count = 0U;
static uint64_t s_last_owner_id = 0U;
static device_link_operation_state_t s_last_state;
static device_link_status_t s_last_status;
static size_t s_last_result_len = 0U;

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

/* Test hooks. */
void ble_link_service_fake_reset(void)
{
    s_update_count = 0U;
    s_last_owner_id = 0U;
    s_last_result_len = 0U;
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
