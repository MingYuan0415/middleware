#ifndef __DEVICE_LINK_SECURITY_H__
#define __DEVICE_LINK_SECURITY_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Derived public password length in ASCII bytes. */
#define DEVICE_LINK_SECURITY_PUBLIC_PASSWORD_BYTES 43U

/** @brief Bytes of the advertised public instance identifier. */
#define DEVICE_LINK_SECURITY_PUBLIC_INSTANCE_BYTES 3U

/** @brief Default SRP username (also used when the config leaves it NULL). */
#define DEVICE_LINK_SECURITY_USERNAME "microtech"

/** @brief Public-discovery SRP derivation label (frozen in security.json). */
#define DEVICE_LINK_SECURITY_PUBLIC_SRP_LABEL \
    "microtech.device-link.v2.public-srp"

/** @brief Public-discovery SRP derivation service UUID (RFC 4122 form). */
#define DEVICE_LINK_SECURITY_PUBLIC_SRP_SERVICE_UUID \
    "2c77e48c-c510-4230-8d05-63d036dc038b"

/* Public-discovery password derivation (fixtures/core/v2/security.json):
 *   input  = UTF-8(label) || 0x00 || UUID_BYTES || 0x02 || instance_id[3]
 *   password = BASE64URL_NO_PADDING(SHA-256(input))
 * The instance identifier is the raw three advertisement-order bytes, not
 * a numeric encoding: the frozen KAT instance "123456" is the byte triple
 * 0x12 0x34 0x56 and derives "L_FahHWW-ZZHIURRoXgvSRBo1n1iTem9WrD4rysV1Tc".
 */

#ifdef UNIT_TEST_HOST
/**
 * @brief Test-only seam: derive the public SRP password for an
 * advertisement instance id (frozen KAT in security.json).
 *
 * @param[in]  instance_id Non-zero advertisement instance id.
 * @param[out] password    Buffer of at least
 *                         DEVICE_LINK_SECURITY_PUBLIC_PASSWORD_BYTES + 1.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG.
 */
esp_err_t device_link_security_test_derive_public_password(
    const uint8_t instance_id[DEVICE_LINK_SECURITY_PUBLIC_INSTANCE_BYTES],
    char password[DEVICE_LINK_SECURITY_PUBLIC_PASSWORD_BYTES + 1U]);
#endif

/** @brief Verifier slot selected for a Security 2 handshake. */
typedef enum
{
    DEVICE_LINK_SECURITY_VERIFIER_NONE = 0, /**< No verifier: handshake not admitted. */
    DEVICE_LINK_SECURITY_VERIFIER_BOOTSTRAP, /**< QR POP of the open pairing window. */
    DEVICE_LINK_SECURITY_VERIFIER_LONG_TERM, /**< Committed authorization record. */
    DEVICE_LINK_SECURITY_VERIFIER_PUBLIC, /**< Public-discovery password of the
                                           *  current advertisement. */
} device_link_security_verifier_kind_t;

/** @brief Security 2 handshake command accepted by the adapter. */
typedef enum
{
    DEVICE_LINK_SECURITY_HANDSHAKE_CMD0 = 0,
    DEVICE_LINK_SECURITY_HANDSHAKE_CMD1,
} device_link_security_handshake_stage_t;

/** @brief Result of one parsed Security 2 handshake exchange. */
typedef struct device_link_security_handshake_result
{
    device_link_security_handshake_stage_t stage; /**< Accepted command. */
    bool authenticated; /**< True only after a successful Cmd1/Resp1. */
} device_link_security_handshake_result_t;

/**
 * @brief Parse and classify one Security 2 handshake request.
 *
 * This performs no session mutation. Transports use it to identify a Cmd0
 * replacement before deciding whether the current indication slot must be
 * retained.
 */
esp_err_t device_link_security_classify_handshake(
    const uint8_t *input, size_t input_len,
    device_link_security_handshake_stage_t *stage);

/**
 * @brief Authentication transition callback.
 *
 * Invoked after a successful Cmd1 proof and Resp1, before the response is
 * returned to the transport. Returns ESP_OK to accept the authenticated
 * transition, or an error to fail the session closed. Runs without the
 * adapter lock.
 *
 * @param[in] arg Callback argument from the configuration.
 * @return ESP_OK, or an error to fail the session closed.
 */
typedef esp_err_t (*device_link_security_authenticated_fn)(void *arg);

/**
 * @brief Session request callback.
 *
 * Invoked with the plaintext application envelope after the Security 2
 * session decrypted it. The callback produces the plaintext response
 * envelope; it must allocate the response with the project allocator and
 * the caller frees it after encryption.
 *
 * @param[in] request Plaintext request envelope.
 * @param[in] request_len Request length.
 * @param[out] response Allocated plaintext response envelope.
 * @param[out] response_len Response length.
 * @param[in] arg Callback argument from the configuration.
 * @return ESP_OK on success.
 */
typedef esp_err_t (*device_link_security_request_fn)(
    const uint8_t *request, size_t request_len,
    uint8_t **response, size_t *response_len, void *arg);

/** @brief Release a response returned by the application request callback. */
typedef void (*device_link_security_response_release_fn)(
    uint8_t *response, size_t response_len, void *arg);

/** @brief Security adapter configuration. */
typedef struct device_link_security_config
{
    const char *username; /**< SRP username, kept for the adapter lifetime. */
    uint32_t session_id; /**< Protocomm session id, e.g. connection generation. */
    device_link_security_request_fn request_cb; /**< Protected request sink. */
    void *request_arg; /**< Callback argument. */
    device_link_security_response_release_fn response_release_cb;
    void *response_release_arg;
    device_link_security_authenticated_fn authenticated_cb; /**< Optional
                                                             *  authentication
                                                             *  transition. */
    void *authenticated_arg; /**< Authentication callback argument. */
} device_link_security_config_t;

/**
 * @brief Initialize the Security 2 adapter.
 *
 * Creates the Protocomm instance with the Security 2 scheme and the
 * application endpoint. No verifier exists until a binding window opens
 * (or a long-term record is loaded), so handshakes fail closed meanwhile.
 * A NULL username falls back to DEVICE_LINK_SECURITY_USERNAME.
 *
 * @param[in] config Configuration, copied.
 * @return ESP_OK or an allocation error.
 */
esp_err_t device_link_security_init(const device_link_security_config_t *config);

/**
 * @brief Release all adapter resources.
 */
void device_link_security_deinit(void);

/**
 * @brief Open the bootstrap verifier for a binding window.
 *
 * Derives a fresh SRP salt and verifier from the window POP into the
 * bootstrap slot. The long-term verifier slot is preserved: a bound peer
 * keeps reconnecting with its long-term credential while a replacement
 * window is open. The salt and verifier are kept only until
 * close_bootstrap.
 *
 * @param[in] pop QR proof of possession bytes.
 * @param[in] pop_len POP length.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM.
 */
esp_err_t device_link_security_open_bootstrap(
    const uint8_t *pop, size_t pop_len);

/**
 * @brief Close the bootstrap verifier and rebuild the instance with the
 * long-term verifier if one is committed, otherwise none.
 * @return ESP_OK or a verifier rebuild error.
 */
esp_err_t device_link_security_close_bootstrap(void);

/**
 * @brief Install the public-discovery verifier for the current
 * advertisement.
 *
 * Derives the public SRP password from the advertisement per the frozen
 * derivation:
 *
 * ```text
 * input = UTF-8("microtech.device-link.v2.public-srp")
 *         || 0x00
 *         || UUID_BYTES("2c77e48c-c510-4230-8d05-63d036dc038b")
 *         || 0x02
 *         || instance_id[3]
 * password = BASE64URL_NO_PADDING(SHA-256(input))
 * ```
 *
 * A fresh random 16-byte salt is generated when the verifier is installed;
 * the salt is never derived from the instance ID. The password is public
 * input, stable across Bluetooth disable/enable within one boot, and
 * changes on a new boot. The long-term and bootstrap slots are preserved.
 * The public slot is kept until close_public, close_bootstrap-side
 * transitions, deinit, or a fresh boot.
 *
 * @param[in] instance_id Current non-zero advertisement instance id.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a NULL or all-zero instance id,
 *         or a derivation/rebuild error.
 */
esp_err_t device_link_security_open_public(
    const uint8_t instance_id[DEVICE_LINK_SECURITY_PUBLIC_INSTANCE_BYTES]);

/**
 * @brief Remove the public-discovery verifier and rebuild the instance
 * with the long-term verifier if one is committed, otherwise none.
 * @return ESP_OK or a verifier rebuild error.
 */
esp_err_t device_link_security_close_public(void);

/**
 * @brief Select and pin the verifier for the next Security 2 handshake.
 *
 * Called before every first handshake command (command 0) with the
 * resolved peer identity and the local pairing-window state. The adapter
 * picks the long-term verifier when the committed record's identity
 * matches the peer; otherwise the bootstrap verifier when a window is
 * open; otherwise no verifier (the handshake is not admitted). The
 * selection is pinned to the session so a later revoke or replacement
 * cannot resurrect a stale handshake.
 *
 * @param[in] peer_addr_type Peer identity address type (0-3; random
 *                           identity type 3 is legal).
 * @param[in] peer_addr Peer identity address bytes.
 * @param[in] peer_addr_len Peer address length.
 * @param[in] pairing_window_open Local pairing window state.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE when
 *         uninitialized.
 */
esp_err_t device_link_security_select_verifier(
    uint8_t peer_addr_type, const uint8_t *peer_addr, size_t peer_addr_len,
    bool pairing_window_open);

/**
 * @brief Report the verifier kind pinned to the current session.
 *
 * @return DEVICE_LINK_SECURITY_VERIFIER_NONE, _BOOTSTRAP, or _LONG_TERM.
 */
device_link_security_verifier_kind_t device_link_security_selected_verifier(void);

/**
 * @brief Process one Security 2 handshake frame.
 *
 * The input is the Protocomm SessionData wire; the output is the
 * SessionData response, allocated by the adapter and freed by the caller
 * with free(). A success means the command was accepted; the handshake
 * sequence may still be mid-flight (command 0), so AUTHENTICATED is not
 * implied. A successful command 1 proof and response 1 transition the
 * session to AUTHENTICATED before this function returns.
 *
 * @param[in] input SessionData bytes.
 * @param[in] input_len Input length.
 * @param[out] output Allocated SessionData response on success; NULL on
 *                    failure.
 * @param[out] output_len Response length on success; zero on failure.
 * @return ESP_OK, or a Protocomm error (the session is closed on failure).
 */
esp_err_t device_link_security_handshake(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len);

/**
 * @brief Process and classify one Security 2 handshake frame.
 *
 * This is the structured form of device_link_security_handshake(). Both
 * request and response are parsed as ESP-IDF SessionData/Sec2Payload; a
 * mismatched response or unsuccessful status fails the session closed. A
 * new Cmd0 retires any existing logical session before opening a fresh
 * epoch.
 *
 * @param[in] input Serialized SessionData request.
 * @param[in] input_len Request length.
 * @param[out] output Allocated serialized response on success; NULL on
 *                    failure.
 * @param[out] output_len Response length on success; zero on failure.
 * @param[out] handshake_result Parsed stage and authentication result.
 * @return ESP_OK, or an error with the current session closed.
 */
esp_err_t device_link_security_handshake_ex(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len,
    device_link_security_handshake_result_t *handshake_result);

/**
 * @brief Decrypt and dispatch one protected application frame.
 *
 * The input is the AES-GCM ciphertext of an Envelope. The adapter
 * decrypts it, invokes the request callback with the plaintext, encrypts
 * the callback response, and returns it in @p output (freed by the
 * caller). Requires a session authenticated by a successful Cmd1/Resp1.
 * Ciphertext of 16 bytes or fewer is malformed and closes the session.
 *
 * All adapter entry points are serialized by an internal mutex, but the
 * request callbacks run WITHOUT the lock and may call back into the
 * adapter (protect, close_session, verifier transitions) without
 * re-entering the mutex. The session identity is snapshotted under the
 * lock and revalidated before the response is encrypted, so a session
 * replaced during the callback fails closed.
 *
 * @param[in] input Ciphertext.
 * @param[in] input_len Ciphertext length (must exceed the 16-byte tag).
 * @param[out] output Allocated ciphertext response on success; NULL on
 *                    failure.
 * @param[out] output_len Response length on success; zero on failure.
 * @return ESP_OK, ESP_ERR_INVALID_STATE when unauthenticated, or a
 *         Protocomm error (the session is closed on failure).
 */
esp_err_t device_link_security_unprotect(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len);

/**
 * @brief Encrypt one outbound plaintext application frame.
 *
 * The adapter encrypts the plaintext Envelope and returns the allocated
 * AES-GCM ciphertext in @p output (freed by the caller). Requires an
 * AUTHENTICATED session; the counter stream is shared with unprotect,
 * so every outbound frame advances the nonce and the peer must decrypt
 * in order.
 *
 * @param[in] plain Plaintext Envelope.
 * @param[in] plain_len Plaintext length.
 * @param[out] cipher Allocated ciphertext on success; NULL on failure.
 * @param[out] cipher_len Ciphertext length on success; zero on failure.
 * @return ESP_OK, ESP_ERR_INVALID_STATE when unauthenticated, or an
 *         encryption error.
 */
esp_err_t device_link_security_protect(
    const uint8_t *plain, size_t plain_len,
    uint8_t **cipher, size_t *cipher_len);

/**
 * @brief Report whether the current session is AUTHENTICATED.
 */
bool device_link_security_is_authenticated(void);

/**
 * @brief Report whether a Security 2 session is established.
 *
 * True once a handshake opened the transport session; the session is
 * AUTHENTICATED only after a successful Cmd1/Resp1.
 */
bool device_link_security_session_open(void);

/**
 * @brief Close the current Protocomm session.
 */
void device_link_security_close_session(void);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_SECURITY_H__ */
