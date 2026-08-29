#ifndef __BLE_NIMBLE_ADV_START_H__
#define __BLE_NIMBLE_ADV_START_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_port_ops.h"

#define BLE_NIMBLE_ADV_DATA_MAX_BYTES 31U

typedef struct ble_nimble_adv_start_ops
{
    bool (*host_ready)(void *arg);
    esp_err_t (*set_pairing_gate)(bool open, void *arg);
    int (*start)(void *arg);
    int (*stop)(void *arg);
    void *arg;
} ble_nimble_adv_start_ops_t;

/**
 * @brief Encode the frozen Device Link v1 legacy advertising payload.
 *
 * Bindable is intentionally ignored because it controls only the pairing
 * gate. The output contains Flags, the complete 128-bit service UUID, and an
 * optional shortened local name.
 *
 * @param[in] config Advertising target.
 * @param[out] output Legacy advertising buffer.
 * @param[out] output_len Encoded byte count.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_SIZE.
 */
esp_err_t ble_nimble_adv_encode(
    const ble_port_adv_config_t *config,
    uint8_t output[BLE_NIMBLE_ADV_DATA_MAX_BYTES], size_t *output_len);

int ble_nimble_adv_start_execute(
    uint32_t generation, bool bindable,
    const ble_nimble_adv_start_ops_t *ops);

#endif /* __BLE_NIMBLE_ADV_START_H__ */
