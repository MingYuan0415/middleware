# nv_storage Host Tests

This suite compiles the module's current `src/nv_storage.c` and
`src/nv_storage_blob.c` directly, replacing only FreeRTOS and the ESP-IDF NVS
port. It covers lifecycle, failure retry, key boundaries, scalar transactions,
Blob defaults and validation, failure atomicity, and concurrent load,
registration, deinit, and callback re-entry.

Blob load must keep the following invariants: `load_begin` sets `LOAD_ACTIVE`
first, and the first acquisition of the registry mutex is used to wait for
registration operations that have already entered; after the lock is released
the registry stays frozen, so the load can traverse `s_blobs` directly. No
registry lock may be held while a callback runs; registration, duplicate load,
and deinit re-entry must return `ESP_ERR_INVALID_STATE` immediately, and the
scalar NVS APIs remain usable.

Run from the repository root:

```sh
cmake -S layers/middleware/components/nv_storage/tests/host \
    -B /tmp/mt-nv-normal -G Ninja -DNV_STORAGE_SANITIZER=none
cmake --build /tmp/mt-nv-normal
ctest --test-dir /tmp/mt-nv-normal --output-on-failure

```

`NV_STORAGE_SANITIZER=address` or `thread` is an additional profile for
memory-ownership or concurrency changes. Select the profile matching the risk
and use a separate build directory; both profiles are not required by default.

Under x86_64, CTest runs the TSan tests with `setarch -R` to disable ASLR for
the test process, avoiding address-mapping conflicts during ThreadSanitizer
startup.
