**English** | [简体中文](README_ZH.md)

# connectivity_manager

Wi-Fi connection policy owner: a policy worker owns the saved-network list and
runs one bounded scan-match-connect cycle whenever Wi-Fi is enabled. Low-level
radio operations are delegated to `wifi_service`.

## Connection policy

`connectivity_manager_request_wifi_enable()` starts one cycle:

1. the radio is initialized lazily, then started (or resumed);
2. the manager scans for the saved networks and receives one presence bit per
   saved profile from `wifi_service`;
3. only profiles present in the scan are attempted, starting with the selected
   profile and then following the in-RAM attempt order;
4. each present profile receives at most
   `CONNECTIVITY_MANAGER_MAX_CONNECT_FAILURES` (3) attempts. Authentication
   failures stop retries of that profile immediately, but the cycle continues
   with the next saved profile; transient association, DHCP, and link-loss
   failures consume its budget;
5. the first successful IPv4 link becomes the selected profile;
6. when no profile connects, or no saved profile is present, the manager stops
   the radio and clears `wifi_enabled`. The switch therefore never remains on
   while the radio is idle. A single cycle is bounded by
   `CONNECTIVITY_MANAGER_CYCLE_MAX_MS` (360 s by default, including one bounded
   scan retry).

A scan error is retried once before it is classified as a radio failure; a
successful scan with an empty match is an `AP_NOT_FOUND` result and closes the
cycle. A foreground `request_scan()` requested during a connect attempt runs
immediately after that attempt's terminal and then resumes the cycle.

`connectivity_manager_request_wifi_disable()` cancels an active scan/connect
cycle, stops the radio, and leaves the saved list untouched. `wifi_enabled` is
the persistent user intent; `manual_hold` describes a deliberately stopped
radio and is not a persisted setting. A connected link lost while Wi-Fi is
enabled starts a fresh scan cycle.

The former automatic-connection switch is removed. `wifi_enabled` is the single
persistent policy: it is saved with the profile record and restored on boot.
The radio is not initialized during boot; a persisted-on cycle starts only after
the first frame and startup commit. A save (`request_save_profile()`) only
stores credentials and never starts a connection. `request_forget()` clears
every saved entry and the persisted record.

## Public API

Public header `include/connectivity_manager.h`:

- `connectivity_manager_init()` / `deinit()` / `suspend()` / `resume()`:
  lifecycle and standby.
- `connectivity_manager_clear_persisted_profile()`: idempotently erase the
  persisted profile before initialization (reset domain).
- Foreground commands: `request_scan()`, `request_wifi_enable()`,
  `request_wifi_disable()`, `request_connect()`, `request_sync_profile()`,
  `request_save_profile()`, `request_connect_network()`,
  `request_forget_network()`, `request_forget()`, `cancel()`; all return a
  nonzero `operation_id`. Every admitted operation has exactly one terminal,
  including storage and radio failure paths.
- `connectivity_manager_get_status()` / `get_scan_snapshot()` /
  `get_profiles()`: read the immutable state, scan, or saved-network snapshot;
  none contains a password.
- Publishes status, scan, and saved-network snapshot events through
  `EVENT_BUS_DECLARE_ID(CONNECTIVITY_MANAGER_MSG)`.

## Saved networks

The manager keeps up to `CONNECTIVITY_MANAGER_MAX_PROFILES` (five) saved
networks and persists them as one list. The count is fixed because the persisted
record reserves five slots.

- `request_save_profile()` upserts by SSID. A save is refused with
  `ESP_ERR_NO_MEM` when every slot holds a different network. Saving does not
  enable Wi-Fi.
- One entry is selected. `request_connect_network()` starts a cycle preferring
  the named entry; `request_wifi_enable()` starts at the selected entry. A
  failed cycle never reselects anything.
- `request_forget_network()` removes one entry. A removal carrying the active
  link is completed after the disconnect terminal; otherwise the link and
  policy are preserved.
- `request_forget()` clears every entry and leaves Wi-Fi disabled.
- A refused write is rolled back in RAM so the list never advertises a
  deletion that did not reach flash.

### Attempt order and scan matching

The attempt order lives in RAM and only changes when a profile is saved,
removed, or selected. A cycle scans once; the scan result contains a presence
mask for every saved profile, not only the five records shown in the UI. A
profile that is absent is skipped and consumes no attempt. `status.ssid` keeps
naming the selected profile; `status.attempt_index` is the candidate profile
index, `status.attempt_ordinal` is its 1-based candidate number, and
`status.attempt_total` is the matched candidate count.

## Configuration and Dependencies

- Kconfig: `CONNECTIVITY_MANAGER_TASK_STACK` (default 8192),
  `CONNECTIVITY_MANAGER_QUEUE_DEPTH` (default 8).
- `CONNECTIVITY_MANAGER_MAX_CONNECT_FAILURES` is a compile-time constant (3).
- CMake: `REQUIRES event_bus`; `PRIV_REQUIRES freertos mbedtls mt_log nvs_flash nv_storage wifi_service`.
- Runtime configuration is `connectivity_manager_config_t`.

## State and Concurrency

- Singleton; statically allocated policy worker, low-level Wi-Fi worker,
  command queue, and semaphores.
- Foreground operations carry a nonzero `operation_id`; every operation has a
  terminal. A failed cycle publishes exactly one terminal and clears the
  operation instead of parking a foreground operation in a long backoff.
- Policy persistence and radio stopping are separate steps. A failed NVS write
  is reported as `STORAGE`; it cannot skip the radio stop.
- `wifi_enabled` is the persistent intent, `manual_hold` the current deliberate
  stop state, and `radio_available` the executor health. They are not
  interchangeable.
- The manager is woken by Wi-Fi service events and command submission; idle
  waits use a deadline or `portMAX_DELAY`, not a fixed 20 ms poll.
- System standby uses `connectivity_manager_suspend()` / `resume()` separately
  from user Wi-Fi disable. Resume restarts a cycle only when `wifi_enabled` is
  still true.

## Persisted record

The `wifi_profile` NVS key holds a versioned record: magic, version, count,
active index, `wifi_enabled`, revision, Device Link sync window, and five fixed
103-byte entry slots. v1/v2/v3 single-profile records and the 592-byte v4
multi-profile record migrate to v5; v4's `auto_connect` byte becomes
`wifi_enabled`.

## Test Boundary

- `tests/connectivity` links the real manager and executor with a controlled
  radio boundary and covers profile migration, bounded cycles, scan matching,
  disable/re-enable, suspend/resume, and operation cancellation.
- The host does not cover the Wi-Fi driver, RF, association/DHCP timing, or NVS
  power loss; those require on-device verification.
