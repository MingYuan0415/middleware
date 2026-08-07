#ifndef __BLE_LINK_SECURITY_OPS_H__
#define __BLE_LINK_SECURITY_OPS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Security 2 session operations injected into the link service.
 *
 * The adapter (device_link_security) owns the session state; the link
 * service uses these operations for the transport type routing (0x00
 * handshake, 0x01 protected). The service never holds security material.
 */
typedef struct ble_link_security_ops
{
    /** @brief Process one handshake frame; response is allocated. */
    esp_err_t (*handshake)(const uint8_t *input, size_t input_len,
                           uint8_t **output, size_t *output_len);

    /** @brief Decrypt and dispatch one protected frame; response ciphertext
     *  is allocated. */
    esp_err_t (*unprotect)(const uint8_t *input, size_t input_len,
                           uint8_t **output, size_t *output_len);

    /** @brief Encrypt one outbound plaintext frame; ciphertext allocated. */
    esp_err_t (*protect)(const uint8_t *plain, size_t plain_len,
                         uint8_t **cipher, size_t *cipher_len);

    /** @brief Whether the current session is AUTHENTICATED. */
    bool (*is_authenticated)(void);

    /** @brief Whether a Security 2 session is established (handshake
     *  started); protected frames then decrypt under it or fail closed. */
    bool (*session_open)(void);

    /** @brief Close the current session. */
    void (*close_session)(void);
} ble_link_security_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_SECURITY_OPS_H__ */
