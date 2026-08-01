#ifndef __IMU_SERVICE_H__
#define __IMU_SERVICE_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One three-axis vector in the units named by its containing field. */
typedef struct imu_service_vector
{
    float x; /**< X axis. */
    float y; /**< Y axis. */
    float z; /**< Z axis. */
} imu_service_vector_t;

/**
 * @brief Raw hardware sample supplied by a BSP IMU operation table.
 *
 * The BSP owns conversion from sensor counts to m/s^2 and degrees/second. The
 * service fills sampled_at_us and sequence when it accepts a sample.
 */
typedef struct imu_service_sample
{
    imu_service_vector_t acceleration_mps2; /**< Acceleration in m/s^2. */
    imu_service_vector_t angular_velocity_dps; /**< Angular velocity in dps. */
    float temperature_c;                  /**< Sensor temperature. */
    uint32_t sensor_timestamp;            /**< Sensor-native timestamp. */
    uint8_t status_int;                   /**< Sensor interrupt status. */
    uint8_t status0;                      /**< Sensor status 0. */
    uint8_t status1;                      /**< Sensor status 1. */
    bool data_ready;                      /**< Hardware data-ready indication. */
    bool interrupt_active;                /**< Optional expander INT1 level. */
    bool interrupt_level_valid;           /**< Whether interrupt_active is valid. */
    int64_t sampled_at_us;                /**< Monotonic service timestamp. */
    uint32_t sequence;                    /**< Monotonic service sequence. */
} imu_service_sample_t;

/** @brief Latest sample and its availability metadata. */
typedef struct imu_service_snapshot
{
    imu_service_sample_t sample; /**< Most recent sample, if valid. */
    int64_t sampled_at_us;        /**< Copy of sample.sampled_at_us. */
    uint32_t sequence;            /**< Copy of sample.sequence. */
    bool valid;                   /**< True when the last read succeeded. */
    bool available;               /**< Current hardware availability probe. */
} imu_service_snapshot_t;

/** @brief Product-owned IMU worker configuration. */
typedef struct imu_service_config
{
    uint32_t sample_rate_hz; /**< Requested sensor sampling rate. */
    uint32_t task_priority;  /**< FreeRTOS worker priority. */
} imu_service_config_t;

/**
 * @brief Board operations consumed by the IMU worker.
 *
 * `read` is the only mandatory operation. All function pointers are copied at
 * registration time and must remain valid until imu_service_deinit().
 */
typedef struct imu_service_imu_ops
{
    bool (*is_available)(void); /**< Optional fast availability probe. */
    esp_err_t (*configure)(uint32_t sample_rate_hz); /**< Optional ODR setup. */
    esp_err_t (*read)(imu_service_sample_t *sample); /**< Read one sample. */
    esp_err_t (*set_enabled)(bool enabled); /**< Optional sensor power gate. */
    esp_err_t (*poll_interrupt)(bool *active); /**< Optional EXIO polling. */
} imu_service_imu_ops_t;

/** @brief Compatibility alias for callers that name the table simply ops. */
typedef imu_service_imu_ops_t imu_service_ops_t;

EVENT_BUS_DECLARE_ID(IMU_SERVICE_MSG);

/** @brief Events published by the IMU worker. */
typedef enum
{
    IMU_SERVICE_MSG_SUB_TYPE_SNAPSHOT_UPDATE = 1,
    IMU_SERVICE_MSG_SUB_TYPE_AVAILABILITY_CHANGED,
    /** Observed INT1 assertion from status or the external interrupt level. */
    IMU_SERVICE_MSG_SUB_TYPE_INTERRUPT,
} imu_service_msg_sub_type_t;

/** @brief Worker lifecycle states. */
typedef enum
{
    IMU_SERVICE_STATE_STOPPED = 0,
    IMU_SERVICE_STATE_STARTING,
    IMU_SERVICE_STATE_RUNNING,
    IMU_SERVICE_STATE_PAUSE_PENDING,
    IMU_SERVICE_STATE_PAUSED,
    IMU_SERVICE_STATE_RESUME_PENDING,
    IMU_SERVICE_STATE_STOPPING,
    IMU_SERVICE_STATE_ERROR,
} imu_service_state_t;

/** @brief Sentinel accepted by suspend/resume/stop to wait without a deadline. */
#define IMU_SERVICE_WAIT_FOREVER UINT32_MAX

/**
 * @brief Register board operations before starting the worker.
 *
 * @param ops operation table; `read` must be non-NULL.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_STATE while active.
 */
esp_err_t imu_service_register_ops(const imu_service_imu_ops_t *ops);

/** @brief Explicitly named alias for imu_service_register_ops(). */
esp_err_t imu_service_register_imu_ops(const imu_service_imu_ops_t *ops);

/**
 * @brief Start continuous sampling and latest-snapshot publication.
 *
 * @return ESP_OK when running; an ESP-IDF error when resources cannot be
 *         created or a lifecycle transition is already in progress.
 */
esp_err_t imu_service_init(const imu_service_config_t *config);

/** @brief Alias for imu_service_init(). */
esp_err_t imu_service_start(const imu_service_config_t *config);

/**
 * @brief Stop the sampling worker and release worker synchronization objects.
 *
 * Lifecycle control APIs must be serialized by the caller. Synchronous reads
 * may execute concurrently and are quiesced by this function.
 *
 * @param timeout_ms maximum wait, or IMU_SERVICE_WAIT_FOREVER.
 * @return ESP_OK when stopped; ESP_ERR_TIMEOUT when the stop can be retried.
 */
esp_err_t imu_service_stop(uint32_t timeout_ms);

/**
 * @brief Stop the worker forever and clear registered board operations.
 *
 * The caller must serialize this function with other lifecycle control APIs.
 *
 * @return ESP_OK when all worker resources are released.
 */
esp_err_t imu_service_deinit(void);

/**
 * @brief Pause sampling and disable the non-wakeup sensor.
 *
 * The worker and synchronous reads are quiesced before the optional BSP
 * power gate is disabled. Synchronous reads are rejected until resume.
 * The caller must serialize this function with other lifecycle control APIs.
 *
 * @param timeout_ms maximum wait, or IMU_SERVICE_WAIT_FOREVER.
 * @return ESP_OK when paused; ESP_ERR_TIMEOUT when cancellation is needed.
 */
esp_err_t imu_service_suspend(uint32_t timeout_ms);

/**
 * @brief Resume sampling after a pause.
 *
 * The caller must serialize this function with other lifecycle control APIs.
 *
 * @param timeout_ms maximum wait, or IMU_SERVICE_WAIT_FOREVER.
 * @return ESP_OK when running; otherwise an ESP-IDF error.
 */
esp_err_t imu_service_resume(uint32_t timeout_ms);

/** @brief Return the current worker state. */
imu_service_state_t imu_service_get_state(void);

/**
 * @brief Copy the latest cached snapshot without touching hardware.
 *
 * @param snapshot receives the snapshot.
 * @return ESP_OK while initialized, or ESP_ERR_INVALID_ARG/INVALID_STATE.
 */
esp_err_t imu_service_get_snapshot(imu_service_snapshot_t *snapshot);

/**
 * @brief Perform one synchronous hardware read.
 *
 * This function bypasses the worker cache and serializes the operation with
 * worker reads. The service must be running and a BSP `read` operation must
 * be registered. Reads are rejected while pausing, paused, resuming, or
 * stopping.
 *
 * @param sample receives one sample and service metadata.
 * @return ESP_OK on a successful read; otherwise an ESP-IDF error.
 */
esp_err_t imu_service_read(imu_service_sample_t *sample);

/** @brief Compatibility alias for imu_service_read(). */
esp_err_t imu_service_read_sample(imu_service_sample_t *sample);

#ifdef __cplusplus
}
#endif

#endif /* __IMU_SERVICE_H__ */
