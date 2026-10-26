**English** | [简体中文](README_ZH.md)

# connectivity_manager

Wi-Fi connection policy owner: a policy worker runs the scan, connect,
reconnect, configuration persistence, and auto-connect state machine, and
delegates low-level radio operations to `wifi_service`.

## Public API

Public header `include/connectivity_manager.h`:

- `connectivity_manager_init()` / `deinit()` / `suspend()` / `resume()`:
  lifecycle and standby.
- `connectivity_manager_clear_persisted_profile()`: idempotently erase the
  persisted profile before initialization (reset domain).
- Foreground commands: `request_scan()`, `request_connect()`,
  `request_sync_profile()`, `request_save_profile()`, `request_disconnect()`,
  `request_reconnect_saved()`, `request_forget()`, `set_auto_connect()`,
  `cancel()`; all return a nonzero `operation_id`.
- `connectivity_manager_get_status()` / `get_scan_snapshot()`: read the
  immutable state or scan snapshot, which never contains the password.
- `connectivity_manager_is_available()`: whether the public API is available.
- Publishes status and scan snapshot events through
  `EVENT_BUS_DECLARE_ID(CONNECTIVITY_MANAGER_MSG)`.

## Configuration and Dependencies

- Kconfig: `CONNECTIVITY_MANAGER_TASK_STACK` (default 6144),
  `CONNECTIVITY_MANAGER_QUEUE_DEPTH` (default 8).
- CMake: `REQUIRES event_bus`; `PRIV_REQUIRES freertos mbedtls mt_log nvs_flash nv_storage wifi_service`.
- At runtime the service is configured by `connectivity_manager_config_t` (the
  policy worker priority and the low-level Wi-Fi worker priority).

## State and Concurrency

- Singleton; a statically allocated policy worker, low-level Wi-Fi worker,
  command queue, and semaphores.
- Foreground operations are identified by an `operation_id` generation; status
  and scan snapshots carry a `generation`.
- A connection command deep-copies the candidate credentials at admission, so
  the caller may release its buffer after the call returns.
- Pending foreground commands and immediate-execution commands share a single
  dispatcher; the SYNC_PROFILE candidate credentials and sync identity use the
  same deep-copy and persistence path.
- CONNECT/DISCONNECT consume only an explicit terminal that matches the current
  session and a nonzero radio operation ID; a link IP_READY alone does not prove
  the candidate succeeded. Only a successful terminal plus a currently valid
  IPv4 commits the candidate profile, and an address renewal does not write the
  profile again.
- The terminal outbox is drained before each policy phase; during backpressure,
  pending/retry/resume intents are not consumed. After the drain the same worker
  advances on its own without an extra driving event; STOPPING does not reopen
  connections.
- A suspended explicit CONNECT/SYNC retains the original manager operation, the
  candidate, and the sync metadata; resume restarts only the executor, and the
  policy phase continues with the new radio ID/epoch. Cancellation and
  replacement publish one local terminal state and do not send cancel to the
  retired radio ID0.
- A retry's absolute deadline is preserved across standby; a failed resume
  control is not retried automatically and does not discard the candidate. When
  the low layer is actually SUSPENDED, the parked intent is saved for an
  explicit resume rollback even if the control returned the original stop error.

## Test Boundary

- No `tests/host`.
- The host does not cover the Wi-Fi driver, RF, association/DHCP timing, or NVS
  power loss; those are covered by `wifi_service` and on-device verification.
