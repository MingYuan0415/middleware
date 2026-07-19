#include "host_event_bus.h"
#include "host_freertos.h"
#include "host_imu.h"
#include "host_timer.h"
#include "imu_service.h"
#include "test_common.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct test_call_context
{
    esp_err_t result;
    atomic_bool done;
} test_call_context_t;

static void *_read_thread(void *context)
{
    test_call_context_t *call = context;
    imu_service_sample_t sample;
    call->result = imu_service_read(&sample);
    atomic_store_explicit(&call->done, true, memory_order_release);
    return NULL;
}

static void *_suspend_thread(void *context)
{
    test_call_context_t *call = context;
    call->result = imu_service_suspend(IMU_SERVICE_WAIT_FOREVER);
    atomic_store_explicit(&call->done, true, memory_order_release);
    return NULL;
}

static void *_stop_thread(void *context)
{
    test_call_context_t *call = context;
    call->result = imu_service_stop(IMU_SERVICE_WAIT_FOREVER);
    atomic_store_explicit(&call->done, true, memory_order_release);
    return NULL;
}

static bool _start_service_without_worker_reads(void)
{
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    TEST_CHECK(host_freertos_reset());
    host_event_bus_reset();
    host_imu_reset();
    host_timer_reset();
    TEST_CHECK(imu_service_register_ops(host_imu_ops()) == ESP_OK);
    TEST_CHECK(imu_service_init() == ESP_OK);
    TEST_CHECK(host_event_bus_wait_for_count(
                   IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED, 1U, 1000U));

    host_imu_set_available(false);
    TEST_CHECK(host_event_bus_wait_for_count(
                   IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED, 2U, 1000U));
    return true;
}

static bool _test_suspend_drains_synchronous_read(void)
{
    TEST_CHECK(_start_service_without_worker_reads());
    host_imu_block_reads(true);

    test_call_context_t read_call = {.result = ESP_FAIL};
    atomic_init(&read_call.done, false);
    pthread_t reader;
    TEST_CHECK(pthread_create(&reader, NULL, _read_thread, &read_call) == 0);
    TEST_CHECK(host_imu_wait_for_blocked_read(1000U));

    test_call_context_t suspend_call = {.result = ESP_FAIL};
    atomic_init(&suspend_call.done, false);
    pthread_t suspender;
    TEST_CHECK(pthread_create(&suspender, NULL, _suspend_thread,
                              &suspend_call) == 0);
    TEST_CHECK(test_wait_for_state(IMU_SERVICE_STATE_PAUSED, 1000U));
    test_sleep_ms(20U);
    TEST_CHECK(!atomic_load_explicit(&suspend_call.done,
                                     memory_order_acquire));
    TEST_CHECK(host_imu_disable_count() == 0U);
    TEST_CHECK(host_freertos_active_mutex_count() == 2U);

    host_imu_block_reads(false);
    TEST_CHECK(pthread_join(reader, NULL) == 0);
    TEST_CHECK(pthread_join(suspender, NULL) == 0);
    TEST_CHECK(read_call.result == ESP_OK);
    TEST_CHECK(suspend_call.result == ESP_OK);
    TEST_CHECK(host_imu_disable_count() == 1U);

    imu_service_sample_t sample;
    TEST_CHECK(imu_service_read(&sample) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_resume(500U) == ESP_OK);
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    return true;
}

static bool _test_stop_drains_synchronous_read(void)
{
    TEST_CHECK(_start_service_without_worker_reads());
    host_imu_block_reads(true);

    test_call_context_t read_call = {.result = ESP_FAIL};
    atomic_init(&read_call.done, false);
    pthread_t reader;
    TEST_CHECK(pthread_create(&reader, NULL, _read_thread, &read_call) == 0);
    TEST_CHECK(host_imu_wait_for_blocked_read(1000U));

    test_call_context_t stop_call = {.result = ESP_FAIL};
    atomic_init(&stop_call.done, false);
    pthread_t stopper;
    TEST_CHECK(pthread_create(&stopper, NULL, _stop_thread, &stop_call) == 0);
    TEST_CHECK(test_wait_for_state(IMU_SERVICE_STATE_STOPPING, 1000U));
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    test_sleep_ms(20U);
    TEST_CHECK(!atomic_load_explicit(&stop_call.done, memory_order_acquire));
    TEST_CHECK(host_imu_disable_count() == 0U);
    TEST_CHECK(host_freertos_active_mutex_count() == 2U);
    TEST_CHECK(host_freertos_active_event_group_count() == 1U);

    host_imu_block_reads(false);
    TEST_CHECK(pthread_join(reader, NULL) == 0);
    TEST_CHECK(pthread_join(stopper, NULL) == 0);
    TEST_CHECK(read_call.result == ESP_OK);
    TEST_CHECK(stop_call.result == ESP_OK);
    TEST_CHECK(host_imu_disable_count() == 1U);
    TEST_CHECK(host_freertos_active_mutex_count() == 0U);
    TEST_CHECK(host_freertos_active_event_group_count() == 0U);
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    return true;
}

static bool _run_all_tests(void)
{
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(_test_suspend_drains_synchronous_read());
    TEST_CHECK(_test_stop_drains_synchronous_read());
    return true;
}

int main(void)
{
    return _run_all_tests() ? 0 : 1;
}
