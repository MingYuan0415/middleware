#ifndef __BLE_ADV_MANAGER_H__
#define __BLE_ADV_MANAGER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_port_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_ADV_MANAGER_MAX_LEASES 4U
#define BLE_ADV_MANAGER_DISCRIMINATOR_BYTES 3U

/** @brief Advertising state machine states. */
typedef enum
{
    BLE_ADV_MANAGER_STATE_STOPPED = 0,
    BLE_ADV_MANAGER_STATE_STARTING,
    BLE_ADV_MANAGER_STATE_FAST,
    BLE_ADV_MANAGER_STATE_SLOW,
    BLE_ADV_MANAGER_STATE_STOPPING,
    BLE_ADV_MANAGER_STATE_FAULTED,
} ble_adv_manager_state_t;

/** @brief Lease advertising class. */
typedef enum
{
    BLE_ADV_MANAGER_MODE_FAST = 0, /**< Fast connectable advertising. */
    BLE_ADV_MANAGER_MODE_SLOW,     /**< Low-power connectable advertising. */
} ble_adv_manager_mode_t;

/** @brief One advertising lease held by a business owner. */
typedef struct ble_adv_lease
{
    uint8_t lease_id;              /**< 1..BLE_ADV_MANAGER_MAX_LEASES. */
    ble_adv_manager_mode_t mode;
    bool bindable;                 /**< Carry BINDABLE flag and discriminator. */
    uint8_t discriminator[BLE_ADV_MANAGER_DISCRIMINATOR_BYTES];
} ble_adv_lease_t;

/**
 * @brief Advertising manager configuration.
 *
 * Fast and slow intervals and the fast window duration are runtime policy,
 * calibrated on hardware; the contract only freezes the advertisement
 * content. The service data layout (adv version, flags, discriminator)
 * follows the Device Link discovery contract. The service UUID is in
 * little-endian wire order, matching the frozen advertising fixture.
 *
 * arm_timer may be NULL when the caller implements the fast window through
 * its own polling of ble_adv_manager_get_fast_window_remaining_ms(); the
 * production port does this in its advertising worker task. When provided,
 * arm_timer(delay > 0) is invoked to notify the caller that a fast window is
 * running (the production port wakes its worker with a queue nudge) and
 * arm_timer(0) marks the window as cancelled; the notification is best-effort
 * and the caller must also poll the remaining-time query.
 */
typedef struct ble_adv_manager_config
{
    uint16_t fast_interval_ms;
    uint16_t slow_interval_ms;
    uint32_t fast_window_ms;
    const uint8_t *short_name;   /**< Short local name bytes, e.g. "MT". */
    size_t short_name_len;
    const uint8_t *service_uuid; /**< 16-byte 128-bit Device Link UUID. */
    uint8_t adv_version;         /**< Advertising version, 1 for v1. */
    uint32_t (*now_ms)(void);    /**< Monotonic millisecond clock. */
    void (*arm_timer)(uint32_t delay_ms, void *arg); /**< Window notify. */
    void *timer_arg;
    const ble_port_ops_t *ops;   /**< Port operations, required. */
    void (*lock)(void *arg);     /**< Optional serialization lock. */
    void (*unlock)(void *arg);
    void *lock_arg;
} ble_adv_manager_config_t;

/**
 * @brief Initialize the manager in STOPPED state.
 *
 * All entry points must be called serially from a single owner task or under
 * the caller's own synchronization, matching the port event feeding model.
 *
 * @param[in] config Configuration, kept for the manager lifetime.
 */
void ble_adv_manager_init(const ble_adv_manager_config_t *config);

/**
 * @brief Release the manager and its configuration.
 *
 * After this call every entry point returns ESP_ERR_INVALID_STATE (or the
 * stopped-state fallback for queries) until the next init. The production
 * port calls this during teardown so no callback outlives the lock it uses.
 */
void ble_adv_manager_deinit(void);

/**
 * @brief Acquire an advertising lease.
 *
 * The first lease starts connectable advertising (fast unless only slow
 * leases are held); the last release stops it. A fast lease acquired while
 * advertising at slow speed escalates advertising to fast. A bindable lease
 * is released without affecting other leases; the advertisement carries the
 * discriminator only while at least one bindable lease is held.
 *
 * @param[out] out         Assigned lease.
 * @param[in] mode         Advertising class.
 * @param[in] bindable     Set BINDABLE and carry the discriminator.
 * @param[in] discriminator 24-bit discovery discriminator in little-endian
 *                          wire order, required when bindable is set.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM when full,
 *         ESP_ERR_INVALID_STATE after deinit, or a port error from the
 *         advertising operations.
 */
esp_err_t ble_adv_manager_acquire_lease(
    ble_adv_lease_t *out, ble_adv_manager_mode_t mode, bool bindable,
    const uint8_t discriminator[BLE_ADV_MANAGER_DISCRIMINATOR_BYTES]);

/**
 * @brief Release a lease.
 *
 * @param[in] lease_id Lease id returned by acquire.
 * @return ESP_OK, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE after deinit, or
 *         a port error when stopping advertising fails.
 */
esp_err_t ble_adv_manager_release_lease(uint8_t lease_id);

/**
 * @brief Feed one translated port event into the manager.
 *
 * Handles ADV_STARTED, ADV_STOPPED, ADV_COMPLETE, CONNECT, DISCONNECT, SYNC,
 * and RESET; other events are ignored.
 *
 * @param[in] event Port event.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE after deinit,
 *         or a port error from a transition the event triggered.
 */
esp_err_t ble_adv_manager_handle_event(const ble_port_event_t *event);

/**
 * @brief Notify the manager that the fast window expired.
 *
 * Called by the timer armed through the configuration; transitions fast
 * advertising to slow when the window expires.
 */
void ble_adv_manager_handle_fast_window_expired(void);

/**
 * @brief Query the time until the fast window expires.
 *
 * @return Remaining milliseconds in the fast window, UINT32_MAX when no fast
 *         window is running (the caller can use this as an unbounded wait),
 *         or 0 when the window has expired and the caller must call
 *         ble_adv_manager_handle_fast_window_expired().
 */
uint32_t ble_adv_manager_get_fast_window_remaining_ms(void);

/**
 * @brief Query the current state.
 */
ble_adv_manager_state_t ble_adv_manager_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_ADV_MANAGER_H__ */
