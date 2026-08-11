#ifndef __BLE_NIMBLE_ADV_START_H__
#define __BLE_NIMBLE_ADV_START_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct ble_nimble_adv_start_ops
{
    bool (*host_ready)(void *arg);
    esp_err_t (*set_pairing_gate)(bool open, void *arg);
    int (*start)(void *arg);
    int (*stop)(void *arg);
    void *arg;
} ble_nimble_adv_start_ops_t;

int ble_nimble_adv_start_execute(
    uint32_t generation, bool bindable,
    const ble_nimble_adv_start_ops_t *ops);

#endif /* __BLE_NIMBLE_ADV_START_H__ */
