[English](README.md) | **简体中文**

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
- pending 前台命令与立即执行命令共用唯一 dispatcher；SYNC_PROFILE 的候选凭据和 sync identity 采用相同深拷贝与持久化路径。
- CONNECT/DISCONNECT 只消费匹配当前 session 和非零 radio operation ID 的显式 terminal；link IP_READY 本身不能证明候选成功。只有成功 terminal 加当前有效 IPv4 才提交候选 profile，地址续租不会重复写 profile。
- terminal outbox 在每轮策略 phase 前排空；背压期间不消费 pending/retry/恢复意图。排空后由同一 worker 自主推进，不依赖额外驱动事件；STOPPING 不重开连接。
- 挂起中的显式 CONNECT/SYNC 保留原 manager operation、候选与 sync 元数据；恢复仅重启 executor，由 policy phase 使用新 radio ID/epoch 续接。取消和替代在本地发布一次终态，不对已退休的 radio ID0 发 cancel。
- retry 的绝对 deadline 跨待机保留；resume 控制失败不自动重试，也不丢弃候选。低层已实际 SUSPENDED 时，即使控制返回原 stop 错误，也保存 parked 意图供显式 resume 回滚。

## 测试边界

- 无 `tests/host`。
- 宿主不覆盖 Wi-Fi 驱动、射频、关联/DHCP 时序与 NVS 掉电；这些由 `wifi_service` 和上板验证。
