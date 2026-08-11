#include "ble_nimble_store_reset.h"

#include <stddef.h>

esp_err_t ble_nimble_store_reset_run(
    const ble_nimble_store_reset_ops_t *ops)
{
    if (ops == NULL || ops->erase_namespace == NULL ||
            ops->clear_runtime == NULL || ops->audit_empty == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ops->erase_namespace(ops->context);

    if (result != ESP_OK)
    {
        return result;
    }
    result = ops->clear_runtime(ops->context);
    if (result != ESP_OK)
    {
        return result;
    }
    result = ops->erase_namespace(ops->context);
    if (result != ESP_OK)
    {
        return result;
    }
    return ops->audit_empty(ops->context);
}
