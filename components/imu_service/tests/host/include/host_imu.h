#ifndef __IMU_SERVICE_HOST_IMU_H__
#define __IMU_SERVICE_HOST_IMU_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "imu_service.h"

/** @brief Reset the fake IMU to an available, successful device. */
void host_imu_reset(void);
/** @brief Return the fake operation table. */
const imu_service_imu_ops_t *host_imu_ops(void);
/** @brief Change the availability probe result. */
void host_imu_set_available(bool available);
/** @brief Change the result returned by reads. */
void host_imu_set_read_result(esp_err_t result);
/** @brief Change the result returned by sensor configuration. */
void host_imu_set_configure_result(esp_err_t result);
/** @brief Change the raw sample returned by reads. */
void host_imu_set_sample(const imu_service_sample_t *sample);
/** @brief Change the result returned by interrupt polling. */
void host_imu_set_poll_result(esp_err_t result);
/** @brief Install interrupt levels consumed in order, holding the last level. */
void host_imu_set_interrupt_levels(const bool *levels, size_t count);
/** @brief Script STATUSINT values returned by successive hardware reads. */
void host_imu_set_status_int_values(const uint8_t *values, size_t count);
/** @brief Change the result returned when enabling the device. */
void host_imu_set_enable_result(esp_err_t result);
/** @brief Change the result returned when disabling the device. */
void host_imu_set_disable_result(esp_err_t result);
/** @brief Block or release fake hardware reads. */
void host_imu_block_reads(bool blocked);
/** @brief Wait until a fake hardware read is blocked. */
bool host_imu_wait_for_blocked_read(uint32_t timeout_ms);
/** @brief Wait until at least count reads have completed. */
bool host_imu_wait_for_reads(uint32_t count, uint32_t timeout_ms);
/** @brief Return the completed read count. */
uint32_t host_imu_read_count(void);
/** @brief Return the number of configuration calls. */
uint32_t host_imu_configure_count(void);
/** @brief Return the most recently requested hardware sample rate. */
uint32_t host_imu_configured_sample_rate_hz(void);
/** @brief Return the number of enable calls. */
uint32_t host_imu_enable_count(void);
/** @brief Return the number of disable calls. */
uint32_t host_imu_disable_count(void);

#endif /* __IMU_SERVICE_HOST_IMU_H__ */
