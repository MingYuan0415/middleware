#include "ble_nimble_smp_io.h"

#include "ble_link_service.h"
#include "ble_link_session.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define TEST_ASSERT_TRUE(value) assert(value)
#define TEST_ASSERT_FALSE(value) assert(!(value))
#define TEST_ASSERT_EQUAL(expected, actual) assert((expected) == (actual))

#define TEST_BOOT_ID UINT64_C(0x0123456789abcdef)

typedef struct smp_fixture smp_fixture_t;

typedef struct reply_thread_arg
{
    smp_fixture_t *fixture;
    uint64_t token;
    bool accept;
    uint32_t timeout_ms;
    esp_err_t result;
} reply_thread_arg_t;

struct smp_fixture
{
    ble_nimble_smp_io_t io;
    ble_nimble_smp_io_ops_t ops;
    pthread_mutex_t wake_lock;
    pthread_cond_t wake_changed;
    unsigned wake_calls;
    atomic_bool host_current;
    atomic_bool host_ready;
    atomic_uint_fast64_t now_us;
    atomic_uint_fast32_t generation;
    atomic_uint_fast16_t conn_handle;
    atomic_uint inject_calls;
    atomic_int inject_result;
    atomic_uint retire_calls;
    atomic_uint publish_calls;
    atomic_uint_fast64_t published_token;
    atomic_uint_fast32_t published_passkey;
    atomic_uint_fast64_t retired_token;
    atomic_bool retired_accept;
    atomic_bool inject_blocked;
    atomic_bool inject_entered;
    atomic_int last_inject;
    atomic_bool inject_release;
    atomic_bool cancel_on_inject;
    atomic_bool real_service;
};

static bool _is_host_current(void *arg)
{
    smp_fixture_t *fixture = arg;
    return atomic_load(&fixture->host_current);
}

static void _wake_owner(void *arg)
{
    smp_fixture_t *fixture = arg;
    (void)pthread_mutex_lock(&fixture->wake_lock);
    ++fixture->wake_calls;
    (void)pthread_cond_broadcast(&fixture->wake_changed);
    (void)pthread_mutex_unlock(&fixture->wake_lock);
}

static uint64_t _now_us(void *arg)
{
    smp_fixture_t *fixture = arg;
    return atomic_load(&fixture->now_us);
}

static esp_err_t _current_connection(uint32_t *generation,
                                     uint16_t *conn_handle, void *arg)
{
    smp_fixture_t *fixture = arg;
    *generation = (uint32_t)atomic_load(&fixture->generation);
    *conn_handle = (uint16_t)atomic_load(&fixture->conn_handle);
    return ESP_OK;
}

static bool _host_ready(void *arg)
{
    smp_fixture_t *fixture = arg;
    return atomic_load(&fixture->host_ready);
}

static int _inject(uint16_t conn_handle, bool accept, void *arg)
{
    smp_fixture_t *fixture = arg;
    TEST_ASSERT_EQUAL((uint16_t)atomic_load(&fixture->conn_handle), conn_handle);
    atomic_fetch_add(&fixture->inject_calls, 1U);
    atomic_store(&fixture->last_inject, accept ? 1 : 0);

    if (atomic_load(&fixture->cancel_on_inject) &&
            atomic_load(&fixture->inject_calls) == 1U)
    {
        /* This is deliberately reentrant: helper must not hold its lock over
         * the SDK callback, and cancel must not recursively inject reject. */
        ble_nimble_smp_io_cancel(&fixture->io);
    }

    if (atomic_load(&fixture->inject_blocked))
    {
        (void)pthread_mutex_lock(&fixture->wake_lock);
        atomic_store(&fixture->inject_entered, true);
        (void)pthread_cond_broadcast(&fixture->wake_changed);
        while (!atomic_load(&fixture->inject_release))
        {
            (void)pthread_cond_wait(&fixture->wake_changed, &fixture->wake_lock);
        }
        (void)pthread_mutex_unlock(&fixture->wake_lock);
    }

    return atomic_load(&fixture->inject_result);
}

static esp_err_t _publish_confirmation(uint32_t passkey, uint64_t token,
                                       void *arg)
{
    smp_fixture_t *fixture = arg;
    atomic_fetch_add(&fixture->publish_calls, 1U);
    atomic_store(&fixture->published_passkey, passkey);
    atomic_store(&fixture->published_token, token);
    if (atomic_load(&fixture->real_service))
    {
        return ble_link_service_publish_numeric_comparison(passkey, token);
    }
    return ESP_OK;
}

static void _retire_confirmation(uint64_t token, bool accept, void *arg)
{
    smp_fixture_t *fixture = arg;
    atomic_fetch_add(&fixture->retire_calls, 1U);
    atomic_store(&fixture->retired_token, token);
    atomic_store(&fixture->retired_accept, accept);
    if (atomic_load(&fixture->real_service))
    {
        TEST_ASSERT_EQUAL(ESP_OK,
                          ble_link_service_confirm_binding(token, accept));
    }
}

static const ble_nimble_smp_io_ops_t s_ops =
{
    .is_host_current = _is_host_current,
    .wake_owner = _wake_owner,
    .now_us = _now_us,
    .current_connection = _current_connection,
    .host_ready = _host_ready,
    .inject = _inject,
    .publish_confirmation = _publish_confirmation,
    .retire_confirmation = _retire_confirmation,
};

static void _fixture_init(smp_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    (void)pthread_mutex_init(&fixture->wake_lock, NULL);
    (void)pthread_cond_init(&fixture->wake_changed, NULL);
    atomic_init(&fixture->host_current, false);
    atomic_init(&fixture->host_ready, true);
    atomic_init(&fixture->now_us, 0U);
    atomic_init(&fixture->generation, 11U);
    atomic_init(&fixture->conn_handle, 7U);
    atomic_init(&fixture->inject_calls, 0U);
    atomic_init(&fixture->inject_result, 0);
    atomic_init(&fixture->retire_calls, 0U);
    atomic_init(&fixture->publish_calls, 0U);
    atomic_init(&fixture->published_token, 0U);
    atomic_init(&fixture->published_passkey, 0U);
    atomic_init(&fixture->retired_token, 0U);
    atomic_init(&fixture->retired_accept, false);
    atomic_init(&fixture->inject_blocked, false);
    atomic_init(&fixture->inject_entered, false);
    atomic_init(&fixture->inject_release, false);
    atomic_init(&fixture->cancel_on_inject, false);
    atomic_init(&fixture->real_service, false);
    atomic_init(&fixture->last_inject, 0);
    fixture->ops = s_ops;
    fixture->ops.arg = fixture;
    TEST_ASSERT_EQUAL(ESP_OK, ble_nimble_smp_io_init(&fixture->io, &fixture->ops));
    ble_nimble_smp_io_open(&fixture->io);
}

static void _fixture_reset(smp_fixture_t *fixture)
{
    if (fixture->io.initialized)
    {
        ble_nimble_smp_io_host_exited(&fixture->io);
    }
    atomic_store(&fixture->host_current, false);
    atomic_store(&fixture->host_ready, true);
    atomic_store(&fixture->now_us, 0U);
    atomic_store(&fixture->generation, 11U);
    atomic_store(&fixture->conn_handle, 7U);
    atomic_store(&fixture->inject_calls, 0U);
    atomic_store(&fixture->last_inject, 0);
    atomic_store(&fixture->inject_result, 0);
    atomic_store(&fixture->retire_calls, 0U);
    atomic_store(&fixture->publish_calls, 0U);
    atomic_store(&fixture->published_token, 0U);
    atomic_store(&fixture->published_passkey, 0U);
    atomic_store(&fixture->retired_token, 0U);
    atomic_store(&fixture->retired_accept, false);
    atomic_store(&fixture->inject_blocked, false);
    atomic_store(&fixture->inject_entered, false);
    atomic_store(&fixture->inject_release, false);
    atomic_store(&fixture->cancel_on_inject, false);
    atomic_store(&fixture->real_service, false);
    (void)pthread_mutex_lock(&fixture->wake_lock);
    fixture->wake_calls = 0U;
    (void)pthread_mutex_unlock(&fixture->wake_lock);
    ble_nimble_smp_io_open(&fixture->io);
}

static unsigned _wake_count(smp_fixture_t *fixture)
{
    unsigned result;
    (void)pthread_mutex_lock(&fixture->wake_lock);
    result = fixture->wake_calls;
    (void)pthread_mutex_unlock(&fixture->wake_lock);
    return result;
}

static void _wait_for_wake(smp_fixture_t *fixture, unsigned previous)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    (void)pthread_mutex_lock(&fixture->wake_lock);
    while (fixture->wake_calls <= previous)
    {
        const int wait_result = pthread_cond_timedwait(
                                    &fixture->wake_changed, &fixture->wake_lock, &deadline);
        TEST_ASSERT_EQUAL(0, wait_result);
    }
    (void)pthread_mutex_unlock(&fixture->wake_lock);
}

static bool _dispatch_once(smp_fixture_t *fixture)
{
    atomic_store(&fixture->host_current, true);
    if (!ble_nimble_smp_io_claim_event(&fixture->io))
    {
        atomic_store(&fixture->host_current, false);
        return false;
    }
    ble_nimble_smp_io_run_host(&fixture->io);
    atomic_store(&fixture->host_current, false);
    return true;
}

static void *_reply_thread(void *arg)
{
    reply_thread_arg_t *reply = arg;
    reply->result = ble_nimble_smp_io_reply(&reply->fixture->io,
                                            reply->token,
                                            reply->accept,
                                            reply->timeout_ms);
    return NULL;
}

static void _offer(smp_fixture_t *fixture, uint64_t token, uint16_t handle)
{
    TEST_ASSERT_EQUAL(ESP_OK, ble_nimble_smp_io_offer(
                          &fixture->io,
                          (uint32_t)atomic_load(&fixture->generation),
                          handle, token, 123456U));
    TEST_ASSERT_TRUE(atomic_load(&fixture->publish_calls) > 0U);
    TEST_ASSERT_EQUAL(token, atomic_load(&fixture->published_token));
}

static esp_err_t _reply_and_dispatch(smp_fixture_t *fixture, uint64_t token,
                                     bool accept)
{
    reply_thread_arg_t arg =
    {
        .fixture = fixture,
        .token = token,
        .accept = accept,
        .timeout_ms = 1000U,
        .result = ESP_FAIL,
    };
    pthread_t thread;
    const unsigned wake_before = _wake_count(fixture);
    TEST_ASSERT_EQUAL(0, pthread_create(&thread, NULL, _reply_thread, &arg));
    _wait_for_wake(fixture, wake_before);
    TEST_ASSERT_TRUE(_dispatch_once(fixture));
    TEST_ASSERT_EQUAL(0, pthread_join(thread, NULL));
    return arg.result;
}

static void test_offer_rejects_while_reply_queued(smp_fixture_t *fixture)
{
    reply_thread_arg_t old_reply =
    {
        .fixture = fixture, .token = 80U, .accept = true,
        .timeout_ms = 1000U, .result = ESP_FAIL,
    };
    pthread_t old_thread;
    const unsigned wake_before = _wake_count(fixture);

    _offer(fixture, 80U, 7U);
    TEST_ASSERT_EQUAL(0, pthread_create(&old_thread, NULL, _reply_thread,
                                        &old_reply));
    _wait_for_wake(fixture, wake_before);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      ble_nimble_smp_io_offer(&fixture->io,
                              11U, 7U, 81U, 654321U));
    TEST_ASSERT_EQUAL(80U, atomic_load(&fixture->published_token));
    TEST_ASSERT_TRUE(_dispatch_once(fixture));
    TEST_ASSERT_EQUAL(0, pthread_join(old_thread, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, old_reply.result);

    _offer(fixture, 81U, 7U);
    TEST_ASSERT_EQUAL(ESP_OK, _reply_and_dispatch(fixture, 81U, true));
}

static void test_reject_invalid_and_handle_zero(smp_fixture_t *fixture)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_nimble_smp_io_offer(&fixture->io, 0U, 0U, 1U, 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_nimble_smp_io_offer(&fixture->io, 1U, UINT16_MAX,
                              1U, 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ble_nimble_smp_io_offer(&fixture->io, 1U, 0U, 0U, 1U));
    _offer(fixture, 1U, 0U);
    atomic_store(&fixture->conn_handle, 0U);
    const esp_err_t zero_result = _reply_and_dispatch(fixture, 1U, true);
    TEST_ASSERT_EQUAL(ESP_OK, zero_result);
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->inject_calls));
    TEST_ASSERT_EQUAL(1, atomic_load(&fixture->last_inject));
}

static void test_forget_and_handle_reuse(smp_fixture_t *fixture)
{
    reply_thread_arg_t old_reply =
    {
        .fixture = fixture, .token = 10U, .accept = true,
        .timeout_ms = 1000U, .result = ESP_FAIL,
    };
    pthread_t old_thread;
    const unsigned wake_before = _wake_count(fixture);
    _offer(fixture, 10U, 7U);
    TEST_ASSERT_EQUAL(0, pthread_create(&old_thread, NULL, _reply_thread,
                                        &old_reply));
    _wait_for_wake(fixture, wake_before);
    ble_nimble_smp_io_forget_connection(&fixture->io, 11U, 7U);
    TEST_ASSERT_EQUAL(0, pthread_join(old_thread, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, old_reply.result);
    TEST_ASSERT_EQUAL(0U, atomic_load(&fixture->inject_calls));
    TEST_ASSERT_TRUE(_dispatch_once(fixture));
    TEST_ASSERT_FALSE(ble_nimble_smp_io_pending(&fixture->io));

    atomic_store(&fixture->generation, 12U);
    _offer(fixture, 11U, 7U);
    TEST_ASSERT_EQUAL(ESP_OK, _reply_and_dispatch(fixture, 11U, true));
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->inject_calls));
    TEST_ASSERT_EQUAL(1, atomic_load(&fixture->last_inject));
}

static void test_queued_timeout_is_tombstone(smp_fixture_t *fixture)
{
    reply_thread_arg_t arg =
    {
        .fixture = fixture, .token = 20U, .accept = true,
        .timeout_ms = 25U, .result = ESP_FAIL,
    };
    pthread_t thread;
    const unsigned wake_before = _wake_count(fixture);
    _offer(fixture, 20U, 7U);
    TEST_ASSERT_EQUAL(0, pthread_create(&thread, NULL, _reply_thread, &arg));
    _wait_for_wake(fixture, wake_before);
    TEST_ASSERT_EQUAL(0, pthread_join(thread, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, arg.result);
    TEST_ASSERT_TRUE(_dispatch_once(fixture));
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->retire_calls));
    TEST_ASSERT_EQUAL(0U, atomic_load(&fixture->inject_calls));
    TEST_ASSERT_FALSE(ble_nimble_smp_io_pending(&fixture->io));

    _offer(fixture, 21U, 7U);
    TEST_ASSERT_EQUAL(ESP_OK, _reply_and_dispatch(fixture, 21U, true));
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->inject_calls));
}

typedef struct dispatch_arg
{
    smp_fixture_t *fixture;
} dispatch_arg_t;

static void *_dispatch_thread(void *arg)
{
    dispatch_arg_t *dispatch = arg;
    (void)_dispatch_once(dispatch->fixture);
    return NULL;
}

static void test_executing_timeout_detaches_without_second_inject(
    smp_fixture_t *fixture)
{
    reply_thread_arg_t reply =
    {
        .fixture = fixture, .token = 30U, .accept = true,
        .timeout_ms = 30U, .result = ESP_FAIL,
    };
    dispatch_arg_t dispatch = {.fixture = fixture};
    pthread_t reply_thread;
    pthread_t host_thread;
    const unsigned wake_before = _wake_count(fixture);
    _offer(fixture, 30U, 7U);
    atomic_store(&fixture->inject_blocked, true);
    TEST_ASSERT_EQUAL(0, pthread_create(&reply_thread, NULL, _reply_thread,
                                        &reply));
    _wait_for_wake(fixture, wake_before);
    TEST_ASSERT_EQUAL(0, pthread_create(&host_thread, NULL, _dispatch_thread,
                                        &dispatch));

    (void)pthread_mutex_lock(&fixture->wake_lock);
    while (!atomic_load(&fixture->inject_entered))
    {
        (void)pthread_cond_wait(&fixture->wake_changed, &fixture->wake_lock);
    }
    (void)pthread_mutex_unlock(&fixture->wake_lock);
    atomic_store(&fixture->now_us, UINT64_C(1000000));
    TEST_ASSERT_EQUAL(0, pthread_join(reply_thread, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, reply.result);

    atomic_store(&fixture->inject_release, true);
    (void)pthread_mutex_lock(&fixture->wake_lock);
    (void)pthread_cond_broadcast(&fixture->wake_changed);
    (void)pthread_mutex_unlock(&fixture->wake_lock);
    TEST_ASSERT_EQUAL(0, pthread_join(host_thread, NULL));
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->inject_calls));
    TEST_ASSERT_FALSE(ble_nimble_smp_io_pending(&fixture->io));
}

static void test_failed_inject_restores_prompt(smp_fixture_t *fixture)
{
    _offer(fixture, 40U, 7U);
    atomic_store(&fixture->inject_result, -1);
    TEST_ASSERT_EQUAL(ESP_FAIL, _reply_and_dispatch(fixture, 40U, true));
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->inject_calls));
    TEST_ASSERT_TRUE(fixture->io.offer_pending);
    TEST_ASSERT_EQUAL(0U, atomic_load(&fixture->retire_calls));

    atomic_store(&fixture->inject_result, 0);
    TEST_ASSERT_EQUAL(ESP_OK, _reply_and_dispatch(fixture, 40U, false));
    TEST_ASSERT_EQUAL(2U, atomic_load(&fixture->inject_calls));
    TEST_ASSERT_EQUAL(0, atomic_load(&fixture->last_inject));
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->retire_calls));
}

static void test_reentrant_cancel_does_not_reject_again(smp_fixture_t *fixture)
{
    _offer(fixture, 50U, 7U);
    atomic_store(&fixture->cancel_on_inject, true);
    atomic_store(&fixture->inject_result, -1);
    TEST_ASSERT_EQUAL(ESP_FAIL, _reply_and_dispatch(fixture, 50U, true));
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->inject_calls));
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->retire_calls));
    atomic_store(&fixture->cancel_on_inject, false);
    TEST_ASSERT_FALSE(ble_nimble_smp_io_pending(&fixture->io));
}

static void test_cancel_rejects_once_and_close_drains(smp_fixture_t *fixture)
{
    _offer(fixture, 60U, 7U);
    ble_nimble_smp_io_cancel(&fixture->io);
    TEST_ASSERT_TRUE(ble_nimble_smp_io_pending(&fixture->io));
    TEST_ASSERT_TRUE(_dispatch_once(fixture));
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->inject_calls));
    TEST_ASSERT_EQUAL(0, atomic_load(&fixture->last_inject));
    ble_nimble_smp_io_cancel(&fixture->io);
    TEST_ASSERT_TRUE(_dispatch_once(fixture) == false);
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->inject_calls));

    _offer(fixture, 61U, 7U);
    ble_nimble_smp_io_close(&fixture->io);
    TEST_ASSERT_FALSE(fixture->io.accepting);
    TEST_ASSERT_FALSE(ble_nimble_smp_io_pending(&fixture->io));
    TEST_ASSERT_FALSE(_dispatch_once(fixture));
    TEST_ASSERT_EQUAL(1U, atomic_load(&fixture->inject_calls));
    ble_nimble_smp_io_host_exited(&fixture->io);
    ble_nimble_smp_io_open(&fixture->io);
    TEST_ASSERT_TRUE(fixture->io.accepting);
}

static void test_open_requested_after_persistent_event(smp_fixture_t *fixture)
{
    _offer(fixture, 70U, 7U);
    ble_nimble_smp_io_cancel(&fixture->io);
    TEST_ASSERT_TRUE(ble_nimble_smp_io_claim_event(&fixture->io));
    ble_nimble_smp_io_open(&fixture->io);
    TEST_ASSERT_TRUE(fixture->io.open_requested);
    atomic_store(&fixture->host_current, true);
    ble_nimble_smp_io_run_host(&fixture->io);
    atomic_store(&fixture->host_current, false);
    TEST_ASSERT_TRUE(fixture->io.accepting);
    ble_nimble_smp_io_cancel(&fixture->io);
    TEST_ASSERT_FALSE(_dispatch_once(fixture));
}

static esp_err_t _service_output(const uint8_t *value, size_t len,
                                 ble_link_service_tx_channel_t channel,
                                 bool is_last,
                                 const ble_link_operation_identity_t *identity,
                                 void *arg)
{
    (void)value;
    (void)len;
    (void)channel;
    (void)is_last;
    (void)identity;
    (void)arg;
    return ESP_OK;
}

static void test_real_service_publish_and_retire(smp_fixture_t *fixture)
{
    uint64_t token = 0U;
    ble_link_confirmation_snapshot_t confirmation;
    atomic_store(&fixture->generation, 11U);
    atomic_store(&fixture->conn_handle, 7U);
    atomic_store(&fixture->host_ready, true);
    ble_nimble_smp_io_open(&fixture->io);
    ble_link_session_init(TEST_BOOT_ID);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_session_handle_event(
                          11U, BLE_LINK_SESSION_EVENT_ACL_CONNECTED));
    ble_link_service_init(TEST_BOOT_ID, _service_output, NULL, NULL, 0U);
    ble_link_service_set_pairing_window(true);
    TEST_ASSERT_EQUAL(ESP_OK, ble_link_service_on_connect(11U, 7U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      ble_link_service_allocate_confirmation_token(&token));
    TEST_ASSERT_TRUE(token != 0U);
    atomic_store(&fixture->real_service, true);
    _offer(fixture, token, 7U);
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_TRUE(confirmation.pending);
    TEST_ASSERT_EQUAL(token, confirmation.token);
    TEST_ASSERT_EQUAL(123456U, confirmation.numeric_comparison);
    TEST_ASSERT_EQUAL(ESP_OK, _reply_and_dispatch(fixture, token, true));
    confirmation = ble_link_service_get_confirmation();
    TEST_ASSERT_FALSE(confirmation.pending);
    ble_link_service_reset();
}

int main(void)
{
    smp_fixture_t fixture;
    _fixture_init(&fixture);

    test_offer_rejects_while_reply_queued(&fixture);
    _fixture_reset(&fixture);
    test_reject_invalid_and_handle_zero(&fixture);
    _fixture_reset(&fixture);
    test_forget_and_handle_reuse(&fixture);
    _fixture_reset(&fixture);
    test_queued_timeout_is_tombstone(&fixture);
    _fixture_reset(&fixture);
    test_executing_timeout_detaches_without_second_inject(&fixture);
    _fixture_reset(&fixture);
    test_failed_inject_restores_prompt(&fixture);
    _fixture_reset(&fixture);
    test_reentrant_cancel_does_not_reject_again(&fixture);
    _fixture_reset(&fixture);
    test_cancel_rejects_once_and_close_drains(&fixture);
    _fixture_reset(&fixture);
    test_open_requested_after_persistent_event(&fixture);
    _fixture_reset(&fixture);
    test_real_service_publish_and_retire(&fixture);

    ble_nimble_smp_io_host_exited(&fixture.io);
    (void)pthread_cond_destroy(&fixture.wake_changed);
    (void)pthread_mutex_destroy(&fixture.wake_lock);
    return 0;
}
