**English** | [简体中文](README_ZH.md)

# nv_storage

Lifecycle owner of the default NVS partition: provides K-V scalar reads and
writes plus two tiers of fixed-capacity blob support — registration, default
values, validation, and automatic loading.

## Public API

Public header `include/nv_storage.h`:

- `nv_storage_init()` / `deinit()`: initializes or releases the default NVS
  partition and the blob registry; this component owns that partition's
  lifecycle exclusively.
- `nv_storage_set_u8/get_u8()`, `set_u16/get_u16()`, `set_u32/get_u32()`,
  `set_str/get_str()`, `set_blob/get_blob()`, `erase_key()`: the scalar layer
  opens a handle temporarily for each operation and closes it after the
  read/write.
- `nv_storage_blob_register()`: registers a blob with the fixed pool (key,
  buffer, size, default-value callback, optional validation callback).
- `nv_storage_blob_load_all()`: loads all registered blobs using the
  "read, validate, fall back to default" logic.
- Callback types `nv_storage_blob_default_cb_t` /
  `nv_storage_blob_validate_cb_t`.

## Configuration and Dependencies

- Kconfig: `NV_STORAGE_BLOB_POOL_SIZE` (default 16, 1..64).
- `idf_component.yml`: only `idf: ">=5.0"`.
- CMake: `PRIV_REQUIRES freertos nvs_flash mt_log`.
- Runtime configuration comes from the key, buffer, and callbacks supplied at
  registration; scalar keys are at most 15 bytes.

## State and Concurrency

- Singleton; a static mutex serializes the blob registry; the scalar API does
  not depend on that lock and may be used while blobs are loading.
- The registry is frozen while blobs load; the service holds no registry lock
  while a callback runs.
- Until deinit succeeds, other components must not independently
  init/deinit the default partition or retain NVS handles across deinit.

## Test Boundary

- `tests/host` exists and covers blob registration, loading, default-value
  fallback, and NVS fault injection.
- The host does not cover ESP32-S3 NVS flash wear, power-loss consistency, or
  encryption.
