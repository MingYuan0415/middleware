#define DBG_TAG "ble_revoke_jrnl"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "ble_nimble_port_revoke_journal.h"

#include "nv_storage.h"

#include "nvs.h"

#include <string.h>

#define BLE_NIMBLE_PORT_REVOKE_JOURNAL_KEY "ble.revoke"
#define BLE_NIMBLE_PORT_REVOKE_JOURNAL_MAGIC UINT32_C(0x4252564A)
#define BLE_NIMBLE_PORT_REVOKE_JOURNAL_VERSION UINT16_C(1)
#define BLE_NIMBLE_PORT_REVOKE_JOURNAL_INTEGRITY UINT32_C(0x5C1A9F27)

typedef struct ble_nimble_port_revoke_journal_marker
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t integrity;
    uint32_t trailing_reserved;
} ble_nimble_port_revoke_journal_marker_t;

_Static_assert(sizeof(ble_nimble_port_revoke_journal_marker_t) == 16U,
               "Revoke journal marker layout must remain stable");

static bool _ble_nimble_port_revoke_journal_marker_valid(
    const ble_nimble_port_revoke_journal_marker_t *marker)
{
    return marker != NULL &&
           marker->magic == BLE_NIMBLE_PORT_REVOKE_JOURNAL_MAGIC &&
           marker->version == BLE_NIMBLE_PORT_REVOKE_JOURNAL_VERSION &&
           marker->reserved == 0U &&
           marker->integrity == BLE_NIMBLE_PORT_REVOKE_JOURNAL_INTEGRITY &&
           marker->trailing_reserved == 0U;
}

static esp_err_t _ble_nimble_port_revoke_journal_read(
    ble_nimble_port_revoke_journal_marker_t *marker, bool *present)
{
    size_t size = sizeof(*marker);

    *present = false;
    const esp_err_t result = nv_storage_get_blob(
                                 BLE_NIMBLE_PORT_REVOKE_JOURNAL_KEY, marker,
                                 &size);

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (result == ESP_ERR_NVS_INVALID_LENGTH)
    {
        LOG_E("revoke journal malformed size=%u", (unsigned)size);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (result != ESP_OK)
    {
        return result;
    }
    if (size != sizeof(*marker) ||
            !_ble_nimble_port_revoke_journal_marker_valid(marker))
    {
        LOG_E("revoke journal malformed size=%u", (unsigned)size);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *present = true;
    return ESP_OK;
}

static esp_err_t _ble_nimble_port_revoke_journal_write(void)
{
    const ble_nimble_port_revoke_journal_marker_t marker =
    {
        .magic = BLE_NIMBLE_PORT_REVOKE_JOURNAL_MAGIC,
        .version = BLE_NIMBLE_PORT_REVOKE_JOURNAL_VERSION,
        .integrity = BLE_NIMBLE_PORT_REVOKE_JOURNAL_INTEGRITY,
    };

    return nv_storage_set_blob(BLE_NIMBLE_PORT_REVOKE_JOURNAL_KEY, &marker,
                               sizeof(marker));
}

esp_err_t ble_nimble_port_revoke_journal_begin(void)
{
    ble_nimble_port_revoke_journal_marker_t marker;
    bool present = false;
    const esp_err_t result =
        _ble_nimble_port_revoke_journal_read(&marker, &present);

    if (result == ESP_OK && present)
    {
        return ESP_OK;
    }
    /* A malformed journal is repaired by the rewrite below; a storage read
     * failure keeps the caller fail closed. */
    if (result != ESP_OK && result != ESP_ERR_INVALID_RESPONSE)
    {
        return result;
    }
    return _ble_nimble_port_revoke_journal_write();
}

esp_err_t ble_nimble_port_revoke_journal_pending(bool *out_pending)
{
    ble_nimble_port_revoke_journal_marker_t marker;

    if (out_pending == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return _ble_nimble_port_revoke_journal_read(&marker, out_pending);
}

esp_err_t ble_nimble_port_revoke_journal_end(void)
{
    const esp_err_t result =
        nv_storage_erase_key(BLE_NIMBLE_PORT_REVOKE_JOURNAL_KEY);

    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}
