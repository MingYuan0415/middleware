#include "ble_link_cleanup_obligation.h"

#include <string.h>

static bool _ble_link_cleanup_identity_valid(
    const ble_link_operation_identity_t *identity)
{
    return identity != NULL && identity->generation != 0U &&
           identity->token != 0U &&
           (identity->kind == BLE_LINK_OPERATION_PROVISIONAL_DISCARD ||
            identity->kind == BLE_LINK_OPERATION_PEER_CLEANUP) &&
           identity->conn_handle != UINT16_MAX;
}

static ble_link_cleanup_slot_t *_ble_link_cleanup_slot(
    ble_link_cleanup_state_t *state, size_t index)
{
    if (index < BLE_LINK_CLEANUP_OBLIGATION_CAPACITY)
    {
        return &state->slots[index];
    }
    return index == BLE_LINK_CLEANUP_OBLIGATION_CAPACITY ?
           &state->overflow : NULL;
}

static const ble_link_cleanup_slot_t *_ble_link_cleanup_const_slot(
    const ble_link_cleanup_state_t *state, size_t index)
{
    if (index < BLE_LINK_CLEANUP_OBLIGATION_CAPACITY)
    {
        return &state->slots[index];
    }
    return index == BLE_LINK_CLEANUP_OBLIGATION_CAPACITY ?
           &state->overflow : NULL;
}

static bool _ble_link_cleanup_same_physical_target(
    const ble_link_cleanup_request_t *left,
    const ble_link_cleanup_request_t *right)
{
    if (left == NULL || right == NULL ||
            left->provisional != right->provisional)
    {
        return false;
    }
    const bool same_acl =
        left->identity.generation == right->identity.generation &&
        left->identity.conn_handle == right->identity.conn_handle;

    if (left->provisional && !same_acl)
    {
        /* Promotion is transaction-scoped, so provisional work from
         * different ACL generations must retain distinct identities. */
        return false;
    }
    if (left->peer_addr_valid && right->peer_addr_valid)
    {
        return left->peer_addr_type == right->peer_addr_type &&
               memcmp(left->peer_addr, right->peer_addr,
                      sizeof(left->peer_addr)) == 0;
    }
    if (same_acl)
    {
        return true;
    }
    /* With MAX_BONDS=1, unresolved non-provisional delete-all requests
     * address the same physical global store even across retired ACLs. */
    return !left->provisional && !left->peer_addr_valid &&
           !right->peer_addr_valid &&
           left->delete_all_if_unresolved &&
           right->delete_all_if_unresolved;
}

static void _ble_link_cleanup_merge_request(
    ble_link_cleanup_request_t *retained,
    const ble_link_cleanup_request_t *request)
{
    retained->terminate_conn = retained->terminate_conn ||
                               request->terminate_conn;
    retained->invalidate_authorization =
        retained->invalidate_authorization ||
        request->invalidate_authorization;
    retained->delete_all_if_unresolved =
        retained->delete_all_if_unresolved ||
        request->delete_all_if_unresolved;
    if (!retained->peer_addr_valid && request->peer_addr_valid)
    {
        retained->peer_addr_type = request->peer_addr_type;
        memcpy(retained->peer_addr, request->peer_addr,
               sizeof(retained->peer_addr));
        retained->peer_addr_valid = true;
    }
}

static bool _ble_link_cleanup_request_equal(
    const ble_link_cleanup_request_t *left,
    const ble_link_cleanup_request_t *right)
{
    if (left == NULL || right == NULL ||
            !ble_link_operation_identity_equal(
                &left->identity, &right->identity) ||
            left->peer_addr_valid != right->peer_addr_valid ||
            left->delete_all_if_unresolved !=
            right->delete_all_if_unresolved ||
            left->provisional != right->provisional ||
            left->terminate_conn != right->terminate_conn ||
            left->invalidate_authorization !=
            right->invalidate_authorization)
    {
        return false;
    }
    return !left->peer_addr_valid ||
           (left->peer_addr_type == right->peer_addr_type &&
            memcmp(left->peer_addr, right->peer_addr,
                   sizeof(left->peer_addr)) == 0);
}

void ble_link_cleanup_reset(ble_link_cleanup_state_t *state)
{
    if (state != NULL)
    {
        memset(state, 0, sizeof(*state));
    }
}

bool ble_link_cleanup_retain(
    ble_link_cleanup_state_t *state,
    const ble_link_cleanup_request_t *request, uint64_t now_us)
{
    if (state == NULL || request == NULL ||
            !_ble_link_cleanup_identity_valid(&request->identity))
    {
        return false;
    }
    ble_link_cleanup_slot_t *empty = NULL;

    for (size_t i = 0U; i <= BLE_LINK_CLEANUP_OBLIGATION_CAPACITY; ++i)
    {
        ble_link_cleanup_slot_t *const slot =
            _ble_link_cleanup_slot(state, i);

        if (slot->active &&
                (ble_link_operation_identity_equal(
                     &slot->request.identity, &request->identity) ||
                 _ble_link_cleanup_same_physical_target(
                     &slot->request, request)))
        {
            _ble_link_cleanup_merge_request(&slot->request, request);
            return true;
        }
        if (!slot->active && empty == NULL)
        {
            empty = slot;
        }
    }
    if (empty == NULL)
    {
        return false;
    }
    *empty = (ble_link_cleanup_slot_t)
    {
        .active = true,
        .request = *request,
        .retry_not_before_us = now_us,
    };
    return true;
}

bool ble_link_cleanup_take_due(
    ble_link_cleanup_state_t *state, uint64_t now_us,
    ble_link_cleanup_request_t *request)
{
    if (state == NULL || request == NULL)
    {
        return false;
    }
    for (size_t i = 0U; i <= BLE_LINK_CLEANUP_OBLIGATION_CAPACITY; ++i)
    {
        ble_link_cleanup_slot_t *const slot =
            _ble_link_cleanup_slot(state, i);

        if (slot->active && !slot->in_progress &&
                slot->retry_not_before_us <= now_us)
        {
            slot->in_progress = true;
            *request = slot->request;
            return true;
        }
    }
    return false;
}

void ble_link_cleanup_finish(
    ble_link_cleanup_state_t *state,
    const ble_link_cleanup_request_t *request, bool complete,
    uint64_t retry_not_before_us)
{
    if (state == NULL || request == NULL)
    {
        return;
    }
    for (size_t i = 0U; i <= BLE_LINK_CLEANUP_OBLIGATION_CAPACITY; ++i)
    {
        ble_link_cleanup_slot_t *const slot =
            _ble_link_cleanup_slot(state, i);

        if (!slot->active || !slot->in_progress ||
                !ble_link_operation_identity_equal(
                    &slot->request.identity, &request->identity))
        {
            continue;
        }
        if (complete)
        {
            if (_ble_link_cleanup_request_equal(
                        &slot->request, request))
            {
                memset(slot, 0, sizeof(*slot));
            }
            else
            {
                /* A producer strengthened the physical cleanup while the
                 * owner executed its older snapshot. Preserve the merged
                 * payload for an immediate follow-up instead of treating the
                 * older success as completion of work it never performed. */
                slot->in_progress = false;
                slot->retry_not_before_us = 0U;
            }
        }
        else
        {
            slot->in_progress = false;
            slot->retry_not_before_us = retry_not_before_us;
        }
        return;
    }
}

bool ble_link_cleanup_terminal_fence_retain(
    ble_link_cleanup_state_t *state,
    uint32_t generation, uint16_t conn_handle)
{
    if (state == NULL || generation == 0U || conn_handle == UINT16_MAX)
    {
        return false;
    }
    if (state->terminal_fence_active)
    {
        return state->terminal_fence_generation == generation &&
               state->terminal_fence_conn_handle == conn_handle;
    }
    state->terminal_fence_active = true;
    state->terminal_fence_generation = generation;
    state->terminal_fence_conn_handle = conn_handle;
    return true;
}

bool ble_link_cleanup_terminal_fence_matches(
    const ble_link_cleanup_state_t *state,
    uint32_t generation, uint16_t conn_handle)
{
    return state != NULL && state->terminal_fence_active &&
           generation != 0U && conn_handle != UINT16_MAX &&
           state->terminal_fence_generation == generation &&
           state->terminal_fence_conn_handle == conn_handle;
}

bool ble_link_cleanup_terminal_fence_release(
    ble_link_cleanup_state_t *state,
    uint32_t generation, uint16_t conn_handle)
{
    if (!ble_link_cleanup_terminal_fence_matches(
                state, generation, conn_handle))
    {
        return false;
    }
    state->terminal_fence_active = false;
    state->terminal_fence_generation = 0U;
    state->terminal_fence_conn_handle = 0U;
    return true;
}

bool ble_link_cleanup_pending(const ble_link_cleanup_state_t *state)
{
    if (state == NULL)
    {
        return false;
    }
    for (size_t i = 0U; i <= BLE_LINK_CLEANUP_OBLIGATION_CAPACITY; ++i)
    {
        const ble_link_cleanup_slot_t *const slot =
            _ble_link_cleanup_const_slot(state, i);

        if (slot->active)
        {
            return true;
        }
    }
    return false;
}

bool ble_link_cleanup_pending_for_acl(
    const ble_link_cleanup_state_t *state,
    uint32_t generation, uint16_t conn_handle)
{
    if (state == NULL || generation == 0U || conn_handle == UINT16_MAX)
    {
        return false;
    }
    for (size_t i = 0U; i <= BLE_LINK_CLEANUP_OBLIGATION_CAPACITY; ++i)
    {
        const ble_link_cleanup_slot_t *const slot =
            _ble_link_cleanup_const_slot(state, i);

        if (slot->active &&
                slot->request.identity.generation == generation &&
                slot->request.identity.conn_handle == conn_handle)
        {
            return true;
        }
    }
    return false;
}

bool ble_link_cleanup_provisional_pending_for_acl(
    const ble_link_cleanup_state_t *state,
    uint32_t generation, uint16_t conn_handle)
{
    if (state == NULL || generation == 0U || conn_handle == UINT16_MAX)
    {
        return false;
    }
    for (size_t i = 0U; i <= BLE_LINK_CLEANUP_OBLIGATION_CAPACITY; ++i)
    {
        const ble_link_cleanup_slot_t *const slot =
            _ble_link_cleanup_const_slot(state, i);

        if (slot->active && slot->request.provisional &&
                slot->request.identity.generation == generation &&
                slot->request.identity.conn_handle == conn_handle)
        {
            return true;
        }
    }
    return false;
}

bool ble_link_cleanup_admission_allowed(
    const ble_link_cleanup_state_t *state)
{
    return state != NULL && !ble_link_cleanup_pending(state) &&
           !state->terminal_fence_active;
}

uint64_t ble_link_cleanup_remaining_us(
    const ble_link_cleanup_state_t *state, uint64_t now_us)
{
    if (state == NULL)
    {
        return UINT64_MAX;
    }
    uint64_t remaining = UINT64_MAX;

    for (size_t i = 0U; i <= BLE_LINK_CLEANUP_OBLIGATION_CAPACITY; ++i)
    {
        const ble_link_cleanup_slot_t *const slot =
            _ble_link_cleanup_const_slot(state, i);

        if (!slot->active || slot->in_progress)
        {
            continue;
        }
        if (slot->retry_not_before_us <= now_us)
        {
            return 0U;
        }
        const uint64_t slot_remaining =
            slot->retry_not_before_us - now_us;

        if (slot_remaining < remaining)
        {
            remaining = slot_remaining;
        }
    }
    return remaining;
}
