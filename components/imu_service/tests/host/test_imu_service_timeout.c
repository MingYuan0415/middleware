#include "host_event_bus.h"
#include "host_freertos.h"
#include "host_imu.h"
#include "host_timer.h"
#include "imu_service.h"
#include "test_common.h"

#include <stdbool.h>
#include <stdint.h>

static bool _start_service(void)
{
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    TEST_CHECK(host_freertos_reset());
    host_event_bus_reset();
    host_imu_reset();
    host_timer_reset();
    TEST_CHECK(imu_service_register_ops(host_imu_ops()) == ESP_OK);
    TEST_CHECK(imu_service_init(test_imu_config()) == ESP_OK);
    TEST_CHECK(host_imu_wait_for_reads(1U, 1000U));
    return true;
}

static bool _test_lifecycle_timeouts(void)
{
    TEST_CHECK(_start_service());

    host_freertos_block_notifications(true);
    const uint32_t suspend_notifications =
        host_freertos_notification_count();
    TEST_CHECK(imu_service_suspend(5U) == ESP_ERR_TIMEOUT);
    TEST_CHECK(host_freertos_notification_count() >=
               suspend_notifications + 2U);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_RESUME_PENDING);
    TEST_CHECK(host_freertos_active_mutex_count() == 2U);
    TEST_CHECK(host_freertos_active_event_group_count() == 1U);
    host_freertos_block_notifications(false);
    TEST_CHECK(test_wait_for_state(IMU_SERVICE_STATE_RUNNING, 1000U));

    TEST_CHECK(imu_service_suspend(500U) == ESP_OK);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_PAUSED);
    host_freertos_block_notifications(true);
    TEST_CHECK(imu_service_resume(5U) == ESP_ERR_TIMEOUT);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_RESUME_PENDING);
    host_freertos_block_notifications(false);
    TEST_CHECK(test_wait_for_state(IMU_SERVICE_STATE_RUNNING, 1000U));

    host_freertos_block_notifications(true);
    TEST_CHECK(imu_service_stop(5U) == ESP_ERR_TIMEOUT);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_STOPPING);
    TEST_CHECK(host_freertos_active_mutex_count() == 2U);
    TEST_CHECK(host_freertos_active_event_group_count() == 1U);
    TEST_CHECK(host_freertos_active_task_count() == 1U);
    host_freertos_block_notifications(false);
    TEST_CHECK(imu_service_stop(500U) == ESP_OK);
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_STOPPED);
    TEST_CHECK(host_freertos_active_mutex_count() == 0U);
    TEST_CHECK(host_freertos_active_event_group_count() == 0U);
    TEST_CHECK(host_freertos_active_task_count() == 0U);
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    return true;
}

static bool _test_suspend_failure_recovery(void)
{
    TEST_CHECK(_start_service());
    host_imu_set_disable_result(ESP_FAIL);
    TEST_CHECK(imu_service_suspend(500U) == ESP_FAIL);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_PAUSED);
    TEST_CHECK(host_imu_disable_count() == 1U);

    imu_service_sample_t sample;
    TEST_CHECK(imu_service_read(&sample) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_resume(500U) == ESP_OK);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_RUNNING);
    TEST_CHECK(host_imu_enable_count() == 2U);

    host_imu_set_disable_result(ESP_OK);
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(host_imu_disable_count() == 2U);
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    return true;
}

static bool _test_resume_failure_retry(void)
{
    TEST_CHECK(_start_service());
    TEST_CHECK(imu_service_suspend(500U) == ESP_OK);
    host_imu_set_enable_result(ESP_FAIL);
    TEST_CHECK(imu_service_resume(500U) == ESP_FAIL);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_PAUSED);
    TEST_CHECK(host_imu_enable_count() == 2U);

    host_imu_set_enable_result(ESP_OK);
    TEST_CHECK(imu_service_resume(500U) == ESP_OK);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_RUNNING);
    TEST_CHECK(host_imu_enable_count() == 3U);
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    return true;
}

static bool _test_disable_failure_retry(void)
{
    TEST_CHECK(_start_service());
    host_imu_set_disable_result(ESP_FAIL);
    TEST_CHECK(imu_service_deinit() == ESP_FAIL);
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_ERROR);
    TEST_CHECK(host_imu_disable_count() == 1U);
    TEST_CHECK(host_freertos_active_mutex_count() == 2U);
    TEST_CHECK(host_freertos_active_event_group_count() == 1U);
    TEST_CHECK(host_freertos_active_task_count() == 0U);

    host_imu_set_disable_result(ESP_OK);
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(host_imu_disable_count() == 2U);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_STOPPED);
    TEST_CHECK(host_freertos_active_mutex_count() == 0U);
    TEST_CHECK(host_freertos_active_event_group_count() == 0U);
    TEST_CHECK(host_freertos_active_task_count() == 0U);
    return true;
}

static bool _run_all_tests(void)
{
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(_test_lifecycle_timeouts());
    TEST_CHECK(_test_suspend_failure_recovery());
    TEST_CHECK(_test_resume_failure_retry());
    TEST_CHECK(_test_disable_failure_retry());
    return true;
}

int main(void)
{
    return _run_all_tests() ? 0 : 1;
}
