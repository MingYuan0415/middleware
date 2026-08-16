#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "device_link_operation.h"

static bool _terminal(device_link_operation_state_t state)
{
    return state == DEVICE_LINK_OPERATION_SUCCEEDED ||
           state == DEVICE_LINK_OPERATION_FAILED ||
           state == DEVICE_LINK_OPERATION_CANCELED;
}

static device_link_operation_t *_find(
    device_link_operation_table_t *table, uint64_t operation_id)
{
    for (size_t i = 0U; i < DEVICE_LINK_MAX_OPERATIONS; ++i)
    {
        if (table->slots[i].id == operation_id)
        {
            return &table->slots[i];
        }
    }
    return NULL;
}

esp_err_t device_link_operation_find_by_owner(
    device_link_operation_table_t *table, uint64_t owner_id,
    device_link_operation_t *out)
{
    if (table == NULL || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < DEVICE_LINK_MAX_OPERATIONS; ++i)
    {
        const device_link_operation_t *operation = &table->slots[i];

        if (operation->id != 0U && operation->owner_id == owner_id &&
                !_terminal(operation->state))
        {
            *out = *operation;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void device_link_operation_sweep(
    device_link_operation_table_t *table, uint64_t now_ms)
{
    if (table == NULL)
    {
        return;
    }
    for (size_t i = 0U; i < DEVICE_LINK_MAX_OPERATIONS; ++i)
    {
        device_link_operation_t *slot = &table->slots[i];

        if (slot->id != 0U && _terminal(slot->state) &&
                now_ms >= slot->terminal_at_ms &&
                now_ms - slot->terminal_at_ms >=
                DEVICE_LINK_OPERATION_RETENTION_MS)
        {
            memset(slot, 0, sizeof(*slot));
        }
    }
}

esp_err_t device_link_operation_table_init(
    device_link_operation_table_t *table, uint64_t boot_id)
{
    if (table == NULL || boot_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(table, 0, sizeof(*table));
    table->boot_id = boot_id;
    table->next_id = 1U;
    return ESP_OK;
}

esp_err_t device_link_operation_start(
    device_link_operation_table_t *table, uint64_t now_ms,
    uint8_t domain_id, uint8_t method_id, uint64_t owner_id,
    device_link_operation_cancel_t cancel, void *cancel_arg,
    uint64_t *operation_id)
{
    if (table == NULL || table->boot_id == 0U ||
            domain_id == DEVICE_LINK_DOMAIN_INVALID || method_id == 0U ||
            owner_id == 0U || operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    device_link_operation_sweep(table, now_ms);
    device_link_operation_t *slot = NULL;

    for (size_t i = 0U; i < DEVICE_LINK_MAX_OPERATIONS; ++i)
    {
        if (table->slots[i].id == 0U)
        {
            slot = &table->slots[i];
            break;
        }
    }
    if (slot == NULL || table->next_id == 0U)
    {
        return ESP_ERR_NO_MEM;
    }
    memset(slot, 0, sizeof(*slot));
    slot->id = table->next_id++;
    slot->owner_id = owner_id;
    slot->domain_id = domain_id;
    slot->method_id = method_id;
    slot->state = DEVICE_LINK_OPERATION_PENDING;
    slot->status = DEVICE_LINK_STATUS_OK;
    slot->cancel = cancel;
    slot->cancel_arg = cancel_arg;
    *operation_id = slot->id;
    return ESP_OK;
}

esp_err_t device_link_operation_update(
    device_link_operation_table_t *table, uint64_t now_ms,
    uint64_t operation_id, device_link_operation_state_t state,
    device_link_status_t status, const uint8_t *result, size_t result_len)
{
    if (table == NULL || operation_id == 0U ||
            state < DEVICE_LINK_OPERATION_PENDING ||
            state > DEVICE_LINK_OPERATION_CANCELED ||
            status < DEVICE_LINK_STATUS_OK ||
            status > DEVICE_LINK_STATUS_INTERNAL ||
            (result == NULL && result_len != 0U) ||
            result_len > DEVICE_LINK_OPERATION_RESULT_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* Frozen operation-state semantics (core v2 OperationStatus): in-flight
     * states report OK, SUCCEEDED reports OK and may carry the declared
     * result payload, FAILED requires a non-OK error, and non-success
     * terminal records never carry a result payload. */
    const bool in_flight = state == DEVICE_LINK_OPERATION_PENDING ||
                           state == DEVICE_LINK_OPERATION_RUNNING;
    const bool non_success_terminal =
        state == DEVICE_LINK_OPERATION_FAILED ||
        state == DEVICE_LINK_OPERATION_CANCELED;

    if ((in_flight && status != DEVICE_LINK_STATUS_OK) ||
            (state == DEVICE_LINK_OPERATION_SUCCEEDED &&
             status != DEVICE_LINK_STATUS_OK) ||
            (state == DEVICE_LINK_OPERATION_FAILED &&
             status == DEVICE_LINK_STATUS_OK) ||
            (non_success_terminal && result_len != 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    device_link_operation_t *operation = _find(table, operation_id);

    if (operation == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (_terminal(operation->state) ||
            (operation->state == DEVICE_LINK_OPERATION_RUNNING &&
             state == DEVICE_LINK_OPERATION_PENDING))
    {
        return ESP_ERR_INVALID_STATE;
    }
    operation->state = state;
    operation->status = status;
    operation->result_len = result_len;
    if (result_len != 0U)
    {
        memcpy(operation->result, result, result_len);
    }
    if (_terminal(state))
    {
        operation->terminal_at_ms = now_ms;
    }
    return ESP_OK;
}

esp_err_t device_link_operation_get(
    device_link_operation_table_t *table, uint64_t now_ms,
    uint64_t operation_id, const device_link_operation_t **operation)
{
    if (table == NULL || operation_id == 0U || operation == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *operation = NULL;
    device_link_operation_sweep(table, now_ms);
    device_link_operation_t *found = _find(table, operation_id);

    if (found == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *operation = found;
    return ESP_OK;
}

esp_err_t device_link_operation_cancel(
    device_link_operation_table_t *table, uint64_t now_ms,
    uint64_t operation_id)
{
    if (table == NULL || operation_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    device_link_operation_t *operation = _find(table, operation_id);

    if (operation == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (_terminal(operation->state))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (operation->cancel != NULL)
    {
        const esp_err_t result = operation->cancel(
                                     operation->owner_id,
                                     operation->cancel_arg);

        if (result != ESP_OK)
        {
            return result;
        }
    }
    return device_link_operation_update(
               table, now_ms, operation_id, DEVICE_LINK_OPERATION_CANCELED,
               DEVICE_LINK_STATUS_OK, NULL, 0U);
}
