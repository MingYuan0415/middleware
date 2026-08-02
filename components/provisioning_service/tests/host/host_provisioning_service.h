#ifndef __HOST_PROVISIONING_SERVICE_H__
#define __HOST_PROVISIONING_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "connectivity_manager.h"
#include "esp_err.h"

void host_provisioning_reset(void);
bool host_provisioning_wait_transport(bool started, uint32_t timeout_ms);
esp_err_t host_provisioning_request(
    uint8_t *input, size_t input_length,
    uint8_t **output, ssize_t *output_length);
void host_provisioning_emit_ble(bool connected);
void host_provisioning_publish_status(
    const connectivity_manager_status_snapshot_t *status);
void host_provisioning_publish_scan(
    const connectivity_manager_scan_snapshot_t *scan);
void host_provisioning_stage_init_refresh(
    const connectivity_manager_status_snapshot_t *status,
    const connectivity_manager_scan_snapshot_t *scan);
uint64_t host_provisioning_canceled_operation(void);
unsigned host_provisioning_cancel_count(void);
unsigned host_provisioning_transport_start_count(void);
unsigned host_provisioning_transport_stop_count(void);
void host_provisioning_fail_next_stop(esp_err_t result);
void host_provisioning_block_next_stop(void);
bool host_provisioning_wait_stop_blocked(uint32_t timeout_ms);
void host_provisioning_release_stop(void);
bool host_provisioning_application_secrets_zeroized(void);
const char *host_provisioning_device_name(void);
const char *host_provisioning_protocol_version(void);
bool host_provisioning_transport_shape_valid(void);

#endif /* __HOST_PROVISIONING_SERVICE_H__ */
