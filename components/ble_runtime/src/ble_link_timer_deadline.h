#ifndef BLE_LINK_TIMER_DEADLINE_H
#define BLE_LINK_TIMER_DEADLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_link_operation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_LINK_TIMER_DEADLINE_SLOT_COUNT 3U
#define BLE_LINK_TIMER_DEADLINE_CONN_ANY UINT16_MAX

/**
 * @brief Identity-qualified update for one link deadline slot.
 */
typedef struct ble_link_timer_deadline_command
{
    bool armed;
    unsigned int kind;
    uint32_t revision;
    ble_link_operation_identity_t identity;
    uint64_t deadline_us;
} ble_link_timer_deadline_command_t;

/**
 * @brief Immutable identity captured when one deadline expires.
 */
typedef struct ble_link_timer_deadline_expiry
{
    unsigned int kind;
    ble_link_operation_identity_t identity;
} ble_link_timer_deadline_expiry_t;

/**
 * @brief State for one owner-managed absolute deadline.
 */
typedef struct ble_link_timer_deadline_slot
{
    bool armed;
    uint32_t revision;
    ble_link_operation_identity_t identity;
    uint64_t deadline_us;
} ble_link_timer_deadline_slot_t;

/**
 * @brief Fixed deadline state owned by the Device Link timer worker.
 */
typedef struct ble_link_timer_deadline_state
{
    ble_link_timer_deadline_slot_t slots[
        BLE_LINK_TIMER_DEADLINE_SLOT_COUNT];
} ble_link_timer_deadline_state_t;

/**
 * @brief Retained ACL termination obligation owned by the timer worker.
 */
typedef struct ble_link_timer_terminate_state
{
    bool pending;
    bool submitted;
    ble_link_operation_identity_t identity;
    uint64_t retry_not_before_us;
} ble_link_timer_terminate_state_t;

/** @brief Retained termination for an ACL rejected before generation issue. */
typedef struct ble_link_rejected_terminate_state
{
    bool pending;
    bool submitted;
    uint32_t admission_token;
    uint16_t conn_handle;
    uint64_t retry_not_before_us;
} ble_link_rejected_terminate_state_t;

/**
 * @brief Clear every deadline slot.
 *
 * @param[out] state State to reset.
 */
void ble_link_timer_deadline_reset(
    ble_link_timer_deadline_state_t *state);

/**
 * @brief Apply an identity-qualified arm or disarm update.
 *
 * Revisions must increase. A nonzero generation or token on a disarm must
 * match the active slot; stale completion and disconnect events are no-ops.
 * A zero generation/token plus `BLE_LINK_TIMER_DEADLINE_CONN_ANY` forms the
 * explicit owner-shutdown wildcard.
 *
 * @param[in,out] state Deadline state.
 * @param[in] command Update to apply.
 * @return true when the update was applied or was already converged.
 */
bool ble_link_timer_deadline_apply(
    ble_link_timer_deadline_state_t *state,
    const ble_link_timer_deadline_command_t *command);

/**
 * @brief Collect and retire all deadlines at or before @p now_us.
 *
 * @param[in,out] state Deadline state.
 * @param[in] now_us Current monotonic time in microseconds.
 * @param[out] expiries Fixed output array with one entry per slot.
 * @return Number of collected expiries.
 */
size_t ble_link_timer_deadline_collect(
    ble_link_timer_deadline_state_t *state, uint64_t now_us,
    ble_link_timer_deadline_expiry_t expiries[
        BLE_LINK_TIMER_DEADLINE_SLOT_COUNT]);

/**
 * @brief Retire one armed slot after its wake timer cannot be maintained.
 *
 * @param[in,out] state Deadline state.
 * @param[in] kind Slot index.
 * @param[out] expiry Captured identity when the slot was armed.
 * @return true when an armed obligation was retired.
 */
bool ble_link_timer_deadline_retire(
    ble_link_timer_deadline_state_t *state, unsigned int kind,
    ble_link_timer_deadline_expiry_t *expiry);

/**
 * @brief Get the remaining time to the nearest armed deadline.
 *
 * @param[in] state Deadline state.
 * @param[in] now_us Current monotonic time in microseconds.
 * @return Remaining microseconds, zero when due, or UINT64_MAX when idle.
 */
uint64_t ble_link_timer_deadline_remaining_us(
    const ble_link_timer_deadline_state_t *state, uint64_t now_us);

/**
 * @brief Read one slot for owner-side timer-handle reconciliation.
 *
 * @param[in] state Deadline state.
 * @param[in] kind Slot index.
 * @return Slot pointer, or NULL for invalid input.
 */
const ble_link_timer_deadline_slot_t *ble_link_timer_deadline_get_slot(
    const ble_link_timer_deadline_state_t *state, unsigned int kind);

/** @brief Reset a retained ACL termination obligation. */
void ble_link_timer_terminate_reset(
    ble_link_timer_terminate_state_t *state);

/** @brief Retain or replace an ACL termination obligation. */
bool ble_link_timer_terminate_request(
    ble_link_timer_terminate_state_t *state,
    const ble_link_operation_identity_t *identity, uint64_t now_us);

/** @brief Copy the obligation when its retry deadline is due. */
bool ble_link_timer_terminate_due(
    const ble_link_timer_terminate_state_t *state, uint64_t now_us,
    ble_link_timer_terminate_state_t *obligation);

/** @brief Mark an exact HCI terminate as submitted, awaiting DISCONNECT. */
void ble_link_timer_terminate_submitted(
    ble_link_timer_terminate_state_t *state,
    const ble_link_timer_terminate_state_t *obligation);

/** @brief Retire one generation/handle ACL on terminal DISCONNECT/RESET. */
bool ble_link_timer_terminate_retire(
    ble_link_timer_terminate_state_t *state,
    const ble_link_operation_identity_t *identity);

/** @brief Record the identity-qualified result of one termination attempt. */
void ble_link_timer_terminate_finish(
    ble_link_timer_terminate_state_t *state,
    const ble_link_timer_terminate_state_t *obligation,
    bool complete, uint64_t retry_not_before_us);

/** @brief Get the remaining time to a retained termination retry. */
uint64_t ble_link_timer_terminate_remaining_us(
    const ble_link_timer_terminate_state_t *state, uint64_t now_us);

/** @brief Reset a rejected-ACL termination obligation. */
void ble_link_rejected_terminate_reset(
    ble_link_rejected_terminate_state_t *state);

/** @brief Retain one pre-generation rejected ACL identity. */
bool ble_link_rejected_terminate_request(
    ble_link_rejected_terminate_state_t *state, uint32_t admission_token,
    uint16_t conn_handle, uint64_t now_us);

/** @brief Copy a rejected termination whose retry deadline is due. */
bool ble_link_rejected_terminate_due(
    const ble_link_rejected_terminate_state_t *state, uint64_t now_us,
    ble_link_rejected_terminate_state_t *obligation);

/** @brief Mark a rejected terminate submitted, pending terminal callback. */
void ble_link_rejected_terminate_submitted(
    ble_link_rejected_terminate_state_t *state,
    const ble_link_rejected_terminate_state_t *obligation);

/** @brief Complete or back off one exact rejected termination attempt. */
void ble_link_rejected_terminate_finish(
    ble_link_rejected_terminate_state_t *state,
    const ble_link_rejected_terminate_state_t *obligation,
    bool complete, uint64_t retry_not_before_us);

/** @brief Retire one exact rejected ACL after its terminal callback. */
bool ble_link_rejected_terminate_retire(
    ble_link_rejected_terminate_state_t *state, uint32_t admission_token,
    uint16_t conn_handle);

/** @brief Remaining time before a rejected terminate retry is eligible. */
uint64_t ble_link_rejected_terminate_remaining_us(
    const ble_link_rejected_terminate_state_t *state, uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
