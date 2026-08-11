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
