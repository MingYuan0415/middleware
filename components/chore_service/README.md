**English** | [简体中文](README_ZH.md)

# chore_service

General-purpose bounded background worker: runs short, bounded background jobs
(periodic maintenance, snapshot formatting, small storage queries, and so on)
on a dedicated task.

## Public API

Public header `include/chore_service.h`:

- `chore_service_init()` / `chore_service_deinit()`: initialize or stop the
  singleton worker; deinit cancels every job and releases each job argument
  before returning.
- `chore_service_submit()`: submit a one-shot or periodic job (`delay_ms`,
  `period_ms`); on success the argument ownership transfers to the service.
- `chore_service_cancel()`: request cooperative cancellation and wait for the
  job to go quiet.
- `chore_service_suspend()` / `chore_service_resume()`: suspend or resume the
  worker around light sleep.
- `chore_service_get_status()` / `chore_service_is_available()`: status snapshot
  and availability query.
- `chore_service_cancel_pending()`: the job polls the cancellation token in its
  run loop.

## Configuration and Dependencies

- Kconfig: `CHORE_SERVICE_TASK_STACK_SIZE` (default 4096),
  `CHORE_SERVICE_JOB_CAPACITY` (default 8, 1..16).
- `idf_component.yml`: only `idf: ">=5.0"`.
- CMake: `PRIV_REQUIRES mt_log esp_timer freertos heap`.
- The runtime policy is supplied by `chore_service_config_t` (worker priority,
  duration warning threshold).

## State and Concurrency

- Singleton; one pinned worker task and a fixed-capacity job pool.
- A mutex protects the queue and an event group acknowledges cancellation per
  slot; `run`/`release` execute in the worker context and must not call LVGL or
  block for long.
- Job handles carry a generation, so an old handle never matches a new job after
  an instance switch.

## Test Boundary

- `tests/host` exists and covers submission, cancellation, suspend, and deinit
  drain together with the concurrency semantics.
- The host does not cover on-device task scheduling timing or actual stack
  watermark values.
