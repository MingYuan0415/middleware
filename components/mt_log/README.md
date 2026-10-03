# mt_log

统一日志门面：按每个源文件定义的 `DBG_LVL` 在编译期裁剪 `LOG_E/W/I/D/V`，并提供错误跳转宏。

## 对外 API

公共头 `include/mt_log.h`：

- `LOG_E()` / `LOG_W()` / `LOG_I()` / `LOG_D()` / `LOG_V()`：按 `DBG_LVL` 与 `DBG_TAG` 映射到 ESP-IDF 日志宏；低于阈值的调用被裁剪为 `(void)0`。
- `DBG_ERROR` 到 `DBG_VERBOSE`：日志级别常量。
- `MT_ERROR_HANDLE(err, line)`：记录首个错误行并 `goto exit`（要求调用方声明错误/行变量与 `exit` 标签）。
- `mt_log_init()`：进程日志门面初始化，当前直接返回 `ESP_OK`。

## 配置与依赖

- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：无额外 `REQUIRES` / `PRIV_REQUIRES`。
- 无 Kconfig；`DBG_TAG` 与 `DBG_LVL` 由各源文件在使用前自行定义。

## 状态与并发

- 无状态、无锁，仅编译期宏和一个空初始化函数。

## 测试边界

- 无 `tests/host`；行为等价于对 ESP-IDF 日志宏的封装。