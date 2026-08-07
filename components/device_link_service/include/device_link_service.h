#ifndef __DEVICE_LINK_SERVICE_H__
#define __DEVICE_LINK_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "event_bus.h"

#include "ble_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_LINK_SERVICE_QR_MAX_BYTES 256U
#define DEVICE_LINK_SERVICE_WAIT_FOREVER UINT32_MAX

EVENT_BUS_DECLARE_ID(DEVICE_LINK_SERVICE_MSG);

/** @brief Device Link service lifecycle policy. */
typedef struct device_link_service_config
{
    const ble_runtime_host_port_t *runtime_port; /**< Required, e.g. ble_nimble_port_get(). */
    uint32_t task_priority; /**< Worker task priority. */
    uint32_t window_ms; /**< Binding window duration in milliseconds. */
} device_link_service_config_t;

/** @brief Device Link service state visible to applications. */
typedef enum
{
    DEVICE_LINK_SERVICE_STATE_STOPPED = 0,
    DEVICE_LINK_SERVICE_STATE_STARTING,
    DEVICE_LINK_SERVICE_STATE_ADVERTISING,
    DEVICE_LINK_SERVICE_STATE_CONNECTED,
    DEVICE_LINK_SERVICE_STATE_WINDOW,
    DEVICE_LINK_SERVICE_STATE_SUSPENDED,
    DEVICE_LINK_SERVICE_STATE_ERROR,
} device_link_service_state_t;

/** @brief Event-bus message subtypes published by the service. */
typedef enum
{
    DEVICE_LINK_SERVICE_MSG_SUB_TYPE_STATUS = 1,
} device_link_service_msg_sub_type_t;

#define DEVICE_LINK_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT \
    DEVICE_LINK_SERVICE_MSG_SUB_TYPE_STATUS

/** @brief Device Link service status snapshot. */
typedef struct device_link_service_status
{
    uint64_t generation; /**< Monotonic status generation for this boot. */
    device_link_service_state_t state; /**< Current service state. */
    int32_t last_error; /**< Local diagnostic error, never sent over BLE. */
    uint32_t window_remaining_ms; /**< Remaining binding window time. */
    bool available; /**< Service lifecycle is initialized. */
    bool active; /**< A binding window owns the bindable advertisement. */
    bool client_connected; /**< One BLE transport client is connected. */
    bool qr_ready; /**< QR bootstrap data may be copied. */
} device_link_service_status_t;

typedef device_link_service_status_t device_link_service_snapshot_t;

/**
 * @brief Initialize the Device Link service and start the BLE runtime.
 *
 * The service owns the ble_runtime lifecycle for its whole lifetime: the
 * NimBLE host starts, the persistent connectable advertisement lease is
 * acquired, and the binding window machinery becomes available. A failed
 * start releases everything and returns the error; init may be retried.
 *
 * @param[in] config Service policy, copied before the worker starts.
 * @return ESP_OK on success; otherwise an argument, lifecycle, or port error.
 */
esp_err_t device_link_service_init(const device_link_service_config_t *config);

/**
 * @brief Stop the service and release all resources.
 * @param[in] timeout_ms Maximum wait or DEVICE_LINK_SERVICE_WAIT_FOREVER.
 * @return ESP_OK when stopped; otherwise a lifecycle or timeout error.
 */
esp_err_t device_link_service_deinit(uint32_t timeout_ms);

/**
 * @brief Generate a fresh QR secret set and open the binding window.
 *
 * The worker acquires a fast bindable advertisement lease carrying a fresh
 * 24-bit discriminator, opens the link session pairing window, and exposes
 * the QR payload through device_link_service_copy_qr().
 *
 * @return ESP_OK when admitted, otherwise a lifecycle or queue error.
 */
esp_err_t device_link_service_open_window(void);

/**
 * @brief Close the binding window and clear every window secret.
 * @return ESP_OK when admitted or already idle; otherwise a lifecycle error.
 */
esp_err_t device_link_service_close_window(void);

/**
 * @brief Close the binding window and suspend the service before standby.
 *
 * The suspend command joins the worker FIFO, so an open that raced into the
 * queue first is closed by the suspend itself; the service then has no
 * window and the suspended flag is set. The call waits (bounded by
 * timeout_ms, or forever with DEVICE_LINK_SERVICE_WAIT_FOREVER) until the
 * worker applied the state, so standby preparation can rely on it.
 *
 * @param[in] timeout_ms Maximum wait for the state to be applied.
 * @return ESP_OK when applied, ESP_ERR_TIMEOUT, or a queue error.
 */
esp_err_t device_link_service_suspend(uint32_t timeout_ms);

/**
 * @brief Restore the idle state after suspend.
 *
 * Resume clears the suspended flag only; it never opens a pairing window.
 * A window is opened exclusively by an explicit
 * device_link_service_open_window() call from user action, so a wake or
 * standby rollback cannot create binding material silently.
 *
 * @param[in] timeout_ms Reserved for lifecycle symmetry.
 * @return ESP_OK when admitted; otherwise a lifecycle error.
 */
esp_err_t device_link_service_resume(uint32_t timeout_ms);

/**
 * @brief Copy the Device Link status snapshot.
 * @param[out] status Receives the immutable status snapshot.
 * @return ESP_OK on success; otherwise an argument or lifecycle error.
 */
esp_err_t device_link_service_get_status(
    device_link_service_status_t *status);

/**
 * @brief Copy the active QR bootstrap JSON for display.
 * @param[out] output Receives the NUL-terminated QR JSON.
 * @param[in] capacity Output capacity in bytes.
 * @param[out] out_length Receives the JSON byte count excluding NUL.
 * @return ESP_OK on success; otherwise an argument, state, or size error.
 * @warning The caller must overwrite the output when the page is paused.
 */
esp_err_t device_link_service_copy_qr(
    char *output, size_t capacity, size_t *out_length);

/**
 * @brief Report whether a binding window currently owns BLE.
 * @return true while a binding window is open; false otherwise.
 */
bool device_link_service_is_active(void);

/**
 * @brief Report whether the service blocks standby.
 *
 * Interim policy: any open binding window or any connected ACL blocks
 * light sleep, because the transport cannot yet distinguish an authorized
 * session from a public link_state reader and has no disconnect path.
 * Once P3.3/P3.4 provide session-aware state and a disconnect API, an idle
 * unauthenticated ACL should quiesce instead of blocking standby.
 *
 * @return true while a window is open or a client is connected.
 */
bool device_link_service_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_SERVICE_H__ */
