#include "ble_link_timer_deadline.h"

#include <string.h>

void ble_link_timer_deadline_reset(
    ble_link_timer_deadline_state_t *state)
{
    if (state != NULL)
    {
        memset(state, 0, sizeof(*state));
    }
}

bool ble_link_timer_deadline_apply(
    ble_link_timer_deadline_state_t *state,
    const ble_link_timer_deadline_command_t *command)
{
    if (state == NULL || command == NULL ||
            command->kind >= BLE_LINK_TIMER_DEADLINE_SLOT_COUNT ||
            command->revision == 0U)
    {
        return false;
    }
    ble_link_timer_deadline_slot_t *const slot =
        &state->slots[command->kind];

    if (command->revision <= slot->revision)
    {
        return false;
    }
    if (command->armed)
    {
        if (command->identity.generation == 0U ||
                command->identity.token == 0U ||
                command->identity.kind == BLE_LINK_OPERATION_INVALID ||
                command->identity.conn_handle ==
                BLE_LINK_TIMER_DEADLINE_CONN_ANY ||
                command->deadline_us == 0U)
        {
            return false;
        }
    }
    else if (slot->armed &&
             ((command->identity.generation != 0U &&
               command->identity.generation != slot->identity.generation) ||
              (command->identity.security_epoch != 0U &&
               command->identity.security_epoch !=
               slot->identity.security_epoch) ||
              (command->identity.flow_id != 0U &&
               command->identity.flow_id != slot->identity.flow_id) ||
              (command->identity.token != 0U &&
               command->identity.token != slot->identity.token) ||
              (command->identity.kind != BLE_LINK_OPERATION_INVALID &&
               command->identity.kind != slot->identity.kind) ||
              (command->identity.conn_handle !=
               BLE_LINK_TIMER_DEADLINE_CONN_ANY &&
               command->identity.conn_handle !=
               slot->identity.conn_handle)))
    {
        return false;
    }
    *slot = (ble_link_timer_deadline_slot_t)
    {
        .armed = command->armed,
        .revision = command->revision,
        .identity = command->identity,
        .deadline_us = command->armed ? command->deadline_us : 0U,
    };
    return true;
}

size_t ble_link_timer_deadline_collect(
    ble_link_timer_deadline_state_t *state, uint64_t now_us,
    ble_link_timer_deadline_expiry_t expiries[
        BLE_LINK_TIMER_DEADLINE_SLOT_COUNT])
{
    if (state == NULL || expiries == NULL)
    {
        return 0U;
    }
    size_t count = 0U;

    for (unsigned int kind = 0U;
            kind < BLE_LINK_TIMER_DEADLINE_SLOT_COUNT; ++kind)
    {
        ble_link_timer_deadline_slot_t *const slot = &state->slots[kind];

        if (!slot->armed || slot->deadline_us > now_us)
        {
            continue;
        }
        expiries[count] = (ble_link_timer_deadline_expiry_t)
        {
            .kind = kind,
            .identity = slot->identity,
        };
        slot->armed = false;
        slot->deadline_us = 0U;
        ++count;
    }
    return count;
}

bool ble_link_timer_deadline_retire(
    ble_link_timer_deadline_state_t *state, unsigned int kind,
    ble_link_timer_deadline_expiry_t *expiry)
{
    if (state == NULL || expiry == NULL ||
            kind >= BLE_LINK_TIMER_DEADLINE_SLOT_COUNT ||
            !state->slots[kind].armed)
    {
        return false;
    }
    ble_link_timer_deadline_slot_t *const slot = &state->slots[kind];

    *expiry = (ble_link_timer_deadline_expiry_t)
    {
        .kind = kind,
        .identity = slot->identity,
    };
    slot->armed = false;
    slot->deadline_us = 0U;
    return true;
}

uint64_t ble_link_timer_deadline_remaining_us(
    const ble_link_timer_deadline_state_t *state, uint64_t now_us)
{
    if (state == NULL)
    {
        return UINT64_MAX;
    }
    uint64_t remaining_us = UINT64_MAX;

    for (unsigned int kind = 0U;
            kind < BLE_LINK_TIMER_DEADLINE_SLOT_COUNT; ++kind)
    {
        const ble_link_timer_deadline_slot_t *const slot =
            &state->slots[kind];

        if (!slot->armed)
        {
            continue;
        }
        if (slot->deadline_us <= now_us)
        {
            return 0U;
        }
        const uint64_t slot_remaining = slot->deadline_us - now_us;

        if (slot_remaining < remaining_us)
        {
            remaining_us = slot_remaining;
        }
    }
    return remaining_us;
}

const ble_link_timer_deadline_slot_t *ble_link_timer_deadline_get_slot(
    const ble_link_timer_deadline_state_t *state, unsigned int kind)
{
    if (state == NULL || kind >= BLE_LINK_TIMER_DEADLINE_SLOT_COUNT)
    {
        return NULL;
    }
    return &state->slots[kind];
}

void ble_link_timer_terminate_reset(
    ble_link_timer_terminate_state_t *state)
{
    if (state != NULL)
    {
        memset(state, 0, sizeof(*state));
    }
}

bool ble_link_timer_terminate_request(
    ble_link_timer_terminate_state_t *state,
    const ble_link_operation_identity_t *identity, uint64_t now_us)
{
    if (state == NULL || identity == NULL || identity->generation == 0U ||
            identity->kind != BLE_LINK_OPERATION_TERMINATE ||
            identity->conn_handle == BLE_LINK_TIMER_DEADLINE_CONN_ANY)
    {
        return false;
    }
    if (state->pending &&
            state->identity.generation == identity->generation &&
            state->identity.conn_handle == identity->conn_handle)
    {
        return true;
    }
    *state = (ble_link_timer_terminate_state_t)
    {
        .pending = true,
        .identity = *identity,
        .retry_not_before_us = now_us,
    };
    return true;
}

bool ble_link_timer_terminate_due(
    const ble_link_timer_terminate_state_t *state, uint64_t now_us,
    ble_link_timer_terminate_state_t *obligation)
{
    if (state == NULL || obligation == NULL || !state->pending ||
            state->submitted ||
            state->retry_not_before_us > now_us)
    {
        return false;
    }
    *obligation = *state;
    return true;
}

void ble_link_timer_terminate_submitted(
    ble_link_timer_terminate_state_t *state,
    const ble_link_timer_terminate_state_t *obligation)
{
    if (state != NULL && obligation != NULL && state->pending &&
            ble_link_operation_identity_equal(
                &state->identity, &obligation->identity))
    {
        state->submitted = true;
    }
}

bool ble_link_timer_terminate_retire(
    ble_link_timer_terminate_state_t *state,
    const ble_link_operation_identity_t *identity)
{
    if (state == NULL || identity == NULL || !state->pending ||
            state->identity.generation != identity->generation ||
            state->identity.conn_handle != identity->conn_handle)
    {
        return false;
    }
    ble_link_timer_terminate_reset(state);
    return true;
}

void ble_link_timer_terminate_finish(
    ble_link_timer_terminate_state_t *state,
    const ble_link_timer_terminate_state_t *obligation,
    bool complete, uint64_t retry_not_before_us)
{
    if (state == NULL || obligation == NULL || !state->pending ||
            !ble_link_operation_identity_equal(
                &state->identity, &obligation->identity))
    {
        return;
    }
    if (complete)
    {
        ble_link_timer_terminate_reset(state);
    }
    else
    {
        state->retry_not_before_us = retry_not_before_us;
    }
}

uint64_t ble_link_timer_terminate_remaining_us(
    const ble_link_timer_terminate_state_t *state, uint64_t now_us)
{
    if (state == NULL || !state->pending || state->submitted)
    {
        return UINT64_MAX;
    }
    return state->retry_not_before_us <= now_us ? 0U :
           state->retry_not_before_us - now_us;
}

void ble_link_rejected_terminate_reset(
    ble_link_rejected_terminate_state_t *state)
{
    if (state != NULL)
    {
        memset(state, 0, sizeof(*state));
    }
}

bool ble_link_rejected_terminate_request(
    ble_link_rejected_terminate_state_t *state, uint32_t admission_token,
    uint16_t conn_handle, uint64_t now_us)
{
    if (state == NULL || admission_token == 0U ||
            conn_handle == BLE_LINK_TIMER_DEADLINE_CONN_ANY)
    {
        return false;
    }
    if (state->pending)
    {
        return state->admission_token == admission_token &&
               state->conn_handle == conn_handle;
    }
    *state = (ble_link_rejected_terminate_state_t)
    {
        .pending = true,
        .admission_token = admission_token,
        .conn_handle = conn_handle,
        .retry_not_before_us = now_us,
    };
    return true;
}

bool ble_link_rejected_terminate_due(
    const ble_link_rejected_terminate_state_t *state, uint64_t now_us,
    ble_link_rejected_terminate_state_t *obligation)
{
    if (state == NULL || obligation == NULL || !state->pending ||
            state->submitted || state->retry_not_before_us > now_us)
    {
        return false;
    }
    *obligation = *state;
    return true;
}

void ble_link_rejected_terminate_submitted(
    ble_link_rejected_terminate_state_t *state,
    const ble_link_rejected_terminate_state_t *obligation)
{
    if (state != NULL && obligation != NULL && state->pending &&
            state->admission_token == obligation->admission_token &&
            state->conn_handle == obligation->conn_handle)
    {
        state->submitted = true;
    }
}

void ble_link_rejected_terminate_finish(
    ble_link_rejected_terminate_state_t *state,
    const ble_link_rejected_terminate_state_t *obligation,
    bool complete, uint64_t retry_not_before_us)
{
    if (state == NULL || obligation == NULL || !state->pending ||
            state->admission_token != obligation->admission_token ||
            state->conn_handle != obligation->conn_handle)
    {
        return;
    }
    if (complete)
    {
        ble_link_rejected_terminate_reset(state);
    }
    else
    {
        state->retry_not_before_us = retry_not_before_us;
    }
}

bool ble_link_rejected_terminate_retire(
    ble_link_rejected_terminate_state_t *state, uint32_t admission_token,
    uint16_t conn_handle)
{
    if (state == NULL || !state->pending || admission_token == 0U ||
            state->admission_token != admission_token ||
            state->conn_handle != conn_handle)
    {
        return false;
    }
    ble_link_rejected_terminate_reset(state);
    return true;
}

uint64_t ble_link_rejected_terminate_remaining_us(
    const ble_link_rejected_terminate_state_t *state, uint64_t now_us)
{
    if (state == NULL || !state->pending || state->submitted)
    {
        return UINT64_MAX;
    }
    return state->retry_not_before_us <= now_us ? 0U :
           state->retry_not_before_us - now_us;
}
