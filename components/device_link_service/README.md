**English** | [简体中文](README_ZH.md)

# device_link_service

Device Link v1 product policy owner: holds the `ble_runtime` lifecycle, manages
the pairing window, Numeric Comparison local confirmation, the Bluetooth
enablement policy, and bond revocation, and submits Wi-Fi operations to
`connectivity_manager`.

## Public API

Public header `include/device_link_service.h`:

- `device_link_service_init()` / `deinit()`: start or stop the service and the
  BLE runtime (`runtime_port` is required).
- `open_window()` / `close_window()` / `confirm_binding()` /
  `pending_confirmation()`: pairing window and local confirmation; the
  confirmation token must match the one in the status snapshot.
- `revoke_binding()`: local bond revocation (journaled, with no over-the-air
  command).
- `set_enabled()`: records the latest target; the service worker persists it and
  converges through a bounded state machine, and the status snapshot exposes
  desired/effective/policy_error.
- `release_startup_gate()`: the `FACTORY_RESET_GATED` start-up mode commits
  visibility after the reset domain has been cleared.
- `suspend()` / `resume()` / `get_status()` / `is_active()` / `is_busy()`:
  standby and status queries.
- `include/device_link_wifi_adapter.h` provides the GET_INFO firmware triple,
  the submission adapter from Device Link operations to `connectivity_manager`,
  and the bridge lifecycle.

## Configuration and Dependencies

- Kconfig: `DEVICE_LINK_SERVICE_TASK_STACK` (default 6144),
  `DEVICE_LINK_SERVICE_QUEUE_DEPTH` (default 8).
- CMake: `REQUIRES ble_runtime device_link connectivity_manager event_bus nv_storage wifi_service`; `PRIV_REQUIRES esp_hw_support freertos mt_log nvs_flash`.
- At runtime the service is configured by `device_link_service_config_t`
  (runtime port, task priority, window duration, start-up mode, firmware
  version).

## State and Concurrency

- Singleton; a static worker task, command queue, mutex, and recursive mutex.
- init/deinit must come from a single owner task; the status snapshot carries a
  monotonic `generation`.
- GATT/SMP facts are retained by NimBLE callbacks, which wake this owner; the
  callbacks never update LVGL or publish application events directly.
- Bluetooth disable and a full deinit share the shutdown phase: stop advertising
  and the window, close the connection entry through a host barrier, clear the
  logical session, wait for disconnection plus revoke cleanup, and release the
  runtime after a second barrier confirms. Normal cleanup waits do not consume
  failure attempts; a timeout retains the phase and resources, and a later
  request or deinit can continue. Enabling again first completes this round of
  release; after success there is no fixed cooldown.
- Wi-Fi request enqueue and the BLE/manager operation ID registration complete
  inside the same bridge lock; the publishing thread reads the complete mapping
  and releases that lock before calling the link-service completion interface.
- An ordinary disconnect, host RESET, window close, revoke, or Bluetooth
  toggle does not clear an accepted Wi-Fi operation association; the engine
  keeps the device-scoped record, does not replay the indication automatically
  after a disconnect, and a reconnecting client can query/ACK by the original
  ID.
- Status and scan share a fixed-size immutable terminal slot; a transient
  failure of the completion interface retains the first result and scan list,
  and a repeated/updated event only triggers a retry and cannot rewrite that
  terminal state. A new request replaces the old mapping only after the manager
  admits it successfully; only a full service deinit retires the bridge.
- A Bluetooth disable still advances the original operation deadline and the
  completion retries already triggered by wakeups; TX/advertising/window
  handling still requires a ready runtime, and toggling does not reset the
  operation deadline.

## Test Boundary

- `tests/host` exists; it uses fake `connectivity_manager`, `event_bus`, and
  `nv_storage` to cover the window, confirmation, revoke, and Wi-Fi adapter.
- The host does not cover NimBLE callback timing, NVS persistence, RF, or
  Android interoperability.
