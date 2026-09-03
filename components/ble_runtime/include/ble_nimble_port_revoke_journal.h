#ifndef __BLE_NIMBLE_PORT_REVOKE_JOURNAL_H__
#define __BLE_NIMBLE_PORT_REVOKE_JOURNAL_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Persist the local binding revoke intent before any deletion.
 *
 * Idempotent: a valid journal already present returns ESP_OK without a
 * rewrite. A malformed journal is repaired by a rewrite so the durable
 * obligation is never blocked by garbage.
 *
 * @return ESP_OK when a valid journal is present afterwards, otherwise an
 *         NVS storage error.
 */
esp_err_t ble_nimble_port_revoke_journal_begin(void);

/**
 * @brief Report whether a durable revoke intent is present.
 *
 * A malformed journal yields ESP_ERR_INVALID_RESPONSE so every caller fails
 * closed and never deletes or reopens pairing on unreadable intent.
 *
 * @param out_pending receives true when a valid journal exists.
 * @return ESP_OK when the journal is valid or absent,
 *         ESP_ERR_INVALID_RESPONSE when malformed, otherwise an NVS error.
 */
esp_err_t ble_nimble_port_revoke_journal_pending(bool *out_pending);

/**
 * @brief Clear the durable revoke intent.
 *
 * Only the revoke owner calls this after the peer store deletion was
 * verified empty. An absent journal is success (idempotent resume).
 *
 * @return ESP_OK when no valid journal remains, otherwise an NVS error.
 */
esp_err_t ble_nimble_port_revoke_journal_end(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NIMBLE_PORT_REVOKE_JOURNAL_H__ */
