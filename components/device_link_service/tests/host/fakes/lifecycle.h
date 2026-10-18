/** @file Faults at the service's external runtime boundary. */
#ifndef __DEVICE_LINK_LIFECYCLE_FAKE_H__
#define __DEVICE_LINK_LIFECYCLE_FAKE_H__
#include <stdatomic.h>
#include "ble_adv_manager.h"
#include "ble_link_session.h"
#include "ble_port_ops.h"
#include "device_link_service.h"
#include "freertos/semphr.h"

extern atomic_bool fake_cleanup_pending;
extern atomic_bool fake_revoke_pending;
extern atomic_bool fake_delay_adv;
extern atomic_int fake_drain_result;
extern atomic_int fake_revoke_result;
extern atomic_uint fake_drain_calls;
extern atomic_uint fake_cleanup_checks;
extern atomic_int fake_drain_event;
extern atomic_uint fake_drain_event_at;
extern SemaphoreHandle_t fake_drain_entered;
extern SemaphoreHandle_t fake_drain_release;
/** @brief Commit a delayed physical advertising result and notify its owner. */
void fake_lifecycle_adv_complete(ble_adv_manager_state_t state);
extern atomic_int fake_start_result;
extern atomic_int fake_stop_result;
extern atomic_int fake_deinit_result;
extern atomic_int fake_sync_result;
extern atomic_int fake_gate_event;
extern atomic_int fake_gate_result;
extern atomic_uint fake_flags;
extern atomic_uint fake_start_calls;
extern atomic_uint fake_stop_calls;
extern atomic_uint fake_deinit_calls;
extern atomic_int fake_adv_state;
extern atomic_bool fake_work_registered;
extern atomic_bool fake_bridge_fail;
extern SemaphoreHandle_t fake_start_entered;
extern SemaphoreHandle_t fake_start_release;
extern SemaphoreHandle_t fake_stop_entered;
extern SemaphoreHandle_t fake_stop_release;
extern const ble_runtime_host_port_t fake_runtime_port;
/** @brief Reset external fault controls after service teardown. */
void fake_lifecycle_reset(void);
/** @brief Deliver one connection/reset event through the real router. */
void fake_lifecycle_event(ble_port_event_type_t type);
#endif
