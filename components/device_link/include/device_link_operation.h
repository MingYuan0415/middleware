#ifndef __DEVICE_LINK_OPERATION_H__
#define __DEVICE_LINK_OPERATION_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "device_link_protocol.h"
#include "device_link_tlv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OperationStatus.result_payload is bounded by the core v2 contract at
 * 3000 bytes; the table slot must carry the full contract maximum. */
#define DEVICE_LINK_OPERATION_RESULT_BYTES 3000U
#define DEVICE_LINK_OPERATION_RETENTION_MS 60000U

typedef enum device_link_operation_state
{
    DEVICE_LINK_OPERATION_PENDING = 1,
    DEVICE_LINK_OPERATION_RUNNING = 2,
    DEVICE_LINK_OPERATION_SUCCEEDED = 3,
    DEVICE_LINK_OPERATION_FAILED = 4,
    DEVICE_LINK_OPERATION_CANCELED = 5,
} device_link_operation_state_t;

typedef esp_err_t (*device_link_operation_cancel_t)(uint64_t owner_id,
        void *arg);

typedef struct device_link_operation
{
    uint64_t id;
    uint64_t owner_id;
    uint64_t terminal_at_ms;
    uint8_t domain_id;
    uint8_t method_id;
    device_link_operation_state_t state;
    device_link_status_t status;
    const device_link_tlv_schema_t *result_schema;
    uint8_t result[DEVICE_LINK_OPERATION_RESULT_BYTES];
    size_t result_len;
    bool cancel_requested;
    device_link_operation_cancel_t cancel;
    void *cancel_arg;
} device_link_operation_t;

typedef struct device_link_operation_table
{
    uint64_t boot_id;
    uint64_t next_id;
    device_link_operation_t slots[DEVICE_LINK_MAX_OPERATIONS];
} device_link_operation_table_t;

esp_err_t device_link_operation_table_init(
    device_link_operation_table_t *table, uint64_t boot_id);

esp_err_t device_link_operation_start(
    device_link_operation_table_t *table, uint64_t now_ms,
    uint8_t domain_id, uint8_t method_id, uint64_t owner_id,
    device_link_operation_cancel_t cancel, void *cancel_arg,
    uint64_t *operation_id);

/**
 * @brief Start an operation with its frozen result-message schema.
 *
 * A NULL or zero-field schema declares an Empty result. A non-empty schema
 * requires every SUCCEEDED update to carry one complete Typed-TLV message.
 */
esp_err_t device_link_operation_start_with_schema(
    device_link_operation_table_t *table, uint64_t now_ms,
    uint8_t domain_id, uint8_t method_id, uint64_t owner_id,
    const device_link_tlv_schema_t *result_schema,
    device_link_operation_cancel_t cancel, void *cancel_arg,
    uint64_t *operation_id);

esp_err_t device_link_operation_update(
    device_link_operation_table_t *table, uint64_t now_ms,
    uint64_t operation_id, device_link_operation_state_t state,
    device_link_status_t status, const uint8_t *result, size_t result_len);

/**
 * @brief Locate an operation by id without copying the ~3 KB slot.
 *
 * The returned pointer names the table slot and stays valid until the next
 * table mutation (start/update/cancel/sweep). The caller must hold its own
 * serialization (e.g. the service mutex) while reading the slot and must
 * not retain the pointer past it.
 *
 * @param[in]  table        Operation table.
 * @param[in]  now_ms       Current monotonic time; terminal records past the
 *                          retention window are swept first.
 * @param[in]  operation_id Non-zero operation id.
 * @param[out] operation    On ESP_OK, the table slot (never NULL).
 * @return ESP_OK, ESP_ERR_NOT_FOUND, or ESP_ERR_INVALID_ARG.
 */
esp_err_t device_link_operation_get(
    device_link_operation_table_t *table, uint64_t now_ms,
    uint64_t operation_id, const device_link_operation_t **operation);

esp_err_t device_link_operation_cancel(
    device_link_operation_table_t *table, uint64_t now_ms,
    uint64_t operation_id);

/**
 * @brief Find the live operation owned by @p owner_id.
 *
 * @param[in]  table   Operation table.
 * @param[in]  owner_id Owner identity (e.g. an adapter's lower-layer
 *                      operation id).
 * @param[out] out     Live (non-terminal) operation, if any.
 * @return ESP_OK, ESP_ERR_NOT_FOUND, or ESP_ERR_INVALID_ARG.
 */
esp_err_t device_link_operation_find_by_owner(
    device_link_operation_table_t *table, uint64_t owner_id,
    device_link_operation_t *out);

void device_link_operation_sweep(
    device_link_operation_table_t *table, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_OPERATION_H__ */
