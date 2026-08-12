#include "ble_nimble_store_restore_audit.h"

#include <stdio.h>

#define BLE_NIMBLE_STORE_NVS_KEY_BYTES 16U

esp_err_t ble_nimble_store_restore_audit(
    const ble_nimble_store_restore_family_t *families,
    size_t family_count,
    const ble_nimble_store_restore_audit_ops_t *ops)
{
    if (families == NULL || family_count == 0U || ops == NULL ||
            ops->probe == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t family_index = 0U; family_index < family_count;
            ++family_index)
    {
        const ble_nimble_store_restore_family_t *family =
            &families[family_index];

        if (family->key_prefix == NULL || family->key_prefix[0] == '\0' ||
                family->max_entries == 0U ||
                (family->verify_exact_entries && ops->match == NULL) ||
                (!family->verify_exact_entries && ops->count == NULL))
        {
            return ESP_ERR_INVALID_ARG;
        }
        size_t durable_count = 0U;
        size_t restored_count = 0U;

        for (size_t entry = 1U; entry <= family->max_entries; ++entry)
        {
            char key[BLE_NIMBLE_STORE_NVS_KEY_BYTES];
            const int written = snprintf(key, sizeof(key), "%s_%u",
                                         family->key_prefix,
                                         (unsigned int)entry);

            if (written < 0 || (size_t)written >= sizeof(key))
            {
                return ESP_ERR_INVALID_ARG;
            }
            bool present = false;
            const esp_err_t probe_result =
                ops->probe(ops->context, key, &present);

            if (probe_result != ESP_OK)
            {
                return probe_result;
            }
            if (present)
            {
                durable_count++;
                if (family->verify_exact_entries)
                {
                    bool match = false;
                    const esp_err_t match_result = ops->match(
                                                       ops->context,
                                                       family->object_type,
                                                       key, &match);

                    if (match_result != ESP_OK)
                    {
                        return match_result;
                    }
                    if (match)
                    {
                        restored_count++;
                    }
                }
            }
        }
        if (!family->verify_exact_entries)
        {
            const esp_err_t count_result =
                ops->count(ops->context, family->object_type,
                           &restored_count);

            if (count_result != ESP_OK)
            {
                return count_result;
            }
        }
        if (restored_count > family->max_entries ||
                durable_count != restored_count)
        {
            if (ops->mismatch != NULL)
            {
                const ble_nimble_store_restore_mismatch_t mismatch =
                {
                    .key_prefix = family->key_prefix,
                    .durable_count = durable_count,
                    .restored_count = restored_count,
                };

                ops->mismatch(ops->context, &mismatch);
            }
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}
