#include <assert.h>
#include <stdio.h>
#include "lifecycle.h"
#include "host_freertos.h"
#include "host_nv_storage.h"
#include "ble_runtime.h"

static const device_link_service_config_t s_config =
{
    .runtime_port = &fake_runtime_port,
    .window_ms = 120000U,
    .task_priority = 2U,
};

static atomic_bool s_pause_disabled_write;
static SemaphoreHandle_t s_policy_entered;
static SemaphoreHandle_t s_policy_release;

esp_err_t __real_nv_storage_set_blob(const char *key, const void *data, size_t size);

esp_err_t __wrap_nv_storage_set_blob(const char *key, const void *data, size_t size)
{
    /* The version-1 Bluetooth policy stores enabled at byte 5. Pause the
     * rollback that restores disabled after an unsuccessful startup. */
    if (size == 8U && ((const uint8_t *)data)[5] == 0U &&
            atomic_exchange(&s_pause_disabled_write, false))
    {
        xSemaphoreGive(s_policy_entered);
        assert(xSemaphoreTake(s_policy_release, portMAX_DELAY) == pdTRUE);
    }
    return __real_nv_storage_set_blob(key, data, size);
}

static device_link_service_status_t _status(void)
{
    device_link_service_status_t status;
    assert(device_link_service_get_status(&status) == ESP_OK);
    return status;
}

static void _wait_settled(void)
{
    for (unsigned i = 0U; i < 3000U; ++i)
    {
        if (!_status().transitioning) return;
        vTaskDelay(1U);
    }
    assert(!"service did not settle");
}

static void _init(void)
{
    host_freertos_reset_controls();
    host_nv_storage_reset();
    fake_lifecycle_reset();
    assert(device_link_service_init(&s_config) == ESP_OK);
}

static void _deinit(void)
{
    assert(device_link_service_deinit(2000U) == ESP_OK);
    assert(host_freertos_wait_no_tasks(1000U));
    assert(host_freertos_live_semaphores() == 0U);
    assert(host_freertos_live_queues() == 0U);
    assert(!atomic_load(&fake_work_registered));
}

static void _barrier_create(SemaphoreHandle_t *entered, SemaphoreHandle_t *release)
{
    static StaticSemaphore_t storage[4];
    const unsigned slot = entered == &fake_start_entered ? 0U : 2U;

    *entered = xSemaphoreCreateBinaryStatic(&storage[slot]);
    *release = xSemaphoreCreateBinaryStatic(&storage[slot + 1U]);
    assert(*entered != NULL && *release != NULL);
}
static void _barrier_delete(SemaphoreHandle_t *entered, SemaphoreHandle_t *release)
{
    vSemaphoreDelete(*entered);
    vSemaphoreDelete(*release);
    *entered = NULL;
    *release = NULL;
}

static void test_immediate_reversals_and_idempotence(void)
{
    _init();
    const unsigned starts = atomic_load(&fake_start_calls);
    for (unsigned i = 0U; i < 50U; ++i)
    {
        assert(device_link_service_set_enabled(true, 1000U) == ESP_OK);
        assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
        assert(!_status().enabled);
        assert(device_link_service_set_enabled(true, 1000U) == ESP_OK);
        assert(_status().enabled);
    }
    assert(atomic_load(&fake_start_calls) == starts + 50U);
    _deinit();
}

static void test_reverse_while_starting_and_stopping(void)
{
    _init();
    assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
    _barrier_create(&fake_start_entered, &fake_start_release);
    assert(device_link_service_set_enabled(true, 0U) == ESP_OK);
    assert(xSemaphoreTake(fake_start_entered, 1000U) == pdTRUE);
    const unsigned starts = atomic_load(&fake_start_calls);
    assert(device_link_service_set_enabled(false, 5U) == ESP_ERR_TIMEOUT);
    assert(_status().transitioning);
    assert(device_link_service_set_enabled(true, 0U) == ESP_OK);
    assert(device_link_service_set_enabled(false, 0U) == ESP_OK);
    xSemaphoreGive(fake_start_release);
    _wait_settled();
    assert(!_status().enabled && !_status().desired_enabled);
    assert(atomic_load(&fake_start_calls) == starts);
    _barrier_delete(&fake_start_entered, &fake_start_release);

    assert(device_link_service_set_enabled(true, 1000U) == ESP_OK);
    _barrier_create(&fake_stop_entered, &fake_stop_release);
    assert(device_link_service_set_enabled(false, 0U) == ESP_OK);
    assert(xSemaphoreTake(fake_stop_entered, 1000U) == pdTRUE);
    assert(device_link_service_set_enabled(true, 5U) == ESP_ERR_TIMEOUT);
    assert(_status().transitioning);
    xSemaphoreGive(fake_stop_release);
    assert(device_link_service_set_enabled(true, 1000U) == ESP_OK);
    _barrier_delete(&fake_stop_entered, &fake_stop_release);
    _deinit();
}

static void test_failed_start_keeps_cleanup_ownership(void)
{
    _init();
    assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
    atomic_store(&fake_start_result, ESP_FAIL);
    atomic_store(&fake_stop_result, ESP_FAIL);
    _barrier_create(&fake_start_entered, &fake_start_release);
    assert(device_link_service_set_enabled(true, 0U) == ESP_OK);
    assert(xSemaphoreTake(fake_start_entered, 1000U) == pdTRUE);
    assert(device_link_service_set_enabled(false, 5U) == ESP_ERR_TIMEOUT);
    xSemaphoreGive(fake_start_release);
    _wait_settled();
    _barrier_delete(&fake_start_entered, &fake_start_release);
    assert(_status().state == DEVICE_LINK_SERVICE_STATE_ERROR);
    assert(_status().policy_error != ESP_OK);
    assert(atomic_load(&fake_work_registered));
    assert(ble_runtime_get_state() == BLE_RUNTIME_STATE_FAULTED);
    atomic_store(&fake_start_result, ESP_OK);
    atomic_store(&fake_stop_result, ESP_OK);
    assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
    assert(ble_runtime_get_state() == BLE_RUNTIME_STATE_STOPPED);
    assert(device_link_service_set_enabled(true, 1000U) == ESP_OK);
    _deinit();
}

static void test_failed_policy_rollback_retries_same_target(void)
{
    _init();
    assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
    atomic_store(&fake_start_result, ESP_FAIL);
    _barrier_create(&s_policy_entered, &s_policy_release);
    atomic_store(&s_pause_disabled_write, true);
    assert(device_link_service_set_enabled(true, 0U) == ESP_OK);
    assert(xSemaphoreTake(s_policy_entered, 2000U) == pdTRUE);
    host_nv_storage_fail_next_set(ESP_FAIL);
    xSemaphoreGive(s_policy_release);
    _wait_settled();
    _barrier_delete(&s_policy_entered, &s_policy_release);
    assert(_status().policy_error == ESP_FAIL);
    unsigned writes = host_nv_storage_set_count();
    atomic_store(&fake_start_result, ESP_OK);
    assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
    assert(host_nv_storage_set_count() == writes + 1U);
    uint8_t policy[8];
    size_t size = 0U;
    assert(host_nv_storage_copy(policy, sizeof(policy), &size));
    assert(size == sizeof(policy) && policy[5] == 0U);
    _deinit();
}

static void test_init_rollback_failure_retains_dependencies(void)
{
    host_freertos_reset_controls();
    host_nv_storage_reset();
    fake_lifecycle_reset();
    atomic_store(&fake_bridge_fail, true);
    atomic_store(&fake_deinit_result, ESP_FAIL);
    assert(device_link_service_init(&s_config) == ESP_FAIL);
    assert(host_freertos_live_queues() == 1U);
    assert(host_freertos_live_semaphores() == 2U);
    assert(atomic_load(&fake_work_registered));
    atomic_store(&fake_deinit_result, ESP_OK);
    _deinit();
}

static void test_same_target_retry_during_policy_rollback(void)
{
    _init();
    assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
    atomic_store(&fake_start_result, ESP_FAIL);
    atomic_store(&fake_stop_result, ESP_FAIL);
    _barrier_create(&s_policy_entered, &s_policy_release);
    atomic_store(&s_pause_disabled_write, true);
    assert(device_link_service_set_enabled(true, 0U) == ESP_OK);
    assert(xSemaphoreTake(s_policy_entered, 2000U) == pdTRUE);
    assert(ble_runtime_get_state() == BLE_RUNTIME_STATE_FAULTED);
    atomic_store(&fake_start_result, ESP_OK);
    atomic_store(&fake_stop_result, ESP_OK);
    assert(device_link_service_set_enabled(false, 5U) == ESP_ERR_TIMEOUT);
    xSemaphoreGive(s_policy_release);
    _wait_settled();
    assert(ble_runtime_get_state() == BLE_RUNTIME_STATE_STOPPED);
    assert(_status().state == DEVICE_LINK_SERVICE_STATE_DISABLED);
    assert(_status().policy_error == ESP_OK);
    _barrier_delete(&s_policy_entered, &s_policy_release);
    _deinit();
}

static void test_window_callbacks_and_physical_status(void)
{
    _init();
    atomic_store(&fake_gate_event, BLE_PORT_EVENT_CONNECT);
    assert(device_link_service_open_window() == ESP_OK);
    for (unsigned i = 0U; i < 1000U && !_status().client_connected; ++i) vTaskDelay(1U);
    assert(_status().client_connected);
    assert(!_status().active);
    fake_lifecycle_event(BLE_PORT_EVENT_DISCONNECT);
    atomic_store(&fake_gate_event, BLE_PORT_EVENT_RESET);
    assert(device_link_service_open_window() == ESP_OK);
    for (unsigned i = 0U; i < 1000U &&
            _status().last_error != ESP_ERR_INVALID_STATE; ++i) vTaskDelay(1U);
    assert(!_status().active && _status().last_error == ESP_ERR_INVALID_STATE);
    assert(device_link_service_open_window() == ESP_OK);
    for (unsigned i = 0U; i < 1000U && !_status().active; ++i) vTaskDelay(1U);
    assert(_status().active);
    atomic_store(&fake_gate_event, BLE_PORT_EVENT_RESET);
    assert(device_link_service_close_window() == ESP_OK);
    for (unsigned i = 0U; i < 1000U && _status().active; ++i) vTaskDelay(1U);
    assert(!_status().active);
    atomic_store(&fake_adv_state, BLE_ADV_MANAGER_STATE_FAULTED);
    fake_lifecycle_event(BLE_PORT_EVENT_ADV_STARTED);
    for (unsigned i = 0U; i < 1000U && _status().state != DEVICE_LINK_SERVICE_STATE_ERROR; ++i) vTaskDelay(1U);
    assert(_status().state == DEVICE_LINK_SERVICE_STATE_ERROR);
    assert(!_status().public_discovery);
    atomic_store(&fake_flags, BLE_LINK_STATE_FLAG_BOUND);
    fake_lifecycle_event(BLE_PORT_EVENT_RESET);
    for (unsigned i = 0U; i < 1000U && !_status().bound; ++i) vTaskDelay(1U);
    assert(_status().bound && !_status().client_connected);
    _deinit();
}

static void test_shutdown_retires_failed_window_close(void)
{
    _init();
    assert(device_link_service_open_window() == ESP_OK);
    for (unsigned i = 0U; i < 1000U && !_status().active; ++i) vTaskDelay(1U);
    assert(_status().active);
    atomic_store(&fake_gate_result, ESP_FAIL);
    assert(device_link_service_close_window() == ESP_OK);
    for (unsigned i = 0U; i < 1000U && _status().last_error != ESP_FAIL; ++i) vTaskDelay(1U);
    assert(_status().last_error == ESP_FAIL);
    const unsigned stops = atomic_load(&fake_stop_calls);
    assert(device_link_service_set_enabled(false, 1000U) == ESP_FAIL);
    assert(atomic_load(&fake_stop_calls) == stops);
    assert(_status().state == DEVICE_LINK_SERVICE_STATE_ERROR);
    atomic_store(&fake_gate_result, ESP_OK);
    assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
    vTaskDelay(1U); /* No gate retry may run against the released host. */
    assert(!_status().active);
    atomic_store(&fake_gate_result, ESP_OK);
    assert(device_link_service_set_enabled(true, 1000U) == ESP_OK);
    assert(device_link_service_open_window() == ESP_OK);
    for (unsigned i = 0U; i < 1000U && !_status().active; ++i) vTaskDelay(1U);
    assert(_status().active);
    _deinit();
}

static void _wait_count(atomic_uint *counter, unsigned expected)
{
    for (unsigned i = 0U; i < 2000U && atomic_load(counter) < expected; ++i)
    {
        vTaskDelay(1U);
    }
    assert(atomic_load(counter) >= expected);
}

static void test_shutdown_retains_cleanup_and_latest_target(void)
{
    _init();
    fake_lifecycle_event(BLE_PORT_EVENT_CONNECT);
    _barrier_create(&fake_drain_entered, &fake_drain_release);
    atomic_store(&fake_drain_event, BLE_PORT_EVENT_DISCONNECT);
    assert(device_link_service_set_enabled(false, 0U) == ESP_OK);
    assert(xSemaphoreTake(fake_drain_entered, 1000U) == pdTRUE);
    const unsigned starts = atomic_load(&fake_start_calls);
    const unsigned stops = atomic_load(&fake_stop_calls);
    const unsigned checks = atomic_load(&fake_cleanup_checks);
    /* This target equals the old enabled state, but admission is closing. */
    assert(device_link_service_set_enabled(true, 5U) == ESP_ERR_TIMEOUT);
    xSemaphoreGive(fake_drain_release);
    _wait_count(&fake_cleanup_checks, checks + 1U);
    _barrier_delete(&fake_drain_entered, &fake_drain_release);
    _wait_count(&fake_cleanup_checks, checks + 2U);
    const unsigned writes = host_nv_storage_set_count();

    _wait_count(&fake_cleanup_checks, checks + 5U);
    assert(_status().transitioning && _status().desired_enabled);
    assert(_status().policy_error == ESP_OK);
    assert(_status().state == DEVICE_LINK_SERVICE_STATE_STARTING);
    assert(host_nv_storage_set_count() == writes);
    assert(atomic_load(&fake_start_calls) == starts);
    assert(atomic_load(&fake_stop_calls) == stops);
    atomic_store(&fake_cleanup_pending, false);
    assert(device_link_service_set_enabled(true, 2000U) == ESP_OK);
    assert(atomic_load(&fake_stop_calls) == stops + 1U);
    assert(atomic_load(&fake_start_calls) == starts + 1U);
    _deinit();
}

static void test_cleanup_timeout_preserves_resources_for_deinit(void)
{
    _init();
    atomic_store(&fake_cleanup_pending, true);
    const unsigned stops = atomic_load(&fake_stop_calls);
    assert(device_link_service_set_enabled(false, 5U) == ESP_ERR_TIMEOUT);
    _wait_count(&fake_cleanup_checks, 1U);
    host_freertos_advance_ticks(2500U);
    /* An identical in-progress request must not restart the operation budget. */
    assert(device_link_service_set_enabled(false, 0U) == ESP_OK);
    host_freertos_advance_ticks(2501U);
    _wait_settled();
    assert(_status().state == DEVICE_LINK_SERVICE_STATE_ERROR);
    assert(_status().policy_error == ESP_ERR_TIMEOUT);
    assert(atomic_load(&fake_stop_calls) == stops);
    assert(atomic_load(&fake_work_registered));
    assert(device_link_service_deinit(5U) == ESP_ERR_TIMEOUT);
    assert(atomic_load(&fake_stop_calls) == stops);
    atomic_store(&fake_cleanup_pending, false);
    _deinit();
}

static void test_barrier_timeout_keeps_stop_phase(void)
{
    _init();
    atomic_store(&fake_drain_result, ESP_ERR_TIMEOUT);
    const unsigned stops = atomic_load(&fake_stop_calls);
    const unsigned starts = atomic_load(&fake_start_calls);
    assert(device_link_service_set_enabled(false, 1000U) == ESP_ERR_TIMEOUT);
    _wait_settled();
    assert(_status().state == DEVICE_LINK_SERVICE_STATE_ERROR);
    assert(atomic_load(&fake_stop_calls) == stops);
    atomic_store(&fake_drain_result, ESP_OK);
    assert(device_link_service_set_enabled(true, 2000U) == ESP_OK);
    assert(atomic_load(&fake_stop_calls) == stops + 1U);
    assert(atomic_load(&fake_start_calls) == starts + 1U);
    _deinit();
}

static void test_shutdown_waits_for_revoke(void)
{
    _init();
    assert(device_link_service_revoke_binding() == ESP_OK);
    for (unsigned i = 0U; i < 1000U && !atomic_load(&fake_revoke_pending); ++i)
        vTaskDelay(1U);
    assert(atomic_load(&fake_revoke_pending));
    const unsigned stops = atomic_load(&fake_stop_calls);
    assert(device_link_service_set_enabled(false, 5U) == ESP_ERR_TIMEOUT);
    _wait_count(&fake_drain_calls, 1U);
    assert(atomic_load(&fake_stop_calls) == stops);
    atomic_store(&fake_revoke_pending, false);
    assert(device_link_service_set_enabled(false, 2000U) == ESP_OK);
    _deinit();
}

static void test_second_barrier_retains_late_disconnect_cleanup(void)
{
    _init();
    fake_lifecycle_event(BLE_PORT_EVENT_CONNECT);
    atomic_store(&fake_drain_event_at, 2U);
    atomic_store(&fake_drain_event, BLE_PORT_EVENT_DISCONNECT);
    const unsigned stops = atomic_load(&fake_stop_calls);

    assert(device_link_service_set_enabled(false, 5U) == ESP_ERR_TIMEOUT);
    _wait_count(&fake_drain_calls, 2U);
    assert(atomic_load(&fake_cleanup_pending));
    assert(atomic_load(&fake_stop_calls) == stops);
    atomic_store(&fake_cleanup_pending, false);
    assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
    assert(atomic_load(&fake_stop_calls) == stops + 1U);
    _deinit();
}

static void test_unsynchronized_start_uses_partial_rollback(void)
{
    host_freertos_reset_controls();
    host_nv_storage_reset();
    fake_lifecycle_reset();
    atomic_store(&fake_sync_result, ESP_ERR_TIMEOUT);
    atomic_store(&fake_drain_result, ESP_ERR_INVALID_STATE);
    assert(device_link_service_init(&s_config) == ESP_ERR_TIMEOUT);
    assert(atomic_load(&fake_drain_calls) == 0U);
    assert(!atomic_load(&fake_work_registered));
    assert(host_freertos_live_queues() == 0U);
    assert(host_freertos_live_semaphores() == 0U);

    _init();
    assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
    const unsigned drains = atomic_load(&fake_drain_calls);

    atomic_store(&fake_sync_result, ESP_ERR_TIMEOUT);
    atomic_store(&fake_drain_result, ESP_ERR_INVALID_STATE);
    assert(device_link_service_set_enabled(true, 1000U) == ESP_ERR_TIMEOUT);
    _wait_settled();
    assert(atomic_load(&fake_drain_calls) == drains);
    assert(ble_runtime_get_state() == BLE_RUNTIME_STATE_STOPPED);
    atomic_store(&fake_sync_result, ESP_OK);
    atomic_store(&fake_drain_result, ESP_OK);
    assert(device_link_service_set_enabled(true, 1000U) == ESP_OK);
    _deinit();
}

static void test_delayed_adv_completion_updates_idle_service(void)
{
    _init();
    assert(device_link_service_set_enabled(false, 1000U) == ESP_OK);
    atomic_store(&fake_delay_adv, true);
    assert(device_link_service_set_enabled(true, 1000U) == ESP_OK);
    assert(_status().state == DEVICE_LINK_SERVICE_STATE_STARTING);
    fake_lifecycle_adv_complete(BLE_ADV_MANAGER_STATE_FAULTED);
    for (unsigned i = 0U; i < 1000U &&
            _status().state != DEVICE_LINK_SERVICE_STATE_ERROR; ++i) vTaskDelay(1U);
    assert(_status().state == DEVICE_LINK_SERVICE_STATE_ERROR);
    fake_lifecycle_adv_complete(BLE_ADV_MANAGER_STATE_SLOW);
    for (unsigned i = 0U; i < 1000U && !_status().public_discovery; ++i) vTaskDelay(1U);
    assert(_status().state == DEVICE_LINK_SERVICE_STATE_ADVERTISING);
    assert(_status().public_discovery);
    _deinit();
}

int main(void)
{
    test_second_barrier_retains_late_disconnect_cleanup();
    test_unsynchronized_start_uses_partial_rollback();
    test_shutdown_retains_cleanup_and_latest_target();
    test_cleanup_timeout_preserves_resources_for_deinit();
    test_barrier_timeout_keeps_stop_phase();
    test_shutdown_waits_for_revoke();
    test_delayed_adv_completion_updates_idle_service();
    test_immediate_reversals_and_idempotence();
    test_reverse_while_starting_and_stopping();
    test_failed_start_keeps_cleanup_ownership();
    test_failed_policy_rollback_retries_same_target();
    test_init_rollback_failure_retains_dependencies();
    test_same_target_retry_during_policy_rollback();
    test_window_callbacks_and_physical_status();
    test_shutdown_retires_failed_window_close();
    puts("service lifecycle and host callback interleavings pass");
    return 0;
}
