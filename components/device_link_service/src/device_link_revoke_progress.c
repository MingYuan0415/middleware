#define DBG_TAG "device_link_revoke"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "device_link_revoke_progress.h"

#include "ble_nimble_port.h"
#include "ble_nimble_port_revoke_journal.h"

esp_err_t device_link_revoke_progress(void)
{
    esp_err_t result = ble_nimble_port_revoke_binding();

    if (result != ESP_OK)
    {
        return result;
    }
    bool pending = false;

    result = ble_nimble_port_revoke_journal_pending(&pending);
    if (result != ESP_OK)
    {
        return result;
    }
    return pending ? ESP_ERR_NOT_FINISHED : ESP_OK;
}
