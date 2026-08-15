#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"

#include "ble_link_session.h"

#define DBG_TAG "ble_link_session"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

typedef struct ble_link_session
{
    uint64_t boot_id;
    uint32_t generation;
    uint32_t last_generation;    /**< Monotonic high-water mark. */
    bool active;                 /**< ACL connected for the current gen. */
    bool encrypted;
    bool bond_verified;
    bool identity_known;
    bool connection_pairing_window_open;
    uint32_t security2_epoch;    /**< Last accepted Security 2 epoch. */
    bool security2_handshaking;
    bool security2_open;
    bool authorized;
    uint32_t authorization_revision;
    bool bound;                  /**< Committed authorization record exists. */
    bool authorization_transitioning; /**< Authorization txn in flight. */
    bool error_latched;          /**< Unrecoverable runtime state. */
    /* The pairing window is written by the project-core device-link worker
     * and read by NimBLE host-core admission paths, so it must be atomic. */
    atomic_bool pairing_window_open;
} ble_link_session_t;

static ble_link_session_t s_session;
static StaticSemaphore_t s_session_mutex_control;
static SemaphoreHandle_t s_session_mutex;
static esp_err_t _ble_link_session_report_session_match_locked(
    uint32_t generation, uint32_t revision, uint32_t security2_epoch);

static void _ble_link_session_lock(void)
{
    if (s_session_mutex != NULL)
    {
        (void)xSemaphoreTakeRecursive(s_session_mutex, portMAX_DELAY);
    }
}

static void _ble_link_session_unlock(void)
{
    if (s_session_mutex != NULL)
    {
        (void)xSemaphoreGiveRecursive(s_session_mutex);
    }
}

void ble_link_session_init(uint64_t boot_id)
{
    if (s_session_mutex == NULL)
    {
        s_session_mutex = xSemaphoreCreateRecursiveMutexStatic(
                              &s_session_mutex_control);
    }
    _ble_link_session_lock();
    memset(&s_session, 0, sizeof(s_session));
    atomic_init(&s_session.pairing_window_open, false);
    s_session.boot_id = boot_id;
    _ble_link_session_unlock();
}

static void _ble_link_session_reset_locked(void)
{
    /* The boot id and epoch allocator are boot-scoped and survive a full
     * teardown (a runtime restart keeps them); only a fresh boot via
     * init() resets them. */
    const uint64_t boot_id = s_session.boot_id;
    const uint32_t epoch = s_session.security2_epoch;

    memset(&s_session, 0, sizeof(s_session));
    atomic_init(&s_session.pairing_window_open, false);
    s_session.boot_id = boot_id;
    s_session.security2_epoch = epoch;
}

#ifdef UNIT_TEST_HOST
void ble_link_session_test_set_epoch(uint32_t value)
{
    _ble_link_session_lock();
    s_session.security2_epoch = value;
    _ble_link_session_unlock();
}
#endif

void ble_link_session_set_pairing_window(bool open)
{
    atomic_store_explicit(&s_session.pairing_window_open, open,
                          memory_order_release);
}

static void _ble_link_session_clear_link_security_locked(uint32_t generation)
{
    if (generation != s_session.generation || !s_session.active)
    {
        return;
    }
    s_session.encrypted = false;
    s_session.bond_verified = false;
    s_session.identity_known = false;
}

static void _ble_link_session_clear_connection(void)
{
    s_session.active = false;
    s_session.encrypted = false;
    s_session.bond_verified = false;
    s_session.identity_known = false;
    s_session.security2_handshaking = false;
    /* The epoch allocator is boot-scoped and never resets on disconnect. */
    s_session.security2_open = false;
    s_session.authorized = false;
}

static esp_err_t _ble_link_session_handle_event_locked(
    uint32_t generation, ble_link_session_event_t event)
{
    switch (event)
    {
    case BLE_LINK_SESSION_EVENT_ACL_CONNECTED:
        if (generation <= s_session.last_generation)
        {
            return ESP_OK;
        }
        if (s_session.active)
        {
            return ESP_ERR_INVALID_STATE;
        }
        s_session.last_generation = generation;
        s_session.generation = generation;
        s_session.active = true;
        s_session.encrypted = false;
        s_session.bond_verified = false;
        s_session.identity_known = false;
        s_session.security2_open = false;
        s_session.authorized = false;
        return ESP_OK;
    case BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED:
        if (generation != s_session.generation)
        {
            return ESP_OK;
        }
        _ble_link_session_clear_connection();
        return ESP_OK;
    case BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED:
    case BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED:
        if (generation != s_session.generation || !s_session.active)
        {
            return ESP_OK;
        }
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    switch (event)
    {
    case BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED:
        s_session.encrypted = true;
        break;
    case BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED:
        s_session.bond_verified = true;
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t ble_link_session_set_connection_pairing_window(
    uint32_t generation, bool open)
{
    _ble_link_session_lock();
    if (generation != s_session.generation || !s_session.active)
    {
        _ble_link_session_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_session.connection_pairing_window_open = open;
    _ble_link_session_unlock();
    return ESP_OK;
}

esp_err_t ble_link_session_get_connection_pairing_window(
    uint32_t generation, bool *out_open)
{
    if (out_open == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_session_lock();
    if (generation != s_session.generation || !s_session.active)
    {
        _ble_link_session_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    *out_open = s_session.connection_pairing_window_open;
    _ble_link_session_unlock();
    return ESP_OK;
}

static esp_err_t _ble_link_session_security2_begin_locked(
    uint32_t generation, uint32_t *out_epoch)
{
    if (out_epoch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (generation != s_session.generation || !s_session.active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session.security2_epoch >= UINT32_MAX)
    {
        s_session.security2_handshaking = false;
        s_session.security2_open = false;
        s_session.authorized = false;
        return ESP_ERR_INVALID_STATE;
    }
    s_session.security2_epoch++;
    s_session.security2_handshaking = true;
    s_session.security2_open = false;
    /* Any epoch change invalidates the previous session match. */
    s_session.authorized = false;
    *out_epoch = s_session.security2_epoch;
    return ESP_OK;
}

static esp_err_t _ble_link_session_security2_authenticate_current_locked(
    uint32_t generation, uint32_t epoch)
{
    if (generation != s_session.generation || !s_session.active ||
            !s_session.security2_handshaking ||
            epoch != s_session.security2_epoch)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_session.security2_handshaking = false;
    s_session.security2_open = true;
    return ESP_OK;
}

static bool _ble_link_session_authorization_exhausted_locked(void)
{
    return s_session.authorization_revision == UINT32_MAX;
}

static esp_err_t _ble_link_session_set_authorization_locked(
    bool committed, uint32_t revision)
{
    if (revision == 0U)
    {
        /* Revision 0 means "next revision": the caller does not track
         * the current revision, and a revoke must always take effect
         * even after a revision 1 was installed. At the exhausted
         * maximum the monotonic space cannot advance: a revoke still
         * applies, but a commit fails closed (no resurrection). */
        if (s_session.authorization_revision == UINT32_MAX)
        {
            if (committed)
            {
                return ESP_ERR_INVALID_STATE;
            }
            s_session.bound = false;
            s_session.authorized = false;
            return ESP_OK;
        }
        revision = s_session.authorization_revision + 1U;
    }
    if (revision <= s_session.authorization_revision)
    {
        return ESP_OK;
    }
    s_session.authorization_revision = revision;
    s_session.bound = committed;
    /* A record change invalidates the current session match. */
    s_session.authorized = false;
    return ESP_OK;
}

static esp_err_t _ble_link_session_report_session_match_current_locked(
    uint32_t generation, uint32_t revision)
{
    return _ble_link_session_report_session_match_locked(
               generation, revision, s_session.security2_epoch);
}

static esp_err_t _ble_link_session_security2_close_current_locked(
    uint32_t generation)
{
    if (generation != s_session.generation || !s_session.active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* The allocator never wraps: at the maximum it is locked out and the
     * session cannot be reopened for the rest of the boot. */
    if (s_session.security2_epoch < UINT32_MAX)
    {
        s_session.security2_epoch++;
    }
    s_session.security2_handshaking = false;
    s_session.security2_open = false;
    s_session.authorized = false;
    return ESP_OK;
}

static esp_err_t _ble_link_session_report_session_match_locked(
    uint32_t generation, uint32_t revision, uint32_t security2_epoch)
{
    if (generation != s_session.generation || !s_session.active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_session.security2_open ||
            security2_epoch != s_session.security2_epoch)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_session.bound ||
            (revision != 0U &&
             revision != s_session.authorization_revision))
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_session.authorized = true;
    return ESP_OK;
}

static esp_err_t _ble_link_session_query_admission_locked(
    uint32_t generation, ble_link_session_channel_t channel,
    uint32_t *out_error)
{
    if (out_error == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    switch (channel)
    {
    case BLE_LINK_SESSION_CHANNEL_LINK_STATE:
    case BLE_LINK_SESSION_CHANNEL_SESSION:
    case BLE_LINK_SESSION_CHANNEL_CONTROL:
    case BLE_LINK_SESSION_CHANNEL_EVENT:
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (generation != s_session.generation || !s_session.active)
    {
        *out_error = BLE_LINK_ERROR_UNAVAILABLE;
        return ESP_OK;
    }
    switch (channel)
    {
    case BLE_LINK_SESSION_CHANNEL_LINK_STATE:
        *out_error = BLE_LINK_ERROR_OK;
        break;
    case BLE_LINK_SESSION_CHANNEL_SESSION:
        if (s_session.encrypted && s_session.bond_verified &&
                s_session.identity_known)
        {
            *out_error = BLE_LINK_ERROR_OK;
        }
        else
        {
            *out_error = BLE_LINK_ERROR_UNAUTHENTICATED;
        }
        break;
    case BLE_LINK_SESSION_CHANNEL_CONTROL:
    case BLE_LINK_SESSION_CHANNEL_EVENT:
        if (!s_session.encrypted || !s_session.bond_verified ||
                !s_session.identity_known || !s_session.security2_open)
        {
            *out_error = BLE_LINK_ERROR_UNAUTHENTICATED;
        }
        else if (!s_session.authorized)
        {
            *out_error = BLE_LINK_ERROR_PERMISSION_DENIED;
        }
        else
        {
            *out_error = BLE_LINK_ERROR_OK;
        }
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static ble_link_session_state_t _ble_link_session_get_state_locked(
    uint32_t generation)
{
    if (generation != s_session.generation || !s_session.active)
    {
        return BLE_LINK_SESSION_INACTIVE;
    }
    if (s_session.encrypted && s_session.bond_verified &&
            s_session.identity_known && s_session.security2_open &&
            s_session.authorized)
    {
        /* Exactly the control/session admission condition: a state name
         * must never admit traffic the query would reject. */
        return BLE_LINK_SESSION_AUTHORIZED;
    }
    if (s_session.encrypted && s_session.bond_verified &&
            s_session.identity_known)
    {
        return BLE_LINK_SESSION_AUTHENTICATED;
    }
    return BLE_LINK_SESSION_CONNECTED;
}

static esp_err_t _ble_link_session_get_facts_locked(
    uint32_t generation, ble_link_dispatcher_facts_t *facts)
{
    if (facts == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (generation != s_session.generation || !s_session.active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(facts, 0, sizeof(*facts));
    facts->active_boot_id = s_session.boot_id;
    facts->connection_generation = s_session.generation;
    facts->encrypted = s_session.encrypted;
    facts->session_authenticated = s_session.security2_open;
    facts->authorized = s_session.authorized;
    return ESP_OK;
}

static esp_err_t _ble_link_session_set_identity_known_locked(
    uint32_t generation, bool known)
{
    if (generation != s_session.generation || !s_session.active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_session.identity_known = known;
    if (!known)
    {
        /* An unverified identity revokes the current session match. */
        s_session.authorized = false;
    }
    return ESP_OK;
}

static esp_err_t _ble_link_session_get_security_facts_locked(
    uint32_t generation, bool *out_bond_verified,
    bool *out_identity_known)
{
    if (out_bond_verified == NULL || out_identity_known == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (generation != s_session.generation || !s_session.active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *out_bond_verified = s_session.bond_verified;
    *out_identity_known = s_session.identity_known;
    return ESP_OK;
}

static uint32_t _ble_link_session_get_state_flags_locked(void)
{
    uint32_t flags = 0U;

    /* The characteristic is readable on an accepted ACL.  An active ACL is
     * therefore the session owner's reliable indication that the Bluetooth
     * runtime is enabled; the discovery bit describes the current policy,
     * not whether connectable advertising is physically running for this ACL. */
    if (s_session.active)
    {
        flags |= BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED;
    }
    /* BINDABLE advertises the QR bind window; it is mutually exclusive with
     * BOUND (a bound device no longer binds) and with PUBLIC_DISCOVERY.
     * PUBLIC_DISCOVERY reflects the advertising policy (window closed) and
     * stays independent of BOUND: a bound device keeps advertising its
     * public bootstrap endpoint. */
    if (atomic_load_explicit(&s_session.pairing_window_open,
                             memory_order_acquire) &&
            !s_session.bound)
    {
        flags |= BLE_LINK_STATE_FLAG_BINDABLE;
    }
    else if (s_session.active &&
             !atomic_load_explicit(&s_session.pairing_window_open,
                                   memory_order_acquire))
    {
        flags |= BLE_LINK_STATE_FLAG_PUBLIC_DISCOVERY;
    }
    if (s_session.bound)
    {
        flags |= BLE_LINK_STATE_FLAG_BOUND;
    }
    /* AUTHENTICATED implies BOUND: a bootstrap Security 2 session without a
     * committed record does not publish the flag. */
    if (s_session.bound && s_session.encrypted && s_session.bond_verified &&
            s_session.identity_known && s_session.security2_open)
    {
        flags |= BLE_LINK_STATE_FLAG_AUTHENTICATED;
    }
    if (s_session.authorized)
    {
        flags |= BLE_LINK_STATE_FLAG_AUTHORIZED;
    }
    if (s_session.security2_handshaking ||
            s_session.authorization_transitioning)
    {
        flags |= BLE_LINK_STATE_FLAG_TRANSITIONING;
    }
    if (s_session.error_latched ||
            s_session.security2_epoch == UINT32_MAX)
    {
        flags |= BLE_LINK_STATE_FLAG_ERROR;
    }
    return flags;
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
    if (error)
    {
        s_session.error_latched = true;
    }
    _ble_link_session_unlock();
}

void ble_link_session_reset(void)
{
    _ble_link_session_lock();
    _ble_link_session_reset_locked();
    _ble_link_session_unlock();
}

void ble_link_session_clear_link_security(uint32_t generation)
{
    _ble_link_session_lock();
    _ble_link_session_clear_link_security_locked(generation);
    _ble_link_session_unlock();
}

esp_err_t ble_link_session_handle_event(
    uint32_t generation, ble_link_session_event_t event)
{
    _ble_link_session_lock();
    const esp_err_t result = _ble_link_session_handle_event_locked(
                                 generation, event);

    _ble_link_session_unlock();
    return result;
}

esp_err_t ble_link_session_security2_begin(
    uint32_t generation, uint32_t *out_epoch)
{
    _ble_link_session_lock();
    const esp_err_t result = _ble_link_session_security2_begin_locked(
                                 generation, out_epoch);

    _ble_link_session_unlock();
    return result;
}

esp_err_t ble_link_session_security2_authenticate_current(
    uint32_t generation, uint32_t epoch)
{
    _ble_link_session_lock();
    const esp_err_t result =
        _ble_link_session_security2_authenticate_current_locked(
            generation, epoch);

    _ble_link_session_unlock();
    return result;
}

esp_err_t ble_link_session_security2_open(
    uint32_t generation, uint32_t *out_epoch)
{
    _ble_link_session_lock();
    esp_err_t result = _ble_link_session_security2_begin_locked(
                           generation, out_epoch);

    if (result == ESP_OK)
    {
        result = _ble_link_session_security2_authenticate_current_locked(
                     generation, *out_epoch);
    }
    _ble_link_session_unlock();
    return result;
}

uint32_t ble_link_session_security2_epoch(void)
{
    _ble_link_session_lock();
    const uint32_t epoch = s_session.security2_epoch;

    _ble_link_session_unlock();
    return epoch;
}

bool ble_link_session_authorization_exhausted(void)
{
    _ble_link_session_lock();
    const bool exhausted = _ble_link_session_authorization_exhausted_locked();

    _ble_link_session_unlock();
    return exhausted;
}

esp_err_t ble_link_session_set_authorization(bool committed, uint32_t revision)
{
    _ble_link_session_lock();
    const esp_err_t result = _ble_link_session_set_authorization_locked(
                                 committed, revision);

    _ble_link_session_unlock();
    return result;
}

esp_err_t ble_link_session_report_session_match_current(
    uint32_t generation, uint32_t revision)
{
    _ble_link_session_lock();
    const esp_err_t result =
        _ble_link_session_report_session_match_current_locked(
            generation, revision);

    _ble_link_session_unlock();
    return result;
}

esp_err_t ble_link_session_security2_close_current(uint32_t generation)
{
    _ble_link_session_lock();
    const esp_err_t result =
        _ble_link_session_security2_close_current_locked(generation);

    _ble_link_session_unlock();
    return result;
}

esp_err_t ble_link_session_report_session_match(
    uint32_t generation, uint32_t revision, uint32_t security2_epoch)
{
    _ble_link_session_lock();
    const esp_err_t result = _ble_link_session_report_session_match_locked(
                                 generation, revision, security2_epoch);

    _ble_link_session_unlock();
    return result;
}

esp_err_t ble_link_session_query_admission(
    uint32_t generation, ble_link_session_channel_t channel,
    uint32_t *out_error)
{
    _ble_link_session_lock();
    const esp_err_t result = _ble_link_session_query_admission_locked(
                                 generation, channel, out_error);

    _ble_link_session_unlock();
    return result;
}

ble_link_session_state_t ble_link_session_get_state(uint32_t generation)
{
    _ble_link_session_lock();
    const ble_link_session_state_t state =
        _ble_link_session_get_state_locked(generation);

    _ble_link_session_unlock();
    return state;
}

esp_err_t ble_link_session_get_facts(
    uint32_t generation, ble_link_dispatcher_facts_t *facts)
{
    _ble_link_session_lock();
    const esp_err_t result = _ble_link_session_get_facts_locked(
                                 generation, facts);

    _ble_link_session_unlock();
    return result;
}

esp_err_t ble_link_session_set_identity_known(uint32_t generation, bool known)
{
    _ble_link_session_lock();
    const esp_err_t result = _ble_link_session_set_identity_known_locked(
                                 generation, known);

    _ble_link_session_unlock();
    return result;
}

esp_err_t ble_link_session_get_security_facts(
    uint32_t generation, bool *out_bond_verified,
    bool *out_identity_known)
{
    _ble_link_session_lock();
    const esp_err_t result = _ble_link_session_get_security_facts_locked(
                                 generation, out_bond_verified,
                                 out_identity_known);

    _ble_link_session_unlock();
    return result;
}

uint32_t ble_link_session_get_state_flags(void)
{
    _ble_link_session_lock();
    const uint32_t flags = _ble_link_session_get_state_flags_locked();

    _ble_link_session_unlock();
    return flags;
}
