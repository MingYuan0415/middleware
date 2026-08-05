#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_runtime.h"

#define DBG_TAG "ble_runtime"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

typedef enum
{
    BLE_RUNTIME_TEARDOWN_NONE = 0,
    BLE_RUNTIME_TEARDOWN_STOPPED,
    BLE_RUNTIME_TEARDOWN_DEINIT_PENDING,
} ble_runtime_teardown_stage_t;

typedef struct ble_runtime
{
    const ble_runtime_config_t *config;
    ble_runtime_state_t state;
    esp_err_t fault_error;
    bool port_initialized;
    ble_runtime_teardown_stage_t teardown_stage;
    bool initialized;
} ble_runtime_t;

static ble_runtime_t s_runtime;

ble_runtime_state_t ble_runtime_get_state(void)
{
    return s_runtime.state;
}

static void _ble_runtime_enter_faulted(esp_err_t error)
{
    s_runtime.state = BLE_RUNTIME_STATE_FAULTED;
    s_runtime.fault_error = error;
}

static void _ble_runtime_teardown_reset(void)
{
    s_runtime.port_initialized = false;
    s_runtime.teardown_stage = BLE_RUNTIME_TEARDOWN_NONE;
}

static esp_err_t _ble_runtime_port_teardown(void)
{
    esp_err_t result = ESP_OK;

    if (!s_runtime.port_initialized)
    {
        return ESP_OK;
    }
    if (s_runtime.teardown_stage == BLE_RUNTIME_TEARDOWN_NONE)
    {
        result = s_runtime.config->port->stop();
        if (result != ESP_OK)
        {
            return result;
        }
        s_runtime.teardown_stage = BLE_RUNTIME_TEARDOWN_STOPPED;
    }
    result = s_runtime.config->port->deinit();
    if (result != ESP_OK)
    {
        s_runtime.teardown_stage = BLE_RUNTIME_TEARDOWN_DEINIT_PENDING;
        return result;
    }
    _ble_runtime_teardown_reset();
    return ESP_OK;
}

esp_err_t ble_runtime_init(const ble_runtime_config_t *config)
{
    if (config == NULL || config->port == NULL || config->port->init == NULL ||
            config->port->start == NULL || config->port->stop == NULL ||
            config->port->deinit == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime.config = config;
    s_runtime.state = BLE_RUNTIME_STATE_STOPPED;
    s_runtime.fault_error = ESP_OK;
    _ble_runtime_teardown_reset();
    s_runtime.initialized = true;
    return ESP_OK;
}

esp_err_t ble_runtime_start(void)
{
    esp_err_t result;

    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.state != BLE_RUNTIME_STATE_STOPPED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime.state = BLE_RUNTIME_STATE_STARTING;
    result = s_runtime.config->port->init();
    if (result != ESP_OK)
    {
        _ble_runtime_teardown_reset();
        _ble_runtime_enter_faulted(result);
        return result;
    }
    s_runtime.port_initialized = true;
    result = s_runtime.config->port->start();
    if (result != ESP_OK)
    {
        const esp_err_t rollback_result = _ble_runtime_port_teardown();
        if (rollback_result != ESP_OK)
        {
            _ble_runtime_enter_faulted(rollback_result);
        }
        else
        {
            s_runtime.state = BLE_RUNTIME_STATE_STOPPED;
        }
        return result;
    }
    s_runtime.state = BLE_RUNTIME_STATE_RUNNING;
    return ESP_OK;
}

esp_err_t ble_runtime_stop(void)
{
    esp_err_t result;

    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.state == BLE_RUNTIME_STATE_FAULTED)
    {
        result = _ble_runtime_port_teardown();
        if (result != ESP_OK)
        {
            _ble_runtime_enter_faulted(result);
            return result;
        }
        s_runtime.state = BLE_RUNTIME_STATE_STOPPED;
        s_runtime.fault_error = ESP_OK;
        return ESP_OK;
    }
    if (s_runtime.state != BLE_RUNTIME_STATE_RUNNING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime.state = BLE_RUNTIME_STATE_STOPPING;
    result = _ble_runtime_port_teardown();
    if (result != ESP_OK)
    {
        _ble_runtime_enter_faulted(result);
        return result;
    }
    s_runtime.state = BLE_RUNTIME_STATE_STOPPED;
    return ESP_OK;
}

esp_err_t ble_runtime_deinit(void)
{
    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.state != BLE_RUNTIME_STATE_STOPPED &&
            s_runtime.state != BLE_RUNTIME_STATE_FAULTED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.port_initialized)
    {
        const esp_err_t teardown_result = _ble_runtime_port_teardown();
        if (teardown_result != ESP_OK)
        {
            _ble_runtime_enter_faulted(teardown_result);
            return teardown_result;
        }
    }
    s_runtime.initialized = false;
    s_runtime.config = NULL;
    s_runtime.state = BLE_RUNTIME_STATE_STOPPED;
    s_runtime.fault_error = ESP_OK;
    return ESP_OK;
}
