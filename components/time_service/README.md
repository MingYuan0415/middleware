**English** | [简体中文](README_ZH.md)

# time_service

Singleton service for the system clock, RTC bridging, and SNTP synchronization;
it also manages RTC calendar alarms and alarm interrupt events.

## Public API

Public header `include/time_service.h`:

- `time_service_register_rtc_ops()`: registers board-level RTC operations before
  initialization (read, write, alarm, and interrupt polling; all optional).
- `time_service_init()` / `deinit()` / `suspend()` / `resume()`: lifecycle and
  standby; RTC/SNTP control operations are rejected while suspended.
- `time_service_get_utc()` / `get_local()` / `set_local()`: read UTC or local
  time, set the local time and write it back to the RTC on a best-effort basis.
- `time_service_get_quality()` / `get_last_rtc_error()`: clock trustworthiness
  and the most recent RTC result.
- `time_service_alarm_configure()` / `alarm_disable()` / `alarm_get_status()` /
  `alarm_clear()`: repeating calendar alarm control.
- `time_service_set_network_ready()` / `request_sync()` / `cancel_sync()` /
  `wait_sync()` / `sync_ntp()`: SNTP synchronization control; `get_time` /
  `set_time` are compatibility wrappers.
- Publishes the RTC_ALARM subtype of `TIME_SERVICE_MSG`.

## Configuration and Dependencies

- Kconfig: `TIME_SERVICE_SYNC_WORKER_STACK` (default 3072).
- `idf_component.yml`: only `idf: ">=5.0"`.
- CMake: `REQUIRES event_bus`; `PRIV_REQUIRES mt_log nv_storage esp_netif lwip`.
- At runtime `time_service_config_t` provides the POSIX time zone, the SNTP
  server, and the task priority.

## State and Concurrency

- Singleton; a pinned SNTP worker, a mutex, and an event group.
- Multiple RTC operations may block on I2C; lifecycle control calls must be
  serialized.
- Online, offline, and suspended stop or queue SNTP requests; clock
  trustworthiness is expressed by `time_service_quality_t`.

## Test Boundary

- `tests/host` exists and covers the clock, alarm, and SNTP state machines.
- The host does not cover RTC I2C, real networking, or SNTP server behavior.
