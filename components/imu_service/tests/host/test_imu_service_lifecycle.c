#include "host_event_bus.h"
#include "host_freertos.h"
#include "host_imu.h"
#include "host_timer.h"
#include "imu_service.h"
#include "test_common.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool _run_lifecycle_test(void)
{
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(host_freertos_reset());
    host_event_bus_reset();
    host_imu_reset();
    host_timer_reset();

    imu_service_imu_ops_t invalid_ops;
    memset(&invalid_ops, 0, sizeof(invalid_ops));
    imu_service_snapshot_t snapshot;
    imu_service_sample_t sample;
    TEST_CHECK(imu_service_register_ops(NULL) == ESP_ERR_INVALID_ARG);
    TEST_CHECK(imu_service_register_ops(&invalid_ops) == ESP_ERR_INVALID_ARG);
    TEST_CHECK(imu_service_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    TEST_CHECK(imu_service_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_read(NULL) == ESP_ERR_INVALID_ARG);
    TEST_CHECK(imu_service_read(&sample) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_suspend(1U) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_resume(1U) == ESP_ERR_INVALID_STATE);

    const imu_service_sample_t raw_sample =
    {
        .acceleration_mps2 = {.x = 1.25F, .y = -2.5F, .z = 9.75F},
        .angular_velocity_dps = {.x = 10.0F, .y = 20.0F, .z = -30.0F},
        .temperature_c = 27.5F,
        .sensor_timestamp = UINT32_C(0x12345678),
        .status_int = 0xA5U,
        .status0 = 0x11U,
        .status1 = 0x22U,
        .data_ready = true,
    };
    const bool interrupt_levels[] = {false};
    const uint8_t status_int_values[] = {0xA7U, 0xA5U, 0xA7U};
    host_imu_set_sample(&raw_sample);
    host_imu_set_interrupt_levels(interrupt_levels,
                                  sizeof(interrupt_levels) /
                                  sizeof(interrupt_levels[0]));
    host_imu_set_status_int_values(status_int_values,
                                   sizeof(status_int_values) /
                                   sizeof(status_int_values[0]));

    TEST_CHECK(imu_service_register_imu_ops(host_imu_ops()) == ESP_OK);
    TEST_CHECK(imu_service_start(test_imu_config()) == ESP_OK);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_RUNNING);
    TEST_CHECK(imu_service_init(test_imu_config()) == ESP_OK);
    TEST_CHECK(imu_service_register_ops(host_imu_ops()) ==
               ESP_ERR_INVALID_STATE);
    TEST_CHECK(host_imu_configure_count() == 1U);
    TEST_CHECK(host_imu_configured_sample_rate_hz() ==
               test_imu_config()->sample_rate_hz);
    TEST_CHECK(host_imu_enable_count() == 1U);
    TEST_CHECK(host_freertos_active_mutex_count() == 2U);
    TEST_CHECK(host_freertos_active_event_group_count() == 1U);
    TEST_CHECK(host_freertos_active_task_count() == 1U);

    TEST_CHECK(host_event_bus_wait_for_count(
                   IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED, 1U, 1000U));
    TEST_CHECK(host_event_bus_wait_for_count(
                   IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE, 3U, 1000U));
    TEST_CHECK(host_event_bus_wait_for_count(
                   IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT, 2U, 1000U));
    TEST_CHECK(host_event_bus_count(IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT) == 2U);
    TEST_CHECK(host_event_bus_flags(
                   IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED) == 0U);
    TEST_CHECK(host_event_bus_flags(
                   IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE) ==
               EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
    TEST_CHECK(host_event_bus_flags(IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT) == 0U);

    TEST_CHECK(host_event_bus_get_snapshot(
                   IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE, &snapshot));
    TEST_CHECK(snapshot.available);
    TEST_CHECK(snapshot.valid);
    TEST_CHECK(snapshot.sequence != 0U);
    TEST_CHECK(snapshot.sample.sequence == snapshot.sequence);
    TEST_CHECK(snapshot.sample.sampled_at_us == snapshot.sampled_at_us);
    TEST_CHECK(snapshot.sampled_at_us >= INT64_C(1000000));
    TEST_CHECK(snapshot.sample.acceleration_mps2.x == 1.25F);
    TEST_CHECK(snapshot.sample.angular_velocity_dps.z == -30.0F);
    TEST_CHECK(snapshot.sample.temperature_c == 27.5F);
    TEST_CHECK(snapshot.sample.sensor_timestamp == UINT32_C(0x12345678));

    TEST_CHECK(host_event_bus_get_interrupt(&sample));
    TEST_CHECK(sample.interrupt_level_valid);
    TEST_CHECK(sample.interrupt_active);
    TEST_CHECK((sample.status_int & 0x02U) != 0U);
    TEST_CHECK(sample.sequence != 0U);
    TEST_CHECK(sample.sampled_at_us >= INT64_C(1000000));

    TEST_CHECK(imu_service_get_snapshot(&snapshot) == ESP_OK);
    TEST_CHECK(snapshot.available && snapshot.valid);
    TEST_CHECK(imu_service_read_sample(&sample) == ESP_OK);
    TEST_CHECK(sample.acceleration_mps2.y == -2.5F);
    TEST_CHECK(sample.interrupt_level_valid && !sample.interrupt_active);
    TEST_CHECK(sample.sequence != 0U);
    TEST_CHECK(sample.sampled_at_us >= INT64_C(1000000));

    const uint32_t availability_count = host_event_bus_count(
                                            IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED);
    host_imu_set_available(false);
    TEST_CHECK(host_event_bus_wait_for_count(
                   IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED,
                   availability_count + 1U, 1000U));
    TEST_CHECK(host_event_bus_get_snapshot(
                   IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED, &snapshot));
    TEST_CHECK(!snapshot.available && !snapshot.valid);
    host_imu_set_available(true);
    TEST_CHECK(host_event_bus_wait_for_count(
                   IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED,
                   availability_count + 2U, 1000U));
    TEST_CHECK(host_event_bus_wait_for_count(
                   IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE, 4U, 1000U));

    TEST_CHECK(imu_service_suspend(500U) == ESP_OK);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_PAUSED);
    TEST_CHECK(host_imu_disable_count() == 1U);
    TEST_CHECK(imu_service_read(&sample) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_suspend(500U) == ESP_OK);
    TEST_CHECK(host_imu_disable_count() == 1U);
    const uint32_t paused_read_count = host_imu_read_count();
    test_sleep_ms(20U);
    TEST_CHECK(host_imu_read_count() == paused_read_count);

    TEST_CHECK(imu_service_resume(500U) == ESP_OK);
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_RUNNING);
    TEST_CHECK(host_imu_enable_count() == 2U);
    TEST_CHECK(imu_service_resume(500U) == ESP_OK);
    TEST_CHECK(host_imu_enable_count() == 2U);
    TEST_CHECK(host_imu_wait_for_reads(paused_read_count + 1U, 1000U));

    TEST_CHECK(imu_service_stop(500U) == ESP_OK);
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    TEST_CHECK(imu_service_get_state() == IMU_SERVICE_STATE_STOPPED);
    TEST_CHECK(host_imu_disable_count() == 2U);
    TEST_CHECK(host_freertos_active_mutex_count() == 0U);
    TEST_CHECK(host_freertos_active_event_group_count() == 0U);
    TEST_CHECK(host_freertos_active_task_count() == 0U);
    TEST_CHECK(imu_service_stop(1U) == ESP_OK);
    TEST_CHECK(imu_service_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_read(&sample) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_suspend(1U) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_resume(1U) == ESP_ERR_INVALID_STATE);

    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(imu_service_init(test_imu_config()) == ESP_OK);
    TEST_CHECK(imu_service_read(&sample) == ESP_ERR_INVALID_STATE);
    TEST_CHECK(imu_service_stop(500U) == ESP_OK);
    TEST_CHECK(host_freertos_wait_for_tasks(1000U));
    TEST_CHECK(imu_service_deinit() == ESP_OK);
    TEST_CHECK(host_freertos_active_mutex_count() == 0U);
    TEST_CHECK(host_freertos_active_event_group_count() == 0U);
    TEST_CHECK(host_freertos_active_task_count() == 0U);
    return true;
}

int main(void)
{
    return _run_lifecycle_test() ? 0 : 1;
}
