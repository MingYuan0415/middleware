#ifndef __BLE_RUNTIME_H__
#define __BLE_RUNTIME_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Runtime lifecycle states. */
typedef enum
{
    BLE_RUNTIME_STATE_STOPPED = 0,
    BLE_RUNTIME_STATE_STARTING,
    BLE_RUNTIME_STATE_RUNNING,
    BLE_RUNTIME_STATE_STOPPING,
    BLE_RUNTIME_STATE_FAULTED,
} ble_runtime_state_t;

/**
 * @brief Transport backend owned exclusively by the runtime.
 *
 * The port is injected so the lifecycle and all higher managers stay
 * host-testable. The production port is the NimBLE IDF adapter; it is the
 * only component allowed to call nimble_port_init/run/stop/deinit and to
 * write ble_hs_cfg.
 *
 * All callbacks run synchronously in the caller's context and must not call
 * back into the runtime API. Caller constraints:
 *
 * - init must not leave the host stack initialized when it fails. It may
 *   retain other resources, but then it must latch a terminal fault whose
 *   teardown returns the latched error until a device reboot; otherwise it
 *   must release everything itself on failure;
 * - start must not return before the host stack is synchronized;
 * - stop must not return before the advertising task has been quiesced and
 *   nimble_port_stop() and the host event loop have returned; the host and
 *   stop tasks self-delete right after signaling completion and perform no
 *   further host, semaphore, or state access after the signal, so no host
 *   access happens after stop returns;
 * - deinit must leave no host resources behind (a latched terminal fault is
 *   released only by reboot and returns the latched error instead).
 */
typedef struct ble_runtime_host_port
{
    /** @brief Initialize the host stack. Called once per start. */
    esp_err_t (*init)(void);

    /** @brief Start the host task and wait until the stack is synchronized. */
    esp_err_t (*start)(void);

    /** @brief Stop the host stack; must not return before teardown completes. */
    esp_err_t (*stop)(void);

    /** @brief Release all host resources. Called once per stop. */
    esp_err_t (*deinit)(void);
} ble_runtime_host_port_t;

/**
 * @brief Runtime configuration.
 */
typedef struct ble_runtime_config
{
    const ble_runtime_host_port_t *port; /**< Transport backend, required. */
} ble_runtime_config_t;

/**
 * @brief Initialize the runtime in STOPPED state.
 *
 * STOPPED means the port holds no resources. A later start performs a full
 * init/start sequence and a stop performs the full stop/deinit teardown, so
 * restarts always run stop -> deinit -> init.
 *
 * @param[in] config Runtime configuration, kept for the runtime lifetime.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE when already
 *         initialized.
 */
esp_err_t ble_runtime_init(const ble_runtime_config_t *config);

/**
 * @brief Start the runtime: STOPPED -> STARTING -> RUNNING.
 *
 * A port start failure rolls the runtime back to STOPPED with all port
 * resources released and returns the start error, unless the rollback
 * teardown itself fails; a failed teardown leaves the runtime FAULTED and
 * returns the teardown error (cleanup errors take precedence over the start
 * error). A port init failure leaves the runtime FAULTED; the runtime keeps
 * port ownership so the outstanding teardown can be retried from FAULTED. If
 * the port init failed while holding a terminal fault (host resources
 * retained), the retried stop/deinit return the latched error and the runtime
 * stays FAULTED until reboot; if the port released everything itself, the
 * retried teardown completes and the runtime returns to STOPPED.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE, or a port error.
 */
esp_err_t ble_runtime_start(void);

/**
 * @brief Stop the runtime: RUNNING -> STOPPING -> STOPPED.
 *
 * A successful stop releases all port resources. From FAULTED, stop retries
 * the outstanding teardown and returns to STOPPED when it succeeds.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE, or a port error.
 */
esp_err_t ble_runtime_stop(void);

/**
 * @brief Deinitialize the runtime.
 *
 * Valid from STOPPED and from FAULTED; from FAULTED it retries the outstanding
 * teardown. A failed teardown keeps the runtime FAULTED and returns the port
 * error.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE, or a port error.
 */
esp_err_t ble_runtime_deinit(void);

/**
 * @brief Query the current runtime state.
 *
 * All lifecycle APIs, including this one, must be called serially from a
 * single owner task; port callbacks must not re-enter the runtime API.
 */
ble_runtime_state_t ble_runtime_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_RUNTIME_H__ */
