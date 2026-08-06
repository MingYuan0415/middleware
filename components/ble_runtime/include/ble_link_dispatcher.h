#ifndef __BLE_LINK_DISPATCHER_H__
#define __BLE_LINK_DISPATCHER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_link_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Frozen Link v1 envelope flag values. */
#define BLE_LINK_CODEC_FLAG_RECOVERY_QUERY 1U

/** @brief Frozen Link v1 protocol major. */
#define BLE_LINK_CODEC_PROTOCOL_MAJOR 1U

/** @brief Link v1 application error codes (microtech.link.v1.LinkError). */
typedef enum
{
    BLE_LINK_ERROR_UNSPECIFIED = 0,
    BLE_LINK_ERROR_OK = 1,
    BLE_LINK_ERROR_MALFORMED_FRAME = 2,
    BLE_LINK_ERROR_UNSUPPORTED_VERSION = 3,
    BLE_LINK_ERROR_UNSUPPORTED_OPERATION = 4,
    BLE_LINK_ERROR_UNSUPPORTED_CAPABILITY = 5,
    BLE_LINK_ERROR_UNAUTHENTICATED = 6,
    BLE_LINK_ERROR_PERMISSION_DENIED = 7,
    BLE_LINK_ERROR_CONFIRMATION_REQUIRED = 8,
    BLE_LINK_ERROR_INVALID_ARGUMENT = 9,
    BLE_LINK_ERROR_BUSY = 10,
    BLE_LINK_ERROR_NOT_FOUND = 11,
    BLE_LINK_ERROR_RESOURCE_EXHAUSTED = 12,
    BLE_LINK_ERROR_CONFLICT = 13,
    BLE_LINK_ERROR_UNAVAILABLE = 14,
    BLE_LINK_ERROR_STORAGE = 15,
    BLE_LINK_ERROR_INTERNAL = 16,
} ble_link_error_t;

/**
 * @brief Connection and session facts the dispatcher validates against.
 *
 * These are runtime facts, never inferred from callback arrival order.
 */
typedef struct ble_link_dispatcher_facts
{
    uint64_t active_boot_id;  /**< Current boot id, nonzero. */
    uint32_t connection_generation;
    bool encrypted;             /**< Link encrypted. */
    bool session_authenticated; /**< Security 2 session authenticated. */
    bool authorized;            /**< Session matches the committed record. */
} ble_link_dispatcher_facts_t;

/**
 * @brief Request handler invoked after all envelope validation passes.
 *
 * Runs in the caller's task context, never inside a host callback. Returns a
 * LinkError value that is sent back to the client; return
 * BLE_LINK_ERROR_OK on success.
 *
 * @param[in] request Decoded request shell; slices stay valid for the call.
 * @param[in] facts   Connection and session facts.
 * @param[in] arg     Handler argument from registration.
 * @return A ble_link_error_t value to send back.
 */
typedef uint32_t (*ble_link_request_handler_t)(
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, void *arg);

/**
 * @brief Dispatch one decoded request envelope.
 *
 * Validation order follows the frozen contract: protocol major, boot id,
 * envelope flags (unknown and duplicate values), request id nonzero, request
 * id uniqueness for the session, and recognized request body. On success the
 * registered handler is invoked; an error returns a stable LinkError value
 * through out_error.
 *
 * @param[in]  envelope  Decoded envelope shell.
 * @param[in]  request   Decoded request shell.
 * @param[in]  facts     Connection and session facts.
 * @param[out] out_error Stable LinkError value to send back; set to
 *                       LINK_ERROR_RESOURCE_EXHAUSTED when the session id
 *                       set cannot grow (out of memory), which also returns
 *                       ESP_ERR_NO_MEM.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM when the session id
 *         set allocation failed.
 */
esp_err_t ble_link_dispatcher_handle_request(
    const ble_link_codec_envelope_t *envelope,
    const ble_link_codec_request_t *request,
    const ble_link_dispatcher_facts_t *facts, uint32_t *out_error);

/**
 * @brief Register a request handler for one request body tag.
 *
 * A tag may be registered at most once; registration replaces nothing and
 * fails with ESP_ERR_INVALID_STATE on a duplicate tag.
 *
 * @param[in] tag     Request body tag.
 * @param[in] handler Handler, required.
 * @param[in] arg     Handler argument.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE.
 */
esp_err_t ble_link_dispatcher_register_request(
    ble_link_codec_request_tag_t tag,
    ble_link_request_handler_t handler, void *arg);

/**
 * @brief Clear the session id set (new application session or new boot).
 *
 * The session id set grows dynamically and is bounded only by memory; it
 * tracks every request id used within the current application session for
 * conflict detection. The caller owns generation filtering: this must only
 * be called for the current connection generation, never from a late
 * callback of a retired generation.
 */
void ble_link_dispatcher_clear_session(void);

/**
 * @brief Reset the dispatcher; all handlers are unregistered.
 */
void ble_link_dispatcher_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_LINK_DISPATCHER_H__ */
