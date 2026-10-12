#include <assert.h>
#include <stddef.h>
#include "ble_link_session.h"

static void _connect(uint32_t generation)
{
    assert(ble_link_session_handle_event(generation, BLE_LINK_SESSION_EVENT_ACL_CONNECTED) == ESP_OK);
}

static void _authenticate(uint32_t generation)
{
    assert(ble_link_session_handle_event(generation, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED) == ESP_OK);
    assert(ble_link_session_handle_event(generation, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED) == ESP_OK);
    assert(ble_link_session_set_identity_known(generation, true) == ESP_OK);
}

static void _test_generation_and_reconnect(void)
{
    ble_link_dispatcher_facts_t facts;
    ble_link_session_init(42U);
    assert(ble_link_session_get_facts(1U, &facts) == ESP_ERR_INVALID_STATE);
    _connect(1U);
    assert(ble_link_session_handle_event(2U, BLE_LINK_SESSION_EVENT_ACL_CONNECTED) == ESP_ERR_INVALID_STATE);
    assert(ble_link_session_handle_event(2U, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED) == ESP_OK);
    assert(ble_link_session_get_facts(1U, &facts) == ESP_OK);
    assert(facts.active_boot_id == 42U && !facts.encrypted);
    _authenticate(1U);
    assert(ble_link_session_handle_event(1U, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED) == ESP_OK);
    _connect(1U);
    assert(ble_link_session_get_facts(1U, &facts) == ESP_ERR_INVALID_STATE);
    _connect(2U);
    _authenticate(2U);
    assert(ble_link_session_handle_event(1U, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED) == ESP_OK);
    assert(ble_link_session_get_facts(2U, &facts) == ESP_OK && facts.encrypted);
}

static void _test_bond_security_and_window(void)
{
    ble_link_dispatcher_facts_t facts;
    bool bond, identity, window;
    ble_link_session_init(42U);
    ble_link_session_set_pairing_window(true);
    _connect(1U);
    assert(ble_link_session_set_connection_pairing_window(1U, true) == ESP_OK);
    ble_link_session_set_pairing_window(false);
    assert(ble_link_session_get_connection_pairing_window(1U, &window) == ESP_OK && window);
    assert(ble_link_session_set_authorization(true, 5U) == ESP_OK);
    assert(ble_link_session_handle_event(1U, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED) == ESP_OK);
    assert((ble_link_session_get_state_flags() & BLE_LINK_STATE_FLAG_AUTHENTICATED) == 0U);
    assert(ble_link_session_handle_event(1U, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED) == ESP_OK);
    assert((ble_link_session_get_state_flags() & BLE_LINK_STATE_FLAG_AUTHENTICATED) == 0U);
    assert(ble_link_session_set_identity_known(1U, true) == ESP_OK);
    assert((ble_link_session_get_state_flags() & BLE_LINK_STATE_FLAG_AUTHENTICATED) != 0U);
    ble_link_session_clear_link_security(2U);
    assert(ble_link_session_get_facts(1U, &facts) == ESP_OK && facts.encrypted);
    ble_link_session_clear_link_security(1U);
    assert(ble_link_session_get_security_facts(1U, &bond, &identity) == ESP_OK);
    assert(!bond && !identity);
    assert((ble_link_session_get_state_flags() & BLE_LINK_STATE_FLAG_AUTHENTICATED) == 0U);
    _authenticate(1U);
    assert(ble_link_session_set_authorization(false, 6U) == ESP_OK);
    assert(ble_link_session_set_authorization(true, 5U) == ESP_OK);
    assert((ble_link_session_get_state_flags() & BLE_LINK_STATE_FLAG_BOUND) == 0U);
    assert(ble_link_session_set_authorization(true, UINT32_MAX) == ESP_OK);
    assert(ble_link_session_set_authorization(true, 0U) == ESP_ERR_INVALID_STATE);
    assert(ble_link_session_set_authorization(false, 0U) == ESP_OK);
    assert((ble_link_session_get_state_flags() & BLE_LINK_STATE_FLAG_BOUND) == 0U);
}

static void _test_reset_and_invalid_arguments(void)
{
    ble_link_dispatcher_facts_t facts;
    bool value;
    ble_link_session_init(42U);
    _connect(1U);
    ble_link_session_set_authorization_transitioning(true);
    ble_link_session_set_error(true);
    ble_link_session_set_error(false);
    assert((ble_link_session_get_state_flags() & BLE_LINK_STATE_FLAG_ERROR) != 0U);
    ble_link_session_reset();
    assert(ble_link_session_get_state_flags() == 0U);
    _connect(2U);
    assert(ble_link_session_get_facts(2U, &facts) == ESP_OK && facts.active_boot_id == 42U);
    assert(ble_link_session_get_connection_pairing_window(2U, &value) == ESP_OK && !value);
    assert(ble_link_session_get_facts(2U, NULL) == ESP_ERR_INVALID_ARG);
    assert(ble_link_session_get_security_facts(2U, NULL, &value) == ESP_ERR_INVALID_ARG);
    assert(ble_link_session_get_connection_pairing_window(2U, NULL) == ESP_ERR_INVALID_ARG);
    assert(ble_link_session_set_identity_known(1U, true) == ESP_ERR_INVALID_STATE);
    assert(ble_link_session_handle_event(2U, (ble_link_session_event_t)99) == ESP_ERR_INVALID_ARG);
    ble_link_session_init(99U);
    _connect(1U);
    assert(ble_link_session_get_facts(1U, &facts) == ESP_OK && facts.active_boot_id == 99U);
}

int main(void)
{
    _test_bond_security_and_window();
    _test_reset_and_invalid_arguments();
    /* Stale identity updates are rejected, while stale asynchronous events are ignored. */
    ble_link_session_init(42U);
    _connect(1U);
    assert(ble_link_session_handle_event(1U, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED) == ESP_OK);
    _connect(2U);
    assert(ble_link_session_handle_event(1U, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED) == ESP_OK);
    assert(ble_link_session_set_identity_known(1U, true) == ESP_ERR_INVALID_STATE);
    _test_generation_and_reconnect();
    return 0;
}
