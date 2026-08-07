#ifndef __DEVICE_LINK_SECURITY_AUTH_H__
#define __DEVICE_LINK_SECURITY_AUTH_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Record magic "MTDL". */
#define DEVICE_LINK_SECURITY_AUTH_MAGIC 0x4d54444cU

/** @brief Committed-record schema version. */
#define DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION 1U

/** @brief Credential identifier length (bytes). */
#define DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES 16U

/** @brief Device authorization identifier length (bytes). */
#define DEVICE_LINK_SECURITY_AUTH_ID_BYTES 16U

/** @brief SRP salt length (bytes). */
#define DEVICE_LINK_SECURITY_AUTH_SALT_BYTES 16U

/** @brief SRP verifier length for the 3072-bit group (bytes). */
#define DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES 384U

/** @brief Peer identity address length (bytes). */
#define DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES 6U

/**
 * @brief Committed authorization record.
 *
 * The only state that grants authorization (device-link-security-v1).
 * The plaintext application password is never persisted.
 */
typedef struct device_link_security_auth_record
{
    uint32_t magic; /**< DEVICE_LINK_SECURITY_AUTH_MAGIC when committed. */
    uint32_t schema_version; /**< DEVICE_LINK_SECURITY_AUTH_SCHEMA_VERSION. */
    uint8_t credential_id[DEVICE_LINK_SECURITY_AUTH_CREDENTIAL_BYTES];
    uint8_t device_auth_id[DEVICE_LINK_SECURITY_AUTH_ID_BYTES];
    uint8_t salt[DEVICE_LINK_SECURITY_AUTH_SALT_BYTES];
    uint8_t verifier[DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES];
    uint8_t peer_addr_type; /**< BLE peer identity address type. */
    uint8_t peer_addr[DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES];
} device_link_security_auth_record_t;

/**
 * @brief Check a record for a valid magic and schema version.
 *
 * @param[in] record Record to validate.
 * @return True when committed and structurally valid.
 */
bool device_link_security_auth_record_valid(
    const device_link_security_auth_record_t *record);

/**
 * @brief Persist the authorization record atomically.
 *
 * Replaces any previous record (a new binding overwrites the old one;
 * the caller invalidates the old authorization before committing).
 *
 * @param[in] record Committed record.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or a storage error.
 */
esp_err_t device_link_security_save_auth_record(
    const device_link_security_auth_record_t *record);

/**
 * @brief Load the persisted authorization record.
 *
 * @param[out] record Output record.
 * @return ESP_OK, ESP_ERR_NOT_FOUND when none is persisted, or a
 *         storage/validation error.
 */
esp_err_t device_link_security_load_auth_record(
    device_link_security_auth_record_t *record);

/**
 * @brief Erase the persisted authorization record.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND when none is persisted, or a storage
 *         error.
 */
esp_err_t device_link_security_erase_auth_record(void);

/**
 * @brief Derive the long-term SRP salt and verifier from the application
 * password.
 *
 * @param[in] password Application password (never persisted by the device).
 * @param[in] password_len Password length.
 * @param[out] salt Derived 16-byte salt.
 * @param[out] verifier Derived 384-byte verifier (3072-bit group).
 * @return ESP_OK or an allocation error.
 */
esp_err_t device_link_security_derive_long_term_verifier(
    const uint8_t *password, size_t password_len,
    uint8_t salt[DEVICE_LINK_SECURITY_AUTH_SALT_BYTES],
    uint8_t verifier[DEVICE_LINK_SECURITY_AUTH_VERIFIER_BYTES]);

/**
 * @brief Load the long-term verifier from the persisted record into the
 * active session.
 *
 * Replaces the active bootstrap verifier, rebuilds the Protocomm
 * instance, and clears any open session. A reconnect handshake outside a
 * window then uses the long-term application credential.
 *
 * @return ESP_OK, ESP_ERR_NOT_FOUND when no record is persisted, or a
 *         rebuild error.
 */
esp_err_t device_link_security_load_long_term_verifier(void);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_SECURITY_AUTH_H__ */
