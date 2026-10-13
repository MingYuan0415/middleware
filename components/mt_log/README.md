# mt_log

统一日志门面：按源文件的 `DBG_LVL` 在编译期裁剪 `LOG_E/W/I/D/V`，并提供错误跳转宏。源文件未定义 `DBG_LVL` 时，使用 Kconfig 全局默认 `CONFIG_MT_LOG_DEFAULT_LEVEL`。

## 对外 API

公共头 `include/mt_log.h`：

- `LOG_E()` / `LOG_W()` / `LOG_I()` / `LOG_D()` / `LOG_V()`：按 `DBG_LVL` 与 `DBG_TAG` 映射到 ESP-IDF 日志宏；低于阈值的调用被裁剪为 `(void)0`。未定义 `DBG_LVL` 时回退到 `CONFIG_MT_LOG_DEFAULT_LEVEL`。
- `DBG_ERROR` 到 `DBG_VERBOSE`：日志级别常量，取值 1..5，与 ESP-IDF `esp_log_level_t` 一致。
- `MT_ERROR_HANDLE(err, line)`：记录首个错误行并 `goto exit`（要求调用方声明错误/行变量与 `exit` 标签）。
- `mt_log_init()`：进程日志门面初始化，当前直接返回 `ESP_OK`。

## 配置与依赖

- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：无额外 `REQUIRES` / `PRIV_REQUIRES`。
- Kconfig `MT_LOG_DEFAULT_LEVEL`：`int`，范围 1..5，默认 3（INFO）。作为未定义 `DBG_LVL` 的源文件的编译期上限；显式定义 `DBG_LVL` 的文件不受其影响。
- `DBG_TAG` 由各源文件在使用前自行定义。

## 状态与并发

- 无状态、无锁，仅编译期宏和一个空初始化函数。

## 测试边界

- 无 `tests/host`；行为等价于对 ESP-IDF 日志宏的封装。