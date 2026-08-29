#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ble_nimble_port_task_config.h"
#include "ble_link_timer_deadline.h"

#define TEST_ASSERT_TRUE(value) assert(value)
#define TEST_ASSERT_FALSE(value) assert(!(value))
#define TEST_ASSERT_EQUAL(expected, actual) assert((expected) == (actual))

static ble_link_timer_deadline_command_t _arm(
    unsigned int kind, uint32_t revision, uint32_t generation,
    uint32_t token, uint64_t deadline_us)
{
    return (ble_link_timer_deadline_command_t)
    {
        .armed = true,
        .kind = kind,
        .revision = revision,
        .identity =
        {
            .generation = generation,
            .security_epoch = generation + 100U,
            .flow_id = token + 100U,
            .token = token,
            .kind = BLE_LINK_OPERATION_TX_INDICATE,
            .conn_handle = 6U,
        },
        .deadline_us = deadline_us,
    };
}

static ble_link_timer_deadline_command_t _disarm(
    unsigned int kind, uint32_t revision, uint32_t generation,
    uint32_t token)
{
    return (ble_link_timer_deadline_command_t)
    {
        .armed = false,
        .kind = kind,
        .revision = revision,
        .identity =
        {
            .generation = generation,
            .security_epoch = generation + 100U,
            .flow_id = token + 100U,
            .token = token,
            .kind = BLE_LINK_OPERATION_TX_INDICATE,
            .conn_handle = 6U,
        },
    };
}

static void test_reset_is_idle(void)
{
    ble_link_timer_deadline_state_t state;

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_EQUAL(UINT64_MAX,
                      ble_link_timer_deadline_remaining_us(&state, 10U));
}

static void test_owner_stack_budget_covers_security_cleanup(void)
{
    /* The production overflow occurred on the timeout -> session abort ->
     * provisional cleanup path. Keep the audited floor tied to the value used
     * by xTaskCreatePinnedToCore(). */
    TEST_ASSERT_TRUE(BLE_NIMBLE_PORT_LINK_TIMER_STACK_BYTES >= 4096U);
}

static void test_absolute_deadline_expires_without_wake(void)
{
    ble_link_timer_deadline_state_t state;
    ble_link_timer_deadline_expiry_t expiries[
        BLE_LINK_TIMER_DEADLINE_SLOT_COUNT];
    const ble_link_timer_deadline_command_t command =
        _arm(0U, 1U, 11U, 21U, 5000U);

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &command));
    TEST_ASSERT_EQUAL(1000U,
                      ble_link_timer_deadline_remaining_us(&state, 4000U));
    TEST_ASSERT_EQUAL(0U, ble_link_timer_deadline_collect(
                          &state, 4999U, expiries));
    TEST_ASSERT_EQUAL(1U, ble_link_timer_deadline_collect(
                          &state, 5000U, expiries));
    TEST_ASSERT_EQUAL(0U, expiries[0].kind);
    TEST_ASSERT_EQUAL(11U, expiries[0].identity.generation);
    TEST_ASSERT_EQUAL(21U, expiries[0].identity.token);
    TEST_ASSERT_EQUAL(6U, expiries[0].identity.conn_handle);
    TEST_ASSERT_EQUAL(0U, ble_link_timer_deadline_collect(
                          &state, 6000U, expiries));
}

static void test_stale_wake_observes_latest_rearm(void)
{
    ble_link_timer_deadline_state_t state;
    ble_link_timer_deadline_expiry_t expiries[
        BLE_LINK_TIMER_DEADLINE_SLOT_COUNT];
    const ble_link_timer_deadline_command_t first =
        _arm(0U, 1U, 11U, 21U, 5000U);
    const ble_link_timer_deadline_command_t rearmed =
        _arm(0U, 2U, 11U, 22U, 8000U);

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &first));
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &rearmed));
    /* The old esp_timer callback may still wake at 5000 us. It cannot
     * expire the current revision before its retained deadline. */
    TEST_ASSERT_EQUAL(0U, ble_link_timer_deadline_collect(
                          &state, 5000U, expiries));
    TEST_ASSERT_EQUAL(3000U,
                      ble_link_timer_deadline_remaining_us(&state, 5000U));
}

static void test_one_sweep_collects_each_due_slot_once(void)
{
    ble_link_timer_deadline_state_t state;
    ble_link_timer_deadline_expiry_t expiries[
        BLE_LINK_TIMER_DEADLINE_SLOT_COUNT];
    const ble_link_timer_deadline_command_t session =
        _arm(0U, 1U, 3U, 4U, 5000U);
    const ble_link_timer_deadline_command_t indication =
        _arm(2U, 1U, 3U, 5U, 5000U);

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &session));
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &indication));
    TEST_ASSERT_EQUAL(2U, ble_link_timer_deadline_collect(
                          &state, 5000U, expiries));
    TEST_ASSERT_EQUAL(0U, ble_link_timer_deadline_collect(
                          &state, 5000U, expiries));
}

static void test_nearest_deadline_bounds_wait(void)
{
    ble_link_timer_deadline_state_t state;
    const ble_link_timer_deadline_command_t later =
        _arm(0U, 1U, 1U, 1U, 9000U);
    const ble_link_timer_deadline_command_t earlier =
        _arm(2U, 1U, 1U, 2U, 6000U);

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &later));
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &earlier));
    TEST_ASSERT_EQUAL(1000U,
                      ble_link_timer_deadline_remaining_us(&state, 5000U));
}

static void test_queue_full_does_not_remove_retained_deadline(void)
{
    ble_link_timer_deadline_state_t state;
    ble_link_timer_deadline_expiry_t expiries[
        BLE_LINK_TIMER_DEADLINE_SLOT_COUNT];
    const ble_link_timer_deadline_command_t retained =
        _arm(1U, 1U, 7U, 8U, 3000U);

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &retained));
    /* A full transport queue drops only its hint. The owner state remains
     * armed and its bounded wait still reaches the absolute deadline. */
    TEST_ASSERT_EQUAL(500U,
                      ble_link_timer_deadline_remaining_us(&state, 2500U));
    TEST_ASSERT_EQUAL(1U, ble_link_timer_deadline_collect(
                          &state, 3000U, expiries));
    TEST_ASSERT_EQUAL(7U, expiries[0].identity.generation);
    TEST_ASSERT_EQUAL(8U, expiries[0].identity.token);
}

static void test_stale_revision_is_noop(void)
{
    ble_link_timer_deadline_state_t state;
    const ble_link_timer_deadline_command_t current =
        _arm(0U, 2U, 2U, 3U, 8000U);
    const ble_link_timer_deadline_command_t stale =
        _arm(0U, 1U, 1U, 2U, 4000U);

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &current));
    TEST_ASSERT_FALSE(ble_link_timer_deadline_apply(&state, &stale));
    TEST_ASSERT_EQUAL(3000U,
                      ble_link_timer_deadline_remaining_us(&state, 5000U));
}

static void test_stale_generation_disarm_is_noop(void)
{
    ble_link_timer_deadline_state_t state;
    const ble_link_timer_deadline_command_t current =
        _arm(0U, 1U, 9U, 10U, 8000U);
    const ble_link_timer_deadline_command_t stale =
        _disarm(0U, 2U, 8U, 10U);

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &current));
    TEST_ASSERT_FALSE(ble_link_timer_deadline_apply(&state, &stale));
    TEST_ASSERT_EQUAL(3000U,
                      ble_link_timer_deadline_remaining_us(&state, 5000U));
}

static void test_stale_token_disarm_is_noop(void)
{
    ble_link_timer_deadline_state_t state;
    const ble_link_timer_deadline_command_t current =
        _arm(2U, 1U, 9U, 11U, 8000U);
    const ble_link_timer_deadline_command_t stale =
        _disarm(2U, 2U, 9U, 10U);

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &current));
    TEST_ASSERT_FALSE(ble_link_timer_deadline_apply(&state, &stale));
    TEST_ASSERT_EQUAL(3000U,
                      ble_link_timer_deadline_remaining_us(&state, 5000U));
}

static void test_stale_connection_disarm_is_noop(void)
{
    ble_link_timer_deadline_state_t state;
    const ble_link_timer_deadline_command_t current =
        _arm(2U, 1U, 9U, 11U, 8000U);
    ble_link_timer_deadline_command_t stale =
        _disarm(2U, 2U, 9U, 11U);

    stale.identity.conn_handle = 7U;
    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &current));
    TEST_ASSERT_FALSE(ble_link_timer_deadline_apply(&state, &stale));
    TEST_ASSERT_EQUAL(3000U,
                      ble_link_timer_deadline_remaining_us(&state, 5000U));
}

static void test_exact_disarm_retires_deadline(void)
{
    ble_link_timer_deadline_state_t state;
    const ble_link_timer_deadline_command_t current =
        _arm(2U, 1U, 9U, 11U, 8000U);
    const ble_link_timer_deadline_command_t exact =
        _disarm(2U, 2U, 9U, 11U);

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &current));
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &exact));
    TEST_ASSERT_EQUAL(UINT64_MAX,
                      ble_link_timer_deadline_remaining_us(&state, 5000U));
}

static void test_shutdown_wildcard_retires_deadline(void)
{
    ble_link_timer_deadline_state_t state;
    const ble_link_timer_deadline_command_t current =
        _arm(1U, 1U, 9U, 11U, 8000U);
    ble_link_timer_deadline_command_t wildcard =
        _disarm(1U, 2U, 0U, 0U);

    memset(&wildcard.identity, 0, sizeof(wildcard.identity));
    wildcard.identity.conn_handle = BLE_LINK_TIMER_DEADLINE_CONN_ANY;

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &current));
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &wildcard));
    TEST_ASSERT_EQUAL(UINT64_MAX,
                      ble_link_timer_deadline_remaining_us(&state, 5000U));
}

static void test_invalid_identity_is_rejected(void)
{
    ble_link_timer_deadline_state_t state;
    const ble_link_timer_deadline_command_t zero_generation =
        _arm(0U, 1U, 0U, 1U, 1000U);
    const ble_link_timer_deadline_command_t zero_token =
        _arm(0U, 1U, 1U, 0U, 1000U);
    ble_link_timer_deadline_command_t zero_connection =
        _arm(0U, 1U, 1U, 1U, 1000U);

    zero_connection.identity.conn_handle = BLE_LINK_TIMER_DEADLINE_CONN_ANY;
    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_FALSE(ble_link_timer_deadline_apply(
                          &state, &zero_generation));
    TEST_ASSERT_FALSE(ble_link_timer_deadline_apply(&state, &zero_token));
    TEST_ASSERT_FALSE(ble_link_timer_deadline_apply(
                          &state, &zero_connection));
    TEST_ASSERT_FALSE(ble_link_timer_deadline_apply(NULL, &zero_token));
    TEST_ASSERT_EQUAL(0U, ble_link_timer_deadline_collect(
                          NULL, 0U, NULL));
    TEST_ASSERT_TRUE(ble_link_timer_deadline_get_slot(&state, 3U) == NULL);
}

static void test_rebuild_failure_retires_once(void)
{
    ble_link_timer_deadline_state_t state;
    ble_link_timer_deadline_expiry_t expiry;
    const ble_link_timer_deadline_command_t command =
        _arm(2U, 1U, 4U, 5U, 9000U);

    ble_link_timer_deadline_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_deadline_apply(&state, &command));
    TEST_ASSERT_TRUE(ble_link_timer_deadline_retire(&state, 2U, &expiry));
    TEST_ASSERT_EQUAL(4U, expiry.identity.generation);
    TEST_ASSERT_EQUAL(5U, expiry.identity.token);
    TEST_ASSERT_FALSE(ble_link_timer_deadline_retire(&state, 2U, &expiry));
}

static void test_terminate_obligation_retries_until_complete(void)
{
    ble_link_timer_terminate_state_t state;
    ble_link_timer_terminate_state_t obligation;
    const ble_link_operation_identity_t identity =
    {
        .generation = 7U,
        .security_epoch = 2U,
        .flow_id = 4U,
        .token = 5U,
        .kind = BLE_LINK_OPERATION_TERMINATE,
        .conn_handle = 3U,
    };

    ble_link_timer_terminate_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_terminate_request(
                         &state, &identity, 1000U));
    TEST_ASSERT_TRUE(ble_link_timer_terminate_due(
                         &state, 1000U, &obligation));
    ble_link_timer_terminate_finish(
        &state, &obligation, false, 1100U);
    TEST_ASSERT_EQUAL(50U,
                      ble_link_timer_terminate_remaining_us(&state, 1050U));
    TEST_ASSERT_FALSE(ble_link_timer_terminate_due(
                          &state, 1099U, &obligation));
    TEST_ASSERT_TRUE(ble_link_timer_terminate_due(
                         &state, 1100U, &obligation));
    ble_link_timer_terminate_submitted(&state, &obligation);
    TEST_ASSERT_EQUAL(UINT64_MAX,
                      ble_link_timer_terminate_remaining_us(&state, 1200U));
    TEST_ASSERT_FALSE(ble_link_timer_terminate_due(
                          &state, 1200U, &obligation));
    const ble_link_operation_identity_t disconnect =
    {
        .generation = identity.generation,
        .security_epoch = identity.security_epoch + 1U,
        .flow_id = identity.flow_id + 1U,
        .token = identity.token + 1U,
        .kind = BLE_LINK_OPERATION_DISCONNECT,
        .conn_handle = identity.conn_handle,
    };

    /* A terminal GAP event has its own kind/token/epoch. Physical ACL
     * retirement is authoritative for the matching generation and handle. */
    TEST_ASSERT_TRUE(ble_link_timer_terminate_retire(
                         &state, &disconnect));
    TEST_ASSERT_FALSE(state.pending);
}

static void test_stale_terminate_result_does_not_clear_reused_handle(void)
{
    ble_link_timer_terminate_state_t state;
    ble_link_timer_terminate_state_t retired;
    ble_link_timer_terminate_state_t current;
    const ble_link_operation_identity_t retired_identity =
    {
        .generation = 7U,
        .kind = BLE_LINK_OPERATION_TERMINATE,
        .conn_handle = 3U,
    };
    ble_link_operation_identity_t current_identity = retired_identity;

    ble_link_timer_terminate_reset(&state);
    TEST_ASSERT_TRUE(ble_link_timer_terminate_request(
                         &state, &retired_identity, 1000U));
    TEST_ASSERT_TRUE(ble_link_timer_terminate_due(
                         &state, 1000U, &retired));
    /* The controller reused handle 3 for generation 8 before the old
     * termination call returned. Its result cannot clear the new duty. */
    current_identity.generation = 8U;
    TEST_ASSERT_TRUE(ble_link_timer_terminate_request(
                         &state, &current_identity, 1001U));
    ble_link_timer_terminate_finish(&state, &retired, true, 0U);
    TEST_ASSERT_TRUE(ble_link_timer_terminate_due(
                         &state, 1001U, &current));
    TEST_ASSERT_EQUAL(8U, current.identity.generation);
    TEST_ASSERT_EQUAL(3U, current.identity.conn_handle);
}

static void test_invalid_terminate_identity_is_rejected(void)
{
    ble_link_timer_terminate_state_t state;
    ble_link_operation_identity_t identity =
    {
        .kind = BLE_LINK_OPERATION_TERMINATE,
        .conn_handle = 3U,
    };

    ble_link_timer_terminate_reset(&state);
    TEST_ASSERT_FALSE(ble_link_timer_terminate_request(
                          &state, &identity, 0U));
    identity.generation = 1U;
    identity.conn_handle = BLE_LINK_TIMER_DEADLINE_CONN_ANY;
    TEST_ASSERT_FALSE(ble_link_timer_terminate_request(
                          &state, &identity, 0U));
}

static void test_rejected_terminate_retries_and_retires_exact_token(void)
{
    ble_link_rejected_terminate_state_t state;
    ble_link_rejected_terminate_state_t first;
    ble_link_rejected_terminate_state_t retry;

    ble_link_rejected_terminate_reset(&state);
    TEST_ASSERT_TRUE(ble_link_rejected_terminate_request(
                         &state, 11U, 3U, 1000U));
    TEST_ASSERT_TRUE(ble_link_rejected_terminate_due(
                         &state, 1000U, &first));
    ble_link_rejected_terminate_finish(
        &state, &first, false, 1100U);
    TEST_ASSERT_FALSE(ble_link_rejected_terminate_due(
                          &state, 1099U, &retry));
    TEST_ASSERT_TRUE(ble_link_rejected_terminate_due(
                         &state, 1100U, &retry));
    ble_link_rejected_terminate_submitted(&state, &retry);
    TEST_ASSERT_EQUAL(UINT64_MAX,
                      ble_link_rejected_terminate_remaining_us(
                          &state, 1200U));
    TEST_ASSERT_FALSE(ble_link_rejected_terminate_retire(
                          &state, 10U, 3U));
    TEST_ASSERT_TRUE(state.pending);
    TEST_ASSERT_TRUE(ble_link_rejected_terminate_retire(
                         &state, 11U, 3U));
    TEST_ASSERT_FALSE(state.pending);
}

static void test_rejected_stale_result_does_not_retire_reused_handle(void)
{
    ble_link_rejected_terminate_state_t state;
    ble_link_rejected_terminate_state_t old;

    ble_link_rejected_terminate_reset(&state);
    TEST_ASSERT_TRUE(ble_link_rejected_terminate_request(
                         &state, 21U, 7U, 1000U));
    TEST_ASSERT_TRUE(ble_link_rejected_terminate_due(
                         &state, 1000U, &old));
    TEST_ASSERT_TRUE(ble_link_rejected_terminate_retire(
                         &state, 21U, 7U));
    TEST_ASSERT_TRUE(ble_link_rejected_terminate_request(
                         &state, 22U, 7U, 1001U));
    ble_link_rejected_terminate_finish(&state, &old, true, 0U);
    TEST_ASSERT_TRUE(state.pending);
    TEST_ASSERT_EQUAL(22U, state.admission_token);
}

int main(void)
{
    test_reset_is_idle();
    test_owner_stack_budget_covers_security_cleanup();
    test_absolute_deadline_expires_without_wake();
    test_stale_wake_observes_latest_rearm();
    test_one_sweep_collects_each_due_slot_once();
    test_nearest_deadline_bounds_wait();
    test_queue_full_does_not_remove_retained_deadline();
    test_stale_revision_is_noop();
    test_stale_generation_disarm_is_noop();
    test_stale_token_disarm_is_noop();
    test_stale_connection_disarm_is_noop();
    test_exact_disarm_retires_deadline();
    test_shutdown_wildcard_retires_deadline();
    test_invalid_identity_is_rejected();
    test_rebuild_failure_retires_once();
    test_terminate_obligation_retries_until_complete();
    test_stale_terminate_result_does_not_clear_reused_handle();
    test_invalid_terminate_identity_is_rejected();
    test_rejected_terminate_retries_and_retires_exact_token();
    test_rejected_stale_result_does_not_retire_reused_handle();
    return 0;
}
