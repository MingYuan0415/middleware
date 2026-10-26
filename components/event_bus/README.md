**English** | [简体中文](README_ZH.md)

# event_bus

Thread-safe fixed-capacity publish/subscribe bus that supports two dispatch
contexts: synchronous publisher-task callbacks and asynchronous UI-worker
callbacks.

## Public API

Public header `include/event_bus.h`:

- `event_bus_init()`: singleton initialization, called once during the
  single-threaded start-up phase.
- `EVENT_BUS_DECLARE_ID()` / `EVENT_BUS_DEFINE_ID()` / `EVENT_BUS_ID()`: message
  identity declaration and definition.
- `event_bus_subscribe()` / `event_bus_unsubscribe()`: subscribe or unsubscribe
  by message and subtype; handles are generation-protected.
- `event_bus_publish()`: publish a message; publisher callbacks run
  synchronously and UI callbacks receive a shared immutable payload copy.
- `event_bus_register_ui_dispatch()` / `unregister_ui_dispatch()`: register or
  unregister the non-blocking UI worker enqueue function.
- `event_bus_register_wake_requester()` / `unregister_wake_requester()`:
  register or unregister the non-blocking wake request.
- `EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST` / `EVENT_BUS_PUBLISH_FLAG_UI_LATEST`:
  wake and UI snapshot coalescing flags.

## Configuration and Dependencies

- Kconfig: `EVENT_BUS_SUBSCRIBER_CAPACITY`, `EVENT_BUS_UI_CALLBACK_CAPACITY`,
  `EVENT_BUS_UI_PAYLOAD_CAPACITY` (default 24), `EVENT_BUS_UI_PAYLOAD_SIZE`
  (default 256).
- `idf_component.yml`: only `idf: ">=5.0"`.
- CMake: `PRIV_REQUIRES mt_log`.
- The UI dispatcher and the wake request function are registered at runtime by
  the assembly layer.

## State and Concurrency

- Singleton; a mutex protects the subscription table and the UI pending pool;
  not ISR-safe.
- A UI subscription is rejected before a dispatcher is registered; unsubscribe
  does not wait for a callback that already passed the liveness check, so the
  caller must synchronize the `user_data` destruction itself.

## Test Boundary

- No `tests/host`.
- The host does not cover real UI worker/LVGL scheduling or the wake hardware
  path.
