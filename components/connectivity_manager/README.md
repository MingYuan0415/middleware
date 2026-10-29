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
  `request_sync_profile()`, `request_save_profile()`,
  `request_connect_network()`, `request_forget_network()`,
  `request_disconnect()`, `request_reconnect_saved()`, `request_forget()`,
  `set_auto_connect()`, `cancel()`; all return a nonzero `operation_id`.
- `connectivity_manager_get_status()` / `get_scan_snapshot()` /
  `get_profiles()`: read the immutable state, scan, or saved-network snapshot;
  none of them contains a password.
- `connectivity_manager_is_available()`: whether the public API is available.
- Publishes status, scan, and saved-network snapshot events through
  `EVENT_BUS_DECLARE_ID(CONNECTIVITY_MANAGER_MSG)`.

## Saved networks

The manager keeps up to `CONNECTIVITY_MANAGER_MAX_PROFILES` (five) saved networks
and persists them as one list, so several credentials survive reboot. The count is
a fixed compile-time constant rather than a configuration item: the persisted
record already reserves five slots, so a tunable limit would only add a second
width to reason about.

- `request_save_profile()` upserts by SSID: an existing entry is replaced, a new
  one appended. A save is refused with `ESP_ERR_NO_MEM` when every slot holds a
  different network, so nothing is silently evicted. Saving also enables
  automatic connection, which is what makes a provisioned network usable after
  the next boot.
- One entry is the selected profile. `request_connect_network()` starts at a named
  entry, while `request_reconnect_saved()` starts at the selected one; the named
  entry becomes selected only once its link succeeds. A failed attempt never
  reselects anything, so the choice the user made survives a sweep that tried
  every other network.
- `request_forget_network()` removes one entry. When that entry carries the link,
  the disconnect terminal removes it; otherwise the established link, its state,
  and the automatic-connect policy are preserved, and the radio does not enter the
  manual-offline hold. `request_forget()` is the contract-level FORGET and clears
  every entry, which is what makes the Device Link `profile_ssid` empty.
- A removal whose write is refused is rolled back in RAM, so the list never
  advertises a deletion that did not reach flash.
- `status.profile_count`, `status.saved_profile`, and the Device Link
  `profile_ssid` projection describe the saved list and the selected entry, not
  an arbitrary one; a failed sweep keeps both pointing at the selected entry.

### Attempt order

The attempt order lives in RAM and only changes when a profile is saved, removed,
or selected; connecting does not write flash to record it.

When more than one network is saved, one connect operation sweeps the list: the
starting entry first, then the remaining entries in attempt order. A classified
`AP_NOT_FOUND`, `ASSOCIATION_TIMEOUT`, `DHCP_TIMEOUT`, or `LINK_LOST` advances to
the next candidate; an authentication failure ends the sweep so a wrong password
surfaces instead of silently connecting elsewhere, and a radio-level failure ends
it because the radio is at fault. An exhausted sweep enters the long-backoff
policy rather than publishing a failure terminal, and cancellation ends it. The
sweep keeps its own cursor: it never rewrites the selected profile, so exhausting
every candidate leaves the user's choice, and its persisted `active_index`,
untouched.

`status.ssid` therefore keeps naming the selected profile throughout a sweep,
which is what Device Link's `profile_ssid` projects. The candidate actually being
tried is reported separately through `status.attempt_index` and
`status.attempt_total` (`UINT8_MAX` and `0` when no sweep runs), so a presenter can
say which network an attempt is on without redefining what the profile SSID means.

Each sweep attempt runs with `wifi_service_connect_options_t` retry limit 0, so a
missing access point reaches the policy layer immediately instead of consuming
the executor's internal backoff. With a single saved network the manager passes
no option block, so the executor keeps its compiled retry budget.

## Configuration and Dependencies

- Kconfig: `CONNECTIVITY_MANAGER_TASK_STACK` (default 8192),
  `CONNECTIVITY_MANAGER_QUEUE_DEPTH` (default 8),
  `CONNECTIVITY_MANAGER_MAX_PROFILES` (fixed 5, not configurable).
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

## Persisted record

The `wifi_profile` NVS key holds one versioned record: a header (magic, version,
count, active index, auto-connect policy, revision, and the Device Link sync
window) followed by five fixed 103-byte entry slots. The in-RAM list and the
record share that width, so a valid record always loads whole. A record whose
count exceeds five is structurally invalid and is rejected rather than partially
loaded.

A v1 (112-byte), v2 (128-byte), or v3 (168-byte) single-profile record is
upgraded on load into a one-entry list and rewritten; if that write fails the
original bytes stay intact and initialization reports a storage failure. An
invalid record is rejected without erasing flash, and a refused write never
leaves the RAM list advertising an entry that is not durable. The writer refuses
to encode a record whose selected index does not address a retained entry.

## Test Boundary

- No component-local `tests/host`; `tests/connectivity` links the real manager
  and executor with a controlled radio boundary and covers profile migration,
  retry/sweep, cancellation, suspend/resume and queue overflow.
- The host does not cover the Wi-Fi driver, RF, association/DHCP timing, or NVS
  power loss; those are covered by `wifi_service` and on-device verification.
