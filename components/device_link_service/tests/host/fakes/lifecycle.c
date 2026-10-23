#include "lifecycle.h"
#include <assert.h>
#include "ble_link_gatt.h"
#include "ble_link_service.h"
#include "ble_nimble_port.h"
#include "ble_nimble_port_revoke_journal.h"
#include "device_link_wifi_adapter.h"

atomic_bool fake_cleanup_pending;
atomic_bool fake_revoke_pending;
atomic_bool fake_delay_adv;
atomic_int fake_drain_result;
atomic_int fake_revoke_result;
atomic_uint fake_drain_calls;
atomic_uint fake_cleanup_checks;
atomic_int fake_drain_event;
atomic_uint fake_drain_event_at;
SemaphoreHandle_t fake_drain_entered;
SemaphoreHandle_t fake_drain_release;
static ble_link_service_wake_fn_t s_wake;
static void *s_wake_arg;
atomic_int fake_start_result;
atomic_int fake_stop_result;
atomic_int fake_deinit_result;
atomic_int fake_sync_result;
atomic_int fake_gate_event;
atomic_int fake_gate_result;
atomic_uint fake_flags;
atomic_uint fake_start_calls;
atomic_uint fake_stop_calls;
atomic_uint fake_deinit_calls;
atomic_int fake_adv_state;
atomic_bool fake_work_registered;
atomic_bool fake_bridge_fail;
SemaphoreHandle_t fake_start_entered;
SemaphoreHandle_t fake_start_release;
SemaphoreHandle_t fake_stop_entered;
SemaphoreHandle_t fake_stop_release;
static atomic_uint s_pauses;
static atomic_uint s_leases;
static atomic_bool s_path_active;
static atomic_bool s_port_initialized;

void fake_lifecycle_reset(void)
{
    assert(!atomic_load(&s_path_active));
    assert(!atomic_load(&s_port_initialized));
    atomic_store(&fake_cleanup_pending, false);
    atomic_store(&fake_revoke_pending, false);
    atomic_store(&fake_delay_adv, false);
    atomic_store(&fake_drain_result, ESP_OK);
    atomic_store(&fake_revoke_result, ESP_OK);
    atomic_store(&fake_drain_calls, 0U);
    atomic_store(&fake_cleanup_checks, 0U);
    atomic_store(&fake_drain_event, -1);
    atomic_store(&fake_drain_event_at, 1U);
    atomic_store(&fake_start_result, ESP_OK);
    atomic_store(&fake_stop_result, ESP_OK);
    atomic_store(&fake_deinit_result, ESP_OK);
    atomic_store(&fake_sync_result, ESP_OK);
    atomic_store(&fake_gate_event, -1);
    atomic_store(&fake_gate_result, ESP_OK);
    atomic_store(&fake_flags, 0U);
    atomic_store(&fake_start_calls, 0U);
    atomic_store(&fake_stop_calls, 0U);
    atomic_store(&fake_deinit_calls, 0U);
    atomic_store(&fake_adv_state, BLE_ADV_MANAGER_STATE_STOPPED);
    atomic_store(&fake_bridge_fail, false);
    atomic_store(&s_pauses, 0U);
    atomic_store(&s_leases, 0U);
    ble_event_router_init();
}

void fake_lifecycle_event(ble_port_event_type_t type)
{
    const ble_port_event_t event =
    {
        .type = type,
        .accepted = true,
        .conn_handle = 7U,
        .identity =
        {
            .generation = 1U,
            .conn_handle = 7U,
            .kind = type == BLE_PORT_EVENT_CONNECT ? BLE_LINK_OPERATION_CONNECT :
            BLE_LINK_OPERATION_DISCONNECT,
        },
    };
    assert(ble_event_router_dispatch(&event) == ESP_OK);
}

static esp_err_t _port_init(void)
{
    assert(!atomic_exchange(&s_path_active, true));
    assert(!atomic_exchange(&s_port_initialized, true));
    atomic_store(&s_pauses, 0U);
    atomic_store(&s_leases, 0U);
    atomic_store(&fake_adv_state, BLE_ADV_MANAGER_STATE_STOPPED);
    atomic_store(&s_path_active, false);
    return ESP_OK;
}
static esp_err_t _port_start(void)
{
    assert(!atomic_exchange(&s_path_active, true));
    atomic_fetch_add(&fake_start_calls, 1U);
    if (fake_start_entered != NULL)
    {
        xSemaphoreGive(fake_start_entered);
        assert(xSemaphoreTake(fake_start_release, portMAX_DELAY) == pdTRUE);
    }
    const esp_err_t result = atomic_load(&fake_start_result);
    atomic_store(&s_path_active, false);
    return result;
}
static esp_err_t _port_stop(void)
{
    assert(!atomic_exchange(&s_path_active, true));
    assert(!atomic_load(&fake_cleanup_pending));
    assert(!atomic_load(&fake_revoke_pending));
    atomic_fetch_add(&fake_stop_calls, 1U);
    if (fake_stop_entered != NULL)
    {
        xSemaphoreGive(fake_stop_entered);
        assert(xSemaphoreTake(fake_stop_release, portMAX_DELAY) == pdTRUE);
    }
    const esp_err_t result = atomic_load(&fake_stop_result);
    atomic_store(&s_path_active, false);
    return result;
}
static esp_err_t _port_deinit(void)
{
    assert(!atomic_exchange(&s_path_active, true));
    atomic_fetch_add(&fake_deinit_calls, 1U);
    const esp_err_t result = atomic_load(&fake_deinit_result);
    if (result == ESP_OK) atomic_store(&s_port_initialized, false);
    atomic_store(&s_path_active, false);
    return result;
}
static esp_err_t _port_gate(bool open)
{
    (void)open;
    assert(atomic_load(&s_port_initialized));
    const int type = atomic_exchange(&fake_gate_event, -1);
    if (type >= 0)
    {
        /* Synchronous host callback proves the service mutex was released. */
        fake_lifecycle_event((ble_port_event_type_t)type);
    }
    return atomic_load(&fake_gate_result);
}
static esp_err_t _port_reset_store(void)
{
    return ESP_OK;
}
const ble_runtime_host_port_t fake_runtime_port =
{
    .init = _port_init, .start = _port_start, .stop = _port_stop,
    .deinit = _port_deinit, .set_pairing_gate = _port_gate,
    .reset_peer_store = _port_reset_store,
};

static void _adv_refresh(void)
{
    const unsigned leases = atomic_load(&s_leases);
    atomic_store(&fake_adv_state, atomic_load(&s_pauses) != 0U || leases == 0U
                 ? BLE_ADV_MANAGER_STATE_STOPPED
                 : atomic_load(&fake_delay_adv) ? BLE_ADV_MANAGER_STATE_STARTING
                 : (leases & 2U) != 0U ? BLE_ADV_MANAGER_STATE_FAST
                 : BLE_ADV_MANAGER_STATE_SLOW);
}
ble_adv_manager_state_t ble_adv_manager_get_state(void)
{
    return (ble_adv_manager_state_t)atomic_load(&fake_adv_state);
}
esp_err_t ble_adv_manager_set_pause_reason(ble_adv_manager_pause_reason_t reason, bool active)
{
    if (active) atomic_fetch_or(&s_pauses, reason);
    else atomic_fetch_and(&s_pauses, ~reason);
    _adv_refresh();
    return ESP_OK;
}
esp_err_t ble_adv_manager_acquire_lease(ble_adv_lease_t *lease,
                                        ble_adv_manager_mode_t mode, bool bindable)
{
    (void)mode;
    lease->lease_id = bindable ? 2U : 1U;
    atomic_fetch_or(&s_leases, lease->lease_id);
    _adv_refresh();
    return ESP_OK;
}
esp_err_t ble_adv_manager_release_lease(uint8_t id)
{
    atomic_fetch_and(&s_leases, ~id);
    _adv_refresh();
    return ESP_OK;
}
void ble_adv_manager_poll(void) {}

void ble_link_gatt_set_work_submit(ble_link_work_submit_fn submit, void *arg)
{
    (void)arg;
    atomic_store(&fake_work_registered, submit != NULL);
}
uint32_t ble_link_session_get_state_flags(void)
{
    return atomic_load(&fake_flags);
}
void ble_link_session_set_pairing_window(bool open)
{
    (void)open;
}
esp_err_t ble_link_session_set_authorization(bool committed, uint32_t revision)
{
    (void)committed;
    (void)revision;
    return ESP_OK;
}
ble_link_confirmation_snapshot_t ble_link_service_get_confirmation(void)
{
    return (ble_link_confirmation_snapshot_t)
    {
        0
    };
}
void ble_link_service_clear_session_state(void) {}
esp_err_t ble_link_service_clear_session_state_if_current(const ble_link_operation_identity_t *id)
{
    (void)id;
    return ESP_OK;
}
esp_err_t ble_link_service_confirm_binding(uint64_t token, bool accept)
{
    (void)token;
    (void)accept;
    return ESP_OK;
}
esp_err_t ble_link_service_execute(ble_link_work_t *work)
{
    (void)work;
    return ESP_OK;
}
void ble_link_service_release_work(ble_link_work_t *work)
{
    (void)work;
}
uint32_t ble_link_service_operation_timeout_remaining_ms(void)
{
    return UINT32_MAX;
}
esp_err_t ble_link_service_pump_tx(void)
{
    return ESP_OK;
}
esp_err_t ble_link_service_set_domain_descriptors(const void *domains, size_t count)
{
    (void)domains;
    (void)count;
    return ESP_OK;
}
void ble_link_service_set_pairing_window(bool open)
{
    (void)open;
}
void ble_link_service_set_worker_wake(ble_link_service_wake_fn_t wake, void *arg)
{
    s_wake = wake;
    s_wake_arg = arg;
}
void fake_lifecycle_adv_complete(ble_adv_manager_state_t state)
{
    atomic_store(&fake_adv_state, state);
    assert(s_wake != NULL);
    s_wake(s_wake_arg);
}
void ble_link_service_tick(uint32_t now)
{
    (void)now;
}
esp_err_t ble_nimble_port_wait_for_host_sync(uint32_t timeout)
{
    (void)timeout;
    return atomic_load(&fake_sync_result);
}
esp_err_t ble_nimble_port_begin_cleanup_drain(void)
{
    const unsigned call = atomic_fetch_add(&fake_drain_calls, 1U) + 1U;
    if (fake_drain_entered != NULL)
    {
        xSemaphoreGive(fake_drain_entered);
        assert(xSemaphoreTake(fake_drain_release, portMAX_DELAY) == pdTRUE);
    }
    const int event = call == atomic_load(&fake_drain_event_at) ?
                      atomic_exchange(&fake_drain_event, -1) : -1;

    if (event >= 0)
    {
        fake_lifecycle_event((ble_port_event_type_t)event);
        atomic_store(&fake_cleanup_pending, true);
    }
    return atomic_load(&fake_drain_result);
}
bool ble_nimble_port_cleanup_pending(void)
{
    atomic_fetch_add(&fake_cleanup_checks, 1U);
    return atomic_load(&fake_cleanup_pending);
}
void ble_nimble_port_numeric_comparison_cancel(void) {}
esp_err_t ble_nimble_port_numeric_comparison_reply(uint64_t token, bool accept)
{
    return ble_link_service_confirm_binding(token, accept);
}
esp_err_t ble_nimble_port_request_disconnect(void)
{
    return ESP_OK;
}
esp_err_t ble_nimble_port_revoke_journal_begin(void)
{
    atomic_store(&fake_revoke_pending, true);
    return ESP_OK;
}
esp_err_t ble_nimble_port_revoke_binding(void)
{
    return atomic_load(&fake_revoke_result);
}
esp_err_t ble_nimble_port_revoke_journal_pending(bool *pending)
{
    *pending = atomic_load(&fake_revoke_pending);
    return ESP_OK;
}
void device_link_wifi_adapter_set_firmware(uint8_t a, uint8_t b, uint8_t c)
{
    (void)a;
    (void)b;
    (void)c;
}
esp_err_t device_link_wifi_adapter_get_descriptor(const void **descriptor)
{
    *descriptor = NULL;
    return ESP_OK;
}
esp_err_t device_link_wifi_adapter_bridge_start(void)
{
    return atomic_load(&fake_bridge_fail) ? ESP_FAIL : ESP_OK;
}
void device_link_wifi_adapter_bridge_stop(void) {}
void device_link_wifi_adapter_tick(void) {}
