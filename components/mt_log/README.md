**English** | [简体中文](README_ZH.md)

# mt_log

Unified logging facade: trims `LOG_E/W/I/D/V` at compile time according to
each source file's `DBG_LVL`. When a source file does not define `DBG_LVL`,
the Kconfig global default `CONFIG_MT_LOG_DEFAULT_LEVEL` applies.

## Public API

Public header `include/mt_log.h`:

- `LOG_E()` / `LOG_W()` / `LOG_I()` / `LOG_D()` / `LOG_V()`: map to the
  ESP-IDF logging macros according to `DBG_LVL` and `DBG_TAG`; calls below the
  threshold are trimmed to `(void)0`. When `DBG_LVL` is undefined, the level
  falls back to `CONFIG_MT_LOG_DEFAULT_LEVEL`.
- `DBG_ERROR` through `DBG_VERBOSE`: log level constants, valued 1..5,
  matching ESP-IDF `esp_log_level_t`.
- `mt_log_init()`: process log facade initialization; currently returns
  `ESP_OK` directly.

## Configuration and Dependencies

- `idf_component.yml`: only `idf: ">=5.0"`.
- CMake: no additional `REQUIRES` / `PRIV_REQUIRES`.
- Kconfig `MT_LOG_DEFAULT_LEVEL`: `int`, range 1..5, default 3 (INFO). It is
  the compile-time ceiling for source files that do not define `DBG_LVL`;
  files that explicitly define `DBG_LVL` are not affected by it.
- `DBG_TAG` is defined by each source file before use.

## State and Concurrency

- Stateless and lock-free; only compile-time macros and an empty
  initialization function.

## Test Boundary

- No `tests/host`; the behavior is equivalent to wrapping the ESP-IDF logging
  macros.
