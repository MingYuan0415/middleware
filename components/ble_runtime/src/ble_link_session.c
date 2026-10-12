#define DBG_TAG "ble_link_session"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ble_link_session.h"

typedef struct ble_link_session
{
    uint64_t boot_id;
    uint32_t generation;
    uint32_t last_generation;
    uint32_t authorization_revision;
    bool active;
    bool encrypted;
    bool bond_verified;
    bool identity_known;
    bool connection_pairing_window_open;
    bool bound;
    bool authorization_transitioning;
    bool error_latched;
    atomic_bool pairing_window_open;
} ble_link_session_t;

static ble_link_session_t s_session;
static StaticSemaphore_t s_mutex_control;
static SemaphoreHandle_t s_mutex;

static void _ble_link_session_lock(void)
{
    if (s_mutex != NULL)
    {
        (void)xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    }
}

static void _ble_link_session_unlock(void)
{
    if (s_mutex != NULL)
    {
        (void)xSemaphoreGiveRecursive(s_mutex);
    }
}

static bool _ble_link_session_current(uint32_t generation)
{
    return s_session.active && generation == s_session.generation;
}

static void _ble_link_session_reset_locked(uint64_t boot_id)
{
    memset(&s_session, 0, sizeof(s_session));
    atomic_init(&s_session.pairing_window_open, false);
    s_session.boot_id = boot_id;
}

void ble_link_session_init(uint64_t boot_id)
{
    if (s_mutex == NULL)
    {
        s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mutex_control);
    }
    _ble_link_session_lock();
    _ble_link_session_reset_locked(boot_id);
    _ble_link_session_unlock();
}

void ble_link_session_reset(void)
{
    _ble_link_session_lock();
    _ble_link_session_reset_locked(s_session.boot_id);
    _ble_link_session_unlock();
}

esp_err_t ble_link_session_handle_event(uint32_t generation, ble_link_session_event_t event)
{
    if (event < BLE_LINK_SESSION_EVENT_ACL_CONNECTED ||
            event > BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_OK;
    _ble_link_session_lock();
    if (event == BLE_LINK_SESSION_EVENT_ACL_CONNECTED)
    {
        if (generation > s_session.last_generation)
        {
            if (s_session.active)
            {
                result = ESP_ERR_INVALID_STATE;
            }
            else
            {
                s_session.last_generation = generation;
                s_session.generation = generation;
                s_session.active = true;
                s_session.encrypted = false;
                s_session.bond_verified = false;
                s_session.identity_known = false;
                s_session.connection_pairing_window_open = false;
            }
        }
    }
    else if (_ble_link_session_current(generation))
    {
        if (event == BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED)
        {
            s_session.active = false;
            s_session.encrypted = false;
            s_session.bond_verified = false;
            s_session.identity_known = false;
        }
        else if (event == BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED)
        {
            s_session.encrypted = true;
        }
        else
        {
            s_session.bond_verified = true;
        }
    }
    _ble_link_session_unlock();
    return result;
}

esp_err_t ble_link_session_set_authorization(bool committed, uint32_t revision)
{
    esp_err_t result = ESP_OK;
    _ble_link_session_lock();
    if (revision == 0U && s_session.authorization_revision == UINT32_MAX)
    {
        if (committed)
        {
            result = ESP_ERR_INVALID_STATE;
        }
        else
        {
            s_session.bound = false;
        }
    }
    else
    {
        if (revision == 0U)
        {
            revision = s_session.authorization_revision + 1U;
        }
        if (revision > s_session.authorization_revision)
        {
            s_session.authorization_revision = revision;
            s_session.bound = committed;
        }
    }
    _ble_link_session_unlock();
    return result;
}

void ble_link_session_set_pairing_window(bool open)
{
    atomic_store_explicit(&s_session.pairing_window_open, open, memory_order_release);
}

esp_err_t ble_link_session_set_connection_pairing_window(uint32_t generation, bool open)
{
    _ble_link_session_lock();
    const bool current = _ble_link_session_current(generation);
    if (current)
    {
        s_session.connection_pairing_window_open = open;
    }
    _ble_link_session_unlock();
    return current ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t ble_link_session_get_connection_pairing_window(uint32_t generation, bool *out_open)
{
    if (out_open == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_session_lock();
    const bool current = _ble_link_session_current(generation);
    if (current)
    {
        *out_open = s_session.connection_pairing_window_open;
    }
    _ble_link_session_unlock();
    return current ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void ble_link_session_clear_link_security(uint32_t generation)
{
    _ble_link_session_lock();
    if (_ble_link_session_current(generation))
    {
        s_session.encrypted = false;
        s_session.bond_verified = false;
        s_session.identity_known = false;
    }
    _ble_link_session_unlock();
}

esp_err_t ble_link_session_get_facts(uint32_t generation, ble_link_dispatcher_facts_t *facts)
{
    if (facts == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_session_lock();
    const bool current = _ble_link_session_current(generation);
    if (current)
    {
        *facts = (ble_link_dispatcher_facts_t)
        {
            .active_boot_id = s_session.boot_id,
            .connection_generation = s_session.generation,
            .encrypted = s_session.encrypted,
        };
    }
    _ble_link_session_unlock();
    return current ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t ble_link_session_set_identity_known(uint32_t generation, bool known)
{
    _ble_link_session_lock();
    const bool current = _ble_link_session_current(generation);
    if (current)
    {
        s_session.identity_known = known;
    }
    _ble_link_session_unlock();
    return current ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t ble_link_session_get_security_facts(uint32_t generation, bool *out_bond_verified,
        bool *out_identity_known)
{
    if (out_bond_verified == NULL || out_identity_known == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_session_lock();
    const bool current = _ble_link_session_current(generation);
    if (current)
    {
        *out_bond_verified = s_session.bond_verified;
        *out_identity_known = s_session.identity_known;
    }
    _ble_link_session_unlock();
    return current ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void ble_link_session_set_authorization_transitioning(bool active)
{
    _ble_link_session_lock();
    s_session.authorization_transitioning = active;
    _ble_link_session_unlock();
}

void ble_link_session_set_error(bool error)
{
    _ble_link_session_lock();
    s_session.error_latched |= error;
    _ble_link_session_unlock();
}

uint32_t ble_link_session_get_state_flags(void)
{
    _ble_link_session_lock();
    const bool open = atomic_load_explicit(&s_session.pairing_window_open, memory_order_acquire);
    uint32_t flags = s_session.active ? BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED : 0U;
    if (open && !s_session.bound)
    {
        flags |= BLE_LINK_STATE_FLAG_BINDABLE;
    }
    else if (s_session.active && !open)
    {
        flags |= BLE_LINK_STATE_FLAG_PUBLIC_DISCOVERY;
    }
    if (s_session.bound)
    {
        flags |= BLE_LINK_STATE_FLAG_BOUND;
    }
    if (s_session.bound && s_session.encrypted && s_session.bond_verified && s_session.identity_known)
    {
        flags |= BLE_LINK_STATE_FLAG_AUTHENTICATED;
    }
    if (s_session.authorization_transitioning)
    {
        flags |= BLE_LINK_STATE_FLAG_TRANSITIONING;
    }
    if (s_session.error_latched)
    {
        flags |= BLE_LINK_STATE_FLAG_ERROR;
    }
    _ble_link_session_unlock();
    return flags;
}
