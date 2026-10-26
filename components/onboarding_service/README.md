**English** | [简体中文](README_ZH.md)

# onboarding_service

Lightweight singleton service that persists the first-boot onboarding state
(PENDING / DEFERRED / COMPLETED).

## Public API

Public header `include/onboarding_service.h`:

- `onboarding_service_init()`: reads the state from `nv_storage`; a missing or
  invalid byte falls back to PENDING and is written back.
- `onboarding_service_deinit()`: deinit.
- `onboarding_service_get_state()`: copies the current state.
- `onboarding_service_defer()` / `complete()` / `reset()`: update and persist
  the state.

## Configuration and Dependencies

- CMake: `REQUIRES nv_storage nvs_flash`.
- `idf_component.yml`: only `idf: ">=5.0"`.
- No Kconfig; the state is stored under the fixed `nv_storage` key
  `onboard_state`.

## State and Concurrency

- Singleton with a static state variable; no locks and no worker; the API
  executes synchronously.
- Returns an error according to the lifecycle when called before init or when
  already initialized; corrupted persisted bytes are corrected to PENDING.

## Test Boundary

- No `tests/host`.
- The host does not cover NVS power loss or the real first-boot path.
