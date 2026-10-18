#define DBG_TAG "ble_nimble_host"
#include "mt_log.h"

#include "ble_nimble_host.h"

#include "host/ble_hs.h"
#include "nimble/nimble_port.h"

void ble_nimble_host_init_events(ble_nimble_host_t *host,
                                 ble_npl_event_fn *pairing,
                                 ble_npl_event_fn *terminate,
                                 ble_npl_event_fn *cleanup)
{
    ble_npl_event_init(&host->pairing_gate_event, pairing, NULL);
    ble_npl_event_init(&host->terminate_event, terminate, NULL);
    ble_npl_event_init(&host->cleanup_drain_event, cleanup, NULL);
    host->events_initialized = true;
}

static void _ble_nimble_host_run(void *arg)
{
    ble_nimble_host_t *host = arg;

    atomic_store_explicit(&host->task,
                          (uintptr_t)xTaskGetCurrentTaskHandle(),
                          memory_order_release);
    nimble_port_run();
    atomic_store_explicit(&host->task, 0U, memory_order_release);
    xSemaphoreGive(host->exited);
    vTaskSuspend(NULL);
}

bool ble_nimble_host_is_current(const ble_nimble_host_t *host)
{
    const uintptr_t task = atomic_load_explicit(&host->task, memory_order_acquire);

    return task != 0U && task == (uintptr_t)xTaskGetCurrentTaskHandle();
}

esp_err_t ble_nimble_host_start(ble_nimble_host_t *host, uint32_t stack_bytes,
                                BaseType_t core)
{
    if (host->started || !host->events_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (host->exited == NULL)
    {
        host->exited = xSemaphoreCreateBinary();
    }
    if (host->stop_done == NULL)
    {
        host->stop_done = xSemaphoreCreateBinary();
    }
    if (host->exited == NULL || host->stop_done == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    host->stop_confirmed = false;
    /* The SDK task wrapper ignores xTaskCreatePinnedToCore failure. Keep
     * task ownership here so a failed start remains safely recoverable. */
    if (xTaskCreatePinnedToCore(_ble_nimble_host_run, "nimble_host", stack_bytes,
                                host, configMAX_PRIORITIES - 4U,
                                &host->host_task, core) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    host->started = true;
    return ESP_OK;
}

static void _ble_nimble_host_stop_worker(void *arg)
{
    ble_nimble_host_t *host = arg;

    host->stop_result = nimble_port_stop();
    xSemaphoreGive(host->stop_done);
    /* The owner deletes this task after consuming the completion token. */
    vTaskSuspend(NULL);
}

esp_err_t ble_nimble_host_stop(ble_nimble_host_t *host, uint32_t timeout_ms,
                               BaseType_t core)
{
    if (!host->started)
    {
        return ESP_OK;
    }
    const TickType_t begin = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    if (!host->stop_confirmed)
    {
        if (host->stop_task == NULL)
        {
            if (xTaskCreatePinnedToCore(_ble_nimble_host_stop_worker, "ble_stop",
                                        2048U, host, 2U, &host->stop_task,
                                        core) != pdPASS)
            {
                return ESP_ERR_NO_MEM;
            }
        }
        if (xSemaphoreTake(host->stop_done, timeout) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelete(host->stop_task);
        host->stop_task = NULL;
        if (host->stop_result != 0 && host->stop_result != BLE_HS_EALREADY)
        {
            return ESP_FAIL;
        }
        host->stop_confirmed = true;
    }
    const TickType_t elapsed = xTaskGetTickCount() - begin;
    const TickType_t remaining = elapsed < timeout ? timeout - elapsed : 0U;

    /* EALREADY confirms protocol OFF only. It does not exit nimble_port_run. */
    if (xSemaphoreTake(host->exited, remaining) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelete(host->host_task);
    host->host_task = NULL;
    host->started = false;
    return ESP_OK;
}

esp_err_t ble_nimble_host_release(ble_nimble_host_t *host)
{
    if (host->started || host->stop_task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (host->events_initialized)
    {
        ble_npl_event_deinit(&host->pairing_gate_event);
        ble_npl_event_deinit(&host->terminate_event);
        ble_npl_event_deinit(&host->cleanup_drain_event);
        host->events_initialized = false;
    }
    if (host->exited != NULL)
    {
        vSemaphoreDelete(host->exited);
        host->exited = NULL;
    }
    if (host->stop_done != NULL)
    {
        vSemaphoreDelete(host->stop_done);
        host->stop_done = NULL;
    }
    return ESP_OK;
}
