#ifndef __BLE_LINK_CLEANUP_OBLIGATION_H__
#define __BLE_LINK_CLEANUP_OBLIGATION_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_link_operation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_LINK_CLEANUP_OBLIGATION_CAPACITY 4U
#define BLE_LINK_CLEANUP_PEER_ADDR_BYTES 6U

/** @brief Complete immutable payload for one retained bond cleanup. */
typedef struct ble_link_cleanup_request
{
    ble_link_operation_identity_t identity;
    uint8_t peer_addr_type;
    uint8_t peer_addr[BLE_LINK_CLEANUP_PEER_ADDR_BYTES];
    bool peer_addr_valid;
    bool delete_all_if_unresolved;
    bool provisional;
    bool terminate_conn;
    bool invalidate_authorization;
} ble_link_cleanup_request_t;

typedef struct ble_link_cleanup_slot
{
    bool active;
    bool in_progress;
    ble_link_cleanup_request_t request;
    uint64_t retry_not_before_us;
} ble_link_cleanup_slot_t;

typedef struct ble_link_cleanup_state
{
    ble_link_cleanup_slot_t slots[BLE_LINK_CLEANUP_OBLIGATION_CAPACITY];
    /**
     * Single fail-closed overflow slot. Production admits one connection and
     * one bond, and rejects new ACLs while any cleanup is retained. Therefore
     * at most one distinct request can race the admission fence after the
     * fixed table was observed full.
     */
    ble_link_cleanup_slot_t overflow;
    bool terminal_fence_active;
    uint32_t terminal_fence_generation;
    uint16_t terminal_fence_conn_handle;
} ble_link_cleanup_state_t;

/** @brief Reset all retained cleanup and terminal-fence state. */
void ble_link_cleanup_reset(ble_link_cleanup_state_t *state);

/** @brief Retain an exact cleanup, coalescing an identical identity. */
bool ble_link_cleanup_retain(
    ble_link_cleanup_state_t *state,
    const ble_link_cleanup_request_t *request, uint64_t now_us);

/** @brief Claim one due cleanup for execution. */
bool ble_link_cleanup_take_due(
    ble_link_cleanup_state_t *state, uint64_t now_us,
    ble_link_cleanup_request_t *request);

/** @brief Complete or schedule a retry for an exact claimed cleanup. */
void ble_link_cleanup_finish(
    ble_link_cleanup_state_t *state,
    const ble_link_cleanup_request_t *request, bool complete,
    uint64_t retry_not_before_us);

/** @brief Retain the terminal write fence for one exact ACL generation. */
bool ble_link_cleanup_terminal_fence_retain(
    ble_link_cleanup_state_t *state,
    uint32_t generation, uint16_t conn_handle);

/** @brief Whether one exact ACL generation is terminal-write fenced. */
bool ble_link_cleanup_terminal_fence_matches(
    const ble_link_cleanup_state_t *state,
    uint32_t generation, uint16_t conn_handle);

/** @brief Release a terminal fence only for the matching retired ACL. */
bool ble_link_cleanup_terminal_fence_release(
    ble_link_cleanup_state_t *state,
    uint32_t generation, uint16_t conn_handle);

/** @brief Whether any peer-store cleanup is retained or in progress. */
bool ble_link_cleanup_pending(const ble_link_cleanup_state_t *state);

/** @brief Whether a retained cleanup owns the exact ACL generation. */
bool ble_link_cleanup_pending_for_acl(
    const ble_link_cleanup_state_t *state,
    uint32_t generation, uint16_t conn_handle);

/** @brief Whether a provisional discard remains for the exact ACL. */
bool ble_link_cleanup_provisional_pending_for_acl(
    const ble_link_cleanup_state_t *state,
    uint32_t generation, uint16_t conn_handle);

/** @brief Connection admission is closed while cleanup remains pending. */
bool ble_link_cleanup_admission_allowed(
    const ble_link_cleanup_state_t *state);

/** @brief Nearest retained cleanup retry deadline. */
uint64_t ble_link_cleanup_remaining_us(
    const ble_link_cleanup_state_t *state, uint64_t now_us);

#ifdef __cplusplus
}
#endif /* __BLE_LINK_CLEANUP_OBLIGATION_H__ */

#endif
