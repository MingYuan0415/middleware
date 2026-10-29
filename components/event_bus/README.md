**English** | [简体中文](README_ZH.md)

# event_bus

Thread-safe fixed-capacity publish/subscribe bus with two dispatch contexts:
synchronous publisher-task callbacks and asynchronous UI-worker callbacks.

## Public API

Public header `include/event_bus.h`:

- `event_bus_init()` / `event_bus_deinit()`: singleton lifecycle. Call once
  during single-threaded start-up; deinit only after every publisher and
  dispatcher has stopped.
- `EVENT_BUS_DECLARE_ID()` / `EVENT_BUS_DEFINE_ID()` / `EVENT_BUS_ID()`: message
  identity. The identity is the address of a descriptor which also carries a
  stable name for logs and statistics.
- `event_bus_subscribe()` / `event_bus_unsubscribe()`: subscribe or unsubscribe
  by message and subtype; handles are generation-protected.
- `event_bus_unsubscribe_sync()`: cancel one subscription and wait for its own
  publisher callbacks before releasing `user_data`.
- `event_bus_flush_sync()`: wait until no publisher callback runs anywhere.
- `event_bus_publish()` / `event_bus_publish_ex()`: publish a message. Publisher
  callbacks run synchronously and receive the caller's payload; UI callbacks
  receive one shared immutable payload copy.
- `event_bus_register_ui_dispatch()` / `unregister_ui_dispatch()`: register or
  unregister the non-blocking UI worker enqueue function and report its queue
  depth.
- `event_bus_abandon_pending_ui()`: teardown hook which releases pending UI
  batches after the dispatcher queue is gone.
- `event_bus_register_wake_requester()` / `unregister_wake_requester()`:
  register or unregister the non-blocking wake request.
- `event_bus_get_stats()`: read the dispatch counters.
- `EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST` / `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` /
  `EVENT_BUS_PUBLISH_FLAG_ATOMIC`: wake, UI snapshot coalescing, and
  all-or-nothing delivery.

## Delivery Model

- **Independent paths.** A UI admission failure never suppresses publisher
  callbacks; `event_bus_publish_ex()` reports the two results separately.
  `EVENT_BUS_PUBLISH_FLAG_ATOMIC` opts into the legacy all-or-nothing rule for
  callers whose own retry loop must not observe the same event twice.
- **Visibility.** A subscriber receives a publication only when it was created
  before that publication. The bus tracks this with one 64-bit monotonic
  sequence per subscribe and publish, so no per-publication subscriber snapshot
  is stored and a long-lived subscription never falls out of the visible range.
- **Coalescing.** `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` replaces the payload of a
  pending batch with the same message and subtype, and the newest sequence
  decides which subscribers receive it. State snapshots must be self-contained;
  a subscriber that appears after the coalescing publication reads the current
  state from its owner on resume.
- **Ordering.** A pending publication gets one UI batch unless it coalesces into
  an older one; callbacks run in subscription-slot order. There is no global
  order across messages.
- **Lifetime.** `event_bus_unsubscribe()` only invalidates the subscription;
  `event_bus_unsubscribe_sync()` additionally waits for this subscription's
  publisher callbacks to drain. UI callbacks are serialized with the
  subscription on the dispatcher task, so a UI subscription must be canceled
  there. A slot which still has publisher callbacks is not reused, so a
  timed-out synchronous unsubscribe temporarily reduces the usable subscriber
  capacity; calling it again with the same handle while callbacks are still
  running resumes the wait, and a repeated call after they have drained returns
  `ESP_OK`.

## Configuration and Dependencies

- Kconfig: `EVENT_BUS_SUBSCRIBER_CAPACITY` (default 24),
  `EVENT_BUS_UI_BATCH_CAPACITY` (default 8), `EVENT_BUS_UI_PAYLOAD_SIZE`
  (default 320), `EVENT_BUS_STATS` (default y), `EVENT_BUS_TRACE` (default n).
  `EVENT_BUS_TRACE` compiles the publication trace into the bus and raises
  this file's compile-time log ceiling; the `event_bus` log tag must still
  allow `ESP_LOG_DEBUG` at runtime (set it with
  `esp_log_level_set("event_bus", ESP_LOG_DEBUG)` after initialization). The
  UI batch capacity must not exceed the depth reported to
  `event_bus_register_ui_dispatch()`.
- `idf_component.yml`: only `idf: ">=5.0"`.
- CMake: `PRIV_REQUIRES mt_log`.
- The UI dispatcher and the wake request function are registered at runtime by
  the assembly layer.
- Each publisher asserts its payload type against
  `EVENT_BUS_MAX_UI_PAYLOAD_SIZE` at build time; a growing snapshot fails the
  build instead of failing at runtime.

## State and Concurrency

- Singleton; one mutex protects the subscription table and the pending batch
  pool, one admission mutex serialises UI batch creation, and one binary
  semaphore wakes synchronous drain waiters. Not ISR-safe.
- A UI subscription is rejected before a dispatcher is registered, and a
  publication with UI subscribers returns `ESP_ERR_INVALID_STATE` while the
  dispatcher is unregistered.
- Pending UI metadata lives in internal RAM; only the payload pool is placed in
  external RAM with `EXT_RAM_BSS_ATTR`.
- `event_bus_abandon_pending_ui()` must only be called once the dispatcher
  queue is destroyed; calling it while a batch callback can still run is a
  use-after-free.
- A rejected UI publication is counted in `event_bus_get_stats()` and logged at
  warning level at most once every five seconds; `late_subscriber_skips` counts
  matching subscribers observed during dispatch which were created after the
  publication.

## Test Boundary

- `tests/host` covers handle generation, long-lived visibility, coalescing,
  path isolation, capacity, abandonment, per-subscription and global drain, the
  late-subscriber counter, and the core state machine on top of the shared
  pthread FreeRTOS fake. Run it with `-DEVENT_BUS_SANITIZER=thread` for the
  race profile.
- The suite also compiles the bus with `EVENT_BUS_TRACE` enabled against a
  logger which evaluates its arguments, so trace-only build breaks fail the
  build.
- The real implementation is also compiled into `tests/integration`,
  `tests/connectivity`, and `test_ble_wifi_recovery` with `HOST_TEST=1`.
- The host does not cover real UI worker/LVGL scheduling or the wake hardware
  path.
