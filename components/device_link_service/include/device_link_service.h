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

#define DEVICE_LINK_SERVICE_WAIT_FOREVER UINT32_MAX

EVENT_BUS_DECLARE_ID(DEVICE_LINK_SERVICE_MSG);

/** @brief Nonzero, boot-scoped identity of a local binding decision. */
typedef uint64_t device_link_confirmation_token_t;

/** @brief Startup exposure policy. */
typedef enum
{
    DEVICE_LINK_SERVICE_STARTUP_NORMAL = 0,
    DEVICE_LINK_SERVICE_STARTUP_FACTORY_RESET_GATED,
} device_link_service_startup_mode_t;

/** @brief Device Link service lifecycle policy. */
typedef struct device_link_service_config
{
    const ble_runtime_host_port_t *runtime_port; /**< Required, e.g. ble_nimble_port_get(). */
    uint32_t task_priority; /**< Worker task priority. */
    uint32_t window_ms; /**< Binding window duration in milliseconds. */
    device_link_service_startup_mode_t startup_mode; /**< Advertising gate. */
} device_link_service_config_t;

/** @brief Device Link service state visible to applications. */
typedef enum
{
    DEVICE_LINK_SERVICE_STATE_STOPPED = 0,
    DEVICE_LINK_SERVICE_STATE_DISABLED,
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
    bool enabled; /**< Effective persistent Bluetooth policy. */
    bool transitioning; /**< Runtime is converging to the requested policy. */
    bool public_discovery; /**< Public discovery advertisement is active. */
    uint8_t instance_id[3]; /**< Boot-scoped public discovery identifier. */
    bool active; /**< A binding window owns the bindable advertisement. */
    bool client_connected; /**< One BLE transport client is connected. */
    bool pending_confirmation; /**< A binding awaits local confirmation. */
    device_link_confirmation_token_t confirmation_token; /**< Exact
                                                          * transaction to
                                                          * confirm. */
    uint32_t numeric_comparison; /**< Six-digit passkey, or zero. */
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
 * Lifecycle admission is exclusive but all init/deinit calls must come
 * from a single owner task (the runtime owner), as with ble_runtime.
 * Status snapshots carry a monotonic generation; consumers must ignore
 * generations that do not advance, because the initial publication may
 * race a subsequent worker publication in arrival order.
 *
 * @param[in] config Service policy, copied before the worker starts.
 * @return ESP_OK on success; otherwise an argument, lifecycle, or port error.
 */
esp_err_t device_link_service_init(const device_link_service_config_t *config);

/**
 * @brief Persist and apply the local Bluetooth enable policy.
 *
 * This API is intended for the local settings owner. It is deliberately not
 * exposed as a Device Link method: a remote peer must not be able to disable
 * the only recovery transport. The worker persists the requested value before
 * changing the NimBLE runtime and retains a bounded retry obligation when the
 * physical transition fails.
 *
 * @param enabled Desired effective policy.
 * @param timeout_ms Total wait for this transition, or WAIT_FOREVER.
 */
esp_err_t device_link_service_set_enabled(bool enabled, uint32_t timeout_ms);

/**
 * @brief Release advertising after factory-reset recovery completed.
 *
 * Only valid for FACTORY_RESET_GATED startup. Init has already acquired the
 * slow non-bindable lease while advertising is paused and the reset marker is
 * still durable. This call commits visibility after the caller clears that
 * marker. A physical advertising-start failure remains owned by the runtime's
 * bounded retry state and does not fail this reset transaction.
 */
esp_err_t device_link_service_release_startup_gate(void);

/**
 * @brief Stop the service and release all resources.
 * @param[in] timeout_ms Maximum wait or DEVICE_LINK_SERVICE_WAIT_FOREVER.
 * @return ESP_OK when stopped; otherwise a lifecycle or timeout error.
 */
esp_err_t device_link_service_deinit(uint32_t timeout_ms);

/**
 * @brief Open the Numeric Comparison pairing window.
 *
 * The worker acquires a fast bindable advertisement lease and opens the
 * link session pairing window.
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
 * @brief Accept or deny the pending binding confirmation.
 *
 * The decision is applied serially in the service worker, so it cannot
 * race a window close or a disconnect. Accepting arms the active
 * authorize transaction; denying invalidates it.
 *
 * @param[in] token Token from the status snapshot that exposed the prompt.
 * @param[in] accept True to confirm the binding, false to deny it.
 * @return ESP_OK when admitted; otherwise a lifecycle error.
 */
esp_err_t device_link_service_confirm_binding(
    device_link_confirmation_token_t token, bool accept);

/**
 * @brief Revoke the current binding (local operation, no wire command).
 *
 * Journals the revoke intent, erases the authorization record and its
 * verifier, clears the session state, and deletes the bond/CCCD on the
 * host core. A crash mid-revoke resumes at startup before advertising.
 *
 * @return ESP_OK when admitted; otherwise a lifecycle error.
 */
esp_err_t device_link_service_revoke_binding(void);

/**
 * @brief Report whether a binding awaits local confirmation.
 *
 * Reads the service snapshot; the fact is refreshed by the worker.
 *
 * @return True while an authorize transaction is active and not yet
 *         confirmed or denied.
 */
bool device_link_service_pending_confirmation(void);

/**
 * @brief Close the binding window and suspend the service before standby.
 *
 * The suspend command joins the worker FIFO, so an open that raced into the
 * queue first is closed by the suspend itself; the service then has no
 * window and the suspended flag is set. The call waits (bounded by
 * timeout_ms, or forever with DEVICE_LINK_SERVICE_WAIT_FOREVER) until the
 * worker applied the state, so standby preparation can rely on it. On
 * timeout the command may still be applied later (it only ever closes the
 * window); the caller must treat the result as unknown and re-check.
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
 * @brief Report whether a binding window currently owns BLE.
 * @return true while a binding window is open; false otherwise.
 */
bool device_link_service_is_active(void);

/**
 * @brief Report whether the service blocks standby.
 *
 * The current conservative product policy treats an open or deferred binding
 * window and every connected ACL as busy. This includes public-only
 * link_state readers, so standby never suspends a live BLE transport.
 *
 * @return true while a window is open/deferred or a client is connected.
 */
bool device_link_service_is_busy(void);

#ifdef UNIT_TEST_HOST
/** @brief Test-only barrier invoked after API admission samples its state. */
typedef void (*device_link_service_test_api_acquire_hook_t)(void *arg);

/** @brief Install a test-only API admission barrier. */
void device_link_service_test_set_api_acquire_hook(
    device_link_service_test_api_acquire_hook_t hook, void *arg);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_SERVICE_H__ */
