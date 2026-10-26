**English** | [简体中文](README_ZH.md)

# timer_service

Singleton service for countdown, stopwatch, and focus (work/rest) cycle timing;
completion edges are published through an optional callback.

## Public API

Public header `include/timer_service.h`:

- `timer_service_init()` / `deinit()`: initialize or stop the service; the
  configuration provides `monotonic_time_us` and an optional completion
  callback.
- `timer_service_get_snapshot()`: reads the unified snapshot (state of the three
  timers, remaining or elapsed time, focus cycle count).
- `timer_service_countdown_start()` / `pause()` / `resume()` / `reset()`:
  countdown control.
- `timer_service_stopwatch_start()` / `pause()` / `reset()`: stopwatch control.
- `timer_service_focus_start()` / `pause()` / `resume()` / `reset()`: focus
  cycle control.
- Completion event type `timer_service_completion_event_t` (countdown or focus
  cycle).

## Configuration and Dependencies

- CMake: `REQUIRES esp_common freertos`.
- `idf_component.yml`: only `idf: ">=5.0"`.
- No Kconfig; the time source is the `monotonic_time_us` injected by the
  assembly layer.

## State and Concurrency

- Singleton static runtime; an atomic spinlock protects the snapshot and state.
- The optional completion callback is dispatched outside the lock and may be
  triggered by a worker or by an API caller; the state machine is
  IDLE / RUNNING / PAUSED / COMPLETED.

## Test Boundary

- `tests/host` exists and covers the countdown, stopwatch, and focus state
  machines and the completion edges.
- The host does not cover on-device clock drift or task scheduling timing.
