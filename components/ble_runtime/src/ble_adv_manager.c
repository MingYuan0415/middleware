#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_adv_manager.h"

#define DBG_TAG "ble_adv_manager"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define BLE_ADV_MANAGER_FLAG_BINDABLE 0x01U
#define BLE_ADV_MANAGER_SERVICE_DATA_BYTES 5U

typedef struct ble_adv_lease_slot
{
    uint8_t id;
    ble_adv_manager_mode_t mode;
    bool bindable;
    uint8_t discriminator[BLE_ADV_MANAGER_DISCRIMINATOR_BYTES];
    bool in_use;
} ble_adv_lease_slot_t;

typedef struct ble_adv_manager
{
    const ble_adv_manager_config_t *config;
    ble_adv_manager_state_t state;
    ble_adv_lease_slot_t leases[BLE_ADV_MANAGER_MAX_LEASES];
    size_t lease_count;
    bool connected;
    bool synced;
    bool stop_outstanding;
    uint32_t fast_deadline_ms;
    bool fast_window_active;
    ble_adv_manager_mode_t started_mode;
    uint32_t start_generation;
    ble_port_adv_config_t adv_config;
    uint8_t service_data[BLE_ADV_MANAGER_SERVICE_DATA_BYTES];
} ble_adv_manager_t;

static ble_adv_manager_t s_manager;

static void _ble_adv_manager_lock(void)
{
    if (s_manager.config != NULL && s_manager.config->lock != NULL)
    {
        s_manager.config->lock(s_manager.config->lock_arg);
    }
}

static void _ble_adv_manager_unlock(void)
{
    if (s_manager.config != NULL && s_manager.config->unlock != NULL)
    {
        s_manager.config->unlock(s_manager.config->lock_arg);
    }
}

static bool _ble_adv_manager_has_fast_lease(void)
{
    for (size_t i = 0U; i < BLE_ADV_MANAGER_MAX_LEASES; ++i)
    {
        if (s_manager.leases[i].in_use &&
                s_manager.leases[i].mode == BLE_ADV_MANAGER_MODE_FAST)
        {
            return true;
        }
    }
    return false;
}

static bool _ble_adv_manager_effective_payload(
    bool *bindable_out, uint8_t discriminator_out[
        BLE_ADV_MANAGER_DISCRIMINATOR_BYTES])
{
    for (size_t i = 0U; i < BLE_ADV_MANAGER_MAX_LEASES; ++i)
    {
        if (s_manager.leases[i].in_use && s_manager.leases[i].bindable)
        {
            *bindable_out = true;
            memcpy(discriminator_out, s_manager.leases[i].discriminator,
                   BLE_ADV_MANAGER_DISCRIMINATOR_BYTES);
            return true;
        }
    }
    *bindable_out = false;
    memset(discriminator_out, 0, BLE_ADV_MANAGER_DISCRIMINATOR_BYTES);
    return false;
}

static bool _ble_adv_manager_payload_changed(void)
{
    bool bindable;
    uint8_t discriminator[BLE_ADV_MANAGER_DISCRIMINATOR_BYTES];

    (void)_ble_adv_manager_effective_payload(&bindable, discriminator);
    if (s_manager.adv_config.service_data == NULL)
    {
        return false;
    }
    return s_manager.service_data[1] != (bindable
                                         ? BLE_ADV_MANAGER_FLAG_BINDABLE
                                         : 0U) ||
           memcmp(&s_manager.service_data[2], discriminator,
                  BLE_ADV_MANAGER_DISCRIMINATOR_BYTES) != 0;
}

static ble_adv_manager_mode_t _ble_adv_manager_effective_mode(void)
{
    if (_ble_adv_manager_has_fast_lease() && s_manager.fast_window_active)
    {
        return BLE_ADV_MANAGER_MODE_FAST;
    }
    return BLE_ADV_MANAGER_MODE_SLOW;
}

static void _ble_adv_manager_arm_fast_window(bool active)
{
    if (s_manager.fast_window_active == active)
    {
        return;
    }
    s_manager.fast_window_active = active;
    if (active)
    {
        s_manager.fast_deadline_ms = s_manager.config->now_ms() +
                                     s_manager.config->fast_window_ms;
    }
    if (s_manager.config->arm_timer != NULL)
    {
        s_manager.config->arm_timer(active ? s_manager.config->fast_window_ms
                                    : 0U,
                                    s_manager.config->timer_arg);
    }
}

static esp_err_t _ble_adv_manager_start(ble_adv_manager_mode_t mode)
{
    const ble_adv_manager_config_t *config = s_manager.config;
    const uint16_t interval = mode == BLE_ADV_MANAGER_MODE_FAST
                              ? config->fast_interval_ms
                              : config->slow_interval_ms;
    bool bindable;
    uint8_t discriminator[BLE_ADV_MANAGER_DISCRIMINATOR_BYTES];
    esp_err_t result;

    (void)_ble_adv_manager_effective_payload(&bindable, discriminator);
    memset(&s_manager.adv_config, 0, sizeof(s_manager.adv_config));
    s_manager.adv_config.interval_ms = interval;
    s_manager.adv_config.short_name = config->short_name;
    s_manager.adv_config.short_name_len = config->short_name_len;
    s_manager.adv_config.service_uuid = config->service_uuid;
    s_manager.service_data[0] = config->adv_version;
    s_manager.service_data[1] = bindable ? BLE_ADV_MANAGER_FLAG_BINDABLE : 0U;
    memcpy(&s_manager.service_data[2], discriminator,
           BLE_ADV_MANAGER_DISCRIMINATOR_BYTES);
    s_manager.adv_config.service_data = s_manager.service_data;
    s_manager.adv_config.service_data_len =
        BLE_ADV_MANAGER_SERVICE_DATA_BYTES;
    s_manager.start_generation++;
    s_manager.adv_config.generation = s_manager.start_generation;
    s_manager.started_mode = mode;
    s_manager.state = BLE_ADV_MANAGER_STATE_STARTING;
    result = config->ops->adv_start(&s_manager.adv_config);
    if (result != ESP_OK)
    {
        s_manager.state = BLE_ADV_MANAGER_STATE_FAULTED;
    }
    return result;
}

static esp_err_t _ble_adv_manager_stop(void)
{
    const esp_err_t result = s_manager.config->ops->adv_stop();

    if (result == ESP_OK)
    {
        s_manager.state = BLE_ADV_MANAGER_STATE_STOPPING;
        s_manager.stop_outstanding = true;
    }
    else
    {
        s_manager.state = BLE_ADV_MANAGER_STATE_FAULTED;
        s_manager.stop_outstanding = true;
    }
    return result;
}

static esp_err_t _ble_adv_manager_stop_completed(void)
{
    s_manager.stop_outstanding = false;
    if (s_manager.lease_count > 0U && !s_manager.connected &&
            s_manager.synced)
    {
        return _ble_adv_manager_start(_ble_adv_manager_effective_mode());
    }
    s_manager.state = BLE_ADV_MANAGER_STATE_STOPPED;
    return ESP_OK;
}

static esp_err_t _ble_adv_manager_converge(void)
{
    ble_adv_manager_mode_t active_mode;
    ble_adv_manager_mode_t effective;

    if (s_manager.state == BLE_ADV_MANAGER_STATE_STOPPING)
    {
        return ESP_OK;
    }
    if (s_manager.lease_count == 0U || !s_manager.synced)
    {
        if (s_manager.state == BLE_ADV_MANAGER_STATE_FAST ||
                s_manager.state == BLE_ADV_MANAGER_STATE_SLOW ||
                s_manager.state == BLE_ADV_MANAGER_STATE_STARTING)
        {
            _ble_adv_manager_arm_fast_window(false);
            return _ble_adv_manager_stop();
        }
        if (s_manager.state == BLE_ADV_MANAGER_STATE_FAULTED)
        {
            if (s_manager.stop_outstanding)
            {
                return _ble_adv_manager_stop();
            }
            s_manager.state = BLE_ADV_MANAGER_STATE_STOPPED;
        }
        return ESP_OK;
    }
    if (s_manager.connected)
    {
        if (s_manager.state == BLE_ADV_MANAGER_STATE_FAST ||
                s_manager.state == BLE_ADV_MANAGER_STATE_SLOW ||
                s_manager.state == BLE_ADV_MANAGER_STATE_STARTING)
        {
            return _ble_adv_manager_stop();
        }
        return ESP_OK;
    }
    if (s_manager.state == BLE_ADV_MANAGER_STATE_STOPPED)
    {
        return _ble_adv_manager_start(_ble_adv_manager_effective_mode());
    }
    if (s_manager.state == BLE_ADV_MANAGER_STATE_FAULTED)
    {
        if (s_manager.stop_outstanding)
        {
            return _ble_adv_manager_stop();
        }
        return _ble_adv_manager_start(_ble_adv_manager_effective_mode());
    }
    if (s_manager.state == BLE_ADV_MANAGER_STATE_STARTING)
    {
        active_mode = s_manager.started_mode;
    }
    else
    {
        active_mode = s_manager.state == BLE_ADV_MANAGER_STATE_FAST
                      ? BLE_ADV_MANAGER_MODE_FAST
                      : BLE_ADV_MANAGER_MODE_SLOW;
    }
    effective = _ble_adv_manager_effective_mode();
    if (effective != active_mode || _ble_adv_manager_payload_changed())
    {
        if (active_mode == BLE_ADV_MANAGER_MODE_FAST &&
                effective == BLE_ADV_MANAGER_MODE_SLOW)
        {
            _ble_adv_manager_arm_fast_window(false);
        }
        return _ble_adv_manager_stop();
    }
    return ESP_OK;
}

void ble_adv_manager_init(const ble_adv_manager_config_t *config)
{
    memset(&s_manager, 0, sizeof(s_manager));
    s_manager.config = config;
    s_manager.state = BLE_ADV_MANAGER_STATE_STOPPED;
    s_manager.synced = true;
}

void ble_adv_manager_deinit(void)
{
    void (*unlock_cb)(void *) = s_manager.config != NULL
                                ? s_manager.config->unlock
                                : NULL;
    void *unlock_arg = s_manager.config != NULL
                       ? s_manager.config->lock_arg
                       : NULL;

    if (s_manager.config != NULL && s_manager.config->lock != NULL)
    {
        s_manager.config->lock(s_manager.config->lock_arg);
    }
    s_manager.config = NULL;
    s_manager.state = BLE_ADV_MANAGER_STATE_STOPPED;
    if (unlock_cb != NULL)
    {
        unlock_cb(unlock_arg);
    }
}

esp_err_t ble_adv_manager_acquire_lease(
    ble_adv_lease_t *out, ble_adv_manager_mode_t mode, bool bindable,
    const uint8_t discriminator[BLE_ADV_MANAGER_DISCRIMINATOR_BYTES])
{
    size_t slot = BLE_ADV_MANAGER_MAX_LEASES;

    if (out == NULL || (bindable && discriminator == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode != BLE_ADV_MANAGER_MODE_FAST &&
            mode != BLE_ADV_MANAGER_MODE_SLOW)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_adv_manager_lock();
    if (s_manager.config == NULL)
    {
        _ble_adv_manager_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_manager.lease_count >= BLE_ADV_MANAGER_MAX_LEASES)
    {
        _ble_adv_manager_unlock();
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0U; i < BLE_ADV_MANAGER_MAX_LEASES; ++i)
    {
        if (!s_manager.leases[i].in_use)
        {
            slot = i;
            break;
        }
    }
    s_manager.leases[slot].id = (uint8_t)(slot + 1U);
    s_manager.leases[slot].mode = mode;
    s_manager.leases[slot].bindable = bindable;
    if (bindable)
    {
        memcpy(s_manager.leases[slot].discriminator, discriminator,
               BLE_ADV_MANAGER_DISCRIMINATOR_BYTES);
    }
    else
    {
        memset(s_manager.leases[slot].discriminator, 0,
               BLE_ADV_MANAGER_DISCRIMINATOR_BYTES);
    }
    s_manager.leases[slot].in_use = true;
    s_manager.lease_count++;
    if (mode == BLE_ADV_MANAGER_MODE_FAST)
    {
        _ble_adv_manager_arm_fast_window(true);
    }
    if (out != NULL)
    {
        out->lease_id = s_manager.leases[slot].id;
        out->mode = mode;
        out->bindable = bindable;
        memcpy(out->discriminator, s_manager.leases[slot].discriminator,
               BLE_ADV_MANAGER_DISCRIMINATOR_BYTES);
    }
    const esp_err_t result = _ble_adv_manager_converge();

    _ble_adv_manager_unlock();
    return result;
}

esp_err_t ble_adv_manager_release_lease(uint8_t lease_id)
{
    bool found = false;

    _ble_adv_manager_lock();
    if (s_manager.config == NULL)
    {
        _ble_adv_manager_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    size_t slot = BLE_ADV_MANAGER_MAX_LEASES;

    for (size_t i = 0U; i < BLE_ADV_MANAGER_MAX_LEASES; ++i)
    {
        if (s_manager.leases[i].in_use && s_manager.leases[i].id == lease_id)
        {
            s_manager.leases[i].in_use = false;
            s_manager.lease_count--;
            slot = i;
            found = true;
            break;
        }
    }
    if (!found)
    {
        _ble_adv_manager_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    /* The released slot must not retain the discovery discriminator. */
    memset(s_manager.leases[slot].discriminator, 0,
           BLE_ADV_MANAGER_DISCRIMINATOR_BYTES);
    const esp_err_t result = _ble_adv_manager_converge();

    _ble_adv_manager_unlock();
    return result;
}

esp_err_t ble_adv_manager_handle_event(const ble_port_event_t *event)
{
    esp_err_t result = ESP_OK;

    if (event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_adv_manager_lock();
    if (s_manager.config == NULL)
    {
        _ble_adv_manager_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    switch (event->type)
    {
    case BLE_PORT_EVENT_ADV_STARTED:
        if (s_manager.state == BLE_ADV_MANAGER_STATE_STARTING &&
                event->generation == s_manager.start_generation)
        {
            if (event->status == 0)
            {
                s_manager.state =
                    s_manager.started_mode == BLE_ADV_MANAGER_MODE_FAST
                    ? BLE_ADV_MANAGER_STATE_FAST
                    : BLE_ADV_MANAGER_STATE_SLOW;
                result = _ble_adv_manager_converge();
            }
            else
            {
                s_manager.state = BLE_ADV_MANAGER_STATE_FAULTED;
            }
        }
        break;
    case BLE_PORT_EVENT_ADV_STOPPED:
        if (s_manager.state == BLE_ADV_MANAGER_STATE_STOPPING)
        {
            if (event->status == 0)
            {
                result = _ble_adv_manager_stop_completed();
            }
            else
            {
                s_manager.state = BLE_ADV_MANAGER_STATE_FAULTED;
                s_manager.stop_outstanding = true;
            }
        }
        break;
    case BLE_PORT_EVENT_ADV_COMPLETE:
        if (s_manager.state == BLE_ADV_MANAGER_STATE_FAST ||
                s_manager.state == BLE_ADV_MANAGER_STATE_SLOW ||
                s_manager.state == BLE_ADV_MANAGER_STATE_STARTING)
        {
            _ble_adv_manager_arm_fast_window(false);
            s_manager.state = BLE_ADV_MANAGER_STATE_STOPPED;
            result = _ble_adv_manager_converge();
        }
        else if (s_manager.state == BLE_ADV_MANAGER_STATE_STOPPING)
        {
            result = _ble_adv_manager_stop_completed();
        }
        break;
    case BLE_PORT_EVENT_CONNECT:
        if (event->status == 0)
        {
            s_manager.connected = true;
            result = _ble_adv_manager_converge();
        }
        break;
    case BLE_PORT_EVENT_DISCONNECT:
        s_manager.connected = false;
        result = _ble_adv_manager_converge();
        break;
    case BLE_PORT_EVENT_SYNC:
        s_manager.synced = true;
        result = _ble_adv_manager_converge();
        break;
    case BLE_PORT_EVENT_RESET:
        s_manager.synced = false;
        s_manager.connected = false;
        s_manager.stop_outstanding = false;
        _ble_adv_manager_arm_fast_window(false);
        s_manager.state = BLE_ADV_MANAGER_STATE_STOPPED;
        break;
    default:
        break;
    }
    _ble_adv_manager_unlock();
    return result;
}

void ble_adv_manager_handle_fast_window_expired(void)
{
    _ble_adv_manager_lock();
    if (s_manager.config != NULL && s_manager.fast_window_active)
    {
        _ble_adv_manager_arm_fast_window(false);
        if (s_manager.state == BLE_ADV_MANAGER_STATE_FAST)
        {
            (void)_ble_adv_manager_converge();
        }
    }
    _ble_adv_manager_unlock();
}

uint32_t ble_adv_manager_get_fast_window_remaining_ms(void)
{
    uint32_t remaining = UINT32_MAX;

    _ble_adv_manager_lock();
    if (s_manager.config != NULL && s_manager.fast_window_active)
    {
        const uint32_t now = s_manager.config->now_ms();
        const uint32_t remaining_now = s_manager.fast_deadline_ms - now;

        remaining = remaining_now <= s_manager.config->fast_window_ms
                    ? remaining_now
                    : 0U;
    }
    _ble_adv_manager_unlock();
    return remaining;
}

ble_adv_manager_state_t ble_adv_manager_get_state(void)
{
    ble_adv_manager_state_t state;

    _ble_adv_manager_lock();
    state = s_manager.config != NULL
            ? s_manager.state
            : BLE_ADV_MANAGER_STATE_STOPPED;
    _ble_adv_manager_unlock();
    return state;
}
