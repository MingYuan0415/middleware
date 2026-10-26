**English** | [简体中文](README_ZH.md)

# sd_storage_service

Pluggable SD filesystem mount service independent of board-level pins; by
default it does not format, and only an explicit mount recovery or an explicit
format may erase the medium. It also owns the volume usage rights and the
medium-level fault state.

## Public API

Public header `include/sd_storage_service.h`:

- `sd_storage_service_register_mount_ops()`: registers the board-level
  mount/unmount/is_mounted/format/get_space adaptation before initialization;
  the service does not own the pin mapping.
- `sd_storage_service_init()`: mounts in normal mode; a mount failure never
  formats.
- `sd_storage_service_recover_and_mount(config, expected_generation)`:
  explicitly allows format-based recovery after a mount failure; a healthy
  mount erases nothing. When a fault or an incomplete cleanup is present, the
  old adapter is really released first.
- `sd_storage_service_unmount(expected_generation)`: safe unmount, idempotent
  once the repeated cleanup succeeds.
- `sd_storage_service_format(config, expected_generation)`: erases the mounted
  medium and remounts; the old generation is invalidated at the start, a
  failure retains the resource cleanup obligation, and no usable state is
  exposed afterwards.
- The three user-confirmed operations check expected_generation and the
  absence of an active lease in the same critical section; an old confirmation
  must not operate on a later volume. A normal init requests no format.
- `sd_storage_service_get_space(usage, total, free)`: queries under a valid
  ACCESS/SESSION/MAINTENANCE lease the caller already holds; it occupies no
  additional lease slot, and on failure or if the generation is invalidated
  during the query the outputs stay unchanged — it never pretends free space
  is zero.
- `sd_storage_service_deinit()`: unmounts and releases the adapter resources;
  `start` / `stop` are aliases.
- `sd_storage_service_get_snapshot()`: a snapshot read under one lock,
  containing mounted, phase, generation, revision, mount_path, last_error, the
  usage right counts, maintenance, and faulted.
- Usage rights:
  `sd_storage_service_usage_acquire/release/report_fault`, split into ACCESS
  (browsing/query), SESSION (recording, pause, playback), and MAINTENANCE
  (unmount/format).
- `sd_storage_service_content_revision()` / `note_content_change(usage)`:
  shared and atomically updated content version maintained by the SD service;
  a file/directory change that is published successfully must hold a valid
  lease.

## Phases and Generations

`sd_storage_service_phase_t` distinguishes `UNMOUNTED`, `MOUNTING`, `MOUNTED`,
`UNMOUNTING`, `FORMATTING`, and `ERROR` externally. A failed normal mount only
enters `ERROR`; it is never inferred as "no card" or "format error".

`generation` changes on mount, unmount, an explicit format, a confirmed
format-recovery attempt, and a fault invalidation. Even if the remount after an
erase fails, the old confirmation and the old results are already invalidated.
Usage rights and fault reports carry a generation; a late fault does not
pollute a later mount. `revision` is the content version shared across
consumers; a successful format also updates it, and normal file changes
increment it inside the original SD critical section.

## Usage Rights Rules

- ACCESS and SESSION may coexist while the medium is mounted and not faulted:
  browsing can run in parallel with recording.
- MAINTENANCE is granted only when no other usage right exists; conversely,
  while maintenance is held new ACCESS/SESSION are rejected, avoiding the
  "check busy, then operate" race window.
- After a medium fault is confirmed, mounted=false, phase=ERROR,
  mount_path=NULL immediately, and the generation advances; all capacity/change
  operations and new usage rights are rejected until a real cleanup and
  remount happen. The original token + original generation can still be
  released, and all lifecycle operations are rejected before the original
  consumer closes its handles and releases — they are not treated as counts
  that already vanished.
- Business errors such as a missing file, permissions, or a non-empty
  directory are not medium faults.
- Fixed eight lease slots. A capacity query reuses the passed-in lease; it
  requests no extra slot even when the table is full, and the slow query runs
  outside the lock so snapshot reads, releases of other leases, and
  non-conflicting acquisitions stay responsive.

## Configuration and Dependencies

- CMake: `PRIV_REQUIRES mt_log freertos`.
- `idf_component.yml`: only `idf: ">=5.0"`.
- No Kconfig; at runtime `sd_storage_service_config_t` provides the mount path,
  the maximum file count, and the allocation unit.

## Concurrency

- Singleton; the critical section protects the state and the usage rights
  table, and **no file I/O happens inside the critical section**.
- No worker task; mount, unmount, and format are executed synchronously by the
  caller (the `file_service` worker).

## Archive and Format Requirements

- The adapter `format()` must remount and confirm that the capacity is readable
  and non-zero, and only return `ESP_OK` when everything succeeded. The
  board-level backend owns the card/SDSPI/diskio/FatFs/VFS resources itself and
  implements the operation with the public `f_mount` / `f_mkfs` combination; it
  does not use the SDK sdcard format combination helper.
- Cleanup commits the resource state phase by phase; a failure retains the
  incomplete phase. mount/unmount/deinit can all retry cleaning up the old
  adapter, successful phases are not released again, and mount does not stack
  old resources on top of new ones.
- The capacity adapter uses `esp_vfs_fat_info()` (`statvfs()` on the host); the
  service queries outside the lock and revalidates the passed-in lease and
  generation after the call returns, publishing the outputs only on success.

## Test Boundary

- `tests/host` covers generation-bound recovery/unmount/format, capacity queries
  with a full table, the shared content version, immediate invalidation on a
  fault plus release of the original lease, and retries of a failed cleanup; a
  pthread-blocking capacity callback verifies that the state lock is not held by
  I/O and that lifecycle operations are rejected. All adapter callbacks assert
  that no state lock is held.
- The host does not cover real SD card protocol, timing, or hot-plug.
