#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_adv_manager.h"
#include "ble_nimble_adv_start.h"

#define BLE_NIMBLE_ADV_TYPE_FLAGS 0x01U
#define BLE_NIMBLE_ADV_TYPE_COMPLETE_UUIDS128 0x07U
#define BLE_NIMBLE_ADV_TYPE_SHORT_NAME 0x08U
#define BLE_NIMBLE_ADV_FLAGS 0x06U

esp_err_t ble_nimble_adv_encode(
    const ble_port_adv_config_t *config,
    uint8_t output[BLE_NIMBLE_ADV_DATA_MAX_BYTES], size_t *output_len)
{
    if (config == NULL || output == NULL || output_len == NULL ||
            config->service_uuid == NULL ||
            (config->short_name_len > 0U && config->short_name == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t name_bytes = config->short_name_len > 0U
                              ? 2U + config->short_name_len : 0U;
    const size_t required = 3U + 18U + name_bytes;

    if (config->short_name_len > UINT8_MAX - 1U ||
            required > BLE_NIMBLE_ADV_DATA_MAX_BYTES)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t length = 0U;

    output[length++] = 2U;
    output[length++] = BLE_NIMBLE_ADV_TYPE_FLAGS;
    output[length++] = BLE_NIMBLE_ADV_FLAGS;
    output[length++] = 17U;
    output[length++] = BLE_NIMBLE_ADV_TYPE_COMPLETE_UUIDS128;
    memcpy(&output[length], config->service_uuid, 16U);
    length += 16U;
    if (config->short_name_len > 0U)
    {
        output[length++] = 1U + (uint8_t)config->short_name_len;
        output[length++] = BLE_NIMBLE_ADV_TYPE_SHORT_NAME;
        memcpy(&output[length], config->short_name, config->short_name_len);
        length += config->short_name_len;
    }
    *output_len = length;
    return ESP_OK;
}

static bool _ble_nimble_adv_start_current(
    uint32_t generation, bool bindable,
    const ble_nimble_adv_start_ops_t *ops)
{
    return ops->host_ready(ops->arg) &&
           ble_adv_manager_start_command_current(generation) &&
           (!bindable || ble_adv_manager_bindable_requested());
}

static void _ble_nimble_adv_start_rollback_gate(
    bool bindable, const ble_nimble_adv_start_ops_t *ops)
{
    if (bindable)
    {
        (void)ops->set_pairing_gate(false, ops->arg);
    }
}

int ble_nimble_adv_start_execute(
    uint32_t generation, bool bindable,
    const ble_nimble_adv_start_ops_t *ops)
{
    if (generation == 0U || ops == NULL || ops->host_ready == NULL ||
            ops->set_pairing_gate == NULL || ops->start == NULL ||
            ops->stop == NULL)
    {
        return (int)ESP_ERR_INVALID_ARG;
    }
    if (!_ble_nimble_adv_start_current(generation, bindable, ops))
    {
        return (int)ESP_ERR_INVALID_STATE;
    }
    const esp_err_t gate_result =
        ops->set_pairing_gate(bindable, ops->arg);

    if (gate_result != ESP_OK)
    {
        /* A timed-out host event may still have applied the open request. */
        _ble_nimble_adv_start_rollback_gate(bindable, ops);
        return (int)gate_result;
    }
    if (!_ble_nimble_adv_start_current(generation, bindable, ops))
    {
        /* The gate wait deliberately runs without the manager lock. A lease
         * cancellation or RESET can therefore retire this command while the
         * persistent host event is in flight. Close the gate before returning
         * even when the failed STOP retained the bindable lease. */
        _ble_nimble_adv_start_rollback_gate(bindable, ops);
        return (int)ESP_ERR_INVALID_STATE;
    }
    const int result = ops->start(ops->arg);

    if (result != 0)
    {
        _ble_nimble_adv_start_rollback_gate(bindable, ops);
        return result;
    }
    if (!_ble_nimble_adv_start_current(generation, bindable, ops))
    {
        /* The physical host call is another concurrency boundary. Retire a
         * successful but stale start locally; the manager's queued STOP still
         * owns eventual convergence and its completion identity. */
        (void)ops->stop(ops->arg);
        _ble_nimble_adv_start_rollback_gate(bindable, ops);
        return (int)ESP_ERR_INVALID_STATE;
    }
    return 0;
}
