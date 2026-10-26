**English** | [简体中文](README_ZH.md)

# power_service

Periodically polls the on-board PMU (AXP2101) for battery and charging
information and for interrupt state, maintains a cached snapshot, and
publishes it over the event bus.

## Public API

Public header `include/power_service.h`:

- `power_service_register_power_ops()`: registers the board-level ops table
  before initialization (`get_info` required, availability and IRQ polling
  optional).
- `power_service_init()` / `deinit()`: starts or stops the sampling worker.
- `power_service_suspend()` / `resume()`: suspends or resumes sampling around
  standby.
- `power_service_get_snapshot()`: copies the cached snapshot (including
  validity).
- `power_service_get_info()`: copies the most recent valid power information.
- Publishes the SNAPSHOT_UPDATE, AVAILABILITY_CHANGED, and IRQ subtypes of
  `POWER_SERVICE_MSG`.

## Configuration and Dependencies

- Kconfig: `POWER_SERVICE_TASK_STACK` (default 3072).
- `idf_component.yml`: only `idf: ">=5.0"`.
- CMake: `REQUIRES event_bus`; `PRIV_REQUIRES mt_log esp_timer`.
- At runtime the telemetry period, IRQ poll period, and task priority come
  from `power_service_config_t` plus the registered ops table.

## State and Concurrency

- Singleton; pinned worker task, mutex, critical section, and event group.
- Snapshot reads never access the PMU; IRQ edges are published as
  non-coalesced events.

## Test Boundary

- `tests/host` exists and covers the snapshot cache, availability edges, and
  the IRQ path.
- The host does not cover PMU I2C, the interrupt line, or the actual battery
  electrical characteristics.
