**English** | [简体中文](README_ZH.md)

# imu_service

Periodically samples the on-board IMU (QMI8658C), maintains the latest
snapshot, and publishes snapshot, availability, and interrupt events over the
event bus.

## Public API

Public header `include/imu_service.h`:

- `imu_service_register_ops()` / `register_imu_ops()`: registers the
  board-level ops table before start (`read` required, the rest optional).
- `imu_service_init()` / `start()`: creates the sampling worker and begins
  continuous sampling.
- `imu_service_stop()` / `deinit()`: stops the worker; deinit releases the
  worker synchronization objects and clears the ops table.
- `imu_service_suspend()` / `resume()`: suspends or resumes sampling; while
  suspended the sensor power gate may optionally be turned off.
- `imu_service_get_state()`: worker lifecycle state.
- `imu_service_get_snapshot()`: copies the cached snapshot without accessing
  the hardware.
- `imu_service_read()` / `read_sample()`: synchronous hardware read serialized
  against the worker read.
- Publishes the SNAPSHOT_UPDATE, AVAILABILITY_CHANGED, and INTERRUPT subtypes
  of `IMU_SERVICE_MSG`.

## Configuration and Dependencies

- Kconfig: `IMU_SERVICE_TASK_STACK` (default 3072).
- `idf_component.yml`: only `idf: ">=5.0"`.
- CMake: `REQUIRES event_bus`; `PRIV_REQUIRES mt_log esp_timer`.
- At runtime the sampling rate and task priority come from
  `imu_service_config_t` plus the registered ops table.

## State and Concurrency

- Singleton; pinned worker task, mutex, critical section, and event group.
- The synchronous read and the worker read share the same serialization
  boundary; reads are rejected while suspending, resuming, or stopping.
- The service fills in the monotonic timestamp and sequence number when it
  accepts a sample.

## Test Boundary

- `tests/host` exists and covers the worker lifecycle, the snapshot/read
  paths, and suspend semantics.
- The host does not cover on-device sensor I2C, INT1 level, or ODR timing.
