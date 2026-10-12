#ifndef __BLE_LINK_SESSION_H__
#define __BLE_LINK_SESSION_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_LINK_STATE_FLAG_BINDABLE UINT32_C(0x01)
#define BLE_LINK_STATE_FLAG_BOUND UINT32_C(0x02)
#define BLE_LINK_STATE_FLAG_PUBLIC_DISCOVERY UINT32_C(0x04)
#define BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED UINT32_C(0x08)
#define BLE_LINK_STATE_FLAG_TRANSITIONING UINT32_C(0x10)
#define BLE_LINK_STATE_FLAG_AUTHENTICATED UINT32_C(0x20)
#define BLE_LINK_STATE_FLAG_ERROR UINT32_C(0x80)

/** @brief Current ACL identity and encryption facts. */
typedef struct ble_link_dispatcher_facts
{
    uint64_t active_boot_id;
    uint32_t connection_generation;
    bool encrypted;
} ble_link_dispatcher_facts_t;

/** @brief Facts supplied by the connection owner and security reducer. */
typedef enum
{
    BLE_LINK_SESSION_EVENT_ACL_CONNECTED = 0,
    BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED,
    BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED,
    BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED,
} ble_link_session_event_t;

/** @brief Initialize boot identity and session state. */
void ble_link_session_init(uint64_t boot_id);
/** @brief Reset runtime state while retaining boot identity. */
void ble_link_session_reset(void);
/** @brief Apply a generation-scoped fact; stale generations are ignored. */
esp_err_t ble_link_session_handle_event(uint32_t generation, ble_link_session_event_t event);
/** @brief Set committed bond state; revision zero advances the local revision. */
esp_err_t ble_link_session_set_authorization(bool committed, uint32_t revision);
/** @brief Set the local admission window independently of the current ACL. */
void ble_link_session_set_pairing_window(bool open);
/** @brief Capture the pairing window admitted for this connection. */
esp_err_t ble_link_session_set_connection_pairing_window(uint32_t generation, bool open);
/** @brief Read the pairing window captured for this connection. */
esp_err_t ble_link_session_get_connection_pairing_window(uint32_t generation, bool *out_open);
/** @brief Clear encryption, bond and identity facts for the current generation. */
void ble_link_session_clear_link_security(uint32_t generation);
/** @brief Read current ACL facts, rejecting a retired generation. */
esp_err_t ble_link_session_get_facts(uint32_t generation, ble_link_dispatcher_facts_t *facts);
/** @brief Record whether the connection's peer identity has been verified. */
esp_err_t ble_link_session_set_identity_known(uint32_t generation, bool known);
/** @brief Read verified bond and identity facts for the current connection. */
esp_err_t ble_link_session_get_security_facts(uint32_t generation, bool *out_bond_verified,
        bool *out_identity_known);
/** @brief Return discovery, bond and verified SC authentication state flags. */
uint32_t ble_link_session_get_state_flags(void);
/** @brief Mark an in-flight binding/revocation transition. */
void ble_link_session_set_authorization_transitioning(bool active);
/** @brief Latch an unrecoverable runtime error until reset. */
void ble_link_session_set_error(bool error);

#ifdef __cplusplus
}
#endif
#endif /* __BLE_LINK_SESSION_H__ */
