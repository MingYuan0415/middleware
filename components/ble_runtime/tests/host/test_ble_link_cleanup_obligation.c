#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ble_link_cleanup_obligation.h"

#define TEST_ASSERT_TRUE(value) assert(value)
#define TEST_ASSERT_FALSE(value) assert(!(value))
#define TEST_ASSERT_EQUAL(expected, actual) assert((expected) == (actual))

static ble_link_cleanup_request_t _request(
    uint32_t generation, uint32_t token, uint16_t conn_handle)
{
    ble_link_cleanup_request_t request;

    memset(&request, 0, sizeof(request));
    request.identity = (ble_link_operation_identity_t)
    {
        .generation = generation,
        .security_epoch = generation + 10U,
        .flow_id = token + 20U,
        .token = token,
        .kind = BLE_LINK_OPERATION_PROVISIONAL_DISCARD,
        .conn_handle = conn_handle,
    };
    request.peer_addr_type = 1U;
    request.peer_addr[0] = (uint8_t)generation;
    request.peer_addr_valid = true;
    request.provisional = true;
    return request;
}

static void test_retry_survives_missing_wake_hint(void)
{
    ble_link_cleanup_state_t state;
    ble_link_cleanup_request_t due;
    const ble_link_cleanup_request_t request = _request(1U, 2U, 3U);

    ble_link_cleanup_reset(&state);
    TEST_ASSERT_TRUE(ble_link_cleanup_admission_allowed(&state));
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(
                         &state, &request, 100U));
    TEST_ASSERT_FALSE(ble_link_cleanup_admission_allowed(&state));
    TEST_ASSERT_TRUE(ble_link_cleanup_pending_for_acl(
                         &state, 1U, 3U));
    /* No queue or wake state participates in the retained reducer. */
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 100U, &due));
    ble_link_cleanup_finish(&state, &due, false, 200U);
    TEST_ASSERT_EQUAL(50U, ble_link_cleanup_remaining_us(&state, 150U));
    TEST_ASSERT_FALSE(ble_link_cleanup_take_due(&state, 199U, &due));
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 200U, &due));
    ble_link_cleanup_finish(&state, &due, true, 0U);
    TEST_ASSERT_EQUAL(UINT64_MAX,
                      ble_link_cleanup_remaining_us(&state, 201U));
    TEST_ASSERT_TRUE(ble_link_cleanup_admission_allowed(&state));
}

static void test_new_acl_does_not_overwrite_old_cleanup(void)
{
    ble_link_cleanup_state_t state;
    ble_link_cleanup_request_t due;
    const ble_link_cleanup_request_t old_acl = _request(4U, 5U, 6U);
    const ble_link_cleanup_request_t new_acl = _request(7U, 8U, 6U);

    ble_link_cleanup_reset(&state);
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(
                         &state, &old_acl, 10U));
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(
                         &state, &new_acl, 11U));
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 11U, &due));
    TEST_ASSERT_EQUAL(old_acl.identity.generation,
                      due.identity.generation);
    TEST_ASSERT_EQUAL(old_acl.peer_addr[0], due.peer_addr[0]);
    ble_link_cleanup_finish(&state, &due, true, 0U);
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 11U, &due));
    TEST_ASSERT_EQUAL(new_acl.identity.generation,
                      due.identity.generation);
    TEST_ASSERT_EQUAL(new_acl.peer_addr[0], due.peer_addr[0]);
}

static void test_fifth_distinct_cleanup_uses_fail_closed_overflow(void)
{
    ble_link_cleanup_state_t state;
    ble_link_cleanup_request_t due;

    ble_link_cleanup_reset(&state);
    for (uint32_t i = 0U; i < BLE_LINK_CLEANUP_OBLIGATION_CAPACITY; ++i)
    {
        const ble_link_cleanup_request_t request =
            _request(i + 1U, i + 10U, (uint16_t)i);

        TEST_ASSERT_TRUE(ble_link_cleanup_retain(
                             &state, &request, i));
    }
    const ble_link_cleanup_request_t overflow = _request(20U, 30U, 40U);
    const ble_link_cleanup_request_t beyond_bound =
        _request(21U, 31U, 41U);

    TEST_ASSERT_TRUE(ble_link_cleanup_retain(
                         &state, &overflow, 0U));
    TEST_ASSERT_FALSE(ble_link_cleanup_retain(
                          &state, &beyond_bound, 0U));
    for (uint32_t i = 0U;
            i <= BLE_LINK_CLEANUP_OBLIGATION_CAPACITY; ++i)
    {
        TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 10U, &due));
        TEST_ASSERT_EQUAL(i < BLE_LINK_CLEANUP_OBLIGATION_CAPACITY ?
                          i + 1U : overflow.identity.generation,
                          due.identity.generation);
        ble_link_cleanup_finish(&state, &due, true, 0U);
    }
    TEST_ASSERT_FALSE(ble_link_cleanup_pending(&state));
}

static void test_repeated_provisional_teardown_coalesces(void)
{
    ble_link_cleanup_state_t state;
    ble_link_cleanup_request_t due;
    ble_link_cleanup_request_t first = _request(30U, 40U, 50U);
    ble_link_cleanup_request_t stronger = _request(30U, 41U, 50U);

    first.peer_addr_valid = false;
    first.delete_all_if_unresolved = true;
    stronger.terminate_conn = true;
    stronger.invalidate_authorization = true;
    TEST_ASSERT_TRUE(memcmp(first.peer_addr, stronger.peer_addr,
                            sizeof(first.peer_addr)) == 0);
    ble_link_cleanup_reset(&state);
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(&state, &first, 0U));
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(&state, &stronger, 1U));
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 1U, &due));
    TEST_ASSERT_EQUAL(first.identity.token, due.identity.token);
    TEST_ASSERT_TRUE(due.peer_addr_valid);
    TEST_ASSERT_TRUE(due.terminate_conn);
    TEST_ASSERT_TRUE(due.invalidate_authorization);
    ble_link_cleanup_finish(&state, &due, true, 0U);
    TEST_ASSERT_FALSE(ble_link_cleanup_take_due(&state, 1U, &due));
}

static void test_repeated_peer_cleanup_does_not_consume_slots(void)
{
    ble_link_cleanup_state_t state;
    ble_link_cleanup_request_t due;
    ble_link_cleanup_request_t first = _request(60U, 70U, 80U);
    ble_link_cleanup_request_t repeated = _request(60U, 71U, 80U);

    first.identity.kind = BLE_LINK_OPERATION_PEER_CLEANUP;
    first.provisional = false;
    repeated.identity.kind = BLE_LINK_OPERATION_PEER_CLEANUP;
    repeated.provisional = false;
    repeated.terminate_conn = true;
    repeated.invalidate_authorization = true;
    ble_link_cleanup_reset(&state);
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(&state, &first, 0U));
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(&state, &repeated, 1U));
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 1U, &due));
    TEST_ASSERT_EQUAL(first.identity.token, due.identity.token);
    TEST_ASSERT_TRUE(due.terminate_conn);
    TEST_ASSERT_TRUE(due.invalidate_authorization);
    ble_link_cleanup_finish(&state, &due, true, 0U);
    TEST_ASSERT_FALSE(ble_link_cleanup_take_due(&state, 1U, &due));
}

static void test_strengthened_in_progress_cleanup_runs_follow_up(void)
{
    ble_link_cleanup_state_t state;
    ble_link_cleanup_request_t executing;
    ble_link_cleanup_request_t stronger = _request(90U, 101U, 110U);
    ble_link_cleanup_request_t first = _request(90U, 100U, 110U);

    first.peer_addr_valid = false;
    first.delete_all_if_unresolved = true;
    ble_link_cleanup_reset(&state);
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(&state, &first, 0U));
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 0U, &executing));
    stronger.terminate_conn = true;
    stronger.invalidate_authorization = true;
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(&state, &stronger, 1U));
    ble_link_cleanup_finish(&state, &executing, true, 0U);
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 1U, &executing));
    TEST_ASSERT_EQUAL(first.identity.token, executing.identity.token);
    TEST_ASSERT_TRUE(executing.peer_addr_valid);
    TEST_ASSERT_EQUAL(stronger.peer_addr_type, executing.peer_addr_type);
    TEST_ASSERT_TRUE(memcmp(stronger.peer_addr, executing.peer_addr,
                            sizeof(stronger.peer_addr)) == 0);
    TEST_ASSERT_TRUE(executing.terminate_conn);
    TEST_ASSERT_TRUE(executing.invalidate_authorization);
    ble_link_cleanup_finish(&state, &executing, true, 0U);
    TEST_ASSERT_FALSE(ble_link_cleanup_pending(&state));
}

static void test_unresolved_delete_all_coalesces_global_target(void)
{
    ble_link_cleanup_state_t state;
    ble_link_cleanup_request_t due;
    ble_link_cleanup_request_t first = _request(200U, 210U, 10U);
    ble_link_cleanup_request_t repeated = _request(201U, 211U, 11U);

    first.identity.kind = BLE_LINK_OPERATION_PEER_CLEANUP;
    first.provisional = false;
    first.peer_addr_valid = false;
    first.delete_all_if_unresolved = true;
    repeated.identity.kind = BLE_LINK_OPERATION_PEER_CLEANUP;
    repeated.provisional = false;
    repeated.peer_addr_valid = false;
    repeated.delete_all_if_unresolved = true;
    repeated.invalidate_authorization = true;
    ble_link_cleanup_reset(&state);
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(&state, &first, 0U));
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(&state, &repeated, 1U));
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 1U, &due));
    TEST_ASSERT_EQUAL(first.identity.generation, due.identity.generation);
    TEST_ASSERT_TRUE(due.invalidate_authorization);
    ble_link_cleanup_finish(&state, &due, true, 0U);
    TEST_ASSERT_FALSE(ble_link_cleanup_pending(&state));
}

static void test_terminal_fence_survives_cleanup_and_handle_reuse(void)
{
    ble_link_cleanup_state_t state;
    ble_link_cleanup_request_t due;
    ble_link_cleanup_request_t request = _request(150U, 160U, 7U);

    ble_link_cleanup_reset(&state);
    request.terminate_conn = true;
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(&state, &request, 0U));
    TEST_ASSERT_TRUE(ble_link_cleanup_terminal_fence_retain(
                         &state, 150U, 7U));
    TEST_ASSERT_TRUE(ble_link_cleanup_terminal_fence_matches(
                         &state, 150U, 7U));
    TEST_ASSERT_FALSE(ble_link_cleanup_terminal_fence_matches(
                          &state, 151U, 7U));
    TEST_ASSERT_FALSE(ble_link_cleanup_admission_allowed(&state));
    TEST_ASSERT_FALSE(ble_link_cleanup_terminal_fence_release(
                          &state, 151U, 7U));
    TEST_ASSERT_TRUE(ble_link_cleanup_terminal_fence_matches(
                         &state, 150U, 7U));
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 0U, &due));
    ble_link_cleanup_finish(&state, &due, true, 0U);
    /* Deletion completion does not reopen admission before the matching
     * live ACL is retired. */
    TEST_ASSERT_FALSE(ble_link_cleanup_admission_allowed(&state));
    TEST_ASSERT_TRUE(ble_link_cleanup_terminal_fence_release(
                         &state, 150U, 7U));
    TEST_ASSERT_TRUE(ble_link_cleanup_admission_allowed(&state));
}

static void test_disconnected_cleanup_does_not_create_permanent_fence(void)
{
    ble_link_cleanup_state_t state;
    ble_link_cleanup_request_t due;
    ble_link_cleanup_request_t request = _request(170U, 180U, 9U);

    request.terminate_conn = true;
    ble_link_cleanup_reset(&state);
    /* The port only retains the terminal fence while this identity is the
     * current accepted ACL. A DISCONNECT-generated delete retains the payload
     * alone because the matching fence was already released. */
    TEST_ASSERT_TRUE(ble_link_cleanup_retain(&state, &request, 0U));
    TEST_ASSERT_FALSE(ble_link_cleanup_terminal_fence_matches(
                          &state, 170U, 9U));
    TEST_ASSERT_TRUE(ble_link_cleanup_take_due(&state, 0U, &due));
    ble_link_cleanup_finish(&state, &due, true, 0U);
    TEST_ASSERT_TRUE(ble_link_cleanup_admission_allowed(&state));
}

int main(void)
{
    test_retry_survives_missing_wake_hint();
    test_new_acl_does_not_overwrite_old_cleanup();
    test_fifth_distinct_cleanup_uses_fail_closed_overflow();
    test_repeated_provisional_teardown_coalesces();
    test_repeated_peer_cleanup_does_not_consume_slots();
    test_strengthened_in_progress_cleanup_runs_follow_up();
    test_unresolved_delete_all_coalesces_global_target();
    test_terminal_fence_survives_cleanup_and_handle_reuse();
    test_disconnected_cleanup_does_not_create_permanent_fence();
    return 0;
}
