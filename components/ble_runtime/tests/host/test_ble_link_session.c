#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_session.h"

#define TEST_ASSERT_TRUE(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            fprintf(stderr, "assertion failed at line %d: %s\n", \
                    __LINE__, #condition); \
            abort(); \
        } \
    } while (0)

#define TEST_ASSERT_EQUAL(expected, actual) \
    do \
    { \
        const long expected_value = (long)(expected); \
        const long actual_value = (long)(actual); \
        if (expected_value != actual_value) \
        { \
            fprintf(stderr, \
                    "assertion failed at line %d: %s == %s (%ld != %ld)\n", \
                    __LINE__, #expected, #actual, expected_value, actual_value); \
            abort(); \
        } \
    } while (0)

#define GEN1 1U
#define GEN2 2U
#define BOOT1 72623859790382856ULL
#define BOOT2 47244640256ULL

static void _connect(uint32_t generation)
{
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          generation,
                          BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
}

static void _authenticate(uint32_t generation)
{
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          generation,
                          BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          generation,
                          BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2(
                          generation, true, 1U));
}

static void _authorize(uint32_t generation, uint32_t revision)
{
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, revision));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          generation, revision, 1U));
}

static uint32_t _query(uint32_t generation,
                       ble_link_session_channel_t channel)
{
    uint32_t error = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_query_admission(
                          generation, channel, &error));
    return error;
}

static void test_boot_init_and_reset(void)
{
    ble_link_session_init(BOOT1);
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_INACTIVE,
                      ble_link_session_get_state(GEN1));
    uint32_t error = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_query_admission(
                          GEN1, BLE_LINK_SESSION_CHANNEL_SESSION,
                          &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAVAILABLE, error);
    ble_link_session_reset();
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_INACTIVE,
                      ble_link_session_get_state(GEN1));
}

static void test_admission_progression(void)
{
    ble_link_session_init(BOOT1);
    /* No ACL: nothing available. */
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAVAILABLE,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_LINK_STATE));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAVAILABLE,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_SESSION));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAVAILABLE,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    _connect(GEN1);
    /* ACL up: link_state readable, session and control not yet. */
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_LINK_STATE));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_SESSION));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Encrypted + bond: session admitted, control still gated. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_SESSION));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Security 2 authenticated: control still needs authorization. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2(
                          GEN1, true, 1U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Authorization committed: control and event admitted. */
    _authorize(GEN1, 1U);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_EVENT));
    /* Revoked: control gated again, session stays. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          false, 2U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_SESSION));
}

static void test_security2_closed_keeps_acl(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 1U);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Security 2 session closed without disconnecting. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2(
                          GEN1, false, 2U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* The ACL, link_state, and encrypted+bond session channel remain. */
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_LINK_STATE));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_SESSION));
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_AUTHENTICATED,
                      ble_link_session_get_state(GEN1));
}

static void test_stale_authorization_revision_ignored(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 5U);
    /* Late revoke with an older revision has no effect. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          false, 3U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Revoke with a newer revision takes effect. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          false, 6U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Late commit with an older revision cannot resurrect it. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, 4U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

static void test_stale_generation_ignored(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN2);
    _authenticate(GEN2);
    /* Late events from a retired generation have no effect. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_session_security2(
                          GEN1, false, 2U));
    /* GEN2 admission is unchanged: session stays admitted. */
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN2, BLE_LINK_SESSION_CHANNEL_SESSION));
    /* The retired generation itself is not admitted. */
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAVAILABLE,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_SESSION));
    /* Disconnect of a retired generation is a no-op. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN2, BLE_LINK_SESSION_CHANNEL_SESSION));
}

static void test_late_connect_ignored(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    /* A late connect from a retired generation is ignored. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_INACTIVE,
                      ble_link_session_get_state(GEN1));
    /* A genuinely new generation connects. */
    _connect(GEN2);
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_CONNECTED,
                      ble_link_session_get_state(GEN2));
}

static void test_disconnect_clears_session(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 1U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1,
                          BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_INACTIVE,
                      ble_link_session_get_state(GEN1));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAVAILABLE,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* A fresh connect starts from scratch. */
    _connect(GEN2);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN2, BLE_LINK_SESSION_CHANNEL_SESSION));
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_CONNECTED,
                      ble_link_session_get_state(GEN2));
}

static void test_second_acl_rejected(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_session_handle_event(
                          GEN2,
                          BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_CONNECTED,
                      ble_link_session_get_state(GEN1));
}

static void test_authorize_before_authentication_rejected(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    /* The persistent record may be written without a session. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, 1U));
    /* But the session match requires an authenticated session. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_report_session_match(GEN1, 5U, 1U));
}

static void test_invalid_channel_rejected(void)
{
    uint32_t error = 0U;

    ble_link_session_init(BOOT1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_session_query_admission(
                          GEN1, (ble_link_session_channel_t)99U, &error));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_session_query_admission(
                          GEN1, BLE_LINK_SESSION_CHANNEL_SESSION, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_link_session_handle_event(
                          GEN1, (ble_link_session_event_t)99U));
}

static void test_facts_reflect_state(void)
{
    ble_link_dispatcher_facts_t facts;

    ble_link_session_init(BOOT1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_get_facts(GEN1, &facts));
    _connect(GEN1);
    _authenticate(GEN1);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_get_facts(GEN1, &facts));
    TEST_ASSERT_EQUAL(BOOT1, facts.active_boot_id);
    TEST_ASSERT_EQUAL(GEN1, facts.connection_generation);
    TEST_ASSERT_TRUE(facts.encrypted);
    TEST_ASSERT_TRUE(facts.session_authenticated);
    TEST_ASSERT_TRUE(!facts.authorized);
}

static void test_state_flags(void)
{
    ble_link_session_init(BOOT1);
    TEST_ASSERT_EQUAL(0U, ble_link_session_get_state_flags());
    ble_link_session_set_pairing_window(true);
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BINDABLE,
                      ble_link_session_get_state_flags());
    /* BOUND requires a committed authorization record, not a bond. */
    _connect(GEN1);
    _authenticate(GEN1);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BINDABLE,
                      ble_link_session_get_state_flags());
    _authorize(GEN1, 1U);
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BINDABLE |
                      BLE_LINK_STATE_FLAG_BOUND,
                      ble_link_session_get_state_flags());
    /* The record persists across disconnect. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1,
                          BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BINDABLE |
                      BLE_LINK_STATE_FLAG_BOUND,
                      ble_link_session_get_state_flags());
    ble_link_session_set_pairing_window(false);
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BOUND,
                      ble_link_session_get_state_flags());
}

static void test_reconnect_with_same_revision(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 5U);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Disconnect and reconnect with the same committed record. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1,
                          BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    _connect(GEN2);
    _authenticate(GEN2);
    /* The record revision is unchanged; only the session match is new. */
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN2, BLE_LINK_SESSION_CHANNEL_CONTROL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(GEN2, 5U, 1U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN2, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

static void test_reconnect_after_security2_closed(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 5U);
    /* Security 2 closes; the record survives. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2(
                          GEN1, false, 2U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Re-authenticated with the same long-term credentials. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2(
                          GEN1, true, 3U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(GEN1, 5U, 3U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

static void test_match_without_record_rejected(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    /* No committed record: a match cannot authorize. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_report_session_match(GEN1, 1U, 1U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

static void test_late_match_after_revoke_rejected(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 5U);
    /* Revoke the record. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          false, 6U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* A late match for the old revision cannot resurrect access. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_report_session_match(GEN1, 5U, 1U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

static void test_stale_revision_match_after_replace_rejected(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 5U);
    /* Record replaced with a newer revision. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, 7U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* A match verified against the old revision is invalid. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_report_session_match(GEN1, 5U, 1U));
    /* A match against the current revision restores access. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN1, 7U, 1U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

static void test_late_security2_epoch_ignored(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 5U);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Security 2 closes with epoch 2. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2(
                          GEN1, false, 2U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* A late authenticated event from the closed handshake (epoch 1) is
     * ignored, and a match bound to it cannot restore access. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2(
                          GEN1, true, 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_report_session_match(GEN1, 5U, 1U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* The current epoch (3) restores access after a fresh handshake. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2(
                          GEN1, true, 3U));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN1, 5U, 3U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

static void test_late_close_does_not_preserve_old_match(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 5U);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* A newer open(3) arrives before the late close(2). */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2(
                          GEN1, true, 3U));
    /* The new epoch invalidates the epoch-1 match: control gated until a
     * match for the current epoch arrives. */
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* The late close(2) is ignored. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2(
                          GEN1, false, 2U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Only a match bound to the current epoch restores access. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN1, 5U, 3U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

static void test_new_boot_resets_session(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 1U);
    ble_link_session_init(BOOT2);
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_INACTIVE,
                      ble_link_session_get_state(GEN1));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAVAILABLE,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    TEST_ASSERT_EQUAL(0U, ble_link_session_get_state_flags());
}

int main(void)
{
    test_boot_init_and_reset();
    test_admission_progression();
    test_security2_closed_keeps_acl();
    test_stale_authorization_revision_ignored();
    test_stale_generation_ignored();
    test_late_connect_ignored();
    test_disconnect_clears_session();
    test_second_acl_rejected();
    test_authorize_before_authentication_rejected();
    test_invalid_channel_rejected();
    test_facts_reflect_state();
    test_state_flags();
    test_reconnect_with_same_revision();
    test_reconnect_after_security2_closed();
    test_match_without_record_rejected();
    test_late_match_after_revoke_rejected();
    test_stale_revision_match_after_replace_rejected();
    test_late_security2_epoch_ignored();
    test_late_close_does_not_preserve_old_match();
    test_new_boot_resets_session();
    printf("ble_link_session: all tests passed\n");
    return 0;
}
