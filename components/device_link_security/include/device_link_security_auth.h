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

/** @brief Public device address. */
#define DEVICE_LINK_SECURITY_PEER_ADDR_PUBLIC 0U

/** @brief Random device address. */
#define DEVICE_LINK_SECURITY_PEER_ADDR_RANDOM 1U

/** @brief Resolved public identity address. */
#define DEVICE_LINK_SECURITY_PEER_ADDR_PUBLIC_ID 2U

/** @brief Resolved random identity address. */
#define DEVICE_LINK_SECURITY_PEER_ADDR_RANDOM_ID 3U

/** @brief Reserved tail bytes fixing the on-wire record size (no ABI
 * padding is ever persisted). */
#define DEVICE_LINK_SECURITY_AUTH_RESERVED_BYTES 1U

/**
 * @brief Committed authorization record.
 *
 * The only state that grants authorization (device-link-security-v1).
 * The plaintext application password is never persisted.
 *
 * The record is persisted as a raw blob, so the layout is frozen: explicit
 * reserved tail bytes replace compiler padding and the static asserts below
 * pin every field offset. The reserved bytes are always written zeroed.
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
    uint8_t reserved[DEVICE_LINK_SECURITY_AUTH_RESERVED_BYTES];
} device_link_security_auth_record_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#include <stddef.h>
_Static_assert(offsetof(device_link_security_auth_record_t, magic) == 0U,
               "auth record magic offset");
_Static_assert(
    offsetof(device_link_security_auth_record_t, schema_version) == 4U,
    "auth record schema offset");
_Static_assert(
    offsetof(device_link_security_auth_record_t, credential_id) == 8U,
    "auth record credential offset");
_Static_assert(
    offsetof(device_link_security_auth_record_t, device_auth_id) == 24U,
    "auth record auth id offset");
_Static_assert(offsetof(device_link_security_auth_record_t, salt) == 40U,
               "auth record salt offset");
_Static_assert(
    offsetof(device_link_security_auth_record_t, verifier) == 56U,
    "auth record verifier offset");
_Static_assert(
    offsetof(device_link_security_auth_record_t, peer_addr_type) == 440U,
    "auth record peer type offset");
_Static_assert(
    offsetof(device_link_security_auth_record_t, peer_addr) == 441U,
    "auth record peer addr offset");
_Static_assert(
    offsetof(device_link_security_auth_record_t, reserved) == 447U,
    "auth record reserved offset");
_Static_assert(
    sizeof(device_link_security_auth_record_t) == 448U,
    "auth record size is frozen at 448 bytes");
#endif

/**
 * @brief Validate a normalized SMP peer identity address.
 *
 * Public identities must be nonzero. Random identities must use the static
 * random class; over-the-air resolvable/private addresses, non-resolvable
 * private addresses, the reserved random class, and static-random values with
 * an all-zero or all-one random part are rejected.
 *
 * @param[in] peer_addr_type BLE peer identity address type (0-3).
 * @param[in] peer_addr Six-byte identity address.
 * @return True only for a normalized public or static-random identity.
 */
bool device_link_security_normalized_identity_valid(
    uint8_t peer_addr_type,
    const uint8_t peer_addr[DEVICE_LINK_SECURITY_AUTH_PEER_ADDR_BYTES]);

/**
 * @brief Check a committed record for structural validity.
 *
 * @param[in] record Record to validate.
 * @return True when committed, normalized to a peer identity, and
 *         structurally valid.
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

/**
 * @brief Journal the start of a local binding revoke.
 *
 * Writes a durable revoke-intent marker before any authorization or bond
 * mutation, so a crash mid-revoke resumes at startup (the startup
 * reconciliation completes the revoke before advertising). The marker is
 * cleared by device_link_security_end_revoke() once the bond deletion
 * succeeded.
 *
 * @return ESP_OK or a storage error.
 */
esp_err_t device_link_security_begin_revoke(void);

/**
 * @brief Clear the revoke-intent journal marker.
 *
 * @return ESP_OK (including when no marker existed) or a storage error.
 */
esp_err_t device_link_security_end_revoke(void);

/**
 * @brief Query whether a revoke-intent marker is journaled.
 *
 * A missing marker is reported as ESP_OK with @p pending set to false. Any
 * other storage or validation error is returned so startup can fail closed.
 *
 * @param[out] pending True while a revoke must still be completed.
 * @return ESP_OK, or a storage/validation error.
 */
esp_err_t device_link_security_revoke_pending(bool *pending);

#ifdef __cplusplus
}
#endif

#endif /* __DEVICE_LINK_SECURITY_AUTH_H__ */
