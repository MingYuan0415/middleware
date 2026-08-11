#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"

#include "ble_gap_manager.h"

#define DBG_TAG "ble_gap_manager"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

#define BLE_GAP_MANAGER_MAX_SUBSCRIBED 32U
#define BLE_GAP_MANAGER_SUBSCRIBE_NOTIFY 0x01U
#define BLE_GAP_MANAGER_SUBSCRIBE_INDICATE 0x02U

typedef struct ble_gap_manager
{
    ble_gap_manager_snapshot_t snapshot;
    ble_gap_manager_admission_cb_t admission_cb;
    void *admission_arg;
    uint16_t subscribed_handles[BLE_GAP_MANAGER_MAX_SUBSCRIBED];
    uint8_t subscribed_kinds[BLE_GAP_MANAGER_MAX_SUBSCRIBED];
    size_t subscribed_count;
    /* Admission reservation: the callback runs without the manager lock,
     * so the to-be-admitted ACL is reserved first and DISCONNECT/RESET
     * (or a second CONNECT) cancel or reject it. Without the reservation,
     * a disconnect arriving during the callback would be ignored and the
     * already-gone ACL could be committed as connected. */
    bool admission_pending;
    uint16_t admission_conn_handle;
    uint32_t admission_token; /**< Monotonic reservation identity. */
} ble_gap_manager_t;

static ble_gap_manager_t s_manager;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_control;

/* The manager owns its serialization: the port writes events from the host
 * task while the Device Link worker reads the snapshot and the TX path
 * queries subscriptions, so no external wrapper can guarantee consistency. */
static void _ble_gap_manager_lock(void)
{
    if (s_mutex != NULL)
    {
        (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void _ble_gap_manager_unlock(void)
{
    if (s_mutex != NULL)
    {
        (void)xSemaphoreGive(s_mutex);
    }
}

void ble_gap_manager_init(void)
{
    if (s_mutex == NULL)
    {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_control);
    }
    _ble_gap_manager_lock();
    memset(&s_manager, 0, sizeof(s_manager));
    s_manager.snapshot.mtu = 23U;
    _ble_gap_manager_unlock();
}

#ifdef UNIT_TEST_HOST
void ble_gap_manager_test_set_generation(uint32_t generation)
{
    _ble_gap_manager_lock();
    s_manager.snapshot.generation = generation;
    _ble_gap_manager_unlock();
}

void ble_gap_manager_test_set_admission_token(uint32_t token)
{
    _ble_gap_manager_lock();
    s_manager.admission_token = token;
    _ble_gap_manager_unlock();
}
#endif

void ble_gap_manager_set_admission_cb(
    ble_gap_manager_admission_cb_t callback, void *arg)
{
    _ble_gap_manager_lock();
    s_manager.admission_cb = callback;
    s_manager.admission_arg = arg;
    _ble_gap_manager_unlock();
}

static void _ble_gap_manager_reset_subscriptions(void)
{
    s_manager.subscribed_count = 0U;
    s_manager.snapshot.subscribed = false;
}

static void _ble_gap_manager_set_subscription(
    uint16_t attr_handle, uint8_t kinds)
{
    for (size_t i = 0U; i < s_manager.subscribed_count; ++i)
    {
        if (s_manager.subscribed_handles[i] == attr_handle)
        {
            s_manager.subscribed_kinds[i] = kinds;
            if (kinds == 0U)
            {
                s_manager.subscribed_handles[i] =
                    s_manager.subscribed_handles[
                        s_manager.subscribed_count - 1U];
                s_manager.subscribed_kinds[i] =
                    s_manager.subscribed_kinds[
                        s_manager.subscribed_count - 1U];
                s_manager.subscribed_count--;
            }
            s_manager.snapshot.subscribed = s_manager.subscribed_count > 0U;
            return;
        }
    }
    if (kinds != 0U &&
            s_manager.subscribed_count < BLE_GAP_MANAGER_MAX_SUBSCRIBED)
    {
        s_manager.subscribed_handles[s_manager.subscribed_count] =
            attr_handle;
        s_manager.subscribed_kinds[s_manager.subscribed_count] = kinds;
        s_manager.subscribed_count++;
        s_manager.snapshot.subscribed = true;
    }
}

static ble_link_operation_kind_t _ble_gap_manager_event_kind(
    ble_gap_manager_event_type_t type)
{
    switch (type)
    {
    case BLE_GAP_MANAGER_EVENT_DISCONNECT:
        return BLE_LINK_OPERATION_DISCONNECT;
    case BLE_GAP_MANAGER_EVENT_MTU:
        return BLE_LINK_OPERATION_MTU;
    case BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE:
        return BLE_LINK_OPERATION_ENCRYPT_CHANGE;
    case BLE_GAP_MANAGER_EVENT_SUBSCRIBE:
        return BLE_LINK_OPERATION_SUBSCRIBE;
    default:
        return BLE_LINK_OPERATION_INVALID;
    }
}

static bool _ble_gap_manager_event_is_current(
    const ble_gap_manager_event_t *event)
{
    const ble_link_operation_identity_t *identity = &event->identity;

    return s_manager.snapshot.connected &&
           event->conn_handle == s_manager.snapshot.conn_handle &&
           identity->conn_handle == event->conn_handle &&
           identity->generation == s_manager.snapshot.generation &&
           identity->kind == _ble_gap_manager_event_kind(event->type) &&
           identity->flow_id == 0U && identity->token == 0U;
}

esp_err_t ble_gap_manager_handle_event(
    const ble_gap_manager_event_t *event)
{
    esp_err_t result = ESP_OK;

    if (event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_gap_manager_lock();
    switch (event->type)
    {
    case BLE_GAP_MANAGER_EVENT_CONNECT:
        if (event->status == 0)
        {
            if (s_manager.snapshot.generation == UINT32_MAX)
            {
                /* Generation zero is reserved for "no connection" and the
                 * allocator must never wrap into a stale callback's
                 * identity. */
                result = ESP_ERR_INVALID_STATE;
            }
            else if (s_manager.snapshot.connected || s_manager.admission_pending)
            {
                result = ESP_ERR_NO_MEM;
            }
            else if (s_manager.admission_cb != NULL)
            {
                /* The admission callback is external: reserve the ACL with
                 * a monotonic token, snapshot the callback, run it WITHOUT
                 * the manager lock (a callback may call back into the
                 * manager), then re-lock and re-validate. The token
                 * protects against handle ABA: if the reservation was
                 * cancelled (DISCONNECT/RESET) and a NEW reservation for
                 * the same handle was created while the callback ran, only
                 * the matching token may commit or clear it. */
                if (s_manager.admission_token == UINT32_MAX)
                {
                    /* The reservation space is exhausted: fail closed. */
                    result = ESP_ERR_INVALID_STATE;
                }
                else
                {
                    ble_gap_manager_admission_cb_t callback =
                        s_manager.admission_cb;
                    void *callback_arg = s_manager.admission_arg;
                    const uint32_t token = ++s_manager.admission_token;
                    bool admit;

                    s_manager.admission_pending = true;
                    s_manager.admission_conn_handle = event->conn_handle;
                    _ble_gap_manager_unlock();
                    admit = callback(callback_arg);
                    _ble_gap_manager_lock();
                    const bool token_matches =
                        s_manager.admission_pending &&
                        s_manager.admission_conn_handle == event->conn_handle &&
                        s_manager.admission_token == token;

                    if (token_matches)
                    {
                        s_manager.admission_pending = false;
                    }
                    if (!admit || !token_matches ||
                            s_manager.snapshot.connected)
                    {
                        result = ESP_ERR_NO_MEM;
                    }
                    else
                    {
                        s_manager.snapshot.conn_handle = event->conn_handle;
                        s_manager.snapshot.generation++;
                        s_manager.snapshot.connected = true;
                        s_manager.snapshot.encrypted = false;
                        _ble_gap_manager_reset_subscriptions();
                        result = ESP_OK;
                    }
                }
            }
            else
            {
                s_manager.snapshot.conn_handle = event->conn_handle;
                s_manager.snapshot.generation++;
                s_manager.snapshot.connected = true;
                s_manager.snapshot.encrypted = false;
                _ble_gap_manager_reset_subscriptions();
                result = ESP_OK;
            }
        }
        break;
    case BLE_GAP_MANAGER_EVENT_DISCONNECT:
        if (s_manager.admission_pending &&
                event->conn_handle == s_manager.admission_conn_handle)
        {
            /* The admission callback is still deciding; this ACL already
             * disconnected, so the pending admission is cancelled. */
            s_manager.admission_pending = false;
        }
        if (_ble_gap_manager_event_is_current(event))
        {
            s_manager.snapshot.connected = false;
            s_manager.snapshot.conn_handle = 0U;
            s_manager.snapshot.encrypted = false;
            _ble_gap_manager_reset_subscriptions();
            s_manager.snapshot.mtu = 23U;
        }
        break;
    case BLE_GAP_MANAGER_EVENT_MTU:
        if (_ble_gap_manager_event_is_current(event))
        {
            s_manager.snapshot.mtu = event->mtu;
        }
        break;
    case BLE_GAP_MANAGER_EVENT_ENCRYPT_CHANGE:
        if (_ble_gap_manager_event_is_current(event))
        {
            s_manager.snapshot.encrypted = event->encrypted;
        }
        break;
    case BLE_GAP_MANAGER_EVENT_SUBSCRIBE:
        if (_ble_gap_manager_event_is_current(event))
        {
            uint8_t kinds = 0U;

            if (event->notify)
            {
                kinds |= BLE_GAP_MANAGER_SUBSCRIBE_NOTIFY;
            }
            if (event->indicate)
            {
                kinds |= BLE_GAP_MANAGER_SUBSCRIBE_INDICATE;
            }
            _ble_gap_manager_set_subscription(event->attr_handle, kinds);
        }
        break;
    case BLE_GAP_MANAGER_EVENT_ADV_COMPLETE:
        break;
    case BLE_GAP_MANAGER_EVENT_RESET:
        s_manager.snapshot.connected = false;
        s_manager.snapshot.conn_handle = 0U;
        s_manager.snapshot.encrypted = false;
        s_manager.admission_pending = false;
        _ble_gap_manager_reset_subscriptions();
        s_manager.snapshot.mtu = 23U;
        break;
    default:
        result = ESP_ERR_INVALID_ARG;
        break;
    }
    _ble_gap_manager_unlock();
    return result;
}

bool ble_gap_manager_is_subscribed(
    uint16_t conn_handle, uint16_t attr_handle)
{
    bool subscribed;

    _ble_gap_manager_lock();
    subscribed = s_manager.snapshot.connected &&
                 conn_handle == s_manager.snapshot.conn_handle;
    if (subscribed)
    {
        subscribed = false;
        for (size_t i = 0U; i < s_manager.subscribed_count; ++i)
        {
            if (s_manager.subscribed_handles[i] == attr_handle)
            {
                subscribed = true;
                break;
            }
        }
    }
    _ble_gap_manager_unlock();
    return subscribed;
}

bool ble_gap_manager_is_subscribed_kind(
    uint16_t conn_handle, uint16_t attr_handle, bool notify)
{
    const uint8_t want = notify ? BLE_GAP_MANAGER_SUBSCRIBE_NOTIFY
                         : BLE_GAP_MANAGER_SUBSCRIBE_INDICATE;
    bool subscribed;

    _ble_gap_manager_lock();
    subscribed = s_manager.snapshot.connected &&
                 conn_handle == s_manager.snapshot.conn_handle;
    if (subscribed)
    {
        subscribed = false;
        for (size_t i = 0U; i < s_manager.subscribed_count; ++i)
        {
            if (s_manager.subscribed_handles[i] == attr_handle &&
                    (s_manager.subscribed_kinds[i] & want) != 0U)
            {
                subscribed = true;
                break;
            }
        }
    }
    _ble_gap_manager_unlock();
    return subscribed;
}

esp_err_t ble_gap_manager_get_snapshot(ble_gap_manager_snapshot_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_gap_manager_lock();
    *out = s_manager.snapshot;
    _ble_gap_manager_unlock();
    return ESP_OK;
}
