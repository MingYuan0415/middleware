**English** | [简体中文](README_ZH.md)

# file_service

Asynchronous storage control and file browsing service. A dedicated worker
performs all file I/O; volume lifecycle, usage rights, mount generation, and
content revision are all owned by `sd_storage_service`.

## Public Interface

- `init/deinit`, `suspend/resume`, `get_snapshot`, and `is_busy` manage the
  lifecycle.
- `request_storage(action, expected_generation, result)` accepts a normal mount,
  explicit format recovery, safe unmount, active format, or capacity refresh.
  The generation must come from the snapshot taken at the moment of the user
  action and must not be re-fetched at submission time to replace the
  confirmation basis.
- At most four browse sessions; `session_list` uses relative paths and an
  optional cursor.
- `session_detail/delete` use `file_service_target_t`, carrying the relative
  path, generation, and revision from when the user selected the item. Both
  submission and execution check the version, so a stale target never deletes a
  different file that later took the same name.
- `session_get_batch/detail` copy the last successfully published data into the
  caller buffer; internal pointers are never lent out. The caller allocates
  large batch/detail buffers independently on the heap, not in the Page arena or
  on a task stack.
- `session_get_result/cursor` copy the status/cursor. Content-change
  notifications and revision queries use the SD service API under a valid lease
  directly; this component holds no second revision.

## Bounded Memory and Ownership

Initialization allocates, outside the state lock, a fixed pool that prefers
PSRAM and falls back to the ordinary 8-bit heap, containing four sessions with
their own request slots, one storage request slot, published batches/details,
and dedicated worker scratch. These objects are neither allocated nor freed
during operation; the queue only copies slot pointers. The worker's 6144-byte
stack is allocated explicitly with `SPIRAM | 8BIT` and does not fall back to
internal RAM; the TCB, queues, and locks remain in internal RAM. No 1 KiB or
larger stack object exists on the worker, initialization, or submission chain.
The task path operates only on SD and must not perform NVS, flash file system,
OTA, or explicit sleep calls from that external stack.
An initialization failure releases all partial allocations; the caller does not
take ownership of the service and does not need to call `deinit`; it may still
re-initialize later.
The worker's CPU affinity uses `CONFIG_MAIN_PROJECT_TASK_CORE_ID`, consistent
with a project configuration of CPU0, CPU1, or no affinity; the host regression
validates the task creation parameters with the CPU1 configuration.

Each session has at most one in-flight operation at a time. Nonzero session IDs
and operation IDs are allocated independently and are not immediately reused on
slot reuse or mount switchover. Closing a session immediately revokes the read
interface; the slot of an in-flight request is retained until the single
completion path closes the handle, releases the lease, and settles pending,
after which it may be opened again.
Cancellation does not terminate an executing system call. Delete atomically
checks cancellation and marks mutation-started before entering `unlink/rmdir`;
after that it retains the real success or failure result and never reports an
already-executed delete as canceled. Closing the page still revokes the read
interface and does not hand the result to a session reopened later. Inside the
state lock only bounded state/result copies and non-blocking pointer enqueues
happen; no file I/O, SD queries, or memory allocation/free.

Each directory batch holds at most 16 entries, directories first and then
regular files, preserving file system order within a class. The cursor carries
stream offset, generation, and revision; each batch closes the directory, and a
continued read reopens and seeks. A running batch writes only scratch, so the
copy getter can still read the previously published complete batch. After 16
entries it still looks ahead to the next eligible entry; `has_more` is published
only when items truly remain, the cursor stops before that entry, and a
directory with exactly 16 entries never produces an empty continuation page.

## Paths and Errors

- The relative path `""` denotes the browse root; the root cannot be a detail or
  delete target.
- Absolute paths, `.`, `..`, empty segments, backslashes, and overlong paths are
  rejected, never silently truncated.
- The name buffer is 1021 bytes (255 four-byte UTF-8 Unicode scalars plus NUL);
  the full backend path is still limited to 1024 bytes, including the
  mount/root prefix and NUL.
- The host checks every path component, including the configured root, rejects
  all symbolic links (even those pointing inside the root), and calls
  `realpath` with an independent `PATH_MAX`-sized scratch before checking the
  public path budget. On-device FAT file systems have no symbolic links.
- Delete uses only `unlink` and `rmdir`, never recursively. For the EACCES that
  FAT may return, it checks whether the directory is actually non-empty, so as
  to distinguish NOT_EMPTY from PERMISSION.
- Both admission failures and asynchronous terminal states provide `error`, a
  structured `reason`, and the saved `system_errno`. Not-found, non-empty,
  permission, path-too-long, out-of-space, out-of-memory, stale, unmounted,
  busy, canceled, and I/O errors are not conflated; an unmounted and
  non-faulted volume reports UNAVAILABLE, a mounted volume with a lease conflict
  reports BUSY, and a medium fault reports IO.
  `readdir` errors are clearly distinguished from normal EOF, and cleanup does
  not overwrite the original errno.

## Mount, Capacity, and Stop

The initial normal mount is a regular storage request counted into pending,
with a nonzero operation ID; a failure does not prevent the service from
reading snapshots and accepting retries. A normal mount does not format; format
recovery must arrive through an explicit action.
Epoch checks for unmount, format, and recovery are completed atomically by the
SD owner together with the no-active-lease check.
Capacity queries pass the current ACCESS lease; the result is collected outside
the lock and published inside it. After a mount, recovery, or format, a capacity
query failure also makes that storage request FAILED and does not publish a
misleading DONE.
When `space.valid` is false, the value must not be treated as usable capacity.
faulted/last_error in the snapshot mirror the SD owner directly and do not
confuse a normal unmount with a medium fault.
A capacity query saves the errno of the failure; only EIO reports a medium fault
before the ACCESS lease is released, invalidating the generation and requiring a
normal mount to re-probe the card after completing old resource cleanup. Other
capacity errors only invalidate the capacity and never mark a healthy volume as
faulted.

`suspend` closes admission and waits for pending to reach zero; on timeout it
still closes admission, and the caller should explicitly `resume` during
standby rollback. `deinit` permanently closes admission, closes sessions, and
drains existing requests without interrupting maintenance.
The worker sends the completion semaphore after its last access to service
state, then loops in delayed park; after receiving the handshake, the owner
calls `vTaskDeleteWithCaps` with a non-null handle and then releases the queues,
locks, and memory.
On timeout the task, stack, all resources, and the stopped state are retained;
`resume` does not reopen a service that is stopping, and another `deinit` can
safely complete the release.

## Host Regression

`tests/host` compiles the real service worker and a pthread-backed FreeRTOS
queue/task shim, uses real POSIX files and a scripted SD owner; a link wrapper
controls blocking I/O, queue-full, and errno. It covers close/reopen, queued
cancellation, copy publication, suspend, initial-mount pending, stop-timeout
retry, stale delete, structured errors, readdir failure, EACCES non-empty
directory, and symbolic links.
The regression covers LIST/DETAIL round-trips of Chinese names, directory-first
batched enumeration of 1100 directories with bounded service allocation, real
cancel/close results after a blocking delete, and capacity query failure after
maintenance. Real SD timing, card-removal interruption, and FatFs differences
still require on-device verification.

The worker's task stack is 6 KiB. A serial build carrying diagnostic candidates
has confirmed that the worker stack resides in PSRAM, and the user completed
file browsing during recording and playback and reported normal behavior. The
host regression covers caps worker creation failure compensation, owner
reclamation, and, under blocking I/O, stop-timeout retention of the task and
lease followed by retry release after unblocking.
These results do not replace the stack high-water mark of the real SD/FatFs call
chain, nor do they mean the deepest paths, unmount, active/recovery format, or
faulty-medium scenarios have all been verified on device.

```sh
cmake -S layers/middleware/components/file_service/tests/host -B /tmp/mt-file-service -G Ninja
cmake --build /tmp/mt-file-service
ctest --test-dir /tmp/mt-file-service --output-on-failure
```
