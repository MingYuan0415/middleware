# connectivity_manager

Wi-Fi 连接策略 owner：策略 worker 负责扫描、连接、重连、配置持久化与自动连接状态机，低层 radio 操作委托给 `wifi_service`。

## 对外 API

公共头 `include/connectivity_manager.h`：

- `connectivity_manager_init()` / `deinit()` / `suspend()` / `resume()`：生命周期与待机。
- `connectivity_manager_clear_persisted_profile()`：初始化前幂等擦除持久化 profile（复位域）。
- 前台命令：`request_scan()`、`request_connect()`、`request_sync_profile()`、`request_save_profile()`、`request_disconnect()`、`request_reconnect_saved()`、`request_forget()`、`set_auto_connect()`、`cancel()`；均返回非零 `operation_id`。
- `connectivity_manager_get_status()` / `get_scan_snapshot()`：读取不含密码的不可变状态或扫描快照。
- `connectivity_manager_is_available()`：公开 API 是否可用。
- 通过 `EVENT_BUS_DECLARE_ID(CONNECTIVITY_MANAGER_MSG)` 发布状态与扫描快照事件。

## 配置与依赖

- Kconfig：`CONNECTIVITY_MANAGER_TASK_STACK`（默认 6144）、`CONNECTIVITY_MANAGER_QUEUE_DEPTH`（默认 8）。
- CMake：`REQUIRES event_bus`；`PRIV_REQUIRES freertos mbedtls mt_log nvs_flash nv_storage wifi_service`。
- 运行时由 `connectivity_manager_config_t`（策略 worker 优先级、低层 Wi-Fi worker 优先级）配置。

## 状态与并发

- 单例；静态分配的策略 worker、低层 Wi-Fi worker、命令队列与信号量。
- 前台操作以 `operation_id` 世代标识；状态与扫描快照带 `generation`。
- 连接命令在准入时深拷贝候选凭据，调用返回后可释放调用方缓冲。

## 测试边界

- 无 `tests/host`。
- 宿主不覆盖 Wi-Fi 驱动、射频、关联/DHCP 时序与 NVS 掉电；这些由 `wifi_service` 和上板验证。