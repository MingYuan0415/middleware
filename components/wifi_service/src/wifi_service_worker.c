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
        goto exit;
    }

    if (wifi_service_port_get_state() != WIFI_SERVICE_PORT_STATE_STARTED)
    {
        wifi_service_worker_enter_cleanup_pending(context, ESP_ERR_INVALID_STATE);
        result = ESP_ERR_INVALID_STATE;
        goto exit;
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

exit:
    return result;
}

static void _wifi_service_finish_suspended_scan(
    wifi_worker_context_t *context, esp_err_t result, bool restore_status)
{
    if (!context->scan_active)
    {
        return;
    }
    context->scan_active = false;
    context->scan.state = WIFI_SERVICE_SCAN_CANCELED;
    context->scan.last_error = result;
    bool publish_restored_status = false;
    if (restore_status &&
            context->status.state == WIFI_SERVICE_STATE_SCANNING)
    {
        context->status.state = context->pre_scan_state;
        context->status.last_error = result;
        publish_restored_status = true;
    }
    wifi_service_worker_complete_operation(context);
    wifi_service_worker_publish_scan(context);
    if (publish_restored_status)
    {
        wifi_service_worker_publish_status(context);
    }
}

static void _wifi_service_recover_clear_failure(
    wifi_worker_context_t *context, esp_err_t scan_result,
    esp_err_t clear_result)
{
    if (context->scan_active && scan_result == ESP_OK)
    {
        esp_err_t boundary_result =
            wifi_service_worker_restart_radio(context);
        bool boundary_ready =
            !wifi_service_port_scan_is_owned() &&
            (boundary_result == ESP_OK ||
             wifi_service_port_get_state() == WIFI_SERVICE_PORT_STATE_STARTED);
        if (!boundary_ready)
        {
            wifi_service_worker_enter_cleanup_pending(
                context, boundary_result != ESP_OK ? boundary_result :
                ESP_ERR_INVALID_STATE);
            _wifi_service_finish_suspended_scan(context, clear_result, false);
            goto exit;
        }
        const bool reconnect_after_completion =
            boundary_result == ESP_OK &&
            context->status.desired_connected && context->has_credentials;
        _wifi_service_finish_suspended_scan(
            context, ESP_OK, !reconnect_after_completion);
        if (reconnect_after_completion)
        {
            esp_err_t reconnect_result =
                wifi_service_worker_connect_driver(context);
            if (reconnect_result != ESP_OK)
            {
                wifi_service_worker_schedule_retry(context, 0,
                                                   reconnect_result);
            }
        }
    }
    if (context->status.available)
    {
        wifi_service_worker_set_runtime(WIFI_RUNTIME_READY, true);
    }

exit:
    return;
}

static esp_err_t _wifi_service_recover_stop_failure(
    wifi_worker_context_t *context, esp_err_t scan_result,
    esp_err_t stop_result, wifi_service_port_state_t port_state)
{
    esp_err_t transition_error = stop_result != ESP_OK ? stop_result :
                                 ESP_ERR_INVALID_STATE;
    if (port_state != WIFI_SERVICE_PORT_STATE_STARTED)
    {
        _wifi_service_finish_suspended_scan(context, transition_error, false);
        wifi_service_worker_enter_cleanup_pending(context, transition_error);
        goto exit;
    }
    context->radio_ready = true;
    const bool reconnect_after_completion = scan_result == ESP_OK &&
                                            context->status.desired_connected &&
                                            context->has_credentials;
    if (scan_result == ESP_OK)
    {
        _wifi_service_finish_suspended_scan(
            context, ESP_OK, !reconnect_after_completion);
    }
    if (reconnect_after_completion)
    {
        esp_err_t rollback = wifi_service_worker_connect_driver(context);
        if (rollback != ESP_OK)
        {
            wifi_service_worker_schedule_retry(context, 0, rollback);
        }
    }
    if (context->status.available)
    {
        wifi_service_worker_set_runtime(WIFI_RUNTIME_READY, true);
    }

exit:
    return transition_error;
}

static esp_err_t _wifi_service_worker_suspend(
    wifi_worker_context_t *context)
{
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    wifi_service_internal_clear_slots_locked();
    xSemaphoreGive(g_wifi_service.state_mutex);

    esp_err_t scan_result = ESP_OK;
    if (context->scan_active)
    {
        scan_result = wifi_service_port_scan_abort();
    }
    esp_err_t result = wifi_service_port_clear_credentials();
    if (result != ESP_OK)
    {
        _wifi_service_recover_clear_failure(context, scan_result, result);
        goto exit;
    }

    result = wifi_service_port_stop();
    context->port_epoch = wifi_service_port_get_epoch();
    wifi_service_port_state_t port_state = wifi_service_port_get_state();
    if (result != ESP_OK || port_state != WIFI_SERVICE_PORT_STATE_STOPPED)
    {
        result = _wifi_service_recover_stop_failure(
                     context, scan_result, result, port_state);
        goto exit;
    }
    _wifi_service_finish_suspended_scan(context,
                                        scan_result == ESP_OK ? ESP_OK :
                                        scan_result,
                                        false);
    context->radio_ready = false;
    context->suspended = true;
    context->scan_id_known = false;
    atomic_store_explicit(&g_wifi_service.event_overflow, false, memory_order_release);
    context->retry_pending = false;
    context->status.state = WIFI_SERVICE_STATE_SUSPENDED;
    context->status.ipv4_address = 0;
    context->status.last_error = ESP_OK;
    wifi_service_worker_set_runtime(WIFI_RUNTIME_SUSPENDED, false);
    wifi_service_worker_publish_status(context);
    result = ESP_OK;

exit:
    return result;
}

static esp_err_t _wifi_service_worker_resume(
    wifi_worker_context_t *context)
{
    wifi_service_worker_cancel_stale_operation(context);
    esp_err_t result = wifi_service_port_start();
    context->port_epoch = wifi_service_port_get_epoch();
    wifi_service_port_state_t port_state = wifi_service_port_get_state();
    if (result != ESP_OK || port_state != WIFI_SERVICE_PORT_STATE_STARTED)
    {
        esp_err_t transition_error = result != ESP_OK ? result :
                                     ESP_ERR_INVALID_STATE;
        result = transition_error;
        if (port_state == WIFI_SERVICE_PORT_STATE_STOPPED)
        {
            wifi_service_worker_set_runtime(WIFI_RUNTIME_SUSPENDED, false);
        }
        else
        {
            wifi_service_worker_enter_cleanup_pending(context,
                    transition_error);
        }
        goto exit;
    }
    context->radio_ready = true;
    if (context->status.desired_connected && context->has_credentials)
    {
        result = wifi_service_worker_connect_driver(context);
        if (result != ESP_OK)
        {
            esp_err_t clear_result =
                wifi_service_port_clear_credentials();
            esp_err_t rollback = wifi_service_port_stop();
            context->port_epoch = wifi_service_port_get_epoch();
            port_state = wifi_service_port_get_state();
            if (clear_result == ESP_OK && rollback == ESP_OK &&
                    port_state == WIFI_SERVICE_PORT_STATE_STOPPED)
            {
                context->radio_ready = false;
                context->port_epoch = wifi_service_port_get_epoch();
                wifi_service_worker_set_runtime(WIFI_RUNTIME_SUSPENDED, false);
                context->status.state = WIFI_SERVICE_STATE_SUSPENDED;
                context->status.last_error = result;
                wifi_service_worker_publish_status(context);
                goto exit;
            }
            esp_err_t rollback_error = clear_result != ESP_OK ?
                                       clear_result :
                                       (rollback != ESP_OK ? rollback :
                                        ESP_ERR_INVALID_STATE);
            wifi_service_worker_enter_cleanup_pending(context, rollback_error);
            result = rollback_error;
            goto exit;
        }
    }
    else
    {
        context->status.state = WIFI_SERVICE_STATE_IDLE;
        context->status.last_error = ESP_OK;
        wifi_service_worker_publish_status(context);
    }
    context->suspended = false;
    wifi_service_worker_set_runtime(WIFI_RUNTIME_READY, true);
    result = ESP_OK;

exit:
    return result;
}

static esp_err_t _wifi_service_worker_deinit(
    wifi_worker_context_t *context)
{
    atomic_store_explicit(&g_wifi_service.event_overflow, false, memory_order_release);
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
                result = _wifi_service_worker_deinit(context);
            }
            else
            {
                result = _wifi_service_worker_resume(context);
                xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
                runtime_state = g_wifi_service.runtime_state;
                xSemaphoreGive(g_wifi_service.state_mutex);
                if (result != ESP_OK &&
                        runtime_state == WIFI_RUNTIME_CLEANUP_PENDING)
                {
                    result = _wifi_service_worker_deinit(context);
                }
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

        wifi_queue_item_t item;
        if (xQueueReceive(g_wifi_service.queue, &item,
                          pdMS_TO_TICKS(
                              CONFIG_WIFI_SERVICE_WORKER_POLL_MS)) == pdTRUE)
        {
            wifi_service_worker_process_item(&context, &item);
        }

        if (!context.suspended && context.retry_pending &&
                !context.scan_active &&
                wifi_service_worker_tick_reached(xTaskGetTickCount(),
                        context.retry_deadline))
        {
            esp_err_t result = wifi_service_worker_restart_radio(&context);
            if (result == ESP_OK)
            {
                result = wifi_service_worker_connect_driver(&context);
            }
            if (result != ESP_OK)
            {
                if (context.status.available)
                {
                    wifi_service_worker_schedule_retry(&context, 0, result);
                }
                else if (context.operation_kind == WIFI_OPERATION_CONNECT)
                {
                    wifi_service_worker_complete_operation(&context);
                }
            }
        }
    }
}
