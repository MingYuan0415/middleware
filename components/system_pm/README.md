**English** | [简体中文](README_ZH.md)

# system_pm

System light-sleep assembly: configures the EXT1 GPIO wake source, runs the
prepare/complete hooks serially, and provides sleep commit validation and a CPU
maximum-frequency lock.

## Public API

Public header `include/system_pm.h`:

- `system_pm_init()` / `deinit()`: copies the configuration, creates the standby
  worker and the CPU frequency lock.
- `system_pm_request_standby()`: enqueues one asynchronous standby transaction.
- `system_pm_cancel_standby()`: cancels a queued preparation or a retried
  standby resume.
- `system_pm_acquire_cpu_max_freq()` / `release_cpu_max_freq()`: acquire or
  release the maximum-frequency PM lock.
- Types: `system_pm_config_t`, `system_pm_wake_source_t`,
  `system_pm_wake_event_t`, `system_pm_sleep_hook_t`,
  `system_pm_commit_guard_t`, `system_pm_wake_reason_t`.

## Configuration and Dependencies

- Kconfig: `SYSTEM_PM_STANDBY_TASK_STACK` (default 4096).
- `idf_component.yml`: only `idf: ">=5.0"`.
- CMake: `PRIV_REQUIRES mt_log esp_pm esp_driver_gpio esp_hw_support`.
- At runtime `system_pm_config_t` provides the wake source, the required
  prepare/complete hooks, the optional wake/commit callbacks, and the task
  priority.

## State and Concurrency

- Singleton; a pinned standby worker task, a mutex, a critical section, and an
  event group.
- Hooks and wake callbacks run in the worker context; a wake callback must only
  enqueue or notify and return immediately.
- The commit guard performs the final validation at the sleep commit point; the
  commit callback only records without blocking.

## Test Boundary

- No `tests/host`.
- The host does not cover real light-sleep entry and wake, RTC GPIO electrical
  behavior, or power consumption.
