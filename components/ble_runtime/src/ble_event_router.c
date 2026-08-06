#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_port_ops.h"

#define DBG_TAG "ble_event_router"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

#define BLE_EVENT_ROUTER_MAX_CONSUMERS 4U

typedef struct ble_event_router
{
    ble_port_event_cb_t callbacks[BLE_EVENT_ROUTER_MAX_CONSUMERS];
    void *args[BLE_EVENT_ROUTER_MAX_CONSUMERS];
    size_t count;
} ble_event_router_t;

static ble_event_router_t s_router;

void ble_event_router_init(void)
{
    memset(&s_router, 0, sizeof(s_router));
}

esp_err_t ble_event_router_register(
    ble_port_event_cb_t callback, void *arg)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < s_router.count; ++i)
    {
        if (s_router.callbacks[i] == callback && s_router.args[i] == arg)
        {
            return ESP_OK;
        }
    }
    if (s_router.count >= BLE_EVENT_ROUTER_MAX_CONSUMERS)
    {
        return ESP_ERR_NO_MEM;
    }
    s_router.callbacks[s_router.count] = callback;
    s_router.args[s_router.count] = arg;
    s_router.count++;
    return ESP_OK;
}

esp_err_t ble_event_router_unregister(
    ble_port_event_cb_t callback, void *arg)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0U; i < s_router.count; ++i)
    {
        if (s_router.callbacks[i] == callback && s_router.args[i] == arg)
        {
            for (size_t j = i; j + 1U < s_router.count; ++j)
            {
                s_router.callbacks[j] = s_router.callbacks[j + 1U];
                s_router.args[j] = s_router.args[j + 1U];
            }
            s_router.count--;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ble_event_router_dispatch(const ble_port_event_t *event)
{
    if (event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ble_port_event_cb_t callbacks[BLE_EVENT_ROUTER_MAX_CONSUMERS];
    void *args[BLE_EVENT_ROUTER_MAX_CONSUMERS];
    const size_t count = s_router.count;

    memcpy(callbacks, s_router.callbacks, count * sizeof(callbacks[0]));
    memcpy(args, s_router.args, count * sizeof(args[0]));
    for (size_t i = 0U; i < count; ++i)
    {
        callbacks[i](event, args[i]);
    }
    return ESP_OK;
}
