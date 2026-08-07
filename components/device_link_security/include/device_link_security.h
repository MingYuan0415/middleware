#ifndef __DEVICE_LINK_SECURITY_H__
#define __DEVICE_LINK_SECURITY_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Default SRP username (also used when the config leaves it NULL). */
#define DEVICE_LINK_SECURITY_USERNAME "microtech"

/**
 * @brief Authentication transition callback.
 *
 * Invoked after the first successful decryption proves the SRP proof,
 * before the request callback dispatches the plaintext. Returns ESP_OK to
 * proceed, or an error to fail the session closed. Runs without the
 * adapter lock (the same unlocked context as the request callback).
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

/** @brief Security adapter configuration. */
typedef struct device_link_security_config
{
    const char *username; /**< SRP username, kept for the adapter lifetime. */
    uint32_t session_id; /**< Protocomm session id, e.g. connection generation. */
    device_link_security_request_fn request_cb; /**< Protected request sink. */
    void *request_arg; /**< Callback argument. */
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
 * Derives a fresh SRP salt and verifier from the window POP and rebuilds
 * the Protocomm instance, so any established session is replaced. The
 * salt and verifier are kept only until close_bootstrap.
 *
 * @param[in] pop QR proof of possession bytes.
 * @param[in] pop_len POP length.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM.
 */
esp_err_t device_link_security_open_bootstrap(
    const uint8_t *pop, size_t pop_len);

/**
 * @brief Close the bootstrap verifier and replace it with the long-term
 * verifier if one is committed (P3.4b), otherwise none.
 */
void device_link_security_close_bootstrap(void);

/**
 * @brief Process one Security 2 handshake frame.
 *
 * The input is the Protocomm SessionData wire; the output is the
 * SessionData response, allocated by the adapter and freed by the caller
 * with free(). A success means the command was accepted; the handshake
 * sequence may still be mid-flight (command 0), so AUTHENTICATED is not
 * implied. The session becomes AUTHENTICATED only when the first
 * protected frame decrypts successfully, which the Security 2 scheme
 * only allows after the SRP proof verified. Poll
 * device_link_security_is_authenticated() after the first successful
 * protected exchange; unprotect() is permitted while pending because
 * the upstream decrypt is the proof-completion gate.
 *
 * @param[in] input SessionData bytes.
 * @param[in] input_len Input length.
 * @param[out] output Allocated SessionData response.
 * @param[out] output_len Response length.
 * @return ESP_OK, or a Protocomm error (the session is closed on failure).
 */
esp_err_t device_link_security_handshake(
    const uint8_t *input, size_t input_len,
    uint8_t **output, size_t *output_len);

/**
 * @brief Decrypt and dispatch one protected application frame.
 *
 * The input is the AES-GCM ciphertext of an Envelope. The adapter
 * decrypts it, invokes the request callback with the plaintext, encrypts
 * the callback response, and returns it in @p output (freed by the
 * caller). Requires an AUTHENTICATED session; the first successful
 * decrypt is the AUTHENTICATED transition. Ciphertext of 16 bytes or
 * fewer is malformed and closes the session.
 *
 * All adapter entry points are serialized by an internal mutex; the
 * request callback runs while the lock is held and must not call back
 * into the adapter.
 *
 * @param[in] input Ciphertext.
 * @param[in] input_len Ciphertext length (must exceed the 16-byte tag).
 * @param[out] output Allocated ciphertext response.
 * @param[out] output_len Response length.
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
 * @param[out] cipher Allocated ciphertext.
 * @param[out] cipher_len Ciphertext length.
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
 * AUTHENTICATED only after a protected frame decrypts.
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
