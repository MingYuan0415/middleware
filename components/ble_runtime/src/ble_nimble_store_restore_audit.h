#ifndef __BLE_NIMBLE_STORE_RESTORE_AUDIT_H__
#define __BLE_NIMBLE_STORE_RESTORE_AUDIT_H__

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One persisted NimBLE store family and its RAM object type. */
typedef struct ble_nimble_store_restore_family
{
    const char *key_prefix;
    size_t max_entries;
    int object_type;
    bool verify_exact_entries;
} ble_nimble_store_restore_family_t;

/** @brief Probe whether one durable NimBLE blob key exists. */
typedef esp_err_t (*ble_nimble_store_restore_probe_fn)(
    void *context, const char *key, bool *out_present);

/** @brief Count one object family in NimBLE's restored RAM store. */
typedef esp_err_t (*ble_nimble_store_restore_count_fn)(
    void *context, int object_type, size_t *out_count);

/** @brief Verify one durable key against an exact restored RAM object. */
typedef esp_err_t (*ble_nimble_store_restore_match_fn)(
    void *context, int object_type, const char *key, bool *out_match);

/** @brief Durable and restored counts for one inconsistent store family. */
typedef struct ble_nimble_store_restore_mismatch
{
    const char *key_prefix;
    size_t durable_count;
    size_t restored_count;
} ble_nimble_store_restore_mismatch_t;

/** @brief Report one durable-versus-restored store mismatch. */
typedef void (*ble_nimble_store_restore_mismatch_fn)(
    void *context, const ble_nimble_store_restore_mismatch_t *mismatch);

/** @brief Injectable durable and RAM access used by the restore audit. */
typedef struct ble_nimble_store_restore_audit_ops
{
    ble_nimble_store_restore_probe_fn probe;
    ble_nimble_store_restore_count_fn count;
    ble_nimble_store_restore_match_fn match;
    ble_nimble_store_restore_mismatch_fn mismatch;
    void *context;
} ble_nimble_store_restore_audit_ops_t;

/**
 * @brief Verify that NimBLE restored every durable store object into RAM.
 *
 * ESP-IDF's persisted config store logs and discards restore errors. This
 * bounded audit counts every durable key that the fixed IDF build can load and
 * compares it with the corresponding public RAM-store count. Families whose
 * public iterator is not reliable are instead verified one durable key at a
 * time through @c match. A mismatch is an internally inconsistent restore and
 * must stop destructive reconciliation.
 *
 * @param[in] families Persisted key families to audit.
 * @param[in] family_count Number of entries in @p families.
 * @param[in] ops Injectable durable-key probe, RAM counter, exact matcher and
 *                optional mismatch reporter.
 * @return ESP_OK, an access error from @p ops, or ESP_ERR_INVALID_STATE when
 *         the durable and RAM counts differ.
 */
esp_err_t ble_nimble_store_restore_audit(
    const ble_nimble_store_restore_family_t *families,
    size_t family_count,
    const ble_nimble_store_restore_audit_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NIMBLE_STORE_RESTORE_AUDIT_H__ */
