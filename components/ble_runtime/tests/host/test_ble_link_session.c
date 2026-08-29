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

#define TEST_ASSERT_FALSE(condition) TEST_ASSERT_TRUE(!(condition))

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

static uint32_t s_auth_epoch;

static void _authenticate(uint32_t generation)
{
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          generation,
                          BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          generation,
                          BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          generation, &s_auth_epoch));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(
                          generation, true));
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
    /* Encrypted + bond: session still gated until identity is verified. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_SESSION));
    /* Identity verified: session admitted, control still gated. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(
                          GEN1, true));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_SESSION));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Security 2 authenticated: control still needs authorization. */
    uint32_t epoch = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          GEN1, &epoch));
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

static void test_encrypted_descriptor_replay_follows_acl_connect(void)
{
    ble_link_session_init(BOOT1);
    /* An ENC_CHANGE may already be reflected in the connection descriptor
     * before NimBLE delivers CONNECT. Security facts applied before the ACL
     * generation exists are intentionally ignored. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_set_identity_known(GEN1, true));
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_INACTIVE,
                      ble_link_session_get_state(GEN1));

    /* The CONNECT callback must establish the generation first, then replay
     * the descriptor's encrypted/bond/identity facts. */
    _connect(GEN1);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_session_set_identity_known(GEN1, true));
    TEST_ASSERT_EQUAL(BLE_LINK_SESSION_AUTHENTICATED,
                      ble_link_session_get_state(GEN1));
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
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_close_current(
                          GEN1));
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
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_security2_close_current(GEN1));
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

static void test_authorized_match_requires_authenticated_facts(void)
{
    /* AUTHORIZED implies AUTHENTICATED: the session match that grants the
     * authorized fact must never run before the link facts (encrypted,
     * verified SC bond, known identity) converge, so the published flag
     * set stays coherent. */
    uint32_t epoch = 0U;

    ble_link_session_init(BOOT1);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, 1U));
    _connect(GEN1);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          GEN1, &epoch));
    /* Security 2 open and bound, but no verified bond yet: refused, and
     * the AUTHORIZED flag must stay masked. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_report_session_match(GEN1, 1U, 1U));
    TEST_ASSERT_EQUAL(0U, ble_link_session_get_state_flags() &
                      BLE_LINK_STATE_FLAG_AUTHORIZED);
    /* Bond verified but identity still unknown: refused. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_report_session_match(GEN1, 1U, 1U));
    TEST_ASSERT_EQUAL(0U, ble_link_session_get_state_flags() &
                      BLE_LINK_STATE_FLAG_AUTHORIZED);
    /* All facts present: the match succeeds and the flag publishes. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(
                          GEN1, true));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN1, 1U, 1U));
    TEST_ASSERT_TRUE((ble_link_session_get_state_flags() &
                      BLE_LINK_STATE_FLAG_AUTHORIZED) != 0U);
    TEST_ASSERT_TRUE((ble_link_session_get_state_flags() &
                      BLE_LINK_STATE_FLAG_AUTHENTICATED) != 0U);
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
    /* AUTHENTICATED implies BOUND: a bootstrap session without a
     * committed record does not publish the flag. */
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BINDABLE |
                      BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED,
                      ble_link_session_get_state_flags());
    /* An authorization transaction in flight drives TRANSITIONING. */
    ble_link_session_set_authorization_transitioning(true);
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BINDABLE |
                      BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED |
                      BLE_LINK_STATE_FLAG_TRANSITIONING,
                      ble_link_session_get_state_flags());
    ble_link_session_set_authorization_transitioning(false);
    _authorize(GEN1, 1U);
    /* BINDABLE excludes BOUND once a record is committed. */
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BOUND |
                      BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED |
                      BLE_LINK_STATE_FLAG_AUTHENTICATED |
                      BLE_LINK_STATE_FLAG_AUTHORIZED,
                      ble_link_session_get_state_flags());
    /* The record persists across disconnect. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1,
                          BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BOUND,
                      ble_link_session_get_state_flags());
    ble_link_session_set_pairing_window(false);
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BOUND,
                      ble_link_session_get_state_flags());

    _connect(GEN2);
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BOUND |
                      BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED |
                      BLE_LINK_STATE_FLAG_PUBLIC_DISCOVERY,
                      ble_link_session_get_state_flags());
    /* ERROR latches until the next boot. */
    ble_link_session_set_error(true);
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BOUND |
                      BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED |
                      BLE_LINK_STATE_FLAG_PUBLIC_DISCOVERY |
                      BLE_LINK_STATE_FLAG_ERROR,
                      ble_link_session_get_state_flags());
    ble_link_session_set_error(false);
    TEST_ASSERT_EQUAL(BLE_LINK_STATE_FLAG_BOUND |
                      BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED |
                      BLE_LINK_STATE_FLAG_PUBLIC_DISCOVERY |
                      BLE_LINK_STATE_FLAG_ERROR,
                      ble_link_session_get_state_flags());
}

static void test_v1_verified_bond_is_authenticated_without_security2(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_session_set_identity_known(GEN1, true));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_session_set_authorization(true, 1U));
    const uint32_t flags = ble_link_session_get_state_flags();

    TEST_ASSERT_TRUE((flags & BLE_LINK_STATE_FLAG_BOUND) != 0U);
    TEST_ASSERT_TRUE((flags & BLE_LINK_STATE_FLAG_AUTHENTICATED) != 0U);
    TEST_ASSERT_EQUAL(0U, flags & BLE_LINK_STATE_FLAG_AUTHORIZED);
}

static void test_removed_prior_bond_restores_bindable_current_acl(void)
{
    ble_link_session_init(BOOT1);
    ble_link_session_set_pairing_window(true);
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_session_set_authorization(true, 1U));
    _connect(GEN1);
    TEST_ASSERT_TRUE((ble_link_session_get_state_flags() &
                      BLE_LINK_STATE_FLAG_BOUND) != 0U);

    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_session_set_authorization(false, 0U));
    const uint32_t flags = ble_link_session_get_state_flags();

    TEST_ASSERT_TRUE((flags & BLE_LINK_STATE_FLAG_BLUETOOTH_ENABLED) != 0U);
    TEST_ASSERT_TRUE((flags & BLE_LINK_STATE_FLAG_BINDABLE) != 0U);
    TEST_ASSERT_EQUAL(0U, flags & BLE_LINK_STATE_FLAG_BOUND);
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
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN2, 5U, s_auth_epoch));
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
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_close_current(
                          GEN1));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Re-authenticated with the same long-term credentials. */
    uint32_t reopen_epoch = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          GEN1, &reopen_epoch));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN1, 5U, reopen_epoch));
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

static void test_security2_epoch_invalidation(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 5U);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* Security 2 closes: control drops to UNAUTHENTICATED. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_close_current(
                          GEN1));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* A match bound to the closed epoch cannot restore access. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_report_session_match(GEN1, 5U, 1U));
    /* A fresh handshake opens with a new epoch; until the match arrives
     * control is gated as PERMISSION_DENIED. */
    uint32_t fresh_epoch = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          GEN1, &fresh_epoch));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* A match bound to a stale epoch stays invalid. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_report_session_match(
                          GEN1, 5U, fresh_epoch - 1U));
    /* The match for the current epoch restores access. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN1, 5U, fresh_epoch));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

static void test_security2_handshake_uses_one_epoch(void)
{
    uint32_t epoch = 0U;

    ble_link_session_init(BOOT1);
    _connect(GEN1);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_LINK_ENCRYPTED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1, BLE_LINK_SESSION_EVENT_SC_BOND_VERIFIED));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_identity_known(
                          GEN1, true));

    /* Cmd0 owns epoch allocation but does not authenticate control. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_begin(
                          GEN1, &epoch));
    TEST_ASSERT_TRUE(epoch != 0U);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_security2_authenticate_current(
                          GEN1, epoch + 1U));

    /* Cmd1 authenticates the epoch allocated by Cmd0 without advancing it. */
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_session_security2_authenticate_current(
                          GEN1, epoch));
    TEST_ASSERT_EQUAL(epoch, ble_link_session_security2_epoch());
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_security2_authenticate_current(
                          GEN1, epoch));
}

static void test_late_close_does_not_preserve_old_match(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    _authorize(GEN1, 5U);
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* A newer open arrives before the late close. */
    uint32_t newer_epoch = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          GEN1, &newer_epoch));
    /* The new epoch invalidates the previous match: control gated until a
     * match for the current epoch arrives. */
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* The late close of a retired epoch is rejected. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_close_current(
                          GEN1));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAUTHENTICATED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
    /* A match for the closed epoch cannot restore access. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_report_session_match(
                          GEN1, 5U, newer_epoch));
    /* A fresh handshake with its own epoch restores access. */
    uint32_t final_epoch = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          GEN1, &final_epoch));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_report_session_match(
                          GEN1, 5U, final_epoch));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

static void test_epoch_exhaustion_survives_reconnect(void)
{
    uint32_t epoch = 0U;

    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    /* Push the allocator to the boundary. */
    ble_link_session_test_set_epoch(UINT32_MAX - 1U);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          GEN1, &epoch));
    TEST_ASSERT_EQUAL(UINT32_MAX, epoch);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_security2_open(GEN1, &epoch));
    /* Disconnect and reconnect must not release the lock. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          GEN1,
                          BLE_LINK_SESSION_EVENT_ACL_DISCONNECTED));
    _connect(GEN2);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_security2_open(GEN2, &epoch));
    /* A full teardown keeps the lock for the boot. */
    ble_link_session_reset();
    _connect(GEN2);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_link_session_security2_open(GEN2, &epoch));
    /* A new boot resets the allocator. */
    ble_link_session_init(BOOT2);
    _connect(GEN1);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_security2_open(
                          GEN1, &epoch));
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

static void test_authorization_exhausted(void)
{
    ble_link_session_init(BOOT1);
    _connect(GEN1);
    _authenticate(GEN1);
    /* Revision 0 auto-advances; a capacity query is non-mutating. */
    TEST_ASSERT_FALSE(ble_link_session_authorization_exhausted());
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, UINT32_MAX - 1U));
    TEST_ASSERT_FALSE(ble_link_session_authorization_exhausted());
    /* The final revision exhausts the space; a further commit is
     * refused before any persistence. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          true, UINT32_MAX));
    TEST_ASSERT_TRUE(ble_link_session_authorization_exhausted());
    /* At exhaustion revision-0 commits fail closed. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_session_set_authorization(
                          true, 0U));
    /* Revoke still applies at exhaustion. */
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_set_authorization(
                          false, 0U));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_PERMISSION_DENIED,
                      _query(GEN1, BLE_LINK_SESSION_CHANNEL_CONTROL));
}

int main(void)
{
    test_authorization_exhausted();
    test_boot_init_and_reset();
    test_admission_progression();
    test_encrypted_descriptor_replay_follows_acl_connect();
    test_security2_closed_keeps_acl();
    test_stale_authorization_revision_ignored();
    test_stale_generation_ignored();
    test_late_connect_ignored();
    test_disconnect_clears_session();
    test_second_acl_rejected();
    test_authorize_before_authentication_rejected();
    test_authorized_match_requires_authenticated_facts();
    test_invalid_channel_rejected();
    test_facts_reflect_state();
    test_state_flags();
    test_v1_verified_bond_is_authenticated_without_security2();
    test_removed_prior_bond_restores_bindable_current_acl();
    test_reconnect_with_same_revision();
    test_reconnect_after_security2_closed();
    test_match_without_record_rejected();
    test_late_match_after_revoke_rejected();
    test_stale_revision_match_after_replace_rejected();
    test_security2_epoch_invalidation();
    test_security2_handshake_uses_one_epoch();
    test_late_close_does_not_preserve_old_match();
    test_epoch_exhaustion_survives_reconnect();
    test_new_boot_resets_session();
    printf("ble_link_session: all tests passed\n");
    return 0;
}
