#include "host_event_bus.h"
#include "host_freertos.h"
#include "host_imu.h"
#include "host_timer.h"
#include "imu_service.h"
#include "test_common.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum init_failure_case
{
    INIT_FAILURE_FIRST_MUTEX = 0,
    INIT_FAILURE_SECOND_MUTEX,
    INIT_FAILURE_EVENT_GROUP,
    INIT_FAILURE_CONFIGURE,
    INIT_FAILURE_ENABLE,
    INIT_FAILURE_TASK,
} init_failure_case_t;

static bool _reset_fakes(void)
{
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    TEST_CHECK(host_freertos_reset());
    host_event_bus_reset();
    host_imu_reset();
    host_timer_reset();
    return true;
}

static bool _run_init_failure(init_failure_case_t failure)
{
    TEST_CHECK(_reset_fakes());
    TEST_CHECK(imu_service_register_ops(host_imu_ops()) == ESP_OK);

    esp_err_t expected = ESP_ERR_NO_MEM;
    switch (failure)
    {
    case INIT_FAILURE_FIRST_MUTEX:
        host_freertos_fail_mutex_create_on(1U);
        break;
    case INIT_FAILURE_SECOND_MUTEX:
        host_freertos_fail_mutex_create_on(2U);
        break;
    case INIT_FAILURE_EVENT_GROUP:
        host_freertos_fail_event_group_create(true);
        break;
    case INIT_FAILURE_CONFIGURE:
        host_imu_set_configure_result(ESP_FAIL);
        expected = ESP_FAIL;
        break;
    case INIT_FAILURE_ENABLE:
        host_imu_set_enable_result(ESP_FAIL);
        expected = ESP_FAIL;
        break;
    case INIT_FAILURE_TASK:
        host_freertos_fail_task_create(true);
        break;
    }

    TEST_CHECK(imu_service_init(test_imu_config()) == expected);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_STOPPED);
    TEST_CHECK(host_freertos_active_mutex_count() == 0U);
    TEST_CHECK(host_freertos_active_event_group_count() == 0U);
    TEST_CHECK(host_freertos_active_task_count() == 0U);
    TEST_CHECK(imu_service_suspend(1U) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_resume(1U) == ESP_ERR_INVALID_STATE);
    if (failure == INIT_FAILURE_CONFIGURE || failure == INIT_FAILURE_ENABLE ||
            failure == INIT_FAILURE_TASK)
    {
        TEST_CHECK(host_imu_configure_count() == 1U);
        TEST_CHECK(host_imu_configured_sample_rate_hz() ==
                   test_imu_config()->sample_rate_hz);
    }
    if (failure == INIT_FAILURE_CONFIGURE)
    {
        TEST_CHECK(host_imu_enable_count() == 0U);
        TEST_CHECK(host_imu_disable_count() == 0U);
    }
    if (failure == INIT_FAILURE_ENABLE || failure == INIT_FAILURE_TASK)
    {
        TEST_CHECK(host_imu_enable_count() == 1U);
        TEST_CHECK(host_imu_disable_count() == 1U);
    }
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    return true;
}

static bool _test_read_and_publish_failures(void)
{
    TEST_CHECK(_reset_fakes());
    host_imu_set_read_result(ESP_FAIL);
    TEST_CHECK(imu_service_register_ops(host_imu_ops()) == ESP_OK);
    TEST_CHECK(imu_service_init(test_imu_config()) == ESP_OK);
    TEST_CHECK(host_imu_wait_for_reads(1U, 1000U));

    imu_service_snapshot_t snapshot;
    TEST_CHECK(imu_service_get_snapshot(&snapshot) == ESP_OK);
    TEST_CHECK(snapshot.available);
    TEST_CHECK(!snapshot.valid);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_RUNNING);

    host_event_bus_set_result(ESP_FAIL);
    host_imu_set_read_result(ESP_OK);
    TEST_CHECK(host_event_bus_wait_for_attempts(
                   IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE, 1U, 1000U));
    TEST_CHECK(host_event_bus_count(
                   IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE) == 0U);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_RUNNING);

    host_event_bus_set_result(ESP_OK);
    TEST_CHECK(host_event_bus_wait_for_count(
                   IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE, 1U, 1000U));
    TEST_CHECK(imu_service_get_snapshot(&snapshot) == ESP_OK);
    TEST_CHECK(snapshot.available && snapshot.valid);

    TEST_CHECK(imu_service_stop(500U) == ESP_OK);
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(host_freertos_active_mutex_count() == 0U);
    TEST_CHECK(host_freertos_active_event_group_count() == 0U);
    return true;
}

static bool _test_interrupt_publish_retry(void)
{
    TEST_CHECK(_reset_fakes());
    const uint8_t status_int_values[] = {0x02U, 0x00U};
    host_imu_set_status_int_values(status_int_values,
                                   sizeof(status_int_values) /
                                   sizeof(status_int_values[0]));
    host_event_bus_set_result(ESP_FAIL);
    TEST_CHECK(imu_service_register_ops(host_imu_ops()) == ESP_OK);
    TEST_CHECK(imu_service_init(test_imu_config()) == ESP_OK);
    TEST_CHECK(host_event_bus_wait_for_attempts(
                   IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT, 2U, 1000U));
    TEST_CHECK(host_imu_read_count() == 1U);
    TEST_CHECK(host_event_bus_count(
                   IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT) == 0U);

    host_event_bus_set_result(ESP_OK);
    TEST_CHECK(host_event_bus_wait_for_count(
                   IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT, 1U, 1000U));
    TEST_CHECK(host_imu_wait_for_reads(2U, 1000U));
    imu_service_sample_t sample;
    TEST_CHECK(host_event_bus_get_interrupt(&sample));
    TEST_CHECK((sample.status_int & 0x02U) != 0U);
    TEST_CHECK(sample.interrupt_active);
    TEST_CHECK(host_event_bus_flags(IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT) == 0U);

    TEST_CHECK(imu_service_stop(500U) == ESP_OK);
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    return true;
}

static bool _test_task_and_disable_failure_retry(void)
{
    TEST_CHECK(_reset_fakes());
    TEST_CHECK(imu_service_register_ops(host_imu_ops()) == ESP_OK);
    host_freertos_fail_task_create(true);
    host_imu_set_disable_result(ESP_FAIL);

    TEST_CHECK(imu_service_init(test_imu_config()) == ESP_ERR_NO_MEM);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_ERROR);
    TEST_CHECK(host_imu_enable_count() == 1U);
    TEST_CHECK(host_imu_disable_count() == 1U);
    TEST_CHECK(host_freertos_active_mutex_count() == 2U);
    TEST_CHECK(host_freertos_active_event_group_count() == 1U);
    TEST_CHECK(host_freertos_active_task_count() == 0U);

    TEST_CHECK(imu_service_deinit() == ESP_FAIL);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_ERROR);
    TEST_CHECK(host_imu_disable_count() == 2U);
    TEST_CHECK(host_freertos_active_mutex_count() == 2U);
    TEST_CHECK(host_freertos_active_event_group_count() == 1U);

    host_imu_set_disable_result(ESP_OK);
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(host_imu_disable_count() == 3U);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_STOPPED);
    TEST_CHECK(host_freertos_active_mutex_count() == 0U);
    TEST_CHECK(host_freertos_active_event_group_count() == 0U);
    return true;
}

static bool _run_all_tests(void)
{
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(_run_init_failure(INIT_FAILURE_FIRST_MUTEX));
    TEST_CHECK(_run_init_failure(INIT_FAILURE_SECOND_MUTEX));
    TEST_CHECK(_run_init_failure(INIT_FAILURE_EVENT_GROUP));
    TEST_CHECK(_run_init_failure(INIT_FAILURE_CONFIGURE));
    TEST_CHECK(_run_init_failure(INIT_FAILURE_ENABLE));
    TEST_CHECK(_run_init_failure(INIT_FAILURE_TASK));
    TEST_CHECK(_test_task_and_disable_failure_retry());
    TEST_CHECK(_test_read_and_publish_failures());
    TEST_CHECK(_test_interrupt_publish_retry());
    return true;
}

int main(void)
{
    return _run_all_tests() ? 0 : 1;
}
