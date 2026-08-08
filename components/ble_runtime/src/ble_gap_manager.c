#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_gap_manager.h"

#define DBG_TAG "ble_gap_manager"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

#define BLE_GAP_MANAGER_MAX_SUBSCRIBED 32U

typedef struct ble_gap_manager
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_admission_cb_t admission_cb;
    void *admission_arg;
    uint16_t subscribed_handles[BLE_GAP_MANAGER_MAX_SUBSCRIBED];
    size_t subscribed_count;
} ble_gap_manager_t;

static ble_gap_manager_t s_manager;

void ble_gap_manager_init(void)
{
    memset(&s_manager, 0, sizeof(s_manager));
    s_manager.snapshot.mtu = 23U;
}

void ble_gap_manager_set_admission_cb(
    ble_gap_manager_admission_cb_t callback, void *arg)
{
    s_manager.admission_cb = callback;
    s_manager.admission_arg = arg;
}

static bool _ble_gap_manager_admit_new_connection(void)
{
    if (s_manager.snapshot.connected)
    {
        return false;
    }
    if (s_manager.admission_cb != NULL)
    {
        return s_manager.admission_cb(s_manager.admission_arg);
    }
    return true;
}

static void _ble_gap_manager_reset_subscriptions(void)
{
    s_manager.subscribed_count = 0U;
    s_manager.snapshot.subscribed = false;
}

static void _ble_gap_manager_set_subscription(
    uint16_t attr_handle, bool subscribed)
{
    for (size_t i = 0U; i < s_manager.subscribed_count; ++i)
    {
        if (s_manager.subscribed_handles[i] == attr_handle)
        {
            if (!subscribed)
            {
                s_manager.subscribed_handles[i] =
                    s_manager.subscribed_handles[
                        s_manager.subscribed_count - 1U];
                s_manager.subscribed_count--;
            }
            s_manager.snapshot.subscribed = s_manager.subscribed_count > 0U;
            return;
        }
    }
    if (subscribed &&
            s_manager.subscribed_count < BLE_GAP_MANAGER_MAX_SUBSCRIBED)
    {
        s_manager.subscribed_handles[s_manager.subscribed_count] =
            attr_handle;
        s_manager.subscribed_count++;
        s_manager.snapshot.subscribed = true;
    }
}

esp_err_t ble_gap_manager_handle_event(
    const ble_gap_manager_event_t *event)
{
    if (event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    switch (event->type)
    {
    case BLE_GAP_MANAGER_EVENT_CONNECT:
        if (event->status != 0)
        {
            return ESP_OK;
        }
        if (!_ble_gap_manager_admit_new_connection())
        {
            return ESP_ERR_NO_MEM;
        }
        s_manager.snapshot.conn_handle = event->conn_handle;
        s_manager.snapshot.generation++;
        s_manager.snapshot.connected = true;
        s_manager.snapshot.encrypted = false;
        _ble_gap_manager_reset_subscriptions();
        return ESP_OK;
    case BLE_GAP_MANAGER_EVENT_DISCONNECT:
        if (event->conn_handle != s_manager.snapshot.conn_handle)
        {
            return ESP_OK;
        }
        s_manager.snapshot.connected = false;
        s_manager.snapshot.conn_handle = 0U;
        s_manager.snapshot.encrypted = false;
        _ble_gap_manager_reset_subscriptions();
        s_manager.snapshot.mtu = 23U;
        return ESP_OK;
    case BLE_GAP_MANAGER_EVENT_MTU:
        if (!s_manager.snapshot.connected ||
                event->conn_handle != s_manager.snapshot.conn_handle)
        {
            return ESP_OK;
        }
        s_manager.snapshot.mtu = event->mtu;
        return ESP_OK;
    case BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE:
        if (!s_manager.snapshot.connected ||
                event->conn_handle != s_manager.snapshot.conn_handle)
        {
            return ESP_OK;
        }
        s_manager.snapshot.encrypted = event->encrypted;
        return ESP_OK;
    case BLE_GAP_MANAGER_EVENT_SUBSCRIBE:
        if (!s_manager.snapshot.connected ||
                event->conn_handle != s_manager.snapshot.conn_handle)
        {
            return ESP_OK;
        }
        _ble_gap_manager_set_subscription(event->attr_handle,
                                          event->subscribed);
        return ESP_OK;
    case BLE_GAP_MANAGER_EVENT_ADV_COMPLETE:
        return ESP_OK;
    case BLE_GAP_MANAGER_EVENT_RESET:
        s_manager.snapshot.connected = false;
        s_manager.snapshot.conn_handle = 0U;
        s_manager.snapshot.encrypted = false;
        _ble_gap_manager_reset_subscriptions();
        s_manager.snapshot.mtu = 23U;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

bool ble_gap_manager_is_subscribed(
    uint16_t conn_handle, uint16_t attr_handle)
{
    if (!s_manager.snapshot.connected ||
            conn_handle != s_manager.snapshot.conn_handle)
    {
        return false;
    }
    for (size_t i = 0U; i < s_manager.subscribed_count; ++i)
    {
        if (s_manager.subscribed_handles[i] == attr_handle)
        {
            return true;
        }
    }
    return false;
}

esp_err_t ble_gap_manager_get_snapshot(ble_gap_manager_snapshot_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_manager.snapshot;
    return ESP_OK;
}
