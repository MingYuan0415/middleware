**English** | [简体中文](README_ZH.md)

# recorder_service

Provides WAV recording, playback, and file management on top of
`audio_service` and `sd_storage_service`, and maintains a state snapshot.

## Public API

Public header `include/recorder_service.h`:

- `recorder_service_init()` / `deinit()`: initialize or stop the service; the
  directory is a volume-relative path (for example `MicroTech/Recordings`), and
  the service resolves the current mount point through `sd_storage_service` on
  every use instead of hardcoding a mount path. Initialization also succeeds
  without a card; recording is simply unable to start.
- `recorder_service_start()` / `pause()` / `resume()` / `stop()`: recording
  commands; completion is reported through the snapshot.
- `recorder_service_play()` / `stop_playback()`: play the specified WAV and stop
  playback.
- `recorder_service_list()` / `delete()`: enumerate finished WAV files and
  delete files, without exposing a directory handle.
  The list is ordered by modification time, newest first (rows with `has_mtime`
  false sort after the known times, and equal times sort by file name
  descending), keeping at most the newest `RECORDER_SERVICE_MAX_FILES` entries;
  when exceeded, the snapshot's `library_truncated` is true and the UI uses it
  to hint `"仅显示最新 N 个"` (only the newest N are shown).
  Each row's `target` retains the full UTF-8 backend path (192 bytes, including
  the terminator) plus the mount generation / content revision current when
  that index was published; playback and deletion receive a pointer to that
  target, and the queue copies the full target instead of re-selecting a file
  by path name. Each row also carries `has_mtime` and `modified_epoch_s` (from
  `stat`) for the UI to display the recording time.
- `recorder_service_refresh()`: callers that only read snapshot counts use it to
  request republication of a stale index; it merges with `list()`'s implicit
  request in the same way, and no request is queued while the index is fresh.
- `recorder_service_get_snapshot()` / `is_busy()`: state snapshot and busy
  query. The snapshot distinguishes `free_bytes` (valid only when `space_valid`
  is true), `space_error`, and `storage_ready`; a failed capacity query is not
  treated as insufficient space. The current SD's `storage_generation` /
  `content_revision` are read outside the recorder lock; `index_generation` /
  `index_revision` / `index_valid` / `file_count` / `library_truncated` are
  copied together with the index inside the lock; `max_duration_ms` comes from
  the initialization configuration, and the UI uses it to show the limit and
  the remaining recording time.

`recorder_service_snapshot_t.name`, `recorder_service_file_t.target.name`, and
the playback/delete commands all store a full file path with a capacity of
`RECORDER_SERVICE_PATH_BYTES` bytes (including the terminating NUL); a
recording file whose concatenated path exceeds that capacity does not enter the
service index.

## File Naming

A new recording is named `REC_YYYYMMDD_HHMMSS` (local time, adopted only when
the system clock is not earlier than 2020); when the clock is not calibrated it
falls back to `REC_<esp_timer microseconds>`, which stays monotonic and
sortable. A second recording within the same second gets a `-2`, `-3` suffix
appended: creation reserves the name with `open(O_CREAT|O_EXCL)` and
additionally probes whether the corresponding `.wav` already exists, so neither
a reboot nor consecutive recordings within the same second overwrite an
existing file.

A double dot inside a name (such as `REC_..a.wav`) does not affect playback or
deletion: the path ownership check only requires that the file name is a direct
child of the recordings directory and contains no separator, while the `.`/`..`
entries themselves are still rejected. The previous "reject anything containing
`..`" rule made such recordings listed by the scan permanently visible yet
impossible to operate on.

## Configuration and Dependencies

- CMake: `PRIV_REQUIRES audio_service esp_timer fatfs freertos mt_log sd_storage_service`.
- `idf_component.yml`: `idf: ">=5.0"`, local dependencies `audio_service`,
  `sd_storage_service`, `mt_log`.
- No Kconfig; at runtime `recorder_service_config_t` provides the directory, the
  maximum recording duration, and the minimum free space.

## State and Concurrency

- Singleton; commands go through a bounded queue to a pinned worker task, and
  recording and playback share one stream task.
  The device stream task's 4096-byte stack is explicitly allocated with
  `SPIRAM | 8BIT` and does not fall back to internal RAM; the shared 4096-byte
  PCM buffer lives in external BSS. The TCB, queues, locks, and the 6144-byte
  command stack remain internal.
  The command stack carries the VFS → FATFS → diskio → SDSPI call chain of every
  index scan; measured at `-O2`, that chain (scan 304, `vfs_fat_stat` 336,
  `f_stat` 96, `sdspi_host_start_command` 272, `start_command_default` 96, not
  counting newlib printf and the sector buffer) needs about 1.8 KB, plus the
  command task's own 576 bytes and interrupt margin; 4096 is not enough, and
  6144 leaves about 3 KB of headroom.
  The normal host and the simulator run only the command worker and do not
  simulate the audio stream, so SESSION is retained until an explicit stop; a
  separate device-branch host target runs the real stream task to verify the
  lifecycle.
- State is identified by `generation` / `operation_id`, and commands complete
  asynchronously; critical sections and atomics protect the shared fields.
  All cross-service calls (including SD state, revisions, leases, and timing)
  execute outside the recorder critical section.
- State machine: IDLE / RECORDING / PAUSED / PLAYING / ERROR.
- While paused the capture device keeps running and the stream task keeps
  reading and discarding input; not reading would accumulate the paused interval
  in the I2S ring buffer and write it into the file after resume, producing
  audio the user never recorded and a data length exceeding the reported
  duration.
- A failed recording (I/O error, duration limit, header write failure) deletes
  its `.part` working file after closing; otherwise the library would never
  list those names and the user could not delete them either, so they would just
  slowly fill the storage card.
- Audio streams: recording takes an `AUDIO_SERVICE_SESSION_RECORD` lease
  (read-write), playback takes an `AUDIO_SERVICE_SESSION_PLAYBACK` lease
  (write-only); the stream task uses the lease for reads and writes, and the
  teardown/error paths clear the same-generation handle only when release
  actually returns `ESP_OK`. Any error (including `ESP_ERR_INVALID_STATE`)
  retains the cleanup obligation; stop, the next start/play, or deinit can
  retry it. start/play clean up the old lease before acquiring the SD SESSION or
  opening/creating the file, and a failure is returned as is without
  overwriting the old generation. When audio is occupied by another writer,
  startup rolls back the file and the SD usage rights and does not stop the
  other stream.
- Occupancy: directory preparation, file creation, recording, pause, close,
  WAV finalize, and index refresh, as well as playback from open to close, hold
  the `sd_storage_service` business usage right (SESSION) throughout; deletion
  takes the exclusive maintenance right. Therefore browsing works during
  recording/playback, but deletion, unmount, and format are rejected.
  After playback acquires SESSION and deletion acquires MAINTENANCE, they first
  verify the target generation and lease plus the current SD generation /
  revision; a stale target fails before the open or delete and releases the
  lease, without touching a new file at the same path. `ESP_OK` only means the
  command was accepted; success can be judged only after waiting for
  `operation_pending == false` on the same `operation_id` and checking
  `last_error`.
- Index: the worker acquires ACCESS before an independent scan and holds it from
  path resolution and the capacity query through the directory and every WAV
  header check and handle close; the refresh after a recording ends and after a
  deletion reuses the existing SESSION / MAINTENANCE. It verifies the SD
  generation and content revision before and after the scan and discards the
  result if they changed; UI queries only read the cache, a stale list returns
  empty and requests an asynchronous refresh, and neither the device nor the
  host performs file/capacity I/O inside a query.
  A failed or interrupted directory scan publishes no valid index; concurrent
  scans use a publication version to prevent an old result from overwriting a
  new index. A change to a valid index increments the snapshot generation, and
  list validation and copying complete in the same critical section.
  The recordings directory is created lazily by the first recording; a missing
  directory on a card that has never recorded counts as an empty library rather
  than a scan failure, and an `opendir()` returning `ENOENT`/`ENOTDIR` still
  publishes an empty index (otherwise the first boot would stay in loading
  forever).
  The index is copied into the runtime cache with the actual entry count only
  when the content really changed; an unconditional 7 KB copy would pay tens of
  microseconds of interrupt latency inside the interrupts-disabled critical
  section while the bytes read are invisible to any reader.
  The full working set of each scan (`RECORDER_SERVICE_MAX_FILES` entries of
  metadata plus two path buffers, about 7.3 KB, type `recorder_scan_t`) is
  allocated independently on the heap and freed after publication or on scan
  failure; it is not placed on the `recorder_cmd` task stack and is not shared
  as scratch with a concurrent teardown scan. The scan compares field by field
  (file name, mtime, epoch) and writes directly into the target slot instead of
  building a candidate record on the stack.
  An allocation failure invalidates the index and releases this ACCESS; it does
  not report a medium fault, and a later list can request a rescan.
- The content revision is maintained centrally by `sd_storage_service`, with no
  `file_service` dependency.
  Recording first closes and finalizes the WAV, then notifies the content
  change, refreshes the index, and releases SESSION, and finally publishes
  IDLE / ERROR and allows the next start; a playback close does not modify the
  content revision.
  The cached capacity carries the mount generation, so a card removal/remount
  never returns the old capacity as a valid value.
- Only `errno == EIO` confirmed by a filesystem call reports a medium fault;
  task out-of-memory, audio failure, permission errors, or insufficient space do
  not mark a healthy SD as faulted, and file close plus lease release are still
  guaranteed.
- The stream task first finishes the audio stop, file close/finalize, index
  refresh, and SESSION release, and finally publishes `task_running=false`
  inside the lock; afterwards it only loops in a delay park and does not delete
  itself. The command task is the only creation/reclaim owner and calls
  `vTaskDeleteWithCaps` with a non-NULL handle outside the lock; while there is
  a stream task awaiting reclamation it receives commands on a 50 ms period, so
  a natural EOF/error is reclaimed without a new user command. Both STOP
  completion and SHUTDOWN exit reclaim first; a deinit timeout neither kills
  I/O nor releases resources still in use.
  These external stack paths access only SD/SDSPI and I2S and perform no NVS,
  flash filesystem, OTA, or explicit sleep.
  SDSPI reuses the SDK slot's 516-byte internal DMA staging; the amount moved
  out is not the net DMA headroom gain.
- deinit keeps initialized/stopping until all cleanup succeeds and rejects new
  commands. When the queue exists and SHUTDOWN has not been sent successfully,
  STOP and SHUTDOWN are sent in FIFO order; an enqueue failure can be retried,
  and after SHUTDOWN has been sent successfully a timeout retry only waits for
  exit and no longer appends commands to a queue nobody consumes.
  After the command task exits it deletes the queue and clears the pointer; a
  later audio release failure still lets the next deinit retry the remaining
  cleanup directly without touching an empty queue. After a successful deinit,
  init, recording, and playback work again.

## Memory Ownership

The index cache (`RECORDER_SERVICE_MAX_FILES` × per-entry metadata, 32 entries
totaling 7,168 B) lives in PSRAM. Internal DRAM simultaneously carries the
display SPI bounce buffer (10 lines × 368 × 2 B × queue 2 = 14,720 B contiguous)
and the I2S full-duplex DMA ring (5,760 B per direction); if the index stayed in
internal DRAM, a single display chunk after recording starts would fail with
`ESP_ERR_NO_MEM` and the UI would stop refreshing.

## Test Boundary

- `tests/host` uses a real pthread command worker with audio/SD fakes and covers
  asynchronous state transitions, boot without a card and remount, a failed
  capacity query / insufficient space, pause and playback SESSION, browsing
  ACCESS coexistence, scan ACCESS blocking maintenance, revision/epoch changes
  during a scan being discarded, and the ordering in which close/content
  invalidation/lease release precede the IDLE publication and the acceptance of
  the next command. While a capacity query blocks, the UI snapshot and list
  still only read the cache.
  `recorder_service_races_test` uses private implementation tests and
  deterministic barriers to cover notification, directory error retry, the
  old/new scan publication race, list validation/copy interleaving, and the
  distinction between EIO and non-storage errors.
  `recorder_service_recovery_test` uses condition variables and test-only
  clock/queue seams to cover STOP/SHUTDOWN enqueue failures, the wait timeout /
  retry after SHUTDOWN has been sent, repeated deinit while the queue is already
  deleted but audio still fails, and re-initialization/recording/playback
  without resetting the fixture.
  The audio fake uses the same mutex to validate token/generation, the exclusive
  kind, and RUNNING I/O, and a failed release retains ERROR and the owner; the
  races/stream regressions also cover recovery that retains the old generation.
  Deterministic lease barriers cover a revision change or a same-path remount
  replacement after playback/deletion acceptance: the original target ends in
  failure, the replacement file is preserved byte for byte, no audio starts, and
  SESSION / MAINTENANCE is released; they also cover successful
  playback/deletion of the current target and the index metadata matching the
  snapshot epoch.
  The scan buffer OOM regression checks index invalidation, ACCESS release, the
  medium staying usable, and later recovery.
  `recorder_service_library_test` covers the limit truncation flag, publication
  ordered by modification time descending, clearing the truncation flag after
  deleting down to the limit, and two recordings within the same second still
  getting different file names.
  `recorder_service_stream_test` defines `ESP_PLATFORM` separately and uses the
  real pthread-backed FreeRTOS stream task with condition variables to control
  audio reads and writes. It covers WAV length/header, explicit stop, maximum
  duration, playback EOF without a command, read/write errors, compensation when
  creation fails, completion before creation returns, immediate start/play,
  deinit of an active recording, and 20 repeated record/playback cycles, and
  verifies that the owner deletion leaves no accumulation of tasks/file
  handles/SESSION.
  Static stack frames do not contain the full libc/VFS/driver call chain and
  cannot replace an on-device stack high-water measurement.
- The host does not cover real I2S audio, SD card I/O, or filesystem power loss.

### Device Verification Scope

The diagnostic candidate
`095ce70ddb04083a6f95fa25c42ab21032c3846c5e9af8ab8cb827d6b1409ffa` has
completed a long recording after a weather HTTPS update, file browsing,
pause/resume, stop, and playback reaching its natural end; the user confirmed
the operations were normal and further confirmed that continuous recording works
with Wi-Fi/BLE enabled simultaneously.
The serial console confirmed that the stream stack and PCM are in PSRAM while
the command stack remains in internal RAM; every observed stream task was
reclaimed by the owner, the task count returned from 28 to 27, and no target LCD
DMA allocation failure or display submit failure occurred.

These results support the current resource placement scheme; they do not
constitute an internal heap headroom guarantee for all wireless loads, nor do
they replace cold boot, standby/wake, or filesystem power-loss verification. The
diagnostic-cleaned firmware and that candidate are stored separately; the
candidate's on-device results cannot claim that the cleaned firmware has been
re-verified on the board.
