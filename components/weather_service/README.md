**English** | [简体中文](README_ZH.md)

# weather_service

Background weather fetch, cache, and immutable-snapshot service covering the
location, current, hourly, daily, and alert datasets.

## Public API

Public header `include/weather_service.h`:

- `weather_service_init()` / `deinit()` / `suspend()` / `resume()`: lifecycle
  and standby.
- `weather_service_set_network_ready()`: reports IPv4 availability; the ready
  edge triggers one location session.
- `weather_service_request_refresh()`: requests one user-initiated refresh,
  limited by the local and upstream Retry-After.
- `weather_service_notify_time_changed()`: asks the worker to re-evaluate
  snapshot freshness after time synchronization.
- `weather_service_get_status()`: small status snapshot.
- `weather_service_snapshot_acquire()` / `snapshot_release()`: reference-counted
  acquire or release of the immutable full snapshot.
- `weather_service_is_available()`: whether the public API is available.
- Publishes the SNAPSHOT subtype of `WEATHER_SERVICE_MSG` (coalescible UI
  notification).

## Configuration and Dependencies

- Kconfig: `WEATHER_SERVICE_TASK_STACK_SIZE` (default 8192),
  `WEATHER_SERVICE_MAX_RESPONSE_BYTES` (default 262144).
- `idf_component.yml`: `idf: ">=5.5"`, `espressif/cjson: "^1.7.19"`.
- CMake: `REQUIRES event_bus`;
  `PRIV_REQUIRES mt_log esp_http_client esp_timer mbedtls espressif__cjson freertos heap`.
- At runtime it is supplied by `weather_service_config_t` (server URL, device
  token, cache directory, refresh period, `allow_private_http`).

## State and Concurrency

- Singleton; worker task, lifecycle mutex, state mutex, and event group.
- Public calls are admitted only while the lifecycle is ready; teardown closes
  admission, waits for in-flight callers, and rejects new calls until cleanup
  completes. `deinit(timeout_ms)` uses one total deadline across lifecycle
  acquisition, state acquisition, caller quiescence and worker shutdown. A
  timed-out deinit retains cleanup-pending ownership and the next deinit retries
  final release instead of reinitializing over live resources.
- Snapshots are shared through acquire/release reference counting; the state
  machine covers unconfigured, waiting-for-network, locating, updating, ready,
  degraded, auth error, rate-limited, suspended, and error.
- Each network cycle carries a lifecycle epoch and session identity. A response,
  parse result, cache completion, status write, or event from an older cycle is
  discarded instead of committing into a resumed or reinitialized service.
- `fetched_at` records device receipt time. Local expiry deadlines run without a
  network refresh, including while connectivity is unavailable.

## Test Boundary

- `tests/host` exists and covers parsing, caching, and the state machine.
- Host coverage includes async HTTPS polling and cancellation, private HTTP
  mode selection, response/parse lifecycle races, offline expiry, and device
  receipt timestamps. Cancellation interrupts an active connected client;
  generation invalidation also suppresses DNS/TCP/TLS pre-connect completions
  that IDF cannot cancel before `HTTP_STATE_CONNECTED`.
- The host does not cover real HTTPS/TLS, DNS, positioning, or upstream API
  behavior.
