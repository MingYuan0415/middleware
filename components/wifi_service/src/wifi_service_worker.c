#include "wifi_service_internal.h"

#include <string.h>

static void _wifi_service_control_complete(uint64_t generation,
        esp_err_t result)
{
    atomic_store_explicit(&g_wifi_service.control_result, result, memory_order_relaxed);
    atomic_store_explicit(&g_wifi_service.control_completed_generation, generation,
                          memory_order_release);
    xSemaphoreGive(g_wifi_service.control_done);
}

static esp_err_t _wifi_service_worker_init_runtime(
    wifi_worker_context_t *context)
{
    atomic_store_explicit(&g_wifi_service.event_overflow, false, memory_order_release);
    wifi_service_worker_clear_pending_publications(context);
    context->radio_ready = false;
    context->suspended = false;
    context->scan_active = false;
    context->scan_id_known = false;
    context->scan_deadline = 0U;
    context->scan_deadline_pending = false;
    context->retry_pending = false;
    context->operation_kind = WIFI_OPERATION_NONE;
    context->operation_session = 0;
    context->operation_id = 0;
    esp_err_t result = wifi_service_port_init();
    if (result != ESP_OK)
    {
        esp_err_t cleanup_result = wifi_service_port_deinit();
        bool clean = wifi_service_port_is_clean();
        memset(&context->status, 0, sizeof(context->status));
        context->status.state = WIFI_SERVICE_STATE_OFFLINE;
        context->status.last_error = result;
        wifi_service_worker_publish_status(context);
        wifi_service_worker_set_runtime(clean ? WIFI_RUNTIME_OFFLINE :
                                        WIFI_RUNTIME_CLEANUP_PENDING, false);
        if (clean)
        {
            wifi_service_worker_clear_pending_publications(context);
        }
        result = clean ? result :
                 (cleanup_result != ESP_OK ? cleanup_result : ESP_FAIL);
        return result;
    }

    if (wifi_service_port_get_state() != WIFI_SERVICE_PORT_STATE_STARTED)
    {
        wifi_service_worker_enter_cleanup_pending(context, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    context->radio_ready = true;
    context->port_epoch = wifi_service_port_get_epoch();
    memset(&context->status, 0, sizeof(context->status));
    context->status.available = true;
    context->status.state = WIFI_SERVICE_STATE_IDLE;
    context->status.last_error = ESP_OK;
    memset(&context->scan, 0, sizeof(context->scan));
    context->scan.state = WIFI_SERVICE_SCAN_IDLE;
    context->scan.last_error = ESP_OK;
    wifi_service_worker_set_runtime(WIFI_RUNTIME_READY, true);
    wifi_service_worker_publish_status(context);
    wifi_service_worker_publish_scan(context);
    wifi_service_worker_publish_availability(true);
    return result;
}

static esp_err_t _wifi_service_worker_suspend(wifi_worker_context_t *context)
{
    wifi_service_internal_clear_scan_targets();
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    wifi_service_internal_clear_slots_locked();
    xSemaphoreGive(g_wifi_service.state_mutex);
    const bool had_scan = context->scan_active;
    context->scan_deadline_pending = false;
    const esp_err_t scan_result = had_scan ? wifi_service_port_scan_abort() : ESP_OK;
    const wifi_service_radio_transition_t transition =
        wifi_service_worker_transition_radio(context, WIFI_RADIO_TARGET_STOPPED);
    const esp_err_t result = scan_result != ESP_OK ? scan_result : transition.result;
    if (had_scan)
    {
        context->scan_active = false;
        context->scan.state = WIFI_SERVICE_SCAN_CANCELED;
        context->scan.last_error = result;
        context->scan.operation_canceled = result == ESP_OK;
        if (context->operation_kind == WIFI_OPERATION_SCAN)
        {
            wifi_service_worker_release_operation(context);
        }
        wifi_service_worker_publish_scan(context);
    }
    if (transition.state != WIFI_SERVICE_PORT_STATE_STOPPED ||
            !transition.credentials_cleared || wifi_service_port_scan_is_owned())
    {
        const esp_err_t error = result != ESP_OK ? result : ESP_ERR_INVALID_STATE;
        wifi_service_worker_enter_cleanup_pending(context, error);
        return error;
    }
    context->status.desired_connected = false;
    context->retry_pending = false;
    context->phase_deadline_pending = false;
    wifi_service_worker_wipe_secret(context);
    wifi_service_worker_release_operation(context);
    context->radio_ready = false;
    context->suspended = true;
    context->scan_id_known = false;
    atomic_store_explicit(&g_wifi_service.event_overflow, false, memory_order_release);
    context->status.state = WIFI_SERVICE_STATE_SUSPENDED;
    context->status.ipv4_address = 0;
    context->status.ssid[0] = '\0';
    context->status.last_error = result;
    wifi_service_worker_set_runtime(WIFI_RUNTIME_SUSPENDED, false);
    wifi_service_worker_publish_status(context);
    return result;
}

static esp_err_t _wifi_service_worker_resume(wifi_worker_context_t *context)
{
    esp_err_t result = wifi_service_port_start();
    context->port_epoch = wifi_service_port_get_epoch();
    const wifi_service_port_state_t state = wifi_service_port_get_state();
    if (result == ESP_OK && state != WIFI_SERVICE_PORT_STATE_STARTED)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result != ESP_OK)
    {
        const wifi_service_radio_transition_t rollback =
            wifi_service_worker_transition_radio(context, WIFI_RADIO_TARGET_STOPPED);
        if (rollback.result == ESP_OK &&
                rollback.state == WIFI_SERVICE_PORT_STATE_STOPPED &&
                rollback.credentials_cleared)
        {
            context->radio_ready = false;
            context->suspended = true;
            context->status.state = WIFI_SERVICE_STATE_SUSPENDED;
            context->status.ipv4_address = 0;
            context->status.last_error = result;
            wifi_service_worker_set_runtime(WIFI_RUNTIME_SUSPENDED, false);
            wifi_service_worker_publish_status(context);
        }
        else
        {
            wifi_service_worker_enter_cleanup_pending(context,
                    rollback.result != ESP_OK ? rollback.result : ESP_ERR_INVALID_STATE);
        }
        return result;
    }
    context->radio_ready = true;
    context->suspended = false;
    context->status.state = WIFI_SERVICE_STATE_IDLE;
    context->status.last_error = ESP_OK;
    context->status.failure = WIFI_SERVICE_FAILURE_NONE;
    wifi_service_worker_set_runtime(WIFI_RUNTIME_READY, true);
    wifi_service_worker_publish_status(context);
    return ESP_OK;
}

static esp_err_t _wifi_service_worker_deinit(
    wifi_worker_context_t *context)
{
    atomic_store_explicit(&g_wifi_service.event_overflow, false, memory_order_release);
    wifi_service_internal_clear_scan_targets();
    context->scan_deadline_pending = false;
    if (context->scan_active)
    {
        (void)wifi_service_port_scan_abort();
        context->scan_active = false;
    }
    context->retry_pending = false;
    context->status.desired_connected = false;
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    wifi_service_internal_clear_slots_locked();
    g_wifi_service.current_session = 0;
    g_wifi_service.current_operation = 0;
    g_wifi_service.cancel_session = 0;
    g_wifi_service.cancel_operation = 0;
    g_wifi_service.claimed_session = 0;
    g_wifi_service.claimed_operation = 0;
    xSemaphoreGive(g_wifi_service.state_mutex);
    wifi_service_worker_wipe_secret(context);
    esp_err_t result = wifi_service_port_deinit();
    atomic_store_explicit(&g_wifi_service.event_overflow, false, memory_order_release);
    bool clean = wifi_service_port_is_clean();
    context->radio_ready = false;
    context->suspended = false;
    context->operation_kind = WIFI_OPERATION_NONE;
    context->operation_session = 0;
    context->operation_id = 0;
    context->scan_id_known = false;
    bool was_available = context->status.available;
    memset(&context->status, 0, sizeof(context->status));
    context->status.state = WIFI_SERVICE_STATE_OFFLINE;
    context->status.last_error = clean ? ESP_ERR_INVALID_STATE :
                                 (result != ESP_OK ? result : ESP_FAIL);
    memset(&context->scan, 0, sizeof(context->scan));
    context->scan.state = WIFI_SERVICE_SCAN_IDLE;
    context->scan.last_error = ESP_ERR_INVALID_STATE;
    wifi_service_worker_set_runtime(clean ? WIFI_RUNTIME_OFFLINE :
                                    WIFI_RUNTIME_CLEANUP_PENDING, false);
    wifi_service_worker_publish_status(context);
    wifi_service_worker_publish_scan(context);
    if (was_available)
    {
        wifi_service_worker_publish_availability(false);
    }
    if (clean)
    {
        wifi_service_worker_clear_pending_publications(context);
    }
    return clean ? ESP_OK :
           (result != ESP_OK ? result : ESP_FAIL);
}

static void _wifi_service_process_control(wifi_worker_context_t *context,
        wifi_control_type_t type, uint64_t generation)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    wifi_runtime_state_t runtime_state = g_wifi_service.runtime_state;
    xSemaphoreGive(g_wifi_service.state_mutex);

    switch (type)
    {
    case WIFI_CONTROL_INIT:
        if (runtime_state == WIFI_RUNTIME_INITIALIZING)
        {
            result = _wifi_service_worker_init_runtime(context);
        }
        break;
    case WIFI_CONTROL_SUSPEND:
        if (runtime_state == WIFI_RUNTIME_SUSPEND_PENDING)
        {
            if (!context->status.available || !context->radio_ready ||
                    context->suspended || wifi_service_port_get_state() !=
                    WIFI_SERVICE_PORT_STATE_STARTED)
            {
                wifi_service_worker_enter_cleanup_pending(context,
                        ESP_ERR_INVALID_STATE);
            }
            else
            {
                result = _wifi_service_worker_suspend(context);
            }
        }
        break;
    case WIFI_CONTROL_RESUME:
        if (runtime_state == WIFI_RUNTIME_RESUME_PENDING)
        {
            if (!context->status.available || context->radio_ready ||
                    !context->suspended || wifi_service_port_get_state() !=
                    WIFI_SERVICE_PORT_STATE_STOPPED)
            {
                wifi_service_worker_enter_cleanup_pending(context,
                        ESP_ERR_INVALID_STATE);
                result = ESP_ERR_INVALID_STATE;
            }
            else
            {
                result = _wifi_service_worker_resume(context);
            }
        }
        else if (runtime_state == WIFI_RUNTIME_DEINITIALIZING ||
                 runtime_state == WIFI_RUNTIME_CLEANUP_PENDING)
        {
            result = _wifi_service_worker_deinit(context);
        }
        break;
    case WIFI_CONTROL_DEINIT:
        if (runtime_state == WIFI_RUNTIME_DEINITIALIZING ||
                runtime_state == WIFI_RUNTIME_CLEANUP_PENDING)
        {
            result = _wifi_service_worker_deinit(context);
        }
        break;
    case WIFI_CONTROL_NONE:
        break;
    }
    _wifi_service_control_complete(generation, result);
}

static TickType_t _wifi_service_remaining_ticks(TickType_t now,
        TickType_t deadline)
{
    const int32_t remaining = (int32_t)(deadline - now);
    return remaining <= 0 ? 0U : (TickType_t)remaining;
}

static TickType_t _wifi_service_next_wait_ticks(
    const wifi_worker_context_t *context)
{
    TickType_t wait = portMAX_DELAY;
    const TickType_t now = xTaskGetTickCount();

    if (context->retry_pending)
    {
        const TickType_t retry = _wifi_service_remaining_ticks(now,
            context->retry_deadline);
        if (retry < wait)
        {
            wait = retry;
        }
    }
    if (context->publish_retry_scheduled)
    {
        const TickType_t publish = _wifi_service_remaining_ticks(now,
            context->publish_retry_deadline);
        if (publish < wait)
        {
            wait = publish;
        }
    }
    if (context->phase_deadline_pending)
    {
        const TickType_t phase = _wifi_service_remaining_ticks(now,
            context->phase_deadline);
        if (phase < wait)
        {
            wait = phase;
        }
    }
    if (context->scan_deadline_pending)
    {
        const TickType_t scan = _wifi_service_remaining_ticks(now,
            context->scan_deadline);
        if (scan < wait)
        {
            wait = scan;
        }
    }
    return wait;
}

static bool _wifi_service_poll_control(wifi_control_type_t *type,
                                       uint64_t *generation)
{
    bool pending = false;
    int request = atomic_exchange_explicit(&g_wifi_service.control_request,
                                           WIFI_CONTROL_NONE,
                                           memory_order_acq_rel);
    if (request != WIFI_CONTROL_NONE)
    {
        *type = (wifi_control_type_t)request;
        *generation = atomic_load_explicit(&g_wifi_service.control_generation,
                                           memory_order_acquire);
        pending = true;
    }
    return pending;
}

void wifi_service_worker_run(void *argument)
{
    (void)argument;
    wifi_worker_context_t context;
    memset(&context, 0, sizeof(context));
    context.status.state = WIFI_SERVICE_STATE_OFFLINE;
    context.status.last_error = ESP_ERR_INVALID_STATE;
    context.scan.state = WIFI_SERVICE_SCAN_IDLE;
    context.scan.last_error = ESP_ERR_INVALID_STATE;

    while (true)
    {
        wifi_control_type_t control;
        uint64_t control_generation;
        if (_wifi_service_poll_control(&control, &control_generation))
        {
            _wifi_service_process_control(&context, control,
                                          control_generation);
            continue;
        }

        wifi_service_worker_cancel_stale_operation(&context);
        wifi_service_worker_reconcile_overflow(&context);
        wifi_service_worker_retry_publications(&context);
        wifi_service_worker_check_phase_deadline(&context);
        wifi_service_worker_check_scan_deadline(&context);

        wifi_queue_item_t item;
        if (xQueueReceive(g_wifi_service.queue, &item,
                          _wifi_service_next_wait_ticks(&context)) == pdTRUE)
        {
            wifi_service_worker_process_item(&context, &item);
        }

        if (context.operation_kind != WIFI_OPERATION_CONNECT)
        {
            context.retry_pending = false;
            continue;
        }
        if (!context.suspended && context.retry_pending &&
                !context.scan_active &&
                wifi_service_worker_tick_reached(xTaskGetTickCount(),
                        context.retry_deadline))
        {
            const wifi_service_radio_transition_t transition =
                wifi_service_worker_transition_radio(&context, WIFI_RADIO_TARGET_STARTED);
            if (transition.result != ESP_OK)
            {
                wifi_service_worker_fail_transition(&context, &transition);
            }
            else
            {
                const esp_err_t result = wifi_service_worker_connect_driver(&context);
                if (result != ESP_OK)
                {
                    wifi_service_worker_schedule_retry(
                        &context, 0, result, WIFI_SERVICE_FAILURE_DRIVER);
                }
            }
        }
    }
}
