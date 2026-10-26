[English](README.md) | **简体中文**

# device_link_service

Device Link v1 产品策略 owner：持有 `ble_runtime` 生命周期，管理配对窗口、Numeric Comparison 本地确认、Bluetooth 启用策略与绑定撤销，并把 Wi-Fi 操作提交给 `connectivity_manager`。

## 对外 API

公共头 `include/device_link_service.h`：

- `device_link_service_init()` / `deinit()`：启动或停止服务与 BLE runtime（`runtime_port` 为必需项）。
- `open_window()` / `close_window()` / `confirm_binding()` / `pending_confirmation()`：配对窗口与本地确认；确认 token 必须与状态快照中的一致。
- `revoke_binding()`：本地撤销绑定（journaled，无线上命令）。
- `set_enabled()`：记录最新目标；service worker 持久化并以有界状态机收敛，状态快照暴露 desired/effective/policy_error。
- `release_startup_gate()`：`FACTORY_RESET_GATED` 启动模式在复位域清除后提交可见性。
- `suspend()` / `resume()` / `get_status()` / `is_active()` / `is_busy()`：待机与状态查询。
- `include/device_link_wifi_adapter.h` 提供 GET_INFO 固件三元组、Device Link 操作到 `connectivity_manager` 的提交适配与 bridge 生命周期。

## 配置与依赖

- Kconfig：`DEVICE_LINK_SERVICE_TASK_STACK`（默认 6144）、`DEVICE_LINK_SERVICE_QUEUE_DEPTH`（默认 8）。
- CMake：`REQUIRES ble_runtime device_link connectivity_manager event_bus nv_storage wifi_service`；`PRIV_REQUIRES esp_hw_support freertos mt_log nvs_flash`。
- 运行时由 `device_link_service_config_t`（runtime port、任务优先级、窗口时长、启动模式、固件版本）提供。

## 状态与并发

- 单例；静态 worker 任务、命令队列、互斥锁与递归互斥锁。
- init/deinit 必须来自单个 owner task；状态快照带单调 `generation`。
- GATT/SMP 事实由 NimBLE 回调保留并唤醒本 owner；回调不直接更新 LVGL 或发布应用事件。
- Bluetooth 关闭与完整 deinit 共用停机阶段：关闭广播与窗口、host 屏障关闭连接入口、清除逻辑会话、等待断连及撤销清理、第二次屏障确认后释放 runtime。正常清理等待不消耗失败次数；超时保留阶段和资源，后续请求或 deinit 可继续推进。反向开启先完成本轮释放，成功后没有固定冷却。
- Wi-Fi 请求入队与 BLE/manager 操作 ID 登记在同一 bridge 锁内完成；发布线程读取完整映射，并在调用 link-service 完成接口前释放该锁。
- 普通断连、host RESET、窗口关闭、revoke 与 Bluetooth 开关不清除已接受的 Wi-Fi 操作关联；engine 继续保留 device-scoped record，断连后不自动重放 indication，客户端回连可按原 ID 查询/ACK。
- status 与 scan 共用固定大小的不可变 terminal slot；完成接口的瞬态失败保留首次结果与扫描列表，重复/更新事件只触发重试，不能改写该终态。新请求仅在 manager 成功准入后替换旧映射；完整 service deinit 才退休 bridge。
- Bluetooth 关闭仍推进原 operation deadline 和既有唤醒触发的完成重试；TX/广播/窗口处理仍要求 runtime ready，操作期限不会因开关重置。

## 测试边界

- `tests/host` 存在，使用 fake `connectivity_manager`、`event_bus`、`nv_storage` 覆盖窗口、确认、撤销与 Wi-Fi adapter。
- 宿主不覆盖 NimBLE 回调时序、NVS 持久化、射频与 Android 互操作。
