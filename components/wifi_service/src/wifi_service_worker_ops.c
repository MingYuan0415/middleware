#define DBG_TAG "wifi_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "wifi_service_internal.h"

#include <string.h>

bool wifi_service_worker_tick_reached(TickType_t now, TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static TickType_t _wifi_service_publish_retry_delay(void)
{
    TickType_t delay = pdMS_TO_TICKS(CONFIG_WIFI_SERVICE_WORKER_POLL_MS);
    if (delay == 0)
    {
        delay = 1;
    }
    return delay;
}

void wifi_service_worker_clear_pending_publications(
    wifi_worker_context_t *context)
{
    context->status_publish_pending = false;
    context->scan_publish_pending = false;
    context->publish_retry_scheduled = false;
}

static void _wifi_service_schedule_publication_retry(
    wifi_worker_context_t *context)
{
    context->publish_retry_deadline = xTaskGetTickCount() +
                                      _wifi_service_publish_retry_delay();
    context->publish_retry_scheduled = true;
}

static bool _wifi_service_try_publish_status(wifi_worker_context_t *context)
{
    bool published = !context->status_publish_pending;
    if (context->status_publish_pending)
    {
        const esp_err_t result = event_bus_publish(
                                     WIFI_SERVICE_MSG,
                                     WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                                     &context->status, sizeof(context->status),
                                     EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
        if (result == ESP_OK)
        {
            context->status_publish_pending = false;
            published = true;
        }
    }
    return published;
}

static bool _wifi_service_try_publish_scan(wifi_worker_context_t *context)
{
    bool published = !context->scan_publish_pending;
    if (context->scan_publish_pending)
    {
        const esp_err_t result = event_bus_publish(
                                     WIFI_SERVICE_MSG,
                                     WIFI_SERVICE_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                                     &context->scan, sizeof(context->scan),
                                     EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
        if (result == ESP_OK)
        {
            context->scan_publish_pending = false;
            published = true;
        }
    }
    return published;
}

void wifi_service_worker_retry_publications(wifi_worker_context_t *context)
{
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    const wifi_runtime_state_t runtime_state = g_wifi_service.runtime_state;
    xSemaphoreGive(g_wifi_service.state_mutex);
    if (runtime_state == WIFI_RUNTIME_OFFLINE)
    {
        wifi_service_worker_clear_pending_publications(context);
        return;
    }
    if (runtime_state != WIFI_RUNTIME_READY &&
            runtime_state != WIFI_RUNTIME_SUSPENDED &&
            runtime_state != WIFI_RUNTIME_CLEANUP_PENDING)
    {
        return;
    }
    if (!context->status_publish_pending && !context->scan_publish_pending)
    {
        context->publish_retry_scheduled = false;
        return;
    }
    if (!context->publish_retry_scheduled)
    {
        _wifi_service_schedule_publication_retry(context);
        return;
    }
    if (!wifi_service_worker_tick_reached(xTaskGetTickCount(),
                                          context->publish_retry_deadline))
    {
        return;
    }

    (void)_wifi_service_try_publish_status(context);
    (void)_wifi_service_try_publish_scan(context);
    if (context->status_publish_pending || context->scan_publish_pending)
    {
        _wifi_service_schedule_publication_retry(context);
    }
    else
    {
        context->publish_retry_scheduled = false;
    }
}

void wifi_service_worker_publish_status(wifi_worker_context_t *context)
{
    context->status.generation = wifi_service_internal_next_generation();
    context->status_publish_pending = true;
    wifi_service_status_snapshot_t snapshot = context->status;
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    g_wifi_service.status_cache = snapshot;
    xSemaphoreGive(g_wifi_service.state_mutex);
    if (!_wifi_service_try_publish_status(context))
    {
        _wifi_service_schedule_publication_retry(context);
    }
    else if (!context->scan_publish_pending)
    {
        context->publish_retry_scheduled = false;
    }
}

void wifi_service_worker_publish_scan(wifi_worker_context_t *context)
{
    context->scan.generation = wifi_service_internal_next_generation();
    context->scan_publish_pending = true;
    wifi_service_scan_snapshot_t snapshot = context->scan;
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    g_wifi_service.scan_cache = snapshot;
    xSemaphoreGive(g_wifi_service.state_mutex);
    if (!_wifi_service_try_publish_scan(context))
    {
        _wifi_service_schedule_publication_retry(context);
    }
    else if (!context->status_publish_pending)
    {
        context->publish_retry_scheduled = false;
    }
}

void wifi_service_worker_publish_availability(bool available)
{
    const wifi_service_availability_event_t event =
    {
        .available = available,
    };
    const esp_err_t result = event_bus_publish(
                                 WIFI_SERVICE_MSG,
                                 WIFI_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED,
                                 &event, sizeof(event), 0);
    if (result != ESP_OK)
    {
        LOG_W("availability publish failed: %d", (int)result);
    }
}

static void _wifi_service_copy_ssid(char destination[], const uint8_t *source,
                                    size_t length)
{
    memset(destination, 0, WIFI_SERVICE_SSID_MAX_BYTES + 1U);
    if (length > WIFI_SERVICE_SSID_MAX_BYTES)
    {
        length = WIFI_SERVICE_SSID_MAX_BYTES;
    }
    if (length > 0)
    {
        memcpy(destination, source, length);
    }
}

void wifi_service_internal_clear_slots_locked(void)
{
    wifi_service_secure_zero(g_wifi_service.credential_slots,
                             sizeof(g_wifi_service.credential_slots));
}

static void _wifi_service_release_slot(uint8_t index, uint64_t generation)
{
    if (index < WIFI_SERVICE_CREDENTIAL_SLOTS)
    {
        xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
        wifi_credential_slot_t *slot = &g_wifi_service.credential_slots[index];
        if (slot->in_use && slot->generation == generation)
        {
            wifi_service_secure_zero(slot, sizeof(*slot));
        }
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
}

static bool _wifi_service_take_slot(const wifi_queue_item_t *item,
                                    wifi_service_port_credentials_t *output)
{
    bool valid = false;
    if (item->credential_slot < WIFI_SERVICE_CREDENTIAL_SLOTS)
    {
        xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
        wifi_credential_slot_t *slot =
            &g_wifi_service.credential_slots[item->credential_slot];
        valid = slot->in_use &&
                slot->generation == item->credential_generation;
        if (valid)
        {
            *output = slot->value;
            wifi_service_secure_zero(slot, sizeof(*slot));
        }
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
    return valid;
}

static void _wifi_service_release_item_secret(const wifi_queue_item_t *item)
{
    if (item->type == WIFI_ITEM_CONNECT)
    {
        _wifi_service_release_slot(item->credential_slot,
                                   item->credential_generation);
    }
}

static void _wifi_service_mark_worker_secret(bool zero)
{
#ifdef WIFI_SERVICE_TESTING
    atomic_store_explicit(&g_wifi_service.worker_credentials_zero, zero,
                          memory_order_release);
#else
    (void)zero;
#endif
}

void wifi_service_worker_wipe_secret(wifi_worker_context_t *context)
{
    wifi_service_secure_zero(&context->credentials,
                             sizeof(context->credentials));
    context->has_credentials = false;
    _wifi_service_mark_worker_secret(true);
}

void wifi_service_worker_set_runtime(wifi_runtime_state_t state,
                                     bool accept_commands)
{
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    if (accept_commands && !g_wifi_service.accept_commands)
    {
        g_wifi_service.admission_generation = wifi_service_internal_next_generation();
    }
    g_wifi_service.runtime_state = state;
    g_wifi_service.accept_commands = accept_commands;
    xSemaphoreGive(g_wifi_service.state_mutex);
}

static void _wifi_service_context_forget_operation(
    wifi_worker_context_t *context);

void wifi_service_worker_enter_cleanup_pending(
    wifi_worker_context_t *context, esp_err_t error)
{
    bool availability_changed = context->status.available;
    context->radio_ready = false;
    context->suspended = false;
    context->retry_pending = false;
    context->status.desired_connected = false;
    context->status.state = WIFI_SERVICE_STATE_OFFLINE;
    context->status.last_error = error;
    context->status.available = false;
    context->status.ipv4_address = 0;
    context->status.ssid[0] = '\0';
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    wifi_service_internal_clear_slots_locked();
    g_wifi_service.runtime_state = WIFI_RUNTIME_CLEANUP_PENDING;
    g_wifi_service.accept_commands = false;
    g_wifi_service.current_operation = 0;
    g_wifi_service.cancel_session = 0;
    g_wifi_service.cancel_operation = 0;
    g_wifi_service.claimed_session = 0;
    g_wifi_service.claimed_operation = 0;
    xSemaphoreGive(g_wifi_service.state_mutex);
    wifi_service_worker_wipe_secret(context);
    _wifi_service_context_forget_operation(context);
    wifi_service_worker_publish_status(context);
    if (availability_changed)
    {
        wifi_service_worker_publish_availability(false);
    }
}

static void _wifi_service_operation_complete(
    wifi_service_session_id_t session_id,
    wifi_service_operation_id_t operation_id)
{
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    if (g_wifi_service.current_session == session_id &&
            g_wifi_service.current_operation == operation_id)
    {
        g_wifi_service.current_operation = 0;
    }
    if (g_wifi_service.cancel_session == session_id &&
            g_wifi_service.cancel_operation == operation_id)
    {
        g_wifi_service.cancel_session = 0;
        g_wifi_service.cancel_operation = 0;
    }
    if (g_wifi_service.claimed_session == session_id &&
            g_wifi_service.claimed_operation == operation_id)
    {
        g_wifi_service.claimed_session = 0;
        g_wifi_service.claimed_operation = 0;
    }
    xSemaphoreGive(g_wifi_service.state_mutex);
}

static bool _wifi_service_command_current(const wifi_queue_item_t *item,
        bool *canceled)
{
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    bool current = g_wifi_service.runtime_state == WIFI_RUNTIME_READY &&
                   g_wifi_service.accept_commands &&
                   g_wifi_service.admission_generation == item->admission_generation &&
                   g_wifi_service.current_session == item->session_id &&
                   g_wifi_service.current_operation == item->operation_id;
    *canceled = current && g_wifi_service.cancel_session == item->session_id &&
                g_wifi_service.cancel_operation == item->operation_id;
    xSemaphoreGive(g_wifi_service.state_mutex);
    return current;
}

static bool _wifi_service_context_operation_stale(
    const wifi_worker_context_t *context, bool *canceled)
{
    *canceled = false;
    if (context->operation_kind == WIFI_OPERATION_NONE)
    {
        return false;
    }
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    bool session_stale = g_wifi_service.current_session != context->operation_session;
    *canceled = g_wifi_service.cancel_session == context->operation_session &&
                g_wifi_service.cancel_operation == context->operation_id;
    xSemaphoreGive(g_wifi_service.state_mutex);
    return session_stale || *canceled;
}

static void _wifi_service_context_set_operation(
    wifi_worker_context_t *context, wifi_operation_kind_t kind,
    wifi_service_session_id_t session_id,
    wifi_service_operation_id_t operation_id)
{
    context->operation_kind = kind;
    context->operation_session = session_id;
    context->operation_id = operation_id;
}

static void _wifi_service_context_forget_operation(
    wifi_worker_context_t *context)
{
    context->operation_kind = WIFI_OPERATION_NONE;
    context->operation_session = 0;
    context->operation_id = 0;
}

void wifi_service_worker_complete_operation(
    wifi_worker_context_t *context)
{
    if (context->operation_kind != WIFI_OPERATION_NONE)
    {
        _wifi_service_operation_complete(context->operation_session,
                                         context->operation_id);
        _wifi_service_context_forget_operation(context);
    }
}

static wifi_terminal_claim_t _wifi_service_context_claim_terminal(
    const wifi_worker_context_t *context)
{
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    const bool canceled = g_wifi_service.cancel_session == context->operation_session &&
                          g_wifi_service.cancel_operation == context->operation_id;
    const bool current = g_wifi_service.runtime_state == WIFI_RUNTIME_READY &&
                         g_wifi_service.accept_commands &&
                         g_wifi_service.current_session == context->operation_session &&
                         g_wifi_service.current_operation == context->operation_id;
    wifi_terminal_claim_t claim = WIFI_TERMINAL_STALE;
    if (canceled)
    {
        claim = WIFI_TERMINAL_CANCELED;
    }
    else if (current)
    {
        g_wifi_service.current_operation = 0;
        claim = WIFI_TERMINAL_CLAIMED;
    }
    xSemaphoreGive(g_wifi_service.state_mutex);
    return claim;
}

static wifi_terminal_claim_t _wifi_service_context_claim_destructive(
    const wifi_worker_context_t *context)
{
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    const bool canceled = g_wifi_service.cancel_session == context->operation_session &&
                          g_wifi_service.cancel_operation == context->operation_id;
    const bool current = g_wifi_service.runtime_state == WIFI_RUNTIME_READY &&
                         g_wifi_service.accept_commands &&
                         g_wifi_service.current_session == context->operation_session &&
                         g_wifi_service.current_operation == context->operation_id &&
                         g_wifi_service.claimed_operation == 0;
    wifi_terminal_claim_t claim = WIFI_TERMINAL_STALE;
    if (canceled)
    {
        claim = WIFI_TERMINAL_CANCELED;
    }
    else if (current)
    {
        g_wifi_service.claimed_session = context->operation_session;
        g_wifi_service.claimed_operation = context->operation_id;
        claim = WIFI_TERMINAL_CLAIMED;
    }
    xSemaphoreGive(g_wifi_service.state_mutex);
    return claim;
}

esp_err_t wifi_service_worker_restart_radio(wifi_worker_context_t *context)
{
    esp_err_t result = wifi_service_port_stop();
    context->port_epoch = wifi_service_port_get_epoch();
    if (result != ESP_OK)
    {
        if (wifi_service_port_get_state() !=
                WIFI_SERVICE_PORT_STATE_STARTED)
        {
            wifi_service_worker_enter_cleanup_pending(context, result);
        }
        else
        {
            context->radio_ready = true;
        }
        return result;
    }
    context->radio_ready = false;
    context->scan_id_known = false;
    result = wifi_service_port_start();
    context->port_epoch = wifi_service_port_get_epoch();
    if (result == ESP_OK && wifi_service_port_get_state() ==
            WIFI_SERVICE_PORT_STATE_STARTED)
    {
        context->radio_ready = true;
        return result;
    }
    if (result == ESP_OK)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    wifi_service_worker_enter_cleanup_pending(context, result);
    return result;
}

static void _wifi_service_set_status_idle(
    wifi_worker_context_t *context, esp_err_t result,
    wifi_service_session_id_t session_id,
    wifi_service_operation_id_t operation_id)
{
    context->retry_pending = false;
    context->status.state = WIFI_SERVICE_STATE_IDLE;
    context->status.last_error = result;
    context->status.ipv4_address = 0;
    context->status.retry_count = 0;
    context->status.desired_connected = false;
    context->status.session_id = session_id;
    context->status.operation_id = operation_id;
    context->status.ssid[0] = '\0';
}

esp_err_t wifi_service_worker_connect_driver(
    wifi_worker_context_t *context)
{
    if (!context->has_credentials || !context->radio_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = wifi_service_port_set_credentials(&context->credentials);
    if (result == ESP_OK)
    {
        result = wifi_service_port_connect();
    }
    if (result == ESP_OK)
    {
        context->retry_pending = false;
        context->status.state = WIFI_SERVICE_STATE_CONNECTING;
        context->status.last_error = ESP_OK;
        context->status.ipv4_address = 0;
        wifi_service_worker_publish_status(context);
    }
    return result;
}

void wifi_service_worker_schedule_retry(wifi_worker_context_t *context,
                                        uint16_t reason,
                                        esp_err_t error)
{
    static const uint32_t delays[WIFI_SERVICE_RETRY_LIMIT] =
    {
        WIFI_SERVICE_RETRY_DELAY_1_MS,
        WIFI_SERVICE_RETRY_DELAY_2_MS,
        WIFI_SERVICE_RETRY_DELAY_3_MS,
    };
    context->status.disconnect_reason = reason;
    context->status.ipv4_address = 0;
    context->status.last_error = error;
    if (!context->status.desired_connected ||
            !context->has_credentials ||
            context->status.retry_count >= WIFI_SERVICE_RETRY_LIMIT)
    {
        context->status.desired_connected = false;
        esp_err_t clear_result = wifi_service_port_clear_credentials();
        wifi_service_worker_wipe_secret(context);
        if (clear_result != ESP_OK)
        {
            wifi_service_worker_enter_cleanup_pending(context, clear_result);
        }
        else
        {
            _wifi_service_set_status_idle(context, error,
                                          context->status.session_id,
                                          context->status.operation_id);
        }
        if (context->operation_kind == WIFI_OPERATION_CONNECT)
        {
            wifi_service_worker_complete_operation(context);
        }
        if (context->status.available)
        {
            wifi_service_worker_publish_status(context);
        }
        return;
    }

    uint8_t index = context->status.retry_count;
    context->status.retry_count++;
    context->retry_deadline = xTaskGetTickCount() +
                              pdMS_TO_TICKS(delays[index]);
    context->retry_pending = true;
    context->status.state = WIFI_SERVICE_STATE_RETRY_WAIT;
    wifi_service_worker_publish_status(context);
}

static void _wifi_service_finish_scan(wifi_worker_context_t *context,
                                      const wifi_service_port_event_t *event)
{
    wifi_service_port_scan_record_t records[WIFI_SERVICE_MAX_SCAN_RECORDS];
    size_t count = 0;
    bool truncated = false;
    bool reconnect_after_completion = false;
    esp_err_t result = wifi_service_port_scan_finish(
                           records, WIFI_SERVICE_MAX_SCAN_RECORDS,
                           &count, &truncated);
    if (result != ESP_OK && wifi_service_port_scan_is_owned())
    {
        esp_err_t boundary_result = wifi_service_worker_restart_radio(context);
        if (wifi_service_port_scan_is_owned())
        {
            wifi_service_worker_enter_cleanup_pending(
                context, boundary_result != ESP_OK ? boundary_result :
                ESP_ERR_INVALID_STATE);
            boundary_result = boundary_result != ESP_OK ? boundary_result :
                              ESP_ERR_INVALID_STATE;
        }
        if (boundary_result != ESP_OK)
        {
            result = boundary_result;
        }
        else if (context->status.desired_connected &&
                 context->has_credentials)
        {
            reconnect_after_completion = true;
        }
    }
    memset(context->scan.records, 0, sizeof(context->scan.records));
    context->scan.record_count = 0;
    context->scan.truncated = false;
    if (result != ESP_OK || event->status != 0)
    {
        context->scan.state = WIFI_SERVICE_SCAN_FAILED;
        context->scan.last_error = result != ESP_OK ? result : ESP_FAIL;
    }
    else
    {
        context->scan.state = WIFI_SERVICE_SCAN_RESULTS;
        context->scan.last_error = ESP_OK;
        context->scan.record_count = (uint8_t)count;
        context->scan.truncated = truncated;
        for (size_t index = 0; index < count; ++index)
        {
            wifi_service_scan_record_t *destination =
                &context->scan.records[index];
            _wifi_service_copy_ssid(destination->ssid, records[index].ssid,
                                    records[index].ssid_length);
            destination->rssi = records[index].rssi;
            destination->channel = records[index].channel;
            destination->security = records[index].security;
        }
    }
    wifi_service_secure_zero(records, sizeof(records));
    context->scan_active = false;
    _wifi_service_context_forget_operation(context);
    wifi_service_worker_publish_scan(context);
    if (reconnect_after_completion)
    {
        esp_err_t reconnect_result = wifi_service_worker_connect_driver(context);
        if (reconnect_result != ESP_OK)
        {
            wifi_service_worker_schedule_retry(context, 0, reconnect_result);
        }
    }
    else if (context->status.state == WIFI_SERVICE_STATE_SCANNING)
    {
        context->status.state = context->pre_scan_state;
        context->status.last_error = result;
        wifi_service_worker_publish_status(context);
    }
}

static void _wifi_service_cancel_scan(wifi_worker_context_t *context,
                                      esp_err_t reason,
                                      bool reconnect)
{
    bool publish_restored_status = false;
    esp_err_t abort_result = wifi_service_port_scan_abort();
    context->scan_active = false;
    context->scan.state = WIFI_SERVICE_SCAN_CANCELED;
    context->scan.last_error = abort_result == ESP_OK ? reason : abort_result;
    esp_err_t restart_result = wifi_service_worker_restart_radio(context);
    bool radio_restarted = restart_result == ESP_OK;
    if (wifi_service_port_scan_is_owned())
    {
        wifi_service_worker_enter_cleanup_pending(
            context, restart_result != ESP_OK ? restart_result :
            ESP_ERR_INVALID_STATE);
        restart_result = restart_result != ESP_OK ? restart_result :
                         ESP_ERR_INVALID_STATE;
    }
    else if (restart_result != ESP_OK &&
             wifi_service_port_get_state() ==
             WIFI_SERVICE_PORT_STATE_STARTED)
    {
        restart_result = ESP_OK;
    }
    const bool reconnect_after_completion = reconnect && radio_restarted &&
                                            restart_result == ESP_OK &&
                                            context->status.desired_connected &&
                                            context->has_credentials;
    if (!reconnect_after_completion &&
            context->status.state == WIFI_SERVICE_STATE_SCANNING)
    {
        context->status.state = restart_result == ESP_OK ?
                                context->pre_scan_state :
                                WIFI_SERVICE_STATE_OFFLINE;
        context->status.last_error = restart_result;
        publish_restored_status = true;
    }
    wifi_service_worker_complete_operation(context);
    wifi_service_worker_publish_scan(context);
    if (publish_restored_status)
    {
        wifi_service_worker_publish_status(context);
    }
    if (reconnect_after_completion)
    {
        esp_err_t reconnect_result = wifi_service_worker_connect_driver(context);
        if (reconnect_result != ESP_OK)
        {
            wifi_service_worker_schedule_retry(context, 0, reconnect_result);
        }
    }
}

static void _wifi_service_cancel_connect(wifi_worker_context_t *context,
        esp_err_t reason)
{
    context->retry_pending = false;
    context->status.desired_connected = false;
    if (context->suspended || !context->radio_ready)
    {
        wifi_service_worker_wipe_secret(context);
        context->status.session_id = context->operation_session;
        context->status.operation_id = context->operation_id;
        context->status.last_error = reason;
        context->status.ipv4_address = 0;
        wifi_service_worker_complete_operation(context);
        wifi_service_worker_publish_status(context);
        return;
    }
    esp_err_t clear_result = wifi_service_port_clear_credentials();
    esp_err_t result = wifi_service_worker_restart_radio(context);
    wifi_service_worker_wipe_secret(context);
    if (clear_result != ESP_OK)
    {
        wifi_service_worker_enter_cleanup_pending(context, clear_result);
        result = clear_result;
    }
    if (context->status.available)
    {
        _wifi_service_set_status_idle(
            context, result == ESP_OK ? reason : result,
            context->operation_session, context->operation_id);
    }
    wifi_service_worker_complete_operation(context);
    if (context->status.available)
    {
        wifi_service_worker_publish_status(context);
    }
}

void wifi_service_worker_cancel_stale_operation(
    wifi_worker_context_t *context)
{
    bool canceled = false;
    if (_wifi_service_context_operation_stale(context, &canceled))
    {
        esp_err_t reason = canceled ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
        if (context->operation_kind == WIFI_OPERATION_SCAN)
        {
            _wifi_service_cancel_scan(context, reason, true);
        }
        else if (context->operation_kind == WIFI_OPERATION_CONNECT)
        {
            _wifi_service_cancel_connect(context, reason);
        }
    }
}

static void _wifi_service_reject_item(wifi_worker_context_t *context,
                                      const wifi_queue_item_t *item,
                                      bool canceled)
{
    _wifi_service_release_item_secret(item);
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    bool runtime_ready = g_wifi_service.runtime_state == WIFI_RUNTIME_READY &&
                         g_wifi_service.accept_commands;
    bool session_current = g_wifi_service.current_session == item->session_id;
    xSemaphoreGive(g_wifi_service.state_mutex);
    if (!runtime_ready || !session_current)
    {
        _wifi_service_operation_complete(item->session_id,
                                         item->operation_id);
    }
    else if (item->type == WIFI_ITEM_SCAN)
    {
        context->scan.session_id = item->session_id;
        context->scan.operation_id = item->operation_id;
        context->scan.state = WIFI_SERVICE_SCAN_CANCELED;
        context->scan.last_error = canceled ? ESP_ERR_INVALID_STATE :
                                   ESP_ERR_NOT_FOUND;
        _wifi_service_operation_complete(item->session_id, item->operation_id);
        wifi_service_worker_publish_scan(context);
    }
    else if (item->type == WIFI_ITEM_CONNECT ||
             item->type == WIFI_ITEM_DISCONNECT)
    {
        context->status.session_id = item->session_id;
        context->status.operation_id = item->operation_id;
        context->status.last_error = canceled ? ESP_ERR_INVALID_STATE :
                                     ESP_ERR_NOT_FOUND;
        _wifi_service_operation_complete(item->session_id, item->operation_id);
        wifi_service_worker_publish_status(context);
    }
}

static void _wifi_service_begin_scan(wifi_worker_context_t *context,
                                     const wifi_queue_item_t *item)
{
    context->pre_scan_state = context->status.state;
    context->scan.session_id = item->session_id;
    context->scan.operation_id = item->operation_id;
    context->scan.record_count = 0;
    context->scan.truncated = false;
    memset(context->scan.records, 0, sizeof(context->scan.records));
    esp_err_t result = wifi_service_port_scan_start();
    if (result != ESP_OK)
    {
        context->scan.state = WIFI_SERVICE_SCAN_FAILED;
        context->scan.last_error = result;
        _wifi_service_operation_complete(item->session_id,
                                         item->operation_id);
        wifi_service_worker_publish_scan(context);
    }
    else
    {
        context->scan_active = true;
        _wifi_service_context_set_operation(context, WIFI_OPERATION_SCAN,
                                            item->session_id,
                                            item->operation_id);
        context->scan.state = WIFI_SERVICE_SCAN_RUNNING;
        context->scan.last_error = ESP_OK;
        context->status.state = WIFI_SERVICE_STATE_SCANNING;
        context->status.last_error = ESP_OK;
        wifi_service_worker_publish_scan(context);
        wifi_service_worker_publish_status(context);
    }
}

static void _wifi_service_begin_connect(wifi_worker_context_t *context,
                                        const wifi_queue_item_t *item)
{
    wifi_service_port_credentials_t credentials;
    memset(&credentials, 0, sizeof(credentials));
    if (!_wifi_service_take_slot(item, &credentials))
    {
        _wifi_service_operation_complete(item->session_id,
                                         item->operation_id);
        goto exit;
    }
    esp_err_t result = wifi_service_worker_restart_radio(context);
    if (result != ESP_OK)
    {
        context->status.session_id = item->session_id;
        context->status.operation_id = item->operation_id;
        context->status.last_error = result;
        _wifi_service_operation_complete(item->session_id,
                                         item->operation_id);
        wifi_service_worker_publish_status(context);
        goto exit;
    }
    wifi_service_worker_wipe_secret(context);
    context->credentials = credentials;
    context->has_credentials = true;
    _wifi_service_mark_worker_secret(false);
    context->status.session_id = item->session_id;
    context->status.operation_id = item->operation_id;
    context->status.desired_connected = true;
    context->status.disconnect_reason = 0;
    context->status.retry_count = 0;
    _wifi_service_copy_ssid(context->status.ssid,
                            context->credentials.ssid,
                            context->credentials.ssid_length);
    _wifi_service_context_set_operation(context, WIFI_OPERATION_CONNECT,
                                        item->session_id,
                                        item->operation_id);
    result = wifi_service_worker_connect_driver(context);
    if (result != ESP_OK)
    {
        wifi_service_worker_schedule_retry(context, 0, result);
    }

exit:
    wifi_service_secure_zero(&credentials, sizeof(credentials));
    return;
}

static void _wifi_service_begin_disconnect(wifi_worker_context_t *context,
        const wifi_queue_item_t *item)
{
    _wifi_service_context_set_operation(context, WIFI_OPERATION_DISCONNECT,
                                        item->session_id,
                                        item->operation_id);
    context->status.session_id = item->session_id;
    context->status.operation_id = item->operation_id;
    const wifi_terminal_claim_t claim =
        _wifi_service_context_claim_destructive(context);
    if (claim != WIFI_TERMINAL_CLAIMED)
    {
        context->status.last_error = claim == WIFI_TERMINAL_CANCELED ?
                                     ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
        wifi_service_worker_complete_operation(context);
        wifi_service_worker_publish_status(context);
        return;
    }
    context->status.desired_connected = false;
    context->retry_pending = false;
    (void)wifi_service_port_disconnect();
    esp_err_t clear_result = wifi_service_port_clear_credentials();
    esp_err_t result = wifi_service_worker_restart_radio(context);
    wifi_service_worker_wipe_secret(context);
    if (clear_result != ESP_OK)
    {
        wifi_service_worker_enter_cleanup_pending(context, clear_result);
        result = clear_result;
    }
    else if (result != ESP_OK && context->status.available)
    {
        wifi_service_worker_enter_cleanup_pending(context, result);
    }
    if (context->status.available)
    {
        _wifi_service_set_status_idle(context, result, item->session_id,
                                      item->operation_id);
    }
    wifi_service_worker_complete_operation(context);
    if (context->status.available)
    {
        wifi_service_worker_publish_status(context);
    }
}

static bool _wifi_service_disconnect_event_relevant(
    const wifi_worker_context_t *context)
{
    wifi_service_state_t state = context->status.state;
    if (state == WIFI_SERVICE_STATE_SCANNING)
    {
        state = context->pre_scan_state;
    }
    return state == WIFI_SERVICE_STATE_CONNECTING ||
           state == WIFI_SERVICE_STATE_WAITING_IP ||
           state == WIFI_SERVICE_STATE_IP_READY;
}

static void _wifi_service_process_scan_done(
    wifi_worker_context_t *context, const wifi_service_port_event_t *event)
{
    if (context->scan_active &&
            context->operation_kind == WIFI_OPERATION_SCAN &&
            (!context->scan_id_known ||
             event->scan_id == context->next_scan_id))
    {
        const wifi_terminal_claim_t claim =
            _wifi_service_context_claim_terminal(context);
        if (claim == WIFI_TERMINAL_CLAIMED)
        {
            context->scan_id_known = true;
            context->next_scan_id = (uint8_t)(event->scan_id + 1U);
            _wifi_service_finish_scan(context, event);
        }
        else
        {
            _wifi_service_cancel_scan(
                context,
                claim == WIFI_TERMINAL_CANCELED ?
                ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND,
                true);
        }
    }
}

static void _wifi_service_process_connected(wifi_worker_context_t *context)
{
    if (!context->suspended && context->status.desired_connected &&
            context->status.state == WIFI_SERVICE_STATE_CONNECTING)
    {
        context->status.state = WIFI_SERVICE_STATE_WAITING_IP;
        context->status.last_error = ESP_OK;
        wifi_service_worker_publish_status(context);
    }
}

static void _wifi_service_process_got_ip(
    wifi_worker_context_t *context, const wifi_service_port_event_t *event)
{
    if (context->suspended || !context->status.desired_connected ||
            (context->status.state != WIFI_SERVICE_STATE_CONNECTING &&
             context->status.state != WIFI_SERVICE_STATE_WAITING_IP))
    {
        return;
    }
    if (context->operation_kind == WIFI_OPERATION_CONNECT)
    {
        const wifi_terminal_claim_t claim =
            _wifi_service_context_claim_terminal(context);
        if (claim != WIFI_TERMINAL_CLAIMED)
        {
            _wifi_service_cancel_connect(
                context, claim == WIFI_TERMINAL_CANCELED ?
                ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND);
            return;
        }
    }
    context->retry_pending = false;
    context->status.state = WIFI_SERVICE_STATE_IP_READY;
    context->status.last_error = ESP_OK;
    context->status.ipv4_address = event->ipv4_address;
    context->status.retry_count = 0;
    if (context->operation_kind == WIFI_OPERATION_CONNECT)
    {
        _wifi_service_context_forget_operation(context);
    }
    wifi_service_worker_publish_status(context);
}

static void _wifi_service_process_link_loss(
    wifi_worker_context_t *context, const wifi_service_port_event_t *event,
    bool restart_radio)
{
    if (!context->suspended && context->status.desired_connected &&
            _wifi_service_disconnect_event_relevant(context))
    {
        if (context->scan_active)
        {
            _wifi_service_cancel_scan(context, ESP_FAIL, false);
        }
        esp_err_t retry_error = event->status != 0 ? event->status : ESP_FAIL;
        uint16_t reason = event->disconnect_reason;
        if (restart_radio)
        {
            esp_err_t restart_result =
                wifi_service_worker_restart_radio(context);
            retry_error = restart_result == ESP_OK ? ESP_FAIL : restart_result;
            reason = 0;
        }
        if (context->status.available && context->radio_ready &&
                context->status.desired_connected)
        {
            wifi_service_worker_schedule_retry(context, reason, retry_error);
        }
    }
}

static void _wifi_service_process_port_event(
    wifi_worker_context_t *context, const wifi_service_port_event_t *event)
{
    if (event->epoch != context->port_epoch)
    {
        return;
    }
    switch (event->type)
    {
    case WIFI_SERVICE_PORT_EVENT_SCAN_DONE:
        _wifi_service_process_scan_done(context, event);
        break;
    case WIFI_SERVICE_PORT_EVENT_STA_CONNECTED:
        _wifi_service_process_connected(context);
        break;
    case WIFI_SERVICE_PORT_EVENT_GOT_IP:
        _wifi_service_process_got_ip(context, event);
        break;
    case WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED:
        _wifi_service_process_link_loss(context, event, false);
        break;
    case WIFI_SERVICE_PORT_EVENT_LOST_IP:
        _wifi_service_process_link_loss(context, event, true);
        break;
    }
}

void wifi_service_worker_process_item(wifi_worker_context_t *context,
                                      const wifi_queue_item_t *item)
{
    if (item->type == WIFI_ITEM_PORT_EVENT)
    {
        _wifi_service_process_port_event(context, &item->event);
    }
    else
    {
        bool canceled = false;
        if (!_wifi_service_command_current(item, &canceled) || canceled)
        {
            _wifi_service_reject_item(context, item, canceled);
            return;
        }
        switch (item->type)
        {
        case WIFI_ITEM_SCAN:
            _wifi_service_begin_scan(context, item);
            break;
        case WIFI_ITEM_CONNECT:
            _wifi_service_begin_connect(context, item);
            break;
        case WIFI_ITEM_DISCONNECT:
            _wifi_service_begin_disconnect(context, item);
            break;
        case WIFI_ITEM_PORT_EVENT:
            break;
        }
    }
}

static void _wifi_service_reconcile_inactive_overflow(
    wifi_worker_context_t *context, bool publish_scan_terminal)
{
    context->status.desired_connected = false;
    context->retry_pending = false;
    context->status.last_error = ESP_ERR_NO_MEM;
    context->status.ipv4_address = 0;
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    wifi_service_internal_clear_slots_locked();
    xSemaphoreGive(g_wifi_service.state_mutex);
    wifi_service_worker_wipe_secret(context);
    if (context->operation_kind != WIFI_OPERATION_NONE)
    {
        wifi_service_worker_complete_operation(context);
    }
    if (publish_scan_terminal)
    {
        wifi_service_worker_publish_scan(context);
    }
    wifi_service_worker_publish_status(context);
}

static void _wifi_service_reconcile_active_overflow(
    wifi_worker_context_t *context, bool publish_scan_terminal)
{
    esp_err_t clear_result = wifi_service_port_clear_credentials();
    esp_err_t result = wifi_service_worker_restart_radio(context);
    if (wifi_service_port_scan_is_owned())
    {
        result = result != ESP_OK ? result : ESP_ERR_INVALID_STATE;
        wifi_service_worker_enter_cleanup_pending(context, result);
    }
    context->status.desired_connected = false;
    context->retry_pending = false;
    wifi_service_worker_wipe_secret(context);
    if (clear_result != ESP_OK)
    {
        wifi_service_worker_enter_cleanup_pending(context, clear_result);
        result = clear_result;
    }
    if (context->status.available)
    {
        _wifi_service_set_status_idle(
            context, result == ESP_OK ? ESP_ERR_NO_MEM : result,
            context->operation_session, context->operation_id);
    }
    if (context->operation_kind != WIFI_OPERATION_NONE)
    {
        wifi_service_worker_complete_operation(context);
    }
    if (publish_scan_terminal)
    {
        wifi_service_worker_publish_scan(context);
    }
    if (context->status.available)
    {
        wifi_service_worker_publish_status(context);
    }
}

void wifi_service_worker_reconcile_overflow(wifi_worker_context_t *context)
{
    if (!atomic_exchange_explicit(&g_wifi_service.event_overflow, false,
                                  memory_order_acq_rel))
    {
        return;
    }
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    bool runtime_ready = g_wifi_service.runtime_state == WIFI_RUNTIME_READY &&
                         g_wifi_service.accept_commands;
    xSemaphoreGive(g_wifi_service.state_mutex);
    if (!runtime_ready)
    {
        return;
    }
    bool publish_scan_terminal = false;
    if (context->scan_active)
    {
        (void)wifi_service_port_scan_abort();
        context->scan_active = false;
        context->scan.state = WIFI_SERVICE_SCAN_FAILED;
        context->scan.last_error = ESP_ERR_NO_MEM;
        publish_scan_terminal = true;
    }
    if (context->suspended || !context->radio_ready)
    {
        _wifi_service_reconcile_inactive_overflow(
            context, publish_scan_terminal);
    }
    else
    {
        _wifi_service_reconcile_active_overflow(context,
                                                publish_scan_terminal);
    }
}
