#define DBG_TAG "wifi_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#if CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0 || \
    CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1
    #include "freertos/idf_additions.h"
#endif

#include "wifi_service_internal.h"

#include <string.h>

EVENT_BUS_DEFINE_ID(WIFI_SERVICE_MSG);

_Static_assert(sizeof(wifi_service_status_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
               "WiFi status snapshot exceeds event-bus capacity");
_Static_assert(sizeof(wifi_service_scan_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
               "WiFi scan snapshot exceeds event-bus capacity");

wifi_service_shared_t g_wifi_service =
{
    .core_state = ATOMIC_VAR_INIT(WIFI_CORE_EMPTY),
    .generation = ATOMIC_VAR_INIT(1U),
#ifdef WIFI_SERVICE_TESTING
    .worker_credentials_zero = ATOMIC_VAR_INIT(true),
#endif
};

uint64_t wifi_service_internal_next_generation(void)
{
    uint64_t value = atomic_fetch_add_explicit(&g_wifi_service.generation, 1U,
                     memory_order_relaxed) + 1U;
    if (value == 0)
    {
        value = atomic_fetch_add_explicit(&g_wifi_service.generation, 1U,
                                          memory_order_relaxed) + 1U;
    }
    return value;
}

void wifi_service_secure_zero(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (size > 0)
    {
        *bytes++ = 0;
        --size;
    }
}

static void _wifi_service_reset_shared_state(void)
{
    g_wifi_service.runtime_state = WIFI_RUNTIME_OFFLINE;
    g_wifi_service.accept_commands = false;
    g_wifi_service.current_session = 0;
    g_wifi_service.current_operation = 0;
    g_wifi_service.admission_generation = wifi_service_internal_next_generation();
    g_wifi_service.cancel_session = 0;
    g_wifi_service.cancel_operation = 0;
    g_wifi_service.claimed_session = 0;
    g_wifi_service.claimed_operation = 0;
    memset(&g_wifi_service.status_cache, 0, sizeof(g_wifi_service.status_cache));
    g_wifi_service.status_cache.state = WIFI_SERVICE_STATE_OFFLINE;
    g_wifi_service.status_cache.last_error = ESP_ERR_INVALID_STATE;
    memset(&g_wifi_service.scan_cache, 0, sizeof(g_wifi_service.scan_cache));
    g_wifi_service.scan_cache.state = WIFI_SERVICE_SCAN_IDLE;
    g_wifi_service.scan_cache.last_error = ESP_ERR_INVALID_STATE;
    wifi_service_secure_zero(g_wifi_service.credential_slots,
                             sizeof(g_wifi_service.credential_slots));
}

static wifi_deadline_t _wifi_service_deadline(uint32_t timeout_ms)
{
    wifi_deadline_t deadline =
    {
        .forever = timeout_ms == WIFI_SERVICE_WAIT_FOREVER,
        .start = xTaskGetTickCount(),
        .total = portMAX_DELAY,
    };
    if (!deadline.forever)
    {
        deadline.total = pdMS_TO_TICKS(timeout_ms);
        if (timeout_ms != 0 && deadline.total == 0)
        {
            deadline.total = 1;
        }
    }
    return deadline;
}

static TickType_t _wifi_service_remaining(const wifi_deadline_t *deadline)
{
    TickType_t remaining = portMAX_DELAY;
    if (!deadline->forever)
    {
        TickType_t elapsed = xTaskGetTickCount() - deadline->start;
        remaining = elapsed >= deadline->total ? 0 : deadline->total - elapsed;
    }
    return remaining;
}

static esp_err_t _wifi_service_ensure_core(const wifi_service_config_t *config)
{
    int state = atomic_load_explicit(&g_wifi_service.core_state,
                                     memory_order_acquire);
    if (state == WIFI_CORE_READY)
    {
        return ESP_OK;
    }
    if (state == WIFI_CORE_BROKEN)
    {
        return ESP_FAIL;
    }
    int expected = WIFI_CORE_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(
                &g_wifi_service.core_state, &expected, WIFI_CORE_CREATING,
                memory_order_acq_rel, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }

    g_wifi_service.config = *config;
    if (!g_wifi_service.primitives_created)
    {
        g_wifi_service.state_mutex = xSemaphoreCreateMutexStatic(
                                         &g_wifi_service.state_mutex_control);
        g_wifi_service.control_mutex = xSemaphoreCreateMutexStatic(
                                           &g_wifi_service.control_mutex_control);
        g_wifi_service.control_done = xSemaphoreCreateBinaryStatic(
                                          &g_wifi_service.control_done_control);
        g_wifi_service.queue = xQueueCreateStatic(
                                   CONFIG_WIFI_SERVICE_QUEUE_DEPTH,
                                   sizeof(wifi_queue_item_t),
                                   g_wifi_service.queue_bytes.bytes,
                                   &g_wifi_service.queue_control);
        if (g_wifi_service.state_mutex == NULL ||
                g_wifi_service.control_mutex == NULL ||
                g_wifi_service.control_done == NULL ||
                g_wifi_service.queue == NULL)
        {
            atomic_store_explicit(&g_wifi_service.core_state, WIFI_CORE_BROKEN,
                                  memory_order_release);
            return ESP_ERR_NO_MEM;
        }
        _wifi_service_reset_shared_state();
        atomic_store(&g_wifi_service.control_inflight, false);
        atomic_store(&g_wifi_service.control_type, WIFI_CONTROL_NONE);
        atomic_store(&g_wifi_service.control_request, WIFI_CONTROL_NONE);
        atomic_store(&g_wifi_service.control_completed_generation, 0);
        atomic_store(&g_wifi_service.event_overflow, false);
        g_wifi_service.primitives_created = true;
    }

    g_wifi_service.worker = xTaskCreateStaticPinnedToCore(
                                wifi_service_worker_run, "wifi_service",
                                CONFIG_WIFI_SERVICE_TASK_STACK, NULL,
                                g_wifi_service.config.task_priority,
                                g_wifi_service.worker_stack,
                                &g_wifi_service.worker_control,
                                CONFIG_MAIN_PROJECT_TASK_CORE_ID);
    if (g_wifi_service.worker == NULL)
    {
        atomic_store_explicit(&g_wifi_service.core_state, WIFI_CORE_EMPTY,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0 || \
    CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1
    LOG_I("task affinity name=wifi_service core=%d",
          (int)xTaskGetCoreID(g_wifi_service.worker));
#endif
    atomic_store_explicit(&g_wifi_service.core_state, WIFI_CORE_READY,
                          memory_order_release);
    return ESP_OK;
}

static esp_err_t _wifi_service_apply_config_locked(
    const wifi_service_config_t *config)
{
    if (g_wifi_service.config.task_priority == config->task_priority)
    {
        return ESP_OK;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    if (g_wifi_service.runtime_state == WIFI_RUNTIME_OFFLINE)
    {
        vTaskPrioritySet(g_wifi_service.worker, config->task_priority);
        g_wifi_service.config = *config;
        result = ESP_OK;
    }
    xSemaphoreGive(g_wifi_service.state_mutex);
    return result;
}

static esp_err_t _wifi_service_prepare_init_locked(bool *immediate)
{
    esp_err_t result = ESP_OK;
    if (g_wifi_service.runtime_state == WIFI_RUNTIME_READY ||
            g_wifi_service.runtime_state == WIFI_RUNTIME_SUSPENDED)
    {
        *immediate = true;
    }
    else if (g_wifi_service.runtime_state == WIFI_RUNTIME_OFFLINE)
    {
        g_wifi_service.runtime_state = WIFI_RUNTIME_INITIALIZING;
        g_wifi_service.accept_commands = false;
    }
    else
    {
        result = ESP_ERR_INVALID_STATE;
    }
    return result;
}

static esp_err_t _wifi_service_prepare_suspend_locked(bool *immediate)
{
    esp_err_t result = ESP_OK;
    if (g_wifi_service.runtime_state == WIFI_RUNTIME_OFFLINE ||
            g_wifi_service.runtime_state == WIFI_RUNTIME_SUSPENDED)
    {
        *immediate = true;
    }
    else if (g_wifi_service.runtime_state == WIFI_RUNTIME_READY)
    {
        g_wifi_service.admission_generation =
            wifi_service_internal_next_generation();
        g_wifi_service.runtime_state = WIFI_RUNTIME_SUSPEND_PENDING;
        g_wifi_service.accept_commands = false;
    }
    else
    {
        result = ESP_ERR_INVALID_STATE;
    }
    return result;
}

static esp_err_t _wifi_service_prepare_resume_locked(bool *immediate)
{
    esp_err_t result = ESP_OK;
    if (g_wifi_service.runtime_state == WIFI_RUNTIME_OFFLINE ||
            g_wifi_service.runtime_state == WIFI_RUNTIME_READY)
    {
        *immediate = true;
    }
    else if (g_wifi_service.runtime_state == WIFI_RUNTIME_SUSPENDED)
    {
        g_wifi_service.admission_generation =
            wifi_service_internal_next_generation();
        g_wifi_service.runtime_state = WIFI_RUNTIME_RESUME_PENDING;
        g_wifi_service.accept_commands = false;
    }
    else if (g_wifi_service.runtime_state == WIFI_RUNTIME_CLEANUP_PENDING)
    {
        g_wifi_service.admission_generation =
            wifi_service_internal_next_generation();
        g_wifi_service.runtime_state = WIFI_RUNTIME_DEINITIALIZING;
        g_wifi_service.accept_commands = false;
    }
    else
    {
        result = ESP_ERR_INVALID_STATE;
    }
    return result;
}

static esp_err_t _wifi_service_prepare_deinit_locked(bool *immediate)
{
    esp_err_t result = ESP_OK;
    if (g_wifi_service.runtime_state == WIFI_RUNTIME_OFFLINE)
    {
        wifi_service_internal_clear_slots_locked();
        *immediate = true;
    }
    else if (g_wifi_service.runtime_state == WIFI_RUNTIME_READY ||
             g_wifi_service.runtime_state == WIFI_RUNTIME_SUSPENDED ||
             g_wifi_service.runtime_state == WIFI_RUNTIME_CLEANUP_PENDING)
    {
        g_wifi_service.admission_generation =
            wifi_service_internal_next_generation();
        g_wifi_service.runtime_state = WIFI_RUNTIME_DEINITIALIZING;
        g_wifi_service.accept_commands = false;
    }
    else
    {
        result = ESP_ERR_INVALID_STATE;
    }
    return result;
}

static esp_err_t _wifi_service_prepare_control(wifi_control_type_t type,
        bool *immediate)
{
    *immediate = false;
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    esp_err_t result = ESP_ERR_INVALID_ARG;
    switch (type)
    {
    case WIFI_CONTROL_INIT:
        result = _wifi_service_prepare_init_locked(immediate);
        break;
    case WIFI_CONTROL_SUSPEND:
        result = _wifi_service_prepare_suspend_locked(immediate);
        break;
    case WIFI_CONTROL_RESUME:
        result = _wifi_service_prepare_resume_locked(immediate);
        break;
    case WIFI_CONTROL_DEINIT:
        result = _wifi_service_prepare_deinit_locked(immediate);
        break;
    case WIFI_CONTROL_NONE:
        result = ESP_ERR_INVALID_ARG;
        break;
    }
    xSemaphoreGive(g_wifi_service.state_mutex);
    return result;
}

static esp_err_t _wifi_service_consume_inflight(
    wifi_control_type_t type, const wifi_deadline_t *deadline, bool *reused)
{
    *reused = false;
    if (!atomic_load_explicit(&g_wifi_service.control_inflight,
                              memory_order_acquire))
    {
        return ESP_OK;
    }
    wifi_control_type_t inflight =
        (wifi_control_type_t)atomic_load(&g_wifi_service.control_type);
    uint64_t generation = atomic_load(&g_wifi_service.control_generation);
    uint64_t completed = atomic_load_explicit(
                             &g_wifi_service.control_completed_generation,
                             memory_order_acquire);
    if (completed != generation && inflight != type)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_wifi_service.control_done,
                       _wifi_service_remaining(deadline)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    completed = atomic_load_explicit(
                    &g_wifi_service.control_completed_generation,
                    memory_order_acquire);
    if (completed != generation)
    {
        return ESP_FAIL;
    }
    esp_err_t completed_result = (esp_err_t)atomic_load_explicit(
                                     &g_wifi_service.control_result,
                                     memory_order_acquire);
    atomic_store(&g_wifi_service.control_inflight, false);
    atomic_store(&g_wifi_service.control_type, WIFI_CONTROL_NONE);
    if (inflight == type)
    {
        *reused = true;
        return completed_result;
    }
    return ESP_OK;
}

static esp_err_t _wifi_service_submit_control(
    wifi_control_type_t type, const wifi_deadline_t *deadline)
{
    bool immediate = false;
    esp_err_t result = _wifi_service_prepare_control(type, &immediate);
    if (result != ESP_OK || immediate)
    {
        return result;
    }

    (void)xSemaphoreTake(g_wifi_service.control_done, 0);
    uint64_t generation = wifi_service_internal_next_generation();
    atomic_store(&g_wifi_service.control_type, type);
    atomic_store(&g_wifi_service.control_generation, generation);
    atomic_store(&g_wifi_service.control_completed_generation, 0);
    atomic_store(&g_wifi_service.control_inflight, true);
    atomic_store_explicit(&g_wifi_service.control_request, type, memory_order_release);

    if (xSemaphoreTake(g_wifi_service.control_done,
                       _wifi_service_remaining(deadline)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    if (atomic_load_explicit(&g_wifi_service.control_completed_generation,
                             memory_order_acquire) != generation)
    {
        return ESP_FAIL;
    }
    result = (esp_err_t)atomic_load_explicit(&g_wifi_service.control_result,
             memory_order_acquire);
    atomic_store(&g_wifi_service.control_inflight, false);
    atomic_store(&g_wifi_service.control_type, WIFI_CONTROL_NONE);
    return result;
}

static esp_err_t _wifi_service_control(wifi_control_type_t type,
                                       uint32_t timeout_ms,
                                       const wifi_service_config_t *config)
{
    if (config == NULL && atomic_load_explicit(&g_wifi_service.core_state,
            memory_order_acquire) != WIFI_CORE_READY)
    {
        return type == WIFI_CONTROL_DEINIT ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    const wifi_service_config_t *effective_config = config != NULL ?
        config : &g_wifi_service.config;
    esp_err_t result = _wifi_service_ensure_core(effective_config);
    bool control_owned = false;
    if (result != ESP_OK)
    {
        return result;
    }
    if (xTaskGetCurrentTaskHandle() == g_wifi_service.worker)
    {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_deadline_t deadline = _wifi_service_deadline(timeout_ms);
    if (xSemaphoreTake(g_wifi_service.control_mutex,
                       _wifi_service_remaining(&deadline)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    control_owned = true;

    if (config != NULL)
    {
        result = _wifi_service_apply_config_locked(config);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }

    bool reused = false;
    result = _wifi_service_consume_inflight(type, &deadline, &reused);
    if (result != ESP_OK || reused)
    {
        goto exit;
    }
    result = _wifi_service_submit_control(type, &deadline);

exit:
    if (control_owned)
    {
        xSemaphoreGive(g_wifi_service.control_mutex);
    }
    return result;
}

esp_err_t wifi_service_init(const wifi_service_config_t *config)
{
    if (config == NULL || config->task_priority == 0U ||
            config->task_priority >= configMAX_PRIORITIES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _wifi_service_control(WIFI_CONTROL_INIT,
                       WIFI_SERVICE_WAIT_FOREVER, config);
    if (result == ESP_OK)
    {
        LOG_I("ready: scan=%u, queue=%u, credentials=RAM-only",
              (unsigned)WIFI_SERVICE_MAX_SCAN_RECORDS,
              (unsigned)CONFIG_WIFI_SERVICE_QUEUE_DEPTH);
    }
    return result;
}

esp_err_t wifi_service_deinit(uint32_t timeout_ms)
{
    return _wifi_service_control(WIFI_CONTROL_DEINIT, timeout_ms, NULL);
}

esp_err_t wifi_service_suspend(uint32_t timeout_ms)
{
    return _wifi_service_control(WIFI_CONTROL_SUSPEND, timeout_ms, NULL);
}

esp_err_t wifi_service_resume(uint32_t timeout_ms)
{
    return _wifi_service_control(WIFI_CONTROL_RESUME, timeout_ms, NULL);
}

bool wifi_service_is_available(void)
{
    bool available = false;
    if (atomic_load(&g_wifi_service.core_state) == WIFI_CORE_READY)
    {
        xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
        available = g_wifi_service.status_cache.available;
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
    return available;
}

bool wifi_service_is_cleanup_pending(void)
{
    bool pending = false;
    if (atomic_load(&g_wifi_service.core_state) == WIFI_CORE_READY)
    {
        xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
        pending = g_wifi_service.runtime_state == WIFI_RUNTIME_CLEANUP_PENDING ||
                  g_wifi_service.runtime_state == WIFI_RUNTIME_DEINITIALIZING;
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
    return pending;
}

esp_err_t wifi_service_session_open(
    wifi_service_session_id_t *out_session_id)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool state_owned = false;
    if (out_session_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_session_id = 0;
    if (atomic_load(&g_wifi_service.core_state) != WIFI_CORE_READY)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    state_owned = true;
    if (g_wifi_service.runtime_state != WIFI_RUNTIME_READY ||
            !g_wifi_service.accept_commands)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    wifi_service_session_id_t session = wifi_service_internal_next_generation();
    g_wifi_service.current_session = session;
    g_wifi_service.current_operation = 0;
    g_wifi_service.cancel_session = 0;
    g_wifi_service.cancel_operation = 0;
    *out_session_id = session;
    result = ESP_OK;

exit:
    if (state_owned)
    {
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
    return result;
}

esp_err_t wifi_service_session_close(wifi_service_session_id_t session_id)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool state_owned = false;
    if (session_id == 0 || atomic_load(&g_wifi_service.core_state) != WIFI_CORE_READY)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    state_owned = true;
    if (g_wifi_service.current_session != session_id)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    g_wifi_service.current_session = 0;
    g_wifi_service.current_operation = 0;
    g_wifi_service.cancel_session = 0;
    g_wifi_service.cancel_operation = 0;
    result = ESP_OK;

exit:
    if (state_owned)
    {
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
    return result;
}

static bool _wifi_service_scan_allowed_locked(void)
{
    return g_wifi_service.status_cache.state == WIFI_SERVICE_STATE_IDLE ||
           g_wifi_service.status_cache.state == WIFI_SERVICE_STATE_IP_READY;
}

static esp_err_t _wifi_service_admit_simple(
    wifi_item_type_t type, wifi_service_session_id_t session_id,
    wifi_service_operation_id_t *out_operation_id)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool state_owned = false;
    if (session_id == 0 || out_operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_operation_id = 0;
    if (atomic_load(&g_wifi_service.core_state) != WIFI_CORE_READY)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    state_owned = true;
    if (g_wifi_service.runtime_state != WIFI_RUNTIME_READY ||
            !g_wifi_service.accept_commands ||
            g_wifi_service.current_session != session_id)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (g_wifi_service.current_operation != 0 ||
            g_wifi_service.claimed_operation != 0 ||
            (type == WIFI_ITEM_SCAN && !_wifi_service_scan_allowed_locked()))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    wifi_service_operation_id_t operation = wifi_service_internal_next_generation();
    wifi_queue_item_t item =
    {
        .type = type,
        .session_id = session_id,
        .operation_id = operation,
        .admission_generation = g_wifi_service.admission_generation,
    };
    if (xQueueSend(g_wifi_service.queue, &item, 0) != pdTRUE)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    g_wifi_service.current_operation = operation;
    *out_operation_id = operation;
    result = ESP_OK;

exit:
    if (state_owned)
    {
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
    return result;
}

esp_err_t wifi_service_request_scan(
    wifi_service_session_id_t session_id,
    wifi_service_operation_id_t *out_operation_id)
{
    return _wifi_service_admit_simple(WIFI_ITEM_SCAN, session_id,
                                      out_operation_id);
}

esp_err_t wifi_service_request_disconnect(
    wifi_service_session_id_t session_id,
    wifi_service_operation_id_t *out_operation_id)
{
    return _wifi_service_admit_simple(WIFI_ITEM_DISCONNECT, session_id,
                                      out_operation_id);
}

static bool _wifi_service_credentials_valid(
    const wifi_service_credentials_t *credentials)
{
    if (credentials == NULL || credentials->ssid == NULL ||
            credentials->ssid_length == 0 ||
            credentials->ssid_length > WIFI_SERVICE_SSID_MAX_BYTES ||
            memchr(credentials->ssid, '\0', credentials->ssid_length) != NULL)
    {
        return false;
    }
    bool valid;
    if (credentials->security == WIFI_SERVICE_SECURITY_OPEN)
    {
        valid = credentials->password_length == 0;
    }
    else
    {
        valid = credentials->security == WIFI_SERVICE_SECURITY_PERSONAL &&
                credentials->password != NULL &&
                credentials->password_length >= 1 &&
                credentials->password_length <=
                WIFI_SERVICE_PASSWORD_MAX_BYTES &&
                memchr(credentials->password, '\0',
                       credentials->password_length) == NULL;
    }
    return valid;
}

static wifi_credential_slot_t *_wifi_service_reserve_credentials_locked(
    const wifi_service_credentials_t *credentials, uint8_t *slot_index)
{
    wifi_credential_slot_t *slot = NULL;
    for (size_t index = 0; index < WIFI_SERVICE_CREDENTIAL_SLOTS; ++index)
    {
        if (!g_wifi_service.credential_slots[index].in_use)
        {
            *slot_index = (uint8_t)index;
            slot = &g_wifi_service.credential_slots[index];
            break;
        }
    }
    if (slot != NULL)
    {
        wifi_service_secure_zero(slot, sizeof(*slot));
        slot->in_use = true;
        slot->generation = wifi_service_internal_next_generation();
        slot->value.ssid_length = (uint8_t)credentials->ssid_length;
        memcpy(slot->value.ssid, credentials->ssid, credentials->ssid_length);
        slot->value.password_length = (uint8_t)credentials->password_length;
        if (credentials->password_length > 0)
        {
            memcpy(slot->value.password, credentials->password,
                   credentials->password_length);
        }
        slot->value.security = credentials->security;
    }
    return slot;
}

static esp_err_t _wifi_service_enqueue_connect_locked(
    wifi_service_session_id_t session_id, wifi_credential_slot_t *slot,
    uint8_t slot_index, wifi_service_operation_id_t *out_operation_id)
{
    esp_err_t result = ESP_ERR_NO_MEM;
    wifi_service_operation_id_t operation =
        wifi_service_internal_next_generation();
    wifi_queue_item_t item =
    {
        .type = WIFI_ITEM_CONNECT,
        .session_id = session_id,
        .operation_id = operation,
        .admission_generation = g_wifi_service.admission_generation,
        .credential_generation = slot->generation,
        .credential_slot = slot_index,
    };
    if (xQueueSend(g_wifi_service.queue, &item, 0) == pdTRUE)
    {
        g_wifi_service.current_operation = operation;
        *out_operation_id = operation;
        result = ESP_OK;
    }
    return result;
}

esp_err_t wifi_service_request_connect(
    wifi_service_session_id_t session_id,
    const wifi_service_credentials_t *credentials,
    wifi_service_operation_id_t *out_operation_id)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool state_owned = false;
    wifi_credential_slot_t *slot = NULL;
    uint8_t slot_index = 0;
    if (session_id == 0 || out_operation_id == NULL ||
            !_wifi_service_credentials_valid(credentials))
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_operation_id = 0;
    if (atomic_load(&g_wifi_service.core_state) != WIFI_CORE_READY)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    state_owned = true;
    if (g_wifi_service.runtime_state != WIFI_RUNTIME_READY ||
            !g_wifi_service.accept_commands ||
            g_wifi_service.current_session != session_id ||
            g_wifi_service.current_operation != 0 ||
            g_wifi_service.claimed_operation != 0)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    slot = _wifi_service_reserve_credentials_locked(credentials, &slot_index);
    if (slot == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    result = _wifi_service_enqueue_connect_locked(
                 session_id, slot, slot_index, out_operation_id);

exit:
    if (result != ESP_OK && slot != NULL)
    {
        wifi_service_secure_zero(slot, sizeof(*slot));
    }
    if (state_owned)
    {
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
    return result;
}

esp_err_t wifi_service_cancel(wifi_service_session_id_t session_id,
                              wifi_service_operation_id_t operation_id)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    bool state_owned = false;
    if (session_id == 0 || operation_id == 0 ||
            atomic_load(&g_wifi_service.core_state) != WIFI_CORE_READY)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
    state_owned = true;
    if (g_wifi_service.claimed_session == session_id &&
            g_wifi_service.claimed_operation == operation_id)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    if (g_wifi_service.current_session != session_id ||
            g_wifi_service.current_operation != operation_id)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    g_wifi_service.cancel_session = session_id;
    g_wifi_service.cancel_operation = operation_id;
    result = ESP_OK;

exit:
    if (state_owned)
    {
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
    return result;
}

esp_err_t wifi_service_get_status(
    wifi_service_status_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&g_wifi_service.core_state) != WIFI_CORE_READY)
    {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = WIFI_SERVICE_STATE_OFFLINE;
        snapshot->last_error = ESP_ERR_INVALID_STATE;
    }
    else
    {
        xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
        *snapshot = g_wifi_service.status_cache;
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
    return ESP_OK;
}

esp_err_t wifi_service_get_scan_snapshot(
    wifi_service_scan_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&g_wifi_service.core_state) != WIFI_CORE_READY)
    {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = WIFI_SERVICE_SCAN_IDLE;
        snapshot->last_error = ESP_ERR_INVALID_STATE;
    }
    else
    {
        xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
        *snapshot = g_wifi_service.scan_cache;
        xSemaphoreGive(g_wifi_service.state_mutex);
    }
    return ESP_OK;
}

esp_err_t wifi_service_port_submit_event(
    const wifi_service_port_event_t *event)
{
    if (event == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load_explicit(&g_wifi_service.core_state, memory_order_acquire) !=
            WIFI_CORE_READY)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const wifi_queue_item_t item =
    {
        .type = WIFI_ITEM_PORT_EVENT,
        .event = *event,
    };
    if (xQueueSend(g_wifi_service.queue, &item, 0) != pdTRUE)
    {
        atomic_store_explicit(&g_wifi_service.event_overflow, true,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#ifdef WIFI_SERVICE_TESTING
bool wifi_service_test_credentials_are_zero(void)
{
    bool zero = true;
    if (atomic_load(&g_wifi_service.core_state) == WIFI_CORE_READY)
    {
        xSemaphoreTake(g_wifi_service.state_mutex, portMAX_DELAY);
        const uint8_t *bytes =
            (const uint8_t *)g_wifi_service.credential_slots;
        for (size_t index = 0;
                index < sizeof(g_wifi_service.credential_slots); ++index)
        {
            if (bytes[index] != 0)
            {
                zero = false;
                break;
            }
        }
        xSemaphoreGive(g_wifi_service.state_mutex);
        zero = zero && atomic_load_explicit(
                   &g_wifi_service.worker_credentials_zero,
                   memory_order_acquire);
    }
    return zero;
}
#endif
