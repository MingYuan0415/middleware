#ifndef __BLE_LINK_SECURITY_OPS_H__
#define __BLE_LINK_SECURITY_OPS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_operation.h"
#include "device_link_security.h"

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
    /** @brief Select and pin the verifier for the next handshake.
     *  See device_link_security_select_verifier(). */
    esp_err_t (*select_verifier)(uint8_t peer_addr_type,
                                 const uint8_t *peer_addr,
                                 size_t peer_addr_len,
                                 bool pairing_window_open);

    /** @brief Kind of the verifier pinned to the current session. */
    device_link_security_verifier_kind_t (*selected_verifier)(void);

    /** @brief Classify a handshake request without mutating its session. */
    esp_err_t (*classify_handshake)(
        const uint8_t *input, size_t input_len,
        device_link_security_handshake_stage_t *stage);

    /** @brief Process one handshake frame.
     *
     * On success the response is allocated for the caller. On failure the
     * implementation returns a NULL output and zero length; dependency
     * failure pointers never cross this adapter boundary.
     */
    esp_err_t (*handshake)(
        const uint8_t *input, size_t input_len,
        uint8_t **output, size_t *output_len,
        device_link_security_handshake_result_t *handshake_result);

    /** @brief Decrypt one protected Device Link v2 message.
     *
     * Response ciphertext is allocated on success. Failure returns a NULL
     * output and zero length.
     */
    esp_err_t (*unprotect)(const uint8_t *input, size_t input_len,
                           uint8_t **output, size_t *output_len);

    /** @brief Protect one outbound Device Link v2 message.
     *
     * Ciphertext is allocated on success. Failure returns a NULL output and
     * zero length.
     */
    esp_err_t (*protect)(const uint8_t *plain, size_t plain_len,
                         uint8_t **cipher, size_t *cipher_len);

    /** @brief Whether the current session is AUTHENTICATED. */
    bool (*is_authenticated)(void);

    /** @brief Whether a Security 2 session is established (handshake
     *  started); protected frames then decrypt under it or fail closed. */
    bool (*session_open)(void);

    /** @brief Close the current session. */
    void (*close_session)(void);

    /** @brief Delete a bond created by the current pairing attempt. */
    esp_err_t (*discard_provisional_bond)(
        const ble_link_operation_identity_t *identity,
        bool terminate_conn);

    /** @brief Promote the current pairing-attempt bond after durable commit. */
    esp_err_t (*promote_provisional_bond)(
        const ble_link_operation_identity_t *identity);

    /**
     * @brief Retain a remote bond-replacement transaction for owner execution.
     *
     * The NimBLE callback only registers the identity with the service. This
     * operation accepts durable invalidation plus peer-store cleanup ownership
     * and must not depend on a bounded command queue.
     */
    esp_err_t (*replace_authorization)(
        const ble_link_operation_identity_t *identity);
} ble_link_security_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_SECURITY_OPS_H__ */
