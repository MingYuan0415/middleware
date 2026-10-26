**English** | [简体中文](README_ZH.md)

# audio_service

Singleton service providing full-duplex PCM playback and capture on top of the
board-level BSP audio device. It manages the exclusive lease, lifecycle, volume,
mute, and the NS4150B power-amplifier enable.

## Public API

Public header `include/audio_service.h`:

- `audio_service_init()` / `audio_service_deinit()`: establish or release the
  service state around the BSP audio device; the service neither acquires nor
  releases BSP device ownership. Deinit returns `ESP_ERR_INVALID_STATE` while an
  active lease exists.
- `audio_service_acquire()` / `audio_service_release()`: acquire/release the
  exclusive stream lease. An initialized READY/ERROR without an owner can be
  acquired; ERROR stops for cleanup first. An already leased service or a
  reservation conflict returns `ESP_ERR_INVALID_STATE`; it neither queues nor
  preempts. Release accepts only the current token/generation, a mismatch does
  not stop DMA, and a failure retains the lease for a same-generation retry.
  Kinds are RECORD (read/write), PLAYBACK (write-only), and MAINTENANCE
  (read/write, the benchmark path).
- `audio_service_configure()`: set the PCM format while READY and without a
  lease.
- `audio_service_suspend()` / `audio_service_resume()`: quiet the stream within
  a bounded wait before system sleep; the lease is retained across suspend and
  the stream restarts on demand at resume; `resume_required` indicates whether
  it was running before.
- `audio_service_write_leased()` / `audio_service_read_leased()`: validate the
  lease, state, admission, and permission and register the in-flight I/O in the
  same critical section, then call the BSP outside the lock; when the service
  rejects, a non-empty transferred output is zeroed. A BSP error may still
  report partial progress, so the caller must handle the already-transferred
  tail position.
- `audio_service_set_volume()` / `get_volume()`, `set_mute()` / `get_mute()`,
  `set_pa()` / `get_pa()`: system-level volume, mute, and power-amplifier
  control, which do not participate in the lease.
- `audio_service_is_available()` / `audio_service_get_state()` /
  `audio_service_get_config()` / `audio_service_is_leased()` /
  `audio_service_get_lease()`: availability, state, configuration, and lease
  diagnostics.

## Configuration and Dependencies

- `idf_component.yml`: only `idf: ">=5.0"`.
- CMake: `REQUIRES bsp`, `PRIV_REQUIRES freertos mt_log`.
- No Kconfig; the start-up policy is supplied by `audio_service_init_config_t`
  (PCM format, initial volume, mute, PA).
- The board device must already be initialized; the BSP holds its resources
  before the service is used.

## State and Concurrency

- Singleton, with no dedicated worker task.
- Lifecycle states: UNINITIALIZED / READY / RUNNING / SUSPENDING / ERROR.

- RUNNING has a valid lease; READY may be idle without an owner, or may retain
  the owner across suspend. The generation is retained across deinit/init, and a
  new lease invalidates the old handle.
- The first init (including synchronization-object creation) is serialized by
  the start-up owner. After initialization, lifecycle transactions are isolated
  by an internal mutex and the SUSPENDING reservation; a conflicting control
  returns `ESP_ERR_INVALID_STATE`, so callers need no separate lifecycle lock.
- release/suspend/deinit close admission first; they unlock to wait only when
  in-flight I/O is nonzero. stop and the state commit complete while holding the
  lock, so zero I/O opens no acquire window. A drain timeout restores the
  previous state/admission; a stop failure keeps ERROR with the owner/cleanup
  information. A release in READY with an owner does not stop again.
- A failed acquire start is compensated by an immediate stop: success returns to
  READY, failure stays at ERROR; the output handle is zeroed and the original
  start error is returned. The next acquire of an ownerless ERROR cleans up
  first; a partial-init ERROR is resolved by deinit/init.
- suspend retains the lease; a suspend in READY is a no-op. resume returns OK
  without starting DMA when READY has no owner, succeeds idempotently when
  RUNNING has an owner, restores the original generation when READY/ERROR has an
  owner, and starts nothing for ERROR without an owner.
- Production `main/app_runtime_pm.c` calls suspend/resume on sleep and on
  rollback/wake. Recording/playback, Flybirds, and the display benchmark all
  hold the exclusive lease; a failed cleanup must retain ownership and retry and
  must not preempt a background consumer or overwrite an old handle. System
  volume/mute/PA requests are not reset by release.

The full consumer and error-recovery contract is in
[`doc/audio-session-lease.md`](../../../../doc/audio-session-lease.md).

## Test Boundary

- `tests/host` exists; it uses host BSP/audio device fakes to cover the
  lifecycle, lease exclusion, stale handles, I/O admission, and the
  suspend/resume handshake.
- The host does not cover on-device I2S/DMA timing, codec and power-amplifier
  electrical behavior, hardware clocks, or power consumption.
