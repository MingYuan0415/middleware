#ifndef __DEVICE_LINK_CONFIRMATION_H__
#define __DEVICE_LINK_CONFIRMATION_H__

#include <stdbool.h>

#include "ble_link_service.h"
#include "device_link_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Synchronize a Device Link status with one confirmation snapshot.
 *
 * @param[in,out] status Status fields to compare and update.
 * @param[in] confirmation Coherent BLE Link confirmation state.
 * @return true when at least one observable confirmation field changed.
 */
bool device_link_confirmation_sync(
    device_link_service_status_t *status,
    const ble_link_confirmation_snapshot_t *confirmation);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_CONFIRMATION_H__ */
