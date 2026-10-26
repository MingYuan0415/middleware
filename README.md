**English** | [简体中文](README_ZH.md)

# MicroTech Middleware

This repository provides the general-purpose ESP-IDF service components of the
MicroTech firmware. The components are mostly singleton services and cover
logging, event dispatch, NVS, time/RTC alarm, Wi-Fi, weather, power sampling and
IRQ, IMU, audio, removable SD storage, and system light sleep; ESP-IDF 5.0 or
later is required.

## Components

| Component | Responsibility and current state | Direct component dependencies |
| --- | --- | --- |
| `mt_log` | Wraps ESP-IDF log levels at compile time per file `DBG_LVL` (falling back to the Kconfig global cap `MT_LOG_DEFAULT_LEVEL` when undefined), and provides a unified `exit` error-handling macro | none |
| `event_bus` | Fixed-capacity, thread-safe publish/subscribe bus supporting two dispatch contexts, publisher task and UI worker | `mt_log` (private) |
| `nv_storage` | Manages the default NVS partition, provides a scalar K-V API and fixed-pool Blob registration/validation/default fallback | `freertos`, `nvs_flash`, `mt_log` (private) |
| `power_service` | Samples the PMU through the board-level ops table and consumes latched IRQs on an independent period; caches snapshots and publishes state/IRQ events | `event_bus`; `mt_log`, `esp_timer` (private) |
| `imu_service` | Periodically samples the QMI8658C adapter, caches a three-axis snapshot with sequence, and publishes state/interrupt events | `event_bus`; `mt_log`, `esp_timer` (private) |
| `audio_service` | Manages BSP ES8311/NS4150B full-duplex PCM, volume, mute, and PA lifecycle under an exclusive lease | `bsp`; `freertos`, `mt_log` (private) |
| `sd_storage_service` | Manages the removable storage lifecycle through board-level mount adapters without binding to SDSPI/SDMMC implementations; exposes phase/generation snapshots, capacity queries, and formatting, and coordinates consumers with generation-validated ACCESS/SESSION/MAINTENANCE usage rights | `freertos`, `mt_log` (private) |
| `file_service` | Dedicated worker executes asynchronous storage control and UTF-8 file browsing; 4 sessions, 16-entry batches, PSRAM-first bounded pool at init, slot reference queue, copied results, deletion with generation/revision and maintenance confirmation | `sd_storage_service`; `fatfs`, `freertos`, `heap`, `mt_log` (private) |
| `timer_service` | Maintains countdown, stopwatch, and focus phases on monotonic time; provides a thread-safe snapshot, completion callbacks, and a command state machine | `esp_common`, `freertos` |
| `recorder_service` | Exclusive recording audio session and SD WAV files; dedicated worker performs recording, playback, deletion, and finalize; scanning holds ACCESS, recording/playback hold SESSION, capacity, generation, and content revision all come from the SD owner, and the UI only reads the cache | `audio_service`, `sd_storage_service`; `esp_timer`, `mt_log` (private) |
| `system_pm` | Serially executes peripheral sleep hooks, ESP32 light sleep, and wake resume, and manages the CPU maximum frequency lock | `mt_log` and ESP-IDF PM/GPIO/hardware support components (private) |
| `time_service` | Maintains the `CST-8` local time zone, RTC/calendar alarm bridging, clock trustworthiness, and system-level asynchronous SNTP sync | `event_bus`, `nv_storage`, network stack, etc. (private) |
| `connectivity_manager` | Sole owner of the production Wi-Fi policy; manages profiles, auto-connect, long backoff, foreground preemption, and standby coordination | `event_bus`; `nv_storage`, `wifi_service` (private) |
| `wifi_service` | Single-radio asynchronous execution layer; serially handles scan, connect, disconnect, and radio suspend, and does not persist STA credentials | `event_bus`; ESP-IDF Wi-Fi/network components (private) |
| `ble_runtime` | Sole low-level owner of the NimBLE host, static GATT, GAP/ADV, Device Link v1 transport, immutable async identity, and TX/deadline scheduling | `bt`, `device_link`; `freertos`, `mt_log` (private) |
| `device_link_service` | Serially owns the binding window, Numeric Comparison confirmation, app state, and the factory-reset startup gate | `ble_runtime`, `device_link`, `event_bus` |
| `factory_reset_service` | Holds the versioned factory-reset journal; reboots only after the marker is persisted, and clears the marker after all reset domains and the advertising precondition converge | `nv_storage`; `mt_log` (private) |
| `weather_service` | Completes one city-level location per IPv4 session (`/api/v1/location`), using the server-issued opaque `location_key` as the location scope identity; sequentially updates real-time, alert, hourly, and daily data, and provides PSRAM immutable snapshots and an A/B offline cache | `event_bus`; HTTP, cJSON, FreeRTOS, heap (private) |
| `chore_service` | Single background chore worker: any task may submit one-shot or periodic jobs (fixed slot pool, cooperative cancellation, no catch-up after suspend), used for short bounded work that is inconvenient to run on the GUI worker or in the calling context; LVGL calls and long blocking are forbidden | `mt_log`, `esp_timer`, `freertos`, `heap` (private) |
| `device_link` | Device Link v1 fixed-binary codec and single-slot operation engine; does not own NimBLE | none |

## Directory Layout

Each `components/<name>/` is an independent ESP-IDF component: `include/` is
the public API, `src/` is the internal implementation, `CMakeLists.txt`
declares the build dependencies, and `idf_component.yml` declares the minimum
IDF version. Configurable services carry a `Kconfig`; standalone host tests
currently also cover `ble_runtime`, `device_link_service`,
`factory_reset_service`, `device_link`, and `chore_service`, with entry points
all under each component's `tests/host/`.

## Integration and Initialization

Add the component directory before `project()` in the application top-level
`CMakeLists.txt`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/path/to/middleware/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
```

Consumer components declare the services they need through `REQUIRES` or
`PRIV_REQUIRES`, for example:

```cmake
idf_component_register(SRCS "app.c" REQUIRES connectivity_manager event_bus)
```

Build dependencies do not initialize the runtime. `mt_log`, `nv_storage`, and
`event_bus` should be initialized during single-threaded startup; call
`power_service_register_power_ops()`, `imu_service_register_imu_ops()`,
`time_service_register_rtc_ops()`, and `sd_storage_service_register_mount_ops()`
respectively before the services start. The init of Audio, SD, IMU, Power, Time,
Connectivity, and System PM all take typed configuration;
`audio_service_init()` directly takes the committed `bsp_audio_ops_t`, so it
must be called after `bsp_init()` succeeds. Before starting Wi-Fi, prepare
ESP-NETIF and the default event loop, then call `connectivity_manager_init()`;
the manager holds the single long-lived `wifi_service` session internally.
Production code must not initialize or control `wifi_service` on its own.
Re-initializing with the same configuration is idempotent; a different
configuration while active returns `ESP_ERR_INVALID_STATE`. Stop the publishers
and wait for accepted work to drain, then deinit in reverse order. `event_bus`
is a process-lifetime singleton and has no deinit interface.

## New Service APIs and Events

- `imu_service`'s board-level table requires `read` and may provide
  `is_available`, `configure`, `set_enabled`, and `poll_interrupt`.
  Initialization uses the runtime `sample_rate_hz` for both the hardware ODR and
  the worker period. The public API supports init/start, stop/deinit,
  suspend/resume, cached `get_snapshot`, and synchronous `read`.
- `audio_service_init_config_t` provides PCM, initial volume, mute, and PA
  together; `audio_service_get_config()` returns the currently effective PCM
  format. Consumers first call `audio_service_acquire()` to take a
  RECORD/PLAYBACK/MAINTENANCE lease, then read/write with the lease; release
  only accepts the current token, and a mismatch does not stop DMA. Leases
  survive suspend, and deinit returns `ESP_ERR_INVALID_STATE` while any lease
  exists. The service publishes no event bus messages.
- `sd_storage_service`'s adapter provides mount/unmount/is_mounted; ordinary
  `init/start(config)` never formats. Destructive recovery can only be initiated
  by an explicit `sd_storage_service_recover_and_mount(config)`.
- Media faults are treated as "confirmation" rather than unmounting on a single
  failure: `sd_storage_service_usage_report_fault()` accumulates a count first,
  and only when no media access succeeded between two consecutive reports
  (capacity query, content change, formatting, remount) does it set `faulted`
  and advance the generation. A single transient timeout (preemption, poor
  contact, clock change) therefore does not escalate into "storage card not
  mounted", and the user does not need to remount manually.
- `power_service`'s `poll_irq` returns the consumed AXP2101 latched status. A
  non-zero status is published with `POWER_SERVICE_MSG_SUB_TYPE_IRQ` and
  `power_service_irq_event_t`; this edge event uses flags `0` and is not
  overwritten by `EVENT_BUS_PUBLISH_FLAG_UI_LATEST`. Telemetry snapshots still
  update on their own period.
- `time_service`'s RTC table now requires the alarm capability to be either
  entirely absent or fully provided as configure/disable/get_status/clear/
  poll_interrupt. `time_service_alarm_*` manages repeating UTC calendar alarms;
  the worker polls the active-low RTC_INT on a fixed 100 ms period and publishes
  `TIME_SERVICE_MSG_SUB_TYPE_RTC_ALARM` sequence events with flags `0`.
- `connectivity_manager` is the sole owner of Wi-Fi profiles, auto-connect, and
  transparent sync. `connectivity_manager_request_sync_profile()` accepts
  credentials with a non-zero client sync ID; the same ID with the same
  credentials is idempotent, the same ID with different credentials conflicts,
  and a new profile replaces the old one only after both IPv4 and the durable
  store succeed. The password is never read back; when the storage result is
  ambiguous the old profile is retained and a recoverable storage/internal error
  is returned. Device Link's Wi-Fi adapter is the first line of defense: `OPEN`
  requires an empty password, `PERSONAL` requires a non-empty 1..64 password,
  and `UNSUPPORTED` goes straight to `INVALID_ARGUMENT`; admission errors map
  by the `allowed_statuses` frozen per method (manager not running/shutdown
  window always `UNAVAILABLE`, queue full maps to `RESOURCE_EXHAUSTED` only for
  `start_scan`/`set_credentials`, and `start_scan`/`disconnect`/
  `reconnect_saved` synchronously return `BUSY` while an operation is in
  flight). If a manager terminal state arrives before Core v2 table admission,
  the completion bridge parks it in a deferred slot with capacity 1 and TTL 1 s
  and merges it on admission, so the slot never leaks as a permanent PENDING.
- `device_link_service`'s worker is the logical owner of ACLs, Numeric
  Comparison confirmation, `command_rx`/`server_tx` delivery, and cleanup
  obligations. Production queues and TX completions wake through task
  notifications; the worker waits for the nearest absolute deadline and sweeps
  on every loop exit. All late events are filtered by
  `{generation, flow_id, token, kind, conn_handle}`. A locally enqueued revoke
  does not mean completion; the worker keeps advertising suspended until the
  revoke journal disappears and keeps advancing on a 100 ms period.
- provisional/orphan/replacement cleanup is retained by the owner with
  100/200/400/800/1000 ms backoff, and ports merge on the physical target with a
  fixed `4 + 1 overflow` capacity to avoid loss when the queue is full. Pending
  cleanup rejects new ACLs; live terminal cleanup retains the session/control
  write fence and a host-serialized terminate retry, the public `link_state`
  remains readable, and advertising resumes only after every cleanup has
  drained. Peer-store deletion uses a single explicit delete, per-type readback,
  and a full host-run sticky error, so a persistence failure cannot be passed
  off as success by RAM absence in the same run; deinit preserves revoke/cleanup
  obligations through a fixed-point drain across a double host barrier.
- `ble_runtime`'s TX scheduler covers queued, in-flight, and pending-delivery
  completions with a fixed `queue_depth + 1` credit, and each successful
  submission produces exactly one terminal state. A failed ADV START/STOP
  retains a generation-scoped obligation and backs off at
  100/200/400/800/1000 ms; ordinary window cancellation uses notification
  wakeups that are unaffected by the ADV queue capacity. The pairing gate's
  requested-open is independent of the cleanup/rejected/revoke/drain holds, and
  the effective open holds only when the hold mask is empty; a rejected ACL
  closes the gate first inside the CONNECT host callback and then retains the
  termination obligation.
- Device Link v1 uses LE Secure Connections Numeric Comparison; binding
  replacement clears the bond locally before pairing again. `SET_CREDENTIALS`
  only persists and does not connect.
- `time_service_set_network_ready()` is a non-blocking level notification.
  System SNTP starts once per IPv4 online period and stops immediately after
  the first successful update; going offline and standby also stop it, and after
  wake it waits for Wi-Fi to reconnect and obtain a new IPv4 before syncing. The
  app's "sync now" may issue one additional request while online; closing the
  page does not cancel the system request.
- `weather_service` keeps location, HTTPS, JSON, retry, and caching entirely in
  a PSRAM worker. Location is requested once per IPv4 session; a manual refresh
  does not re-locate. The `location_key` carried in weather responses is an
  opaque scope identity the server derives on a 0.1° grid: constant within the
  same grid and never exposing coordinates; a key change clears the old data set
  and triggers a full refresh with "real-time first", avoiding stale or mixed
  snapshots across grids. The optional `district` name is a display field
  (present when localization succeeds, never echoed back from the device
  header) and does not participate in location identity; the cache does not
  persist the key or district to disk, and they are re-established by session
  location after a reboot. The UI only acquires/releases immutable snapshots,
  and events carry only generation, state, and the changed mask.
- `chore_service` jobs are short bounded callbacks: they run on a single worker,
  serially; `run` must poll the cancellation token and return promptly, and must
  not call LVGL, issue synchronous HTTP, or wait indefinitely. After a
  successful `submit` the argument ownership transfers to the service, and
  `release` executes exactly once on the worker after completion, cancellation,
  or shutdown; `cancel` is cooperative silent waiting (returning guarantees
  `release` has run) and rejects self-calls from the worker. Periodic jobs are
  scheduled at a fixed delay from the completion instant, are not caught up
  while suspended, and are caught up at most once immediately after wake.
  `suspend`/`resume` follow the repository PAUSE/RESUME handshake (rollback on
  timeout), and the whole transaction is serialized by an independent lifecycle
  lock, preventing opposite commands from merging and acks from overwriting each
  other; shutdown wakes in-flight suspend/resume waiters with the STOPPED bit
  and does not deadlock. The job class APIs may be called from any task and are
  safe against a concurrent `deinit`: the process-lifetime admission gate closes
  atomically first and drains in-flight calls, a new instance reopens admission
  with a new epoch and continues the monotonic slot generation, and old handles
  never point at the new instance; only init and concurrent deinit require the
  caller to serialize. In sleep coordination this service suspends first and
  resumes last.
- `recorder_service`'s public start/pause/resume/stop/play/delete only accept
  the command and return immediately; the actual file system and audio I/O run
  serially on the Recorder's dedicated worker. Recording or playback main loops
  must not be submitted to `chore_service`; Chore is only for short, clearly
  bounded indexing, capacity, and metadata tasks.

Display TE sync is not part of the middleware service API. The BSP exports the
GPIO13 rising edge, the selected SPI frequency (40 MHz by project experience;
80 MHz is an out-of-spec experiment), 4 data lines, and the current 16 bpp
physical parameters through `bsp_display_port_t.te`; `layers/app_manager`
enables TE sync from these and adds the adapter defaults of 13/1 ms and a 66%
refresh window.

## Configuration

Kconfig keeps only static resource budgets: Event Bus three pools of 24 and
payload 256 B, NVS blob pool 16, IMU/Power stack 3072, Time stack 3072,
Connectivity stack 4096 and queue 8, Wi-Fi stack 4096 and queue 16, System PM
stack 4096, Weather stack 8192 and maximum temporary response 256 KiB, Chore
stack 4096 and job capacity 8. The sampling rate, polling period, task
priorities, PCM, mount point, time zone, and SNTP server are all passed in at
runtime by the root `app_product_config_t`. Factory-reset boot first clears the
Wi-Fi profiles, boot state, App Manager preferences, and built-in App state;
after the data partition mounts, `fs_storage_wipe_data()` empties all
application data files under `/data` (weather cache, etc.; the external SD card
is not erased), then in gated mode it clears Device Link authorization,
bond/CCCD, and volatile state, and pre-acquires the slow lease while advertising
is suspended; once the global marker is cleared, only the advertising pause is
lifted, and then the platform and network may continue booting. Before the
marker is cleared, any failure in a persistence, erase, or advertising
prerequisite step aborts this boot; after clearing, a failed physical START is
retried by the ADV owner with bounded backoff. `SYSTEM_PM_DEVELOPMENT_MODE` is
the root product development gate, defined in `main/Kconfig.projbuild`. After
changing Kconfig run
`idf.py reconfigure && idf.py save-defconfig && idf.py build`.

## Concurrency and Resource Boundaries

- `event_bus` APIs are task-context only and do not support ISR. At most 24
  subscriptions, 24 pending UI callbacks, and 24 UI payloads; a matching UI
  subscription allows payloads up to 256 bytes. Publisher callbacks run
  synchronously and UI callbacks run asynchronously; unsubscribing is not a
  silent barrier, and publishers must still be stopped and UI work drained
  before destroying `user_data`.
- `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` is only for overwritable state snapshots
  and must not be used for edge, command, audit, or counting events. Event
  payloads are only valid during the callback.
- `nv_storage` exclusively owns the default NVS partition lifecycle after a
  successful init. Keys are at most 15 bytes, and the Blob registration pool
  holds 16 entries; registration data buffers and callbacks must stay alive
  until a successful deinit. Blob loading freezes the registry, but no lock is
  held while callbacks execute.
- Connectivity public requests are non-blocking admission operations, and a scan
  snapshot keeps at most 5 records; SSIDs are limited to 32 bytes; personal
  network passwords are limited to 64 bytes (contract boundary, 1..64 all
  accepted, physical association failure is left as the operation result). The
  `wifi_service` public interface is reserved for the manager and low-level
  tests. The suspend, wait, I/O, or deinit interfaces of Connectivity, Wi-Fi,
  Weather, Time, Power, IMU, Audio, SD, Chore, and System PM may block, and
  lifecycle calls must be serialized by the upper layer. `chore_service` job
  callbacks themselves run on the worker and may call other service APIs there,
  but they must return promptly and respect each service's context restrictions.
- `system_pm` accepts 1 to 4 unique RTC GPIO wake sources, and their active
  levels must be identical. Wake callbacks should only notify other workers;
  peripheral preparation and resume hooks run in the PM worker, may block, but
  must respect the configured timeouts.
- Light sleep stops the APB/SPI clocks, and the board SD runs over SPI without
  holding a PM lock, so an in-flight card transfer times out with `0x107` and is
  judged a media fault and unmounted by the storage layer. `recorder_service`
  recording/playback sessions hold SESSION usage rights, so the host app's
  standby admission must reject via `recorder_service_is_busy()` both before
  prepare and before the final commit, not only once at the PM tick admission
  check.
- `SYSTEM_PM_DEVELOPMENT_MODE=y` does not keep USB Serial/JTAG working during
  light sleep; ESP32-S3 hardware does not support that. The mode skips app
  standby while a USB host is connected (the display may still turn off) and
  enables IDF's automatic sleep connection guard; normal light sleep resumes
  after USB is unplugged.
- The current board's EXIO3/5/6 go through a TCA9554 and can only be polled by
  the time/power/IMU workers; they cannot be RTC GPIO wake sources. Touch wake
  is not implemented yet and GPIO21 is not registered; the actual wake
  descriptor is still only GPIO0 active-low.

## Host Tests

Requires CMake 3.22+, Ninja, a C11 compiler, and pthread. Pick the affected
component's `tests/host` suite by the impact-surface table in the root
repository's `AGENTS.md`. One ordinary profile example is kept below; add
ASan/TSan only for memory ownership or concurrency changes, matched to the risk.

Run the NVS suite from this middleware submodule root:

```sh
cmake -S components/nv_storage/tests/host -B /tmp/mt-nv -G Ninja \
    -DNV_STORAGE_SANITIZER=none
cmake --build /tmp/mt-nv
ctest --test-dir /tmp/mt-nv --output-on-failure
```

Run the time service's callback generation gate and port drain barrier suite
when needed:

```sh
cmake -S components/time_service/tests/host -B /tmp/mt-time -G Ninja \
    -DTIME_SERVICE_SANITIZER=none
cmake --build /tmp/mt-time
ctest --test-dir /tmp/mt-time --output-on-failure
```

The time suite also covers non-coalesced publishing, clearing, and retriggering
of RTC alarms. Run the power IRQ worker suite:

```sh
cmake -S components/power_service/tests/host -B /tmp/mt-power -G Ninja \
    -DPOWER_SERVICE_SANITIZER=none
cmake --build /tmp/mt-power
ctest --test-dir /tmp/mt-power --output-on-failure
```

Run the system PM suite (configuration validation, init rollback, standby
cycle, wake reason classification, hook failure, resume cancellation, CPU
frequency lock refcounting, deinit retry):

```sh
cmake -S components/system_pm/tests/host -B /tmp/mt-system-pm -G Ninja \
    -DSYSTEM_PM_SANITIZER=none
cmake --build /tmp/mt-system-pm
ctest --test-dir /tmp/mt-system-pm --output-on-failure
```

Run the chore worker suite (one-shot/delayed/periodic scheduling, pool full,
cooperative cancellation, no catch-up after suspend and timeout rollback, deinit
release, self-call rejection, concurrent cancellation):

```sh
cmake -S components/chore_service/tests/host -B /tmp/mt-chore -G Ninja \
    -DCHORE_SERVICE_SANITIZER=none
cmake --build /tmp/mt-chore
ctest --test-dir /tmp/mt-chore --output-on-failure
```

Run the Device Link v1 codec and slot engine suite:

```sh
cmake -S components/device_link/tests/host -B /tmp/mt-device-link -G Ninja \
    -DDEVICE_LINK_SANITIZER=none
cmake --build /tmp/mt-device-link
ctest --test-dir /tmp/mt-device-link --output-on-failure
```

Run the NimBLE/Device Link runtime and service owner suite (requires exporting
`IDF_PATH` for ESP-IDF v6.1 first):

```sh
cmake -S components/ble_runtime/tests/host -B /tmp/mt-ble-runtime -G Ninja \
    -DBLE_RUNTIME_SANITIZER=none
cmake --build /tmp/mt-ble-runtime
ctest --test-dir /tmp/mt-ble-runtime --output-on-failure

cmake -S components/device_link_service/tests/host \
    -B /tmp/mt-device-link-service -G Ninja \
    -DDEVICE_LINK_SERVICE_SANITIZER=none
cmake --build /tmp/mt-device-link-service
ctest --test-dir /tmp/mt-device-link-service --output-on-failure
```

Both sanitizer options accept `address` or `thread`. The `ble_runtime` suite
also runs fixed ESP-IDF v6.1 internal assumption checks; on failure, review the
NimBLE pairing/store/host-event timing.

Run the factory-reset journal persistence, fault injection, and power-loss
recovery suite:

```sh
cmake -S components/factory_reset_service/tests/host \
    -B /tmp/mt-factory-reset -G Ninja \
    -DFACTORY_RESET_SERVICE_SANITIZER=none
cmake --build /tmp/mt-factory-reset
ctest --test-dir /tmp/mt-factory-reset --output-on-failure
```

Run the audio and SD lifecycle suites:

```sh
cmake -S components/audio_service/tests/host -B /tmp/mt-audio -G Ninja
cmake --build /tmp/mt-audio
ctest --test-dir /tmp/mt-audio --output-on-failure

cmake -S components/sd_storage_service/tests/host -B /tmp/mt-sd -G Ninja
cmake --build /tmp/mt-sd
ctest --test-dir /tmp/mt-sd --output-on-failure
```

All listed `*_SANITIZER` options (including `BSP_SANITIZER`,
`AUDIO_SERVICE_SANITIZER`, `SD_STORAGE_SERVICE_SANITIZER`, and
`SYSTEM_PM_SANITIZER`) accept `none`, `address`, and `thread`; use a separate
build directory when switching sanitizers. On x86_64, if `setarch` is present,
CTest automatically disables ASLR for TSan tests. QMI8658C, SDSPI partial
rollback, ES8311 board lifecycle, PCF85063 alarm, and AXP2101 profiles are
covered by `layers/bsp/tests/host`; `layers/app_manager/app_core/tests/host`
validates the mapping from BSP TE parameters to adapter TE sync. All hardware
behavior still needs to build with the full project and be verified on hardware
according to risk.

## License

This project uses the [MIT License](LICENSE).
