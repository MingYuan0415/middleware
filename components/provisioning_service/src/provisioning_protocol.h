#ifndef __PROVISIONING_PROTOCOL_H__
#define __PROVISIONING_PROTOCOL_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "connectivity_manager.h"
#include "esp_err.h"
#include "microtech/provisioning/v1/provisioning.pb-c.h"

#define PROVISIONING_PROTOCOL_DEVICE_ID_BYTES 6U
#define PROVISIONING_PROTOCOL_FIRMWARE_VERSION_BYTES 32U
#define PROVISIONING_PROTOCOL_MAX_PLAINTEXT_REQUEST_BYTES 500U
#define PROVISIONING_PROTOCOL_MAX_PLAINTEXT_RESPONSE_BYTES 484U

typedef Microtech__Provisioning__V1__OperationStatus
provisioning_protocol_operation_t;

/** @brief Protocol state retained across BLE transport reconnections. */
typedef struct provisioning_protocol
{
    connectivity_manager_status_snapshot_t connectivity;
    connectivity_manager_scan_snapshot_t scan;
    provisioning_protocol_operation_t active_operation;
    provisioning_protocol_operation_t terminal_operation;
    char device_id[PROVISIONING_PROTOCOL_DEVICE_ID_BYTES + 1U];
    char firmware_version[PROVISIONING_PROTOCOL_FIRMWARE_VERSION_BYTES];
    bool active_operation_valid;
    bool terminal_operation_valid;
    bool scan_valid;
    bool finish_requested;
    bool credentials_persisted;
} provisioning_protocol_t;

typedef provisioning_protocol_t provisioning_protocol_context_t;

/** @brief Side effects admitted by one protected request. */
typedef struct provisioning_protocol_result
{
    bool finish_session;
    bool operation_admitted;
} provisioning_protocol_result_t;

esp_err_t provisioning_protocol_init(
    provisioning_protocol_t *protocol, const char *device_id,
    const char *firmware_version,
    const connectivity_manager_status_snapshot_t *status);
bool provisioning_protocol_ingest_status(
    provisioning_protocol_t *protocol,
    const connectivity_manager_status_snapshot_t *status);
void provisioning_protocol_ingest_scan(
    provisioning_protocol_t *protocol,
    const connectivity_manager_scan_snapshot_t *scan);
esp_err_t provisioning_protocol_handle(
    provisioning_protocol_t *protocol, uint8_t *input, size_t input_size,
    uint8_t **output, size_t *output_size,
    provisioning_protocol_result_t *result);
uint64_t provisioning_protocol_active_operation(
    const provisioning_protocol_t *protocol);

#endif /* __PROVISIONING_PROTOCOL_H__ */
