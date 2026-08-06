#ifndef __BLE_GATT_REGISTRY_H__
#define __BLE_GATT_REGISTRY_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Registry capacity limits. */
#define BLE_GATT_REGISTRY_MAX_SERVICES 8U
#define BLE_GATT_REGISTRY_MAX_CHARACTERISTICS 32U

/** @brief Characteristic property bitmask. */
typedef enum
{
    BLE_GATT_REGISTRY_PROP_READ = 1U << 0,
    BLE_GATT_REGISTRY_PROP_WRITE = 1U << 1,
    BLE_GATT_REGISTRY_PROP_WRITE_NO_RESPONSE = 1U << 2,
    BLE_GATT_REGISTRY_PROP_NOTIFY = 1U << 3,
    BLE_GATT_REGISTRY_PROP_INDICATE = 1U << 4,
} ble_gatt_registry_prop_t;

/** @brief Admission tag from the Device Link profile. */
typedef enum
{
    BLE_GATT_REGISTRY_ADMISSION_PUBLIC_MINIMUM = 0,
    BLE_GATT_REGISTRY_ADMISSION_ENCRYPTED_SC_BOND,
    BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED,
    BLE_GATT_REGISTRY_ADMISSION_AUTHORIZED_TRANSFER,
} ble_gatt_registry_admission_t;

/** @brief Access operation, translated by the transport port. */
typedef enum
{
    BLE_GATT_REGISTRY_OP_READ_CHR = 0,
    BLE_GATT_REGISTRY_OP_WRITE_CHR,
    BLE_GATT_REGISTRY_OP_READ_DSC,
    BLE_GATT_REGISTRY_OP_WRITE_DSC,
} ble_gatt_registry_op_t;

/**
 * @brief Project-owned access context.
 *
 * The port translates a NimBLE access context into this structure so the
 * routing and handlers stay host-testable. The same buffers the caller owns
 * are exposed here; the handler either consumes write_data or fills read_out.
 *
 * For reads, the handler must return the complete attribute value; NimBLE
 * applies the read offset afterwards, so handlers must never slice by
 * `offset` themselves. `offset` is informational only.
 */
typedef struct ble_gatt_registry_access_context
{
    ble_gatt_registry_op_t op;
    const uint8_t *write_data; /**< Write payload, port-owned. */
    uint16_t write_len;
    uint8_t *read_out;         /**< Read output buffer, port-owned. */
    uint16_t read_capacity;
    uint16_t *read_len;        /**< Bytes written by the handler. */
    uint16_t offset;           /**< Read offset for long reads, informational. */
} ble_gatt_registry_access_context_t;

/**
 * @brief Access callback invoked by the transport port.
 *
 * Returns an ATT-style status code. NimBLE reports both Write Request and
 * Write Command as the same WRITE_CHR operation and the actual write PDU kind
 * is not exposed; handlers must never depend on that distinction. The
 * characteristic properties only declare which write kinds are supported.
 */
typedef int (*ble_gatt_registry_access_cb_t)(
    uint16_t conn_handle, uint16_t attr_handle,
    ble_gatt_registry_access_context_t *context, void *arg);

/** @brief One registered characteristic. */
typedef struct ble_gatt_registry_characteristic
{
    const uint8_t *uuid; /**< 128-bit UUID, wire (little-endian) order. */
    uint16_t properties; /**< ble_gatt_registry_prop_t bitmask. */
    ble_gatt_registry_admission_t read_admission;
    ble_gatt_registry_admission_t write_admission;
    ble_gatt_registry_admission_t tx_admission;
    ble_gatt_registry_access_cb_t access_cb;
    void *arg;
} ble_gatt_registry_characteristic_t;

/** @brief One registered service. */
typedef struct ble_gatt_registry_service
{
    const uint8_t *uuid; /**< 128-bit UUID, wire (little-endian) order. */
    const ble_gatt_registry_characteristic_t *characteristics;
    size_t characteristic_count;
} ble_gatt_registry_service_t;

/**
 * @brief Initialize the registry; it is empty and unsealed.
 */
void ble_gatt_registry_init(void);

/**
 * @brief Register a static service and its characteristics atomically.
 *
 * All services must be registered before the registry is sealed. The whole
 * service is validated first and committed only when every characteristic is
 * valid and fits, so a failure leaves the registry unchanged. Rejections:
 * duplicate service UUID, duplicate characteristic UUID, invalid entries,
 * capacity exhaustion, or registration after sealing.
 *
 * @param[in] service Service definition; the registry keeps the pointer.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE (sealed or
 *         duplicate), or ESP_ERR_NO_MEM.
 */
esp_err_t ble_gatt_registry_register(
    const ble_gatt_registry_service_t *service);

/**
 * @brief Seal the registry: service registration is no longer accepted.
 *
 * Called once before the host stack starts. Handle assignment stays allowed
 * after sealing, because the NimBLE register callback delivers handles only
 * after the host has started.
 *
 * @return ESP_OK or ESP_ERR_INVALID_STATE when already sealed.
 */
esp_err_t ble_gatt_registry_seal(void);

/**
 * @brief Query whether the registry is sealed.
 */
bool ble_gatt_registry_is_sealed(void);

/**
 * @brief Clear all assigned handles.
 *
 * Called by the port before every NimBLE database registration (host start
 * or restart), so reassignment never collides with handles from a previous
 * host run. Service definitions and the seal state are unchanged.
 */
void ble_gatt_registry_clear_handles(void);

/**
 * @brief Assign a value handle to a registered characteristic by UUID.
 *
 * Called from the port's NimBLE register callback, before and after seal.
 * Assignment is idempotent for the same UUID and handle; the same UUID may
 * be reassigned a new handle (host restart re-registers the database), and
 * two different UUIDs must never share a handle.
 *
 * @param[in]  uuid        16-byte UUID to match.
 * @param[in]  value_handle Handle to assign.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NOT_FOUND, or
 *         ESP_ERR_INVALID_STATE (handle collision).
 */
esp_err_t ble_gatt_registry_assign_handle(
    const uint8_t *uuid, uint16_t value_handle);

/**
 * @brief Read the assigned handle of a registered characteristic.
 *
 * @param[in]  uuid  16-byte UUID to match.
 * @param[out] out   Assigned handle.
 * @return ESP_OK, ESP_ERR_NOT_FOUND, or ESP_ERR_INVALID_STATE when the
 *         characteristic has no assigned handle yet.
 */
esp_err_t ble_gatt_registry_get_assigned_handle(
    const uint8_t *uuid, uint16_t *out);

/**
 * @brief Look up a characteristic by its assigned value handle.
 *
 * @param[in]  value_handle Handle to look up; zero is rejected.
 * @param[out] out          Pointer to the characteristic on success.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NOT_FOUND.
 */
esp_err_t ble_gatt_registry_lookup_by_handle(
    uint16_t value_handle, const ble_gatt_registry_characteristic_t **out);

/**
 * @brief Look up a characteristic by UUID.
 *
 * @param[in]  uuid 16-byte UUID to match.
 * @param[out] out  Pointer to the characteristic on success.
 * @return ESP_OK or ESP_ERR_NOT_FOUND.
 */
esp_err_t ble_gatt_registry_lookup_by_uuid(
    const uint8_t *uuid, const ble_gatt_registry_characteristic_t **out);

/**
 * @brief Iterate all registered characteristics in registration order.
 *
 * @param[in]  index Zero-based iteration index.
 * @param[out] out   Pointer to the characteristic on success.
 * @return ESP_OK or ESP_ERR_NOT_FOUND at the end.
 */
esp_err_t ble_gatt_registry_get_characteristic(
    size_t index, const ble_gatt_registry_characteristic_t **out);

/**
 * @brief Iterate all registered services in registration order.
 *
 * @param[in]  index Zero-based iteration index.
 * @param[out] out   Pointer to the service on success.
 * @return ESP_OK or ESP_ERR_NOT_FOUND at the end.
 */
esp_err_t ble_gatt_registry_get_service(
    size_t index, const ble_gatt_registry_service_t **out);

/**
 * @brief Query whether an admission level requires link-layer encryption.
 *
 * The transport port translates this into static ATT security flags
 * (READ_ENC / WRITE_ENC / NOTIFY_INDICATE_ENC) so NimBLE rejects unencrypted
 * access before the project callback runs. Application-level authorization is
 * enforced in access callbacks and the TX scheduler, never with NimBLE's
 * SMP authorization flag.
 *
 * @param[in] admission Admission level.
 * @return True when the admission is ENCRYPTED_SC_BOND or stronger.
 */
bool ble_gatt_registry_admission_requires_encryption(
    ble_gatt_registry_admission_t admission);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_GATT_REGISTRY_H__ */
