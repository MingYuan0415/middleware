/** @file Project-owned events and host task lifecycle. */
#ifndef __BLE_NIMBLE_HOST_H__
#define __BLE_NIMBLE_HOST_H__

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_npl.h"

/** @brief One host generation; controlled by the runtime owner. */
typedef struct ble_nimble_host
{
    struct ble_npl_event pairing_gate_event;
    struct ble_npl_event terminate_event;
    struct ble_npl_event cleanup_drain_event;
    struct ble_npl_event smp_io_event;
    atomic_uintptr_t task;
    TaskHandle_t host_task;
    SemaphoreHandle_t exited;
    SemaphoreHandle_t stop_done;
    TaskHandle_t stop_task;
    int stop_result;
    bool started;
    bool stop_confirmed;
    bool events_initialized;
} ble_nimble_host_t;

/**
 * @brief Initialize project events once after SDK init, before producers run.
 * @param host Runtime-owned lifecycle state, initially zeroed.
 * @param pairing Pairing gate event callback.
 * @param terminate Connection termination event callback.
 * @param cleanup Cleanup drain event callback.
 * @param smp_io Numeric Comparison reply/cancel dispatch callback.
 */
void ble_nimble_host_init_events(ble_nimble_host_t *host,
                                 ble_npl_event_fn *pairing,
                                 ble_npl_event_fn *terminate,
                                 ble_npl_event_fn *cleanup,
                                 ble_npl_event_fn *smp_io);
/**
 * @brief Create the host task, reporting allocation failure to the owner.
 * @param host Runtime-owned lifecycle state with initialized events.
 * @param stack_bytes Host task stack size in bytes.
 * @param core Host task core affinity.
 * @return ESP_OK, ESP_ERR_NO_MEM, or ESP_ERR_INVALID_STATE.
 */
esp_err_t ble_nimble_host_start(ble_nimble_host_t *host, uint32_t stack_bytes,
                                BaseType_t core);
/**
 * @brief Stop once; a timeout retains the worker for the next wait.
 * @param host Runtime-owned lifecycle state.
 * @param timeout_ms Combined stop and host exit wait budget.
 * @param core Stop worker core affinity.
 * @return ESP_OK only after confirmed host exit; timeout or stop error otherwise.
 */
esp_err_t ble_nimble_host_stop(ble_nimble_host_t *host, uint32_t timeout_ms,
                               BaseType_t core);
/**
 * @brief Release events before SDK deinit, after producers and host exit.
 * @param host Runtime-owned lifecycle state.
 * @return ESP_OK, or ESP_ERR_INVALID_STATE while host/stop work remains live.
 */
esp_err_t ble_nimble_host_release(ble_nimble_host_t *host);
/**
 * @brief Test whether the caller runs on this host's event loop.
 * @param host Runtime-owned lifecycle state.
 * @return True only for the live host task.
 */
bool ble_nimble_host_is_current(const ble_nimble_host_t *host);

#endif /* __BLE_NIMBLE_HOST_H__ */
