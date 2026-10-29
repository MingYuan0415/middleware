/**
 * @file Host regression tests for the event bus.
 *
 * The suite exercises the real implementation on top of the pthread-backed
 * FreeRTOS fake and a deterministic non-inline UI dispatcher.
 */

#include "event_bus.h"
#include "event_bus_core.h"

#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

EVENT_BUS_DEFINE_ID(TEST_MSG);
EVENT_BUS_DEFINE_ID(OTHER_MSG);

#define TEST_SUB_TYPE 1U
#define DISPATCH_DEPTH 8U

typedef struct
{
    void (*cb)(void *);
    void *arg;
} dispatch_entry_t;

static dispatch_entry_t s_queue[DISPATCH_DEPTH];
static unsigned s_queue_head;
static unsigned s_queue_tail;
static unsigned s_queue_count;
static esp_err_t s_dispatch_result;
static unsigned s_dispatch_calls;

typedef struct
{
    unsigned count;
    event_bus_msg_id_t msg_id;
    uint32_t sub_type;
    size_t payload_size;
    /* Sync payloads may exceed the UI copy limit. */
    uint8_t payload[EVENT_BUS_MAX_UI_PAYLOAD_SIZE + 1U];
} capture_t;

static esp_err_t _fake_dispatch(void (*cb)(void *), void *arg)
{
    ++s_dispatch_calls;
    if (s_dispatch_result != ESP_OK)
    {
        return s_dispatch_result;
    }
    if (s_queue_count >= DISPATCH_DEPTH)
    {
        return ESP_ERR_NO_MEM;
    }
    s_queue[s_queue_tail].cb = cb;
    s_queue[s_queue_tail].arg = arg;
    s_queue_tail = (s_queue_tail + 1U) % DISPATCH_DEPTH;
    ++s_queue_count;
    return ESP_OK;
}

static void _fake_drain(void)
{
    while (s_queue_count > 0)
    {
        void (*cb)(void *) = s_queue[s_queue_head].cb;
        void *arg = s_queue[s_queue_head].arg;
        s_queue_head = (s_queue_head + 1U) % DISPATCH_DEPTH;
        --s_queue_count;
        cb(arg);
    }
}

static void _reset_dispatcher(void)
{
    s_queue_head = 0;
    s_queue_tail = 0;
    s_queue_count = 0;
    s_dispatch_result = ESP_OK;
    s_dispatch_calls = 0;
}

static void _capture(event_bus_msg_id_t msg_id, uint32_t sub_type,
                     const void *payload, size_t payload_size, void *user_data)
{
    capture_t *capture = user_data;
    ++capture->count;
    capture->msg_id = msg_id;
    capture->sub_type = sub_type;
    capture->payload_size = payload_size;
    if (payload_size > 0)
    {
        assert(payload_size <= sizeof(capture->payload));
        memcpy(capture->payload, payload, payload_size);
    }
}

static void _setUp(void)
{
    _reset_dispatcher();
    assert(event_bus_init() == ESP_OK);
    assert(event_bus_register_ui_dispatch(_fake_dispatch, DISPATCH_DEPTH)
           == ESP_OK);
}

static void _tearDown(void)
{
    _reset_dispatcher();
    event_bus_deinit();
}

static void _test_sync_delivery(void)
{
    _setUp();
    capture_t capture = {0};
    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &capture,
                               EVENT_BUS_DISPATCH_PUBLISHER, &handle) == ESP_OK);
    assert(handle != EVENT_BUS_SUB_HANDLE_INVALID);

    const uint32_t value = UINT32_C(0x11223344);
    event_bus_publish_report_t report;
    assert(event_bus_publish_ex(TEST_MSG, TEST_SUB_TYPE, &value, sizeof(value),
                                0, &report) == ESP_OK);
    assert(capture.count == 1U);
    assert(capture.msg_id == TEST_MSG);
    assert(capture.sub_type == TEST_SUB_TYPE);
    assert(capture.payload_size == sizeof(value));
    assert(memcmp(capture.payload, &value, sizeof(value)) == 0);
    assert(report.matched == 1U);
    assert(report.sync_delivered == 1U);
    assert(report.ui_result == ESP_OK);
    assert(event_bus_unsubscribe(handle) == ESP_OK);
    _tearDown();
}

static void _test_subtype_filter(void)
{
    _setUp();
    capture_t any = {0};
    capture_t exact = {0};
    event_bus_sub_handle_t any_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t exact_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, EVENT_BUS_SUB_TYPE_ANY, _capture,
                               &any, EVENT_BUS_DISPATCH_PUBLISHER,
                               &any_handle) == ESP_OK);
    assert(event_bus_subscribe(TEST_MSG, 5U, _capture, &exact,
                               EVENT_BUS_DISPATCH_PUBLISHER,
                               &exact_handle) == ESP_OK);

    assert(event_bus_publish(TEST_MSG, 6U, NULL, 0, 0) == ESP_OK);
    assert(any.count == 1U);
    assert(exact.count == 0U);

    assert(event_bus_publish(TEST_MSG, 5U, NULL, 0, 0) == ESP_OK);
    assert(any.count == 2U);
    assert(exact.count == 1U);

    assert(event_bus_publish(OTHER_MSG, 5U, NULL, 0, 0) == ESP_OK);
    assert(any.count == 2U);
    assert(exact.count == 1U);
    _tearDown();
}

static void _test_handle_generation(void)
{
    _setUp();
    capture_t first = {0};
    capture_t second = {0};
    event_bus_sub_handle_t first_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t second_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &first,
                               EVENT_BUS_DISPATCH_PUBLISHER,
                               &first_handle) == ESP_OK);
    assert(event_bus_unsubscribe(first_handle) == ESP_OK);
    assert(event_bus_unsubscribe(first_handle) == ESP_ERR_NOT_FOUND);
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &second,
                               EVENT_BUS_DISPATCH_PUBLISHER,
                               &second_handle) == ESP_OK);
    assert(second_handle != first_handle);
    assert(event_bus_unsubscribe(first_handle) == ESP_ERR_NOT_FOUND);

    assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, NULL, 0, 0) == ESP_OK);
    assert(first.count == 0U);
    assert(second.count == 1U);
    _tearDown();
}

static void _test_ui_delivery(void)
{
    _setUp();
    capture_t capture = {0};
    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &capture,
                               EVENT_BUS_DISPATCH_UI, &handle) == ESP_OK);

    const uint16_t value = UINT16_C(0xabcd);
    assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, &value, sizeof(value),
                             0) == ESP_OK);
    assert(capture.count == 0U);
    assert(s_dispatch_calls == 1U);
    _fake_drain();
    assert(capture.count == 1U);
    assert(capture.payload_size == sizeof(value));
    assert(memcmp(capture.payload, &value, sizeof(value)) == 0);
    _tearDown();
}

static void _test_ui_latest_coalescing(void)
{
    _setUp();
    capture_t capture = {0};
    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &capture,
                               EVENT_BUS_DISPATCH_UI, &handle) == ESP_OK);

    for (uint32_t i = 0; i < 1000U; ++i)
    {
        const uint32_t value = i;
        assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, &value, sizeof(value),
                                 EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);
    }
    assert(s_dispatch_calls == 1U);
    assert(s_queue_count == 1U);
    _fake_drain();
    assert(capture.count == 1U);
    const uint32_t expected = 999U;
    assert(memcmp(capture.payload, &expected, sizeof(expected)) == 0);
    _tearDown();
}

static void _test_ui_latest_visibility(void)
{
    _setUp();
    capture_t first = {0};
    capture_t second = {0};
    event_bus_sub_handle_t first_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t second_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &first,
                               EVENT_BUS_DISPATCH_UI, &first_handle) == ESP_OK);

    const uint32_t value = 1U;
    assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, &value, sizeof(value),
                             EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &second,
                               EVENT_BUS_DISPATCH_UI, &second_handle) == ESP_OK);
    const uint32_t newer = 2U;
    assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, &newer, sizeof(newer),
                             EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);

    assert(s_dispatch_calls == 1U);
    _fake_drain();
    assert(first.count == 1U);
    assert(second.count == 1U);
    assert(memcmp(first.payload, &newer, sizeof(newer)) == 0);
    assert(memcmp(second.payload, &newer, sizeof(newer)) == 0);
    _tearDown();
}

static void _test_publish_isolation(void)
{
    _setUp();
    capture_t sync_capture = {0};
    capture_t ui_capture = {0};
    event_bus_sub_handle_t sync_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t ui_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &sync_capture,
                               EVENT_BUS_DISPATCH_PUBLISHER,
                               &sync_handle) == ESP_OK);
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &ui_capture,
                               EVENT_BUS_DISPATCH_UI, &ui_handle) == ESP_OK);

    s_dispatch_result = ESP_ERR_NO_MEM;
    event_bus_publish_report_t report;
    const uint32_t value = 42U;
    assert(event_bus_publish_ex(TEST_MSG, TEST_SUB_TYPE, &value, sizeof(value),
                                0, &report) == ESP_ERR_NO_MEM);
    assert(report.ui_result == ESP_ERR_NO_MEM);
    assert(report.sync_result == ESP_OK);
    assert(report.sync_delivered == 1U);
    assert(sync_capture.count == 1U);
    assert(ui_capture.count == 0U);
    assert(s_dispatch_calls == 1U);
    _tearDown();
}

static void _test_atomic_publish(void)
{
    _setUp();
    capture_t sync_capture = {0};
    capture_t ui_capture = {0};
    event_bus_sub_handle_t sync_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t ui_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &sync_capture,
                               EVENT_BUS_DISPATCH_PUBLISHER,
                               &sync_handle) == ESP_OK);
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &ui_capture,
                               EVENT_BUS_DISPATCH_UI, &ui_handle) == ESP_OK);

    s_dispatch_result = ESP_ERR_NO_MEM;
    event_bus_publish_report_t report;
    assert(event_bus_publish_ex(TEST_MSG, TEST_SUB_TYPE, NULL, 0,
                                EVENT_BUS_PUBLISH_FLAG_ATOMIC, &report)
           == ESP_ERR_NO_MEM);
    assert(report.ui_result == ESP_ERR_NO_MEM);
    assert(report.sync_result == ESP_ERR_NO_MEM);
    assert(report.sync_delivered == 0U);
    assert(sync_capture.count == 0U);

    s_dispatch_result = ESP_OK;
    assert(event_bus_publish_ex(TEST_MSG, TEST_SUB_TYPE, NULL, 0,
                                EVENT_BUS_PUBLISH_FLAG_ATOMIC, &report)
           == ESP_OK);
    assert(report.sync_delivered == 1U);
    assert(sync_capture.count == 1U);
    assert(s_queue_count == 1U);
    _fake_drain();
    assert(ui_capture.count == 1U);
    _tearDown();
}

static void _test_oversize_isolation(void)
{
    _setUp();
    capture_t sync_capture = {0};
    capture_t ui_capture = {0};
    event_bus_sub_handle_t sync_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t ui_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &sync_capture,
                               EVENT_BUS_DISPATCH_PUBLISHER,
                               &sync_handle) == ESP_OK);
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &ui_capture,
                               EVENT_BUS_DISPATCH_UI, &ui_handle) == ESP_OK);

    uint8_t payload[EVENT_BUS_MAX_UI_PAYLOAD_SIZE + 1U];
    memset(payload, 0x5a, sizeof(payload));
    event_bus_publish_report_t report;
    assert(event_bus_publish_ex(TEST_MSG, TEST_SUB_TYPE, payload, sizeof(payload),
                                0, &report) == ESP_ERR_INVALID_SIZE);
    assert(report.ui_result == ESP_ERR_INVALID_SIZE);
    assert(report.sync_delivered == 1U);
    assert(sync_capture.count == 1U);
    assert(sync_capture.payload_size == sizeof(payload));
    assert(s_dispatch_calls == 0U);
    _tearDown();
}

static void _test_ui_batch_depth(void)
{
    _setUp();
    capture_t sync_capture = {0};
    capture_t ui_capture = {0};
    event_bus_sub_handle_t sync_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t ui_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, EVENT_BUS_SUB_TYPE_ANY, _capture,
                               &sync_capture, EVENT_BUS_DISPATCH_PUBLISHER,
                               &sync_handle) == ESP_OK);
    assert(event_bus_subscribe(TEST_MSG, EVENT_BUS_SUB_TYPE_ANY, _capture,
                               &ui_capture, EVENT_BUS_DISPATCH_UI,
                               &ui_handle) == ESP_OK);

    for (uint32_t subtype = 0; subtype < DISPATCH_DEPTH; ++subtype)
    {
        assert(event_bus_publish(TEST_MSG, subtype, NULL, 0, 0) == ESP_OK);
    }
    assert(s_queue_count == DISPATCH_DEPTH);

    event_bus_publish_report_t report;
    assert(event_bus_publish_ex(TEST_MSG, 100U, NULL, 0, 0, &report)
           == ESP_ERR_NO_MEM);
    assert(report.ui_result == ESP_ERR_NO_MEM);
    assert(report.sync_delivered == 1U);
    assert(sync_capture.count == DISPATCH_DEPTH + 1U);

    _fake_drain();
    assert(ui_capture.count == DISPATCH_DEPTH);
    _tearDown();
}

static void _test_abandon_releases_pool(void)
{
    _setUp();
    capture_t ui_capture = {0};
    event_bus_sub_handle_t ui_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, EVENT_BUS_SUB_TYPE_ANY, _capture,
                               &ui_capture, EVENT_BUS_DISPATCH_UI,
                               &ui_handle) == ESP_OK);

    for (uint32_t subtype = 0; subtype < DISPATCH_DEPTH; ++subtype)
    {
        assert(event_bus_publish(TEST_MSG, subtype, NULL, 0, 0) == ESP_OK);
    }
    assert(s_queue_count == DISPATCH_DEPTH);
    assert(event_bus_abandon_pending_ui() == ESP_OK);

    _fake_drain();
    assert(ui_capture.count == 0U);

    for (uint32_t subtype = 0; subtype < DISPATCH_DEPTH; ++subtype)
    {
        assert(event_bus_publish(TEST_MSG, subtype, NULL, 0, 0) == ESP_OK);
    }
    _fake_drain();
    assert(ui_capture.count == DISPATCH_DEPTH);
    _tearDown();
}

static sem_t s_entered;
static sem_t s_release;

static void _blocking_cb(event_bus_msg_id_t msg_id, uint32_t sub_type,
                         const void *payload, size_t payload_size,
                         void *user_data)
{
    (void)msg_id;
    (void)sub_type;
    (void)payload;
    (void)payload_size;
    (void)user_data;
    (void)sem_post(&s_entered);
    (void)sem_wait(&s_release);
}

static void *_publish_thread(void *arg)
{
    (void)arg;
    (void)event_bus_publish(TEST_MSG, TEST_SUB_TYPE, NULL, 0, 0);
    return NULL;
}

static void _test_unsubscribe_sync(void)
{
    _setUp();
    assert(sem_init(&s_entered, 0, 0) == 0);
    assert(sem_init(&s_release, 0, 0) == 0);

    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _blocking_cb, NULL,
                               EVENT_BUS_DISPATCH_PUBLISHER, &handle) == ESP_OK);

    pthread_t thread;
    assert(pthread_create(&thread, NULL, _publish_thread, NULL) == 0);
    assert(sem_wait(&s_entered) == 0);

    assert(event_bus_unsubscribe_sync(handle, 20U) == ESP_ERR_TIMEOUT);
    assert(event_bus_unsubscribe_sync(handle, 20U) == ESP_ERR_TIMEOUT);
    assert(event_bus_flush_sync(20U) == ESP_ERR_TIMEOUT);

    assert(sem_post(&s_release) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(event_bus_flush_sync(1000U) == ESP_OK);
    assert(event_bus_unsubscribe_sync(handle, 0U) == ESP_OK);
    assert(event_bus_unsubscribe_sync(handle, 0U) == ESP_OK);

    capture_t reused_capture = {0};
    event_bus_sub_handle_t reused = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture,
                               &reused_capture, EVENT_BUS_DISPATCH_PUBLISHER,
                               &reused) == ESP_OK);
    assert(reused != handle);
    assert(event_bus_unsubscribe_sync(handle, 0U) == ESP_ERR_NOT_FOUND);
    assert(event_bus_unsubscribe(reused) == ESP_OK);

    (void)sem_destroy(&s_entered);
    (void)sem_destroy(&s_release);
    _tearDown();
}

static void _test_unsubscribe_sync_idle(void)
{
    _setUp();
    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    capture_t capture = {0};
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &capture,
                               EVENT_BUS_DISPATCH_PUBLISHER, &handle) == ESP_OK);
    assert(event_bus_unsubscribe_sync(handle, 1000U) == ESP_OK);
    assert(event_bus_unsubscribe_sync(handle, 1000U) == ESP_OK);
    assert(event_bus_unsubscribe(handle) == ESP_ERR_NOT_FOUND);
    _tearDown();
}

static void _test_sync_drain_per_subscription(void)
{
    _setUp();
    assert(sem_init(&s_entered, 0, 0) == 0);
    assert(sem_init(&s_release, 0, 0) == 0);

    capture_t idle_capture = {0};
    event_bus_sub_handle_t blocking_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t idle_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _blocking_cb, NULL,
                               EVENT_BUS_DISPATCH_PUBLISHER,
                               &blocking_handle) == ESP_OK);
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture,
                               &idle_capture, EVENT_BUS_DISPATCH_PUBLISHER,
                               &idle_handle) == ESP_OK);

    pthread_t thread;
    assert(pthread_create(&thread, NULL, _publish_thread, NULL) == 0);
    assert(sem_wait(&s_entered) == 0);

    /* Only the blocking subscription is in flight. */
    assert(event_bus_unsubscribe_sync(idle_handle, 1000U) == ESP_OK);
    assert(event_bus_unsubscribe_sync(blocking_handle, 20U) == ESP_ERR_TIMEOUT);

    assert(sem_post(&s_release) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(event_bus_flush_sync(1000U) == ESP_OK);
    assert(event_bus_unsubscribe_sync(blocking_handle, 0U) == ESP_OK);

    (void)sem_destroy(&s_entered);
    (void)sem_destroy(&s_release);
    _tearDown();
}

static void _test_subscriber_capacity(void)
{
    _setUp();
    capture_t captures[EVENT_BUS_MAX_SUBSCRIBERS + 1U];
    memset(captures, 0, sizeof(captures));
    event_bus_sub_handle_t handles[EVENT_BUS_MAX_SUBSCRIBERS + 1U];
    for (size_t i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; ++i)
    {
        assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture,
                                   &captures[i], EVENT_BUS_DISPATCH_PUBLISHER,
                                   &handles[i]) == ESP_OK);
    }
    event_bus_sub_handle_t extra = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &captures[0],
                               EVENT_BUS_DISPATCH_PUBLISHER, &extra)
           == ESP_ERR_NO_MEM);
    assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, NULL, 0, 0) == ESP_OK);
    for (size_t i = 0; i < EVENT_BUS_MAX_SUBSCRIBERS; ++i)
    {
        assert(captures[i].count == 1U);
    }
    _tearDown();
}

static void _test_argument_validation(void)
{
    _setUp();
    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    capture_t capture = {0};
    assert(event_bus_subscribe(NULL, TEST_SUB_TYPE, _capture, &capture,
                               EVENT_BUS_DISPATCH_PUBLISHER, &handle)
           == ESP_ERR_INVALID_ARG);
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, NULL, &capture,
                               EVENT_BUS_DISPATCH_PUBLISHER, &handle)
           == ESP_ERR_INVALID_ARG);
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &capture,
                               (event_bus_dispatch_context_t)7, &handle)
           == ESP_ERR_INVALID_ARG);
    assert(event_bus_publish(NULL, 0, NULL, 0, 0) == ESP_ERR_INVALID_ARG);
    assert(event_bus_publish(TEST_MSG, 0, NULL, 1, 0) == ESP_ERR_INVALID_ARG);
    assert(event_bus_publish(TEST_MSG, 0, NULL, 0, UINT32_C(1) << 7)
           == ESP_ERR_INVALID_ARG);
    assert(event_bus_unsubscribe(EVENT_BUS_SUB_HANDLE_INVALID)
           == ESP_ERR_INVALID_ARG);
    _tearDown();
}

static void _test_late_subscriber_stat(void)
{
    _setUp();
    capture_t first = {0};
    capture_t late = {0};
    event_bus_sub_handle_t first_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    event_bus_sub_handle_t late_handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &first,
                               EVENT_BUS_DISPATCH_UI, &first_handle) == ESP_OK);

    const uint32_t value = 7U;
    assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, &value, sizeof(value),
                             EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &late,
                               EVENT_BUS_DISPATCH_UI, &late_handle) == ESP_OK);
    _fake_drain();
    assert(first.count == 1U);
    assert(late.count == 0U);

    event_bus_stats_t stats;
    assert(event_bus_get_stats(&stats) == ESP_OK);
    assert(stats.late_subscriber_skips == 1U);
    _tearDown();
}

static void _test_publish_ex_error_report(void)
{
    _setUp();
    event_bus_publish_report_t report =
    {
        .sync_result = ESP_FAIL,
        .ui_result = ESP_FAIL,
        .sync_delivered = 7U,
    };
    assert(event_bus_publish_ex(NULL, 0, NULL, 0, 0, &report)
           == ESP_ERR_INVALID_ARG);
    assert(report.matched == 0U);
    assert(report.sync_delivered == 0U);
    assert(report.ui_queued == false);
    assert(report.ui_coalesced == false);
    assert(report.sync_result == ESP_OK);
    assert(report.ui_result == ESP_OK);

    assert(event_bus_publish_ex(TEST_MSG, 0, NULL, 1, 0, &report)
           == ESP_ERR_INVALID_ARG);
    assert(report.sync_result == ESP_OK);
    assert(report.ui_result == ESP_OK);
    _tearDown();
}

static void _test_ui_requires_dispatcher(void)
{
    _reset_dispatcher();
    assert(event_bus_init() == ESP_OK);
    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    capture_t capture = {0};
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &capture,
                               EVENT_BUS_DISPATCH_UI, &handle)
           == ESP_ERR_INVALID_STATE);
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &capture,
                               EVENT_BUS_DISPATCH_PUBLISHER, &handle) == ESP_OK);
    event_bus_deinit();
}

static void _test_deinit_reinit(void)
{
    _setUp();
    capture_t capture = {0};
    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &capture,
                               EVENT_BUS_DISPATCH_PUBLISHER, &handle) == ESP_OK);
    assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, NULL, 0, 0) == ESP_OK);
    assert(capture.count == 1U);
    _tearDown();

    _setUp();
    assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, NULL, 0, 0) == ESP_OK);
    assert(capture.count == 1U);
    assert(event_bus_unsubscribe(handle) == ESP_ERR_NOT_FOUND);
    _tearDown();
}

static void _test_stats(void)
{
    _setUp();
    capture_t capture = {0};
    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    assert(event_bus_subscribe(TEST_MSG, TEST_SUB_TYPE, _capture, &capture,
                               EVENT_BUS_DISPATCH_UI, &handle) == ESP_OK);
    const uint32_t value = 7U;
    assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, &value, sizeof(value),
                             EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);
    assert(event_bus_publish(TEST_MSG, TEST_SUB_TYPE, &value, sizeof(value),
                             EVENT_BUS_PUBLISH_FLAG_UI_LATEST) == ESP_OK);
    event_bus_stats_t stats;
    assert(event_bus_get_stats(&stats) == ESP_OK);
    assert(stats.publishes == 2U);
    assert(stats.coalesced == 1U);
    assert(stats.ui_batches == 1U);
    assert(stats.envelope_peak == 1U);
    _fake_drain();
    assert(capture.count == 1U);
    _tearDown();
}

static void _test_long_lived_visibility(void)
{
    event_bus_core_t core;
    event_bus_payload_t payloads[EVENT_BUS_MAX_PENDING_UI_BATCHES];
    event_bus_core_init(&core, payloads);

    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    const int slot = event_bus_core_subscribe(&core, TEST_MSG, TEST_SUB_TYPE,
        _capture, NULL, EVENT_BUS_DISPATCH_PUBLISHER, &handle);
    assert(slot >= 0);
    assert(core.subscriptions[slot].subscribe_seq == 1U);

    size_t decoded_slot = SIZE_MAX;
    uint64_t decoded_seq = 0;
    assert(event_bus_core_decode_handle(handle, &decoded_slot, &decoded_seq));
    assert(decoded_slot == (size_t)slot);
    assert(decoded_seq == 1U);

    /* The old 32-bit predicate flipped after 2^31 sequence allocations. */
    core.seq = UINT64_C(2147483648);
    const uint64_t far_seq = event_bus_core_next_seq(&core);
    assert(far_seq == UINT64_C(2147483649));
    assert(event_bus_core_visible(1U, far_seq));

    size_t ui = 0;
    size_t sync = 0;
    event_bus_core_count(&core, TEST_MSG, TEST_SUB_TYPE, far_seq, &ui, &sync);
    assert(ui == 0U);
    assert(sync == 1U);

    const event_bus_target_t target = event_bus_core_acquire(&core,
        (size_t)slot, TEST_MSG, TEST_SUB_TYPE,
        EVENT_BUS_DISPATCH_PUBLISHER, far_seq);
    assert(target.live);
    assert(event_bus_core_in_flight(&core) == 1U);
    assert(event_bus_core_slot_in_flight(&core, (size_t)slot) == 1U);
    event_bus_core_release(&core, (size_t)slot);
    assert(event_bus_core_in_flight(&core) == 0U);
    assert(event_bus_core_slot_in_flight(&core, (size_t)slot) == 0U);

    /* Four billion allocations also stay visible. */
    core.seq = UINT64_C(4294967295);
    const uint64_t wider_seq = event_bus_core_next_seq(&core);
    assert(wider_seq == UINT64_C(4294967296));
    assert(event_bus_core_visible(1U, wider_seq));
    assert(event_bus_core_unsubscribe(&core, handle) == ESP_OK);
}

static void _test_handle_generation_wrap(void)
{
    event_bus_core_t core;
    event_bus_payload_t payloads[EVENT_BUS_MAX_PENDING_UI_BATCHES];
    event_bus_core_init(&core, payloads);

    /* The low 56 bits encode the handle generation and must never be zero. */
    core.seq = (UINT64_C(1) << 56) - 1U;
    event_bus_sub_handle_t handle = EVENT_BUS_SUB_HANDLE_INVALID;
    const int slot = event_bus_core_subscribe(&core, TEST_MSG, TEST_SUB_TYPE,
        _capture, NULL, EVENT_BUS_DISPATCH_PUBLISHER, &handle);
    assert(slot >= 0);
    assert(core.seq == (UINT64_C(1) << 56) + 1U);
    assert(handle != EVENT_BUS_SUB_HANDLE_INVALID);

    size_t decoded_slot = SIZE_MAX;
    uint64_t decoded_seq = 0;
    assert(event_bus_core_decode_handle(handle, &decoded_slot, &decoded_seq));
    assert(decoded_slot == (size_t)slot);
    assert(decoded_seq == 1U);
    assert(event_bus_core_unsubscribe(&core, handle) == ESP_OK);
}

static void _test_core_slot_quarantine(void)
{
    event_bus_core_t core;
    event_bus_payload_t payloads[EVENT_BUS_MAX_PENDING_UI_BATCHES];
    event_bus_core_init(&core, payloads);

    event_bus_sub_handle_t first = EVENT_BUS_SUB_HANDLE_INVALID;
    const int first_slot = event_bus_core_subscribe(&core, TEST_MSG,
        TEST_SUB_TYPE, _capture, NULL,
        EVENT_BUS_DISPATCH_PUBLISHER, &first);
    assert(first_slot >= 0);
    const uint64_t seq = event_bus_core_next_seq(&core);
    const event_bus_target_t target = event_bus_core_acquire(&core,
        (size_t)first_slot, TEST_MSG, TEST_SUB_TYPE,
        EVENT_BUS_DISPATCH_PUBLISHER, seq);
    assert(target.live);
    assert(event_bus_core_unsubscribe(&core, first) == ESP_OK);

    event_bus_sub_handle_t second = EVENT_BUS_SUB_HANDLE_INVALID;
    const int second_slot = event_bus_core_subscribe(&core, TEST_MSG,
        TEST_SUB_TYPE, _capture, NULL,
        EVENT_BUS_DISPATCH_PUBLISHER, &second);
    assert(second_slot >= 0);
    assert(second_slot != first_slot);

    event_bus_core_release(&core, (size_t)first_slot);
    assert(event_bus_core_slot_in_flight(&core, (size_t)first_slot) == 0U);
    event_bus_sub_handle_t third = EVENT_BUS_SUB_HANDLE_INVALID;
    const int third_slot = event_bus_core_subscribe(&core, TEST_MSG,
        TEST_SUB_TYPE, _capture, NULL,
        EVENT_BUS_DISPATCH_PUBLISHER, &third);
    assert(third_slot == first_slot);
    assert(event_bus_core_unsubscribe(&core, second) == ESP_OK);
    assert(event_bus_core_unsubscribe(&core, third) == ESP_OK);
}

static void _test_core_batch_states(void)
{
    event_bus_core_t core;
    event_bus_payload_t payloads[EVENT_BUS_MAX_PENDING_UI_BATCHES];
    event_bus_core_init(&core, payloads);

    const uint64_t seq = event_bus_core_next_seq(&core);
    const uint32_t value = 1U;
    const int envelope = event_bus_core_begin_batch(&core, TEST_MSG,
        TEST_SUB_TYPE, &value, sizeof(value), true, seq);
    assert(envelope >= 0);
    assert(core.pending_batches == 1U);
    assert(event_bus_core_find_admitted(&core, TEST_MSG, TEST_SUB_TYPE)
           == EVENT_BUS_SLOT_NONE);
    event_bus_core_commit_batch(&core, envelope, ESP_OK);
    assert(event_bus_core_find_admitted(&core, TEST_MSG, TEST_SUB_TYPE)
           == envelope);

    const uint32_t newer = 2U;
    assert(event_bus_core_replace(&core, envelope, &newer, sizeof(newer),
                                  seq + 1U));
    assert(core.envelopes[envelope].publish_seq == seq + 1U);

    event_bus_core_free_batch(&core, envelope);
    assert(core.pending_batches == 0U);
    assert(core.envelopes[envelope].state == EVENT_BUS_ENV_FREE);
    for (size_t i = 0; i < sizeof(newer); ++i)
    {
        assert(payloads[envelope].bytes[i] == 0U);
    }
}

static void _test_core_reset_wipes_payloads(void)
{
    event_bus_core_t core;
    event_bus_payload_t payloads[EVENT_BUS_MAX_PENDING_UI_BATCHES];
    memset(payloads, 0xAA, sizeof(payloads));

    event_bus_core_init(&core, payloads);
    for (size_t i = 0; i < sizeof(payloads); ++i)
    {
        assert(((const uint8_t *)payloads)[i] == 0U);
    }

    const uint64_t seq = event_bus_core_next_seq(&core);
    memset(payloads, 0xAA, sizeof(payloads));
    event_bus_core_reset(&core);
    assert(core.seq == seq);
    for (size_t i = 0; i < sizeof(payloads); ++i)
    {
        assert(((const uint8_t *)payloads)[i] == 0U);
    }
}

static void _test_core_abandon_skips_executing(void)
{
    event_bus_core_t core;
    event_bus_payload_t payloads[EVENT_BUS_MAX_PENDING_UI_BATCHES];
    event_bus_core_init(&core, payloads);

    const uint64_t seq = event_bus_core_next_seq(&core);
    const int admitted = event_bus_core_begin_batch(&core, TEST_MSG,
        TEST_SUB_TYPE, NULL, 0, true, seq);
    assert(admitted >= 0);
    event_bus_core_commit_batch(&core, admitted, ESP_OK);
    const int executing = event_bus_core_begin_batch(&core, TEST_MSG,
        TEST_SUB_TYPE, NULL, 0, false, seq);
    assert(executing >= 0);
    core.envelopes[executing].state = EVENT_BUS_ENV_EXECUTING;

    assert(event_bus_core_abandon(&core) == 1U);
    assert(core.envelopes[admitted].state == EVENT_BUS_ENV_FREE);
    assert(core.envelopes[executing].state == EVENT_BUS_ENV_EXECUTING);
    event_bus_core_free_batch(&core, executing);
    assert(core.pending_batches == 0U);
}

int main(void)
{
    _test_sync_delivery();
    _test_subtype_filter();
    _test_handle_generation();
    _test_ui_delivery();
    _test_ui_latest_coalescing();
    _test_ui_latest_visibility();
    _test_publish_isolation();
    _test_atomic_publish();
    _test_oversize_isolation();
    _test_ui_batch_depth();
    _test_abandon_releases_pool();
    _test_unsubscribe_sync();
    _test_unsubscribe_sync_idle();
    _test_sync_drain_per_subscription();
    _test_subscriber_capacity();
    _test_argument_validation();
    _test_ui_requires_dispatcher();
    _test_deinit_reinit();
    _test_late_subscriber_stat();
    _test_publish_ex_error_report();
    _test_stats();
    _test_long_lived_visibility();
    _test_handle_generation_wrap();
    _test_core_slot_quarantine();
    _test_core_batch_states();
    _test_core_reset_wipes_payloads();
    _test_core_abandon_skips_executing();
    (void)puts("event_bus host regression: OK");
    return 0;
}
