#include "ble_nimble_store_guard.h"

#include <stddef.h>

void ble_nimble_store_guard_reset(ble_nimble_store_guard_t *guard)
{
    if (guard != NULL)
    {
        atomic_store_explicit(&guard->error, ESP_OK, memory_order_release);
    }
}

esp_err_t ble_nimble_store_guard_error(
    const ble_nimble_store_guard_t *guard)
{
    if (guard == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return (esp_err_t)atomic_load_explicit(&guard->error,
                                           memory_order_acquire);
}

void ble_nimble_store_guard_finish(
    ble_nimble_store_guard_t *guard, esp_err_t result)
{
    if (guard == NULL || result == ESP_OK)
    {
        return;
    }
    int expected = ESP_OK;

    (void)atomic_compare_exchange_strong_explicit(
        &guard->error, &expected, (int)result,
        memory_order_acq_rel, memory_order_acquire);
}

int ble_nimble_store_guard_record_write_result(
    ble_nimble_store_guard_t *guard, int result,
    int retryable_capacity_result)
{
    if (result != 0 && result != retryable_capacity_result)
    {
        ble_nimble_store_guard_finish(guard, ESP_FAIL);
    }
    return result;
}

esp_err_t ble_nimble_store_callback_guard_install(
    ble_nimble_store_callback_guard_t *guard,
    ble_nimble_store_write_callback_t **callback_slot,
    ble_nimble_store_write_callback_t *wrapper)
{
    if (guard == NULL || callback_slot == NULL || wrapper == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (guard->active || guard->original != NULL ||
            *callback_slot == wrapper)
    {
        return ESP_ERR_INVALID_STATE;
    }
    guard->active = true;
    if (*callback_slot != NULL)
    {
        guard->original = *callback_slot;
        *callback_slot = wrapper;
    }
    return ESP_OK;
}

esp_err_t ble_nimble_store_callback_guard_reconcile(
    ble_nimble_store_callback_guard_t *guard,
    ble_nimble_store_write_callback_t **callback_slot,
    ble_nimble_store_write_callback_t *wrapper)
{
    if (guard == NULL || callback_slot == NULL || wrapper == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!guard->active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (guard->original == NULL)
    {
        if (*callback_slot == NULL || *callback_slot == wrapper)
        {
            return ESP_ERR_INVALID_STATE;
        }
        guard->original = *callback_slot;
        *callback_slot = wrapper;
        return ESP_OK;
    }
    if (*callback_slot == wrapper)
    {
        return ESP_OK;
    }
    if (*callback_slot != guard->original)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *callback_slot = wrapper;
    return ESP_OK;
}

void ble_nimble_store_callback_guard_restore(
    ble_nimble_store_callback_guard_t *guard,
    ble_nimble_store_write_callback_t **callback_slot,
    ble_nimble_store_write_callback_t *wrapper)
{
    if (guard == NULL)
    {
        return;
    }
    if (guard->active && guard->original != NULL &&
            callback_slot != NULL && *callback_slot == wrapper)
    {
        *callback_slot = guard->original;
    }
    guard->original = NULL;
    guard->active = false;
}
