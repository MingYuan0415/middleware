#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "ble_link_dispatcher.h"

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

static const uint64_t s_boot_id = 72623859790382856ULL;
static const uint32_t s_generation = 1U;
static unsigned int s_handler_calls;

static uint32_t _ok_handler(const ble_link_codec_request_t *request,
                            const ble_link_dispatcher_facts_t *facts,
                            void *arg)
{
    (void)request;
    (void)facts;
    (void)arg;
    s_handler_calls++;
    return BLE_LINK_ERROR_OK;
}

static uint32_t _conflict_handler(const ble_link_codec_request_t *request,
                                  const ble_link_dispatcher_facts_t *facts,
                                  void *arg)
{
    (void)request;
    (void)facts;
    (void)arg;
    return BLE_LINK_ERROR_CONFLICT;
}

static ble_link_codec_envelope_t _make_envelope(
    uint32_t protocol_major, uint64_t boot_id, uint32_t flag)
{
    ble_link_codec_envelope_t envelope;

    memset(&envelope, 0, sizeof(envelope));
    envelope.protocol_major = protocol_major;
    envelope.boot_id = boot_id;
    if (flag != 0U)
    {
        envelope.flags_values[0] = flag;
        envelope.flags_count = 1U;
        envelope.flags = flag;
    }
    return envelope;
}

static ble_link_codec_request_t _make_request(uint64_t request_id,
        ble_link_codec_request_tag_t body)
{
    ble_link_codec_request_t request;

    memset(&request, 0, sizeof(request));
    request.request_id = request_id;
    request.body = body;
    return request;
}

static ble_link_dispatcher_facts_t _make_facts(bool encrypted,
        bool authenticated,
        bool authorized)
{
    ble_link_dispatcher_facts_t facts;

    memset(&facts, 0, sizeof(facts));
    facts.active_boot_id = s_boot_id;
    facts.connection_generation = s_generation;
    facts.encrypted = encrypted;
    facts.session_authenticated = authenticated;
    facts.authorized = authorized;
    return facts;
}

static void _reset_dispatcher(void)
{
    ble_link_dispatcher_reset();
    s_handler_calls = 0U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_register_request(
                          BLE_LINK_CODEC_REQUEST_GET_MANIFEST,
                          _ok_handler, NULL));
}

static void test_ok_dispatch_invokes_handler(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 0U);
    ble_link_codec_request_t request =
        _make_request(1U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, error);
    TEST_ASSERT_EQUAL(1U, s_handler_calls);
}

static void test_unsupported_version_rejected(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(1U, s_boot_id, 0U);
    ble_link_codec_request_t request =
        _make_request(1U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNSUPPORTED_VERSION, error);
    TEST_ASSERT_EQUAL(0U, s_handler_calls);
}

static void test_boot_id_mismatch_rejected(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id + 1U, 0U);
    ble_link_codec_request_t request =
        _make_request(1U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNAVAILABLE, error);
    TEST_ASSERT_EQUAL(0U, s_handler_calls);
}

static void test_unknown_flag_rejected(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 2U);
    ble_link_codec_request_t request =
        _make_request(1U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_INVALID_ARGUMENT, error);
    TEST_ASSERT_EQUAL(0U, s_handler_calls);
}

static void test_recovery_query_flag_accepted(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, BLE_LINK_CODEC_FLAG_RECOVERY_QUERY);
    ble_link_codec_request_t request =
        _make_request(1U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, error);
    TEST_ASSERT_EQUAL(1U, s_handler_calls);
}

static void test_zero_request_id_rejected(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 0U);
    ble_link_codec_request_t request =
        _make_request(0U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_INVALID_ARGUMENT, error);
    TEST_ASSERT_EQUAL(0U, s_handler_calls);
}

static void test_duplicate_request_id_conflict(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 0U);
    ble_link_codec_request_t request =
        _make_request(7U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, error);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_CONFLICT, error);
    TEST_ASSERT_EQUAL(1U, s_handler_calls);
}

static void test_unknown_body_unsupported_operation(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 0U);
    ble_link_codec_request_t request =
        _make_request(1U, (ble_link_codec_request_tag_t)99U);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNSUPPORTED_OPERATION, error);
    TEST_ASSERT_EQUAL(0U, s_handler_calls);
}

static void test_handler_error_propagated(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_register_request(
                          BLE_LINK_CODEC_REQUEST_GET_LINK_SNAPSHOT,
                          _conflict_handler, NULL));
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 0U);
    ble_link_codec_request_t request =
        _make_request(1U, BLE_LINK_CODEC_REQUEST_GET_LINK_SNAPSHOT);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_CONFLICT, error);
}

static void test_clear_session_resets_ids(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 0U);
    ble_link_codec_request_t request =
        _make_request(7U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    ble_link_dispatcher_clear_session();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, error);
    TEST_ASSERT_EQUAL(2U, s_handler_calls);
}

static void test_registration_rules(void)
{
    _reset_dispatcher();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_dispatcher_register_request(
                          BLE_LINK_CODEC_REQUEST_GET_MANIFEST,
                          NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_dispatcher_register_request(
                          (ble_link_codec_request_tag_t)99U,
                          _ok_handler, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ble_link_dispatcher_register_request(
                          BLE_LINK_CODEC_REQUEST_GET_MANIFEST,
                          _ok_handler, NULL));
}

static void test_conflict_priority_over_unknown_body(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 0U);
    ble_link_codec_request_t request =
        _make_request(1U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, error);
    request.body = (ble_link_codec_request_tag_t)99U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, &error));
    /* Duplicate request id takes priority over the unknown body. */
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_CONFLICT, error);
    TEST_ASSERT_EQUAL(1U, s_handler_calls);
}

static void test_session_id_grows_beyond_initial_capacity(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 0U);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);
    uint64_t request_id = 1U;

    for (size_t i = 0U; i < 64U; ++i)
    {
        ble_link_codec_request_t request =
            _make_request(request_id, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);

        TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                              &envelope, &request, &facts, &error));
        TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, error);
        request_id++;
    }
    /* The grown set still rejects duplicates as CONFLICT. */
    ble_link_codec_request_t duplicate =
        _make_request(1U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &duplicate, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_CONFLICT, error);
    /* Clearing frees the set; the same id becomes usable again. */
    ble_link_dispatcher_clear_session();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &duplicate, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_OK, error);
}

static void test_all_five_handlers_registered(void)
{
    _reset_dispatcher();
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_register_request(
                          BLE_LINK_CODEC_REQUEST_GET_LINK_SNAPSHOT,
                          _ok_handler, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_register_request(
                          BLE_LINK_CODEC_REQUEST_AUTHORIZE_PREPARE,
                          _ok_handler, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_register_request(
                          BLE_LINK_CODEC_REQUEST_AUTHORIZE_COMMIT,
                          _ok_handler, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_register_request(
                          BLE_LINK_CODEC_REQUEST_GET_AUTHORIZATION,
                          _ok_handler, NULL));
}

static void test_invalid_arguments_rejected(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 0U);
    ble_link_codec_request_t request =
        _make_request(1U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_dispatcher_handle_request(
                          NULL, &request, &facts, &error));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_dispatcher_handle_request(
                          &envelope, NULL, &facts, &error));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_dispatcher_handle_request(
                          &envelope, &request, NULL, &error));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ble_link_dispatcher_handle_request(
                          &envelope, &request, &facts, NULL));
}

static void test_unknown_body_consumes_request_id(void)
{
    uint32_t error = 0U;

    _reset_dispatcher();
    ble_link_codec_envelope_t envelope =
        _make_envelope(2U, s_boot_id, 0U);
    ble_link_dispatcher_facts_t facts = _make_facts(true, true, false);

    /* Unknown body consumes the request id on first use. */
    ble_link_codec_request_t unknown =
        _make_request(5U, (ble_link_codec_request_tag_t)99U);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &unknown, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_UNSUPPORTED_OPERATION, error);
    /* The same id with a valid body is rejected as CONFLICT. */
    ble_link_codec_request_t valid =
        _make_request(5U, BLE_LINK_CODEC_REQUEST_GET_MANIFEST);

    TEST_ASSERT_EQUAL(ESP_OK, ble_link_dispatcher_handle_request(
                          &envelope, &valid, &facts, &error));
    TEST_ASSERT_EQUAL(BLE_LINK_ERROR_CONFLICT, error);
    TEST_ASSERT_EQUAL(0U, s_handler_calls);
}

int main(void)
{
    test_ok_dispatch_invokes_handler();
    test_unsupported_version_rejected();
    test_boot_id_mismatch_rejected();
    test_unknown_flag_rejected();
    test_recovery_query_flag_accepted();
    test_zero_request_id_rejected();
    test_duplicate_request_id_conflict();
    test_unknown_body_unsupported_operation();
    test_unknown_body_consumes_request_id();
    test_handler_error_propagated();
    test_clear_session_resets_ids();
    test_conflict_priority_over_unknown_body();
    test_session_id_grows_beyond_initial_capacity();
    test_all_five_handlers_registered();
    test_registration_rules();
    test_invalid_arguments_rejected();
    printf("ble_link_dispatcher: all tests passed\n");
    return 0;
}
