#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#include "ble_adv_manager.h"

#define DBG_TAG "ble_adv_manager"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#define BLE_ADV_MANAGER_RETRY_INITIAL_MS 100U
#define BLE_ADV_MANAGER_RETRY_MAX_MS 1000U
#define BLE_ADV_MANAGER_PAUSE_REASON_LEGACY (UINT32_C(1) << 31)

typedef enum
{
    BLE_ADV_MANAGER_RETRY_NONE = 0,
    BLE_ADV_MANAGER_RETRY_START,
    BLE_ADV_MANAGER_RETRY_STOP,
} ble_adv_manager_retry_action_t;

typedef struct ble_adv_lease_slot
{
    uint8_t id;
    ble_adv_manager_mode_t mode;
    bool bindable;
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
    uint32_t pause_reasons;
    bool stop_outstanding;
    bool retry_pending;
    uint8_t retry_attempts;
    ble_adv_manager_retry_action_t retry_action;
    uint32_t retry_generation;
    uint32_t retry_not_before_ms;
    bool retry_start_target_valid;
    ble_adv_manager_mode_t retry_start_mode;
    bool retry_start_bindable;
    uint32_t fast_deadline_ms;
    bool fast_window_active;
    ble_adv_manager_mode_t started_mode;
    uint32_t start_generation;
    uint32_t stop_generation;
    ble_port_adv_config_t adv_config;
} ble_adv_manager_t;

static ble_adv_manager_t s_manager;

static bool _ble_adv_manager_paused(void)
{
    return s_manager.pause_reasons != 0U;
}

static bool _ble_adv_manager_deadline_reached(
    uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void _ble_adv_manager_clear_retry(void)
{
    s_manager.retry_pending = false;
    s_manager.retry_attempts = 0U;
    s_manager.retry_action = BLE_ADV_MANAGER_RETRY_NONE;
    s_manager.retry_generation = 0U;
    s_manager.retry_not_before_ms = 0U;
    s_manager.retry_start_target_valid = false;
    s_manager.retry_start_bindable = false;
}

static uint32_t _ble_adv_manager_retry_delay_ms(uint8_t attempts)
{
    uint32_t delay_ms = BLE_ADV_MANAGER_RETRY_INITIAL_MS;

    for (uint8_t i = 1U; i < attempts; ++i)
    {
        if (delay_ms >= BLE_ADV_MANAGER_RETRY_MAX_MS / 2U)
        {
            return BLE_ADV_MANAGER_RETRY_MAX_MS;
        }
        delay_ms *= 2U;
    }
    return delay_ms > BLE_ADV_MANAGER_RETRY_MAX_MS
           ? BLE_ADV_MANAGER_RETRY_MAX_MS
           : delay_ms;
}

static void _ble_adv_manager_schedule_retry(
    ble_adv_manager_retry_action_t action, uint32_t generation)
{
    const bool start_target_changed =
        action == BLE_ADV_MANAGER_RETRY_START &&
        (!s_manager.retry_start_target_valid ||
         s_manager.retry_start_mode != s_manager.started_mode ||
         s_manager.retry_start_bindable != s_manager.adv_config.bindable);

    if (s_manager.retry_action != action || start_target_changed)
    {
        s_manager.retry_attempts = 0U;
        s_manager.retry_action = action;
    }
    if (action == BLE_ADV_MANAGER_RETRY_START)
    {
        s_manager.retry_start_target_valid = true;
        s_manager.retry_start_mode = s_manager.started_mode;
        s_manager.retry_start_bindable = s_manager.adv_config.bindable;
    }
    s_manager.retry_generation = generation;
    if (s_manager.retry_attempts < UINT8_MAX)
    {
        s_manager.retry_attempts++;
    }
    const uint32_t delay_ms =
        _ble_adv_manager_retry_delay_ms(s_manager.retry_attempts);

    s_manager.retry_not_before_ms = s_manager.config->now_ms() + delay_ms;
    s_manager.retry_pending = true;
    if (s_manager.config->arm_timer != NULL)
    {
        s_manager.config->arm_timer(delay_ms, s_manager.config->timer_arg);
    }
}

static bool _ble_adv_manager_retry_ready(void)
{
    return s_manager.retry_pending &&
           _ble_adv_manager_deadline_reached(
               s_manager.config->now_ms(), s_manager.retry_not_before_ms);
}

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

static bool _ble_adv_manager_effective_bindable(void)
{
    for (size_t i = 0U; i < BLE_ADV_MANAGER_MAX_LEASES; ++i)
    {
        if (s_manager.leases[i].in_use && s_manager.leases[i].bindable)
        {
            return true;
        }
    }
    return false;
}

static bool _ble_adv_manager_bindable_changed(void)
{
    return s_manager.adv_config.bindable !=
           _ble_adv_manager_effective_bindable();
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
    esp_err_t result;

    memset(&s_manager.adv_config, 0, sizeof(s_manager.adv_config));
    s_manager.adv_config.interval_ms = interval;
    s_manager.adv_config.short_name = config->short_name;
    s_manager.adv_config.short_name_len = config->short_name_len;
    s_manager.adv_config.service_uuid = config->service_uuid;
    s_manager.adv_config.bindable = _ble_adv_manager_effective_bindable();
    if (s_manager.start_generation == UINT32_MAX)
    {
        s_manager.state = BLE_ADV_MANAGER_STATE_FAULTED;
        return ESP_ERR_INVALID_STATE;
    }
    s_manager.start_generation++;
    s_manager.adv_config.generation = s_manager.start_generation;
    s_manager.started_mode = mode;
    s_manager.state = BLE_ADV_MANAGER_STATE_STARTING;
    result = config->ops->adv_start(&s_manager.adv_config);
    if (result != ESP_OK)
    {
        s_manager.state = BLE_ADV_MANAGER_STATE_FAULTED;
        _ble_adv_manager_schedule_retry(
            BLE_ADV_MANAGER_RETRY_START, s_manager.start_generation);
    }
    else if (s_manager.retry_action == BLE_ADV_MANAGER_RETRY_START)
    {
        s_manager.retry_pending = false;
    }
    return result;
}

static esp_err_t _ble_adv_manager_stop(void)
{
    if (!s_manager.stop_outstanding)
    {
        if (s_manager.stop_generation == UINT32_MAX)
        {
            s_manager.state = BLE_ADV_MANAGER_STATE_FAULTED;
            return ESP_ERR_INVALID_STATE;
        }
        s_manager.stop_generation++;
        s_manager.stop_outstanding = true;
    }
    s_manager.state = BLE_ADV_MANAGER_STATE_STOPPING;
    const esp_err_t result =
        s_manager.config->ops->adv_stop(s_manager.stop_generation);

    if (result == ESP_OK)
    {
        if (s_manager.retry_action == BLE_ADV_MANAGER_RETRY_STOP)
        {
            s_manager.retry_pending = false;
        }
    }
    else
    {
        /* A synchronous stop failure keeps the stop obligation. */
        s_manager.state = BLE_ADV_MANAGER_STATE_FAULTED;
        s_manager.stop_outstanding = true;
        _ble_adv_manager_schedule_retry(
            BLE_ADV_MANAGER_RETRY_STOP, s_manager.stop_generation);
    }
    return result;
}

static esp_err_t _ble_adv_manager_stop_completed(void)
{
    s_manager.stop_outstanding = false;
    _ble_adv_manager_clear_retry();
    if (s_manager.lease_count > 0U && !s_manager.connected &&
            s_manager.synced && !_ble_adv_manager_paused())
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

    const bool wants_advertising = s_manager.lease_count > 0U &&
                                   !s_manager.connected && s_manager.synced &&
                                   !_ble_adv_manager_paused();

    if (s_manager.state == BLE_ADV_MANAGER_STATE_STOPPING)
    {
        return ESP_OK;
    }
    if (s_manager.state == BLE_ADV_MANAGER_STATE_FAULTED &&
            s_manager.retry_action == BLE_ADV_MANAGER_RETRY_START &&
            !wants_advertising)
    {
        _ble_adv_manager_clear_retry();
        s_manager.state = BLE_ADV_MANAGER_STATE_STOPPED;
    }
    if (s_manager.state == BLE_ADV_MANAGER_STATE_FAULTED &&
            s_manager.retry_pending && !_ble_adv_manager_retry_ready())
    {
        /* The requested target has not converged yet. Returning success
         * here lets a lease owner relinquish its identity during the
         * cooldown even though the failed START/STOP obligation is still
         * live, so security policy could advance ahead of advertising. */
        return ESP_ERR_INVALID_STATE;
    }
    if (_ble_adv_manager_paused())
    {
        /* A pause only stops the physical advertisement and must not
         * destroy the fast window: an unexpired FAST lease resumes at FAST
         * speed. The window is still retired when its deadline passes
         * (handle_fast_window_expired). */
        if (s_manager.state == BLE_ADV_MANAGER_STATE_FAST ||
                s_manager.state == BLE_ADV_MANAGER_STATE_SLOW ||
                s_manager.state == BLE_ADV_MANAGER_STATE_STARTING)
        {
            return _ble_adv_manager_stop();
        }
        if (s_manager.state == BLE_ADV_MANAGER_STATE_FAULTED &&
                s_manager.stop_outstanding)
        {
            return _ble_adv_manager_stop();
        }
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
    if (effective != active_mode || _ble_adv_manager_bindable_changed())
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
    const uint32_t start_generation = s_manager.start_generation;
    const uint32_t stop_generation = s_manager.stop_generation;

    memset(&s_manager, 0, sizeof(s_manager));
    s_manager.config = config;
    s_manager.state = BLE_ADV_MANAGER_STATE_STOPPED;
    s_manager.synced = true;
    /* Generations are boot scoped. Reinitializing the component must not let
     * a late completion from the retired port instance collide with a new
     * command identity. */
    s_manager.start_generation = start_generation;
    s_manager.stop_generation = stop_generation;
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
    ble_adv_lease_t *out, ble_adv_manager_mode_t mode, bool bindable)
{
    size_t slot = BLE_ADV_MANAGER_MAX_LEASES;

    if (out == NULL)
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
    const ble_adv_manager_mode_t released_mode = s_manager.leases[slot].mode;
    const bool window_was_active = s_manager.fast_window_active;
    const uint32_t window_deadline_before = s_manager.fast_deadline_ms;
    const esp_err_t result = _ble_adv_manager_converge();

    if (result != ESP_OK)
    {
        /* A synchronous stop failure restores the complete lease so the
         * owner can retry the release. The fast window is restored to its
         * exact prior state: an active window keeps its
         * original deadline (an expired or inactive window must not be
         * recreated with a fresh full duration). */
        s_manager.leases[slot].in_use = true;
        s_manager.lease_count++;
        if (released_mode == BLE_ADV_MANAGER_MODE_FAST &&
                _ble_adv_manager_has_fast_lease() && window_was_active)
        {
            /* Restore the window only while its original deadline is
             * still in the future (wrap-safe unsigned comparison bounded
             * by the window duration): an expired or inactive window must
             * not be recreated, and arm_timer(0) is reserved for the
             * cancel notification. */
            const uint32_t now = s_manager.config->now_ms();
            /* Wrap-safe unsigned distance: the deadline is still in the
             * future exactly when the modulo distance is within the
             * window duration (a wrapped clock cannot misclassify). */
            const uint32_t remaining =
                (uint32_t)(window_deadline_before - now);
            /* A zero remaining means the deadline is exactly now: expired
             * (an active window always has a strictly positive distance). */
            const bool still_active =
                remaining > 0U &&
                remaining <= s_manager.config->fast_window_ms;

            if (still_active)
            {
                s_manager.fast_window_active = true;
                s_manager.fast_deadline_ms = window_deadline_before;
                if (s_manager.config->arm_timer != NULL)
                {
                    s_manager.config->arm_timer(
                        remaining, s_manager.config->timer_arg);
                }
            }
            else
            {
                /* The window had already expired (or the stop path tore it
                 * down): the invariant "no active window without an
                 * unexpired FAST lease" must hold, so the state is forced
                 * inactive and the cancel is announced. */
                s_manager.fast_window_active = false;
                if (s_manager.config->arm_timer != NULL)
                {
                    s_manager.config->arm_timer(0U,
                                                s_manager.config->timer_arg);
                }
            }
        }
        _ble_adv_manager_unlock();
        return result;
    }
    /* The release converged. The fast window is active exactly while a
     * FAST lease exists (invariant:
     * no FAST lease means no fast window), so a release that removed the
     * last FAST lease retires the window through the regular helper: it
     * both clears the state and announces the cancel to the timer owner
     * (arm_timer(0)), which a direct flag write would skip. */
    if (!_ble_adv_manager_has_fast_lease())
    {
        _ble_adv_manager_arm_fast_window(false);
    }
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
                _ble_adv_manager_clear_retry();
                s_manager.state =
                    s_manager.started_mode == BLE_ADV_MANAGER_MODE_FAST
                    ? BLE_ADV_MANAGER_STATE_FAST
                    : BLE_ADV_MANAGER_STATE_SLOW;
                result = _ble_adv_manager_converge();
            }
            else
            {
                s_manager.state = BLE_ADV_MANAGER_STATE_FAULTED;
                _ble_adv_manager_schedule_retry(
                    BLE_ADV_MANAGER_RETRY_START, event->generation);
            }
        }
        break;
    case BLE_PORT_EVENT_ADV_STOPPED:
        if (s_manager.state == BLE_ADV_MANAGER_STATE_STOPPING &&
                event->generation == s_manager.stop_generation)
        {
            if (event->status == 0)
            {
                result = _ble_adv_manager_stop_completed();
            }
            else
            {
                s_manager.state = BLE_ADV_MANAGER_STATE_FAULTED;
                s_manager.stop_outstanding = true;
                _ble_adv_manager_schedule_retry(
                    BLE_ADV_MANAGER_RETRY_STOP, event->generation);
            }
        }
        break;
    case BLE_PORT_EVENT_ADV_COMPLETE:
        if (event->host_reset_pending)
        {
            /* NimBLE emits ADV_COMPLETE before reset_cb. RESET owns the
             * transition and invalidates queued commands; converging here
             * would enqueue a START against the retiring host generation. */
            break;
        }
        if (s_manager.state == BLE_ADV_MANAGER_STATE_FAST ||
                s_manager.state == BLE_ADV_MANAGER_STATE_SLOW ||
                s_manager.state == BLE_ADV_MANAGER_STATE_STARTING)
        {
            _ble_adv_manager_arm_fast_window(false);
            _ble_adv_manager_clear_retry();
            s_manager.state = BLE_ADV_MANAGER_STATE_STOPPED;
            result = _ble_adv_manager_converge();
        }
        /* A generic ADV_COMPLETE cannot discharge a STOP obligation. Only
         * generation-qualified ADV_STOPPED proves both the physical stop and
         * the synchronous pairing-gate close completed. */
        break;
    case BLE_PORT_EVENT_CONNECT:
        /* A CONNECT dispatched with accepted=false was rejected by the
         * connection admission and must never mark the manager connected:
         * its disconnect would otherwise retire the real ACL's state and
         * restart advertising while the accepted connection is live. */
        if (event->status == 0 && event->accepted)
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
        _ble_adv_manager_clear_retry();
        _ble_adv_manager_arm_fast_window(false);
        s_manager.state = BLE_ADV_MANAGER_STATE_STOPPED;
        break;
    default:
        break;
    }
    _ble_adv_manager_unlock();
    return result;
}

static esp_err_t _ble_adv_manager_set_pause_reason(
    uint32_t reason, bool active)
{
    _ble_adv_manager_lock();
    if (s_manager.config == NULL)
    {
        _ble_adv_manager_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (active)
    {
        s_manager.pause_reasons |= reason;
    }
    else
    {
        s_manager.pause_reasons &= ~reason;
    }
    const esp_err_t result = _ble_adv_manager_converge();

    _ble_adv_manager_unlock();
    return result;
}

esp_err_t ble_adv_manager_set_pause_reason(
    ble_adv_manager_pause_reason_t reason, bool active)
{
    switch (reason)
    {
    case BLE_ADV_MANAGER_PAUSE_REASON_WINDOW_TRANSITION:
    case BLE_ADV_MANAGER_PAUSE_REASON_STARTUP_GATE:
    case BLE_ADV_MANAGER_PAUSE_REASON_PEER_CLEANUP:
    case BLE_ADV_MANAGER_PAUSE_REASON_REVOKE:
    case BLE_ADV_MANAGER_PAUSE_REASON_REJECTED_ACL:
    case BLE_ADV_MANAGER_PAUSE_REASON_SERVICE_SHUTDOWN:
    case BLE_ADV_MANAGER_PAUSE_REASON_REVOKE_PORT:
        return _ble_adv_manager_set_pause_reason((uint32_t)reason, active);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t ble_adv_manager_set_paused(bool paused)
{
    return _ble_adv_manager_set_pause_reason(
               BLE_ADV_MANAGER_PAUSE_REASON_LEGACY, paused);
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

void ble_adv_manager_poll(void)
{
    _ble_adv_manager_lock();
    if (s_manager.config != NULL && s_manager.state ==
            BLE_ADV_MANAGER_STATE_FAULTED &&
            _ble_adv_manager_retry_ready())
    {
        const bool retry_current =
            (s_manager.retry_action == BLE_ADV_MANAGER_RETRY_START &&
             s_manager.retry_generation == s_manager.start_generation &&
             !s_manager.stop_outstanding) ||
            (s_manager.retry_action == BLE_ADV_MANAGER_RETRY_STOP &&
             s_manager.retry_generation == s_manager.stop_generation &&
             s_manager.stop_outstanding);

        if (retry_current)
        {
            s_manager.retry_pending = false;
        }
        else
        {
            _ble_adv_manager_clear_retry();
        }
        (void)_ble_adv_manager_converge();
    }
    _ble_adv_manager_unlock();
}

bool ble_adv_manager_start_command_current(uint32_t generation)
{
    bool current = false;

    _ble_adv_manager_lock();
    if (s_manager.config != NULL && generation != 0U && s_manager.synced &&
            s_manager.state == BLE_ADV_MANAGER_STATE_STARTING &&
            !s_manager.stop_outstanding &&
            generation == s_manager.start_generation &&
            s_manager.lease_count > 0U && !s_manager.connected &&
            !_ble_adv_manager_paused())
    {
        current = true;
    }
    _ble_adv_manager_unlock();
    return current;
}

bool ble_adv_manager_stop_command_current(uint32_t generation)
{
    bool current = false;

    _ble_adv_manager_lock();
    if (s_manager.config != NULL && generation != 0U &&
            s_manager.state == BLE_ADV_MANAGER_STATE_STOPPING &&
            s_manager.stop_outstanding &&
            generation == s_manager.stop_generation)
    {
        current = true;
    }
    _ble_adv_manager_unlock();
    return current;
}

uint32_t ble_adv_manager_get_retry_remaining_ms(void)
{
    uint32_t remaining = UINT32_MAX;

    _ble_adv_manager_lock();
    if (s_manager.config != NULL && s_manager.retry_pending)
    {
        const uint32_t now = s_manager.config->now_ms();

        remaining = _ble_adv_manager_deadline_reached(
                        now, s_manager.retry_not_before_ms)
                    ? 0U
                    : s_manager.retry_not_before_ms - now;
    }
    _ble_adv_manager_unlock();
    return remaining;
}

bool ble_adv_manager_bindable_requested(void)
{
    _ble_adv_manager_lock();
    const bool bindable = _ble_adv_manager_effective_bindable();
    _ble_adv_manager_unlock();
    return bindable;
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
