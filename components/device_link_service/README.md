# device_link_service

Device Link v1 产品策略 owner：持有 `ble_runtime` 生命周期，管理配对窗口、Numeric Comparison 本地确认、Bluetooth 启用策略与绑定撤销，并把 Wi-Fi 操作提交给 `connectivity_manager`。

## 对外 API

公共头 `include/device_link_service.h`：

- `device_link_service_init()` / `deinit()`：启动或停止服务与 BLE runtime（`runtime_port` 为必需项）。
- `open_window()` / `close_window()` / `confirm_binding()` / `pending_confirmation()`：配对窗口与本地确认；确认 token 必须与状态快照中的一致。
- `revoke_binding()`：本地撤销绑定（journaled，无线上命令）。
- `set_enabled()`：本地持久化并应用 Bluetooth 启用策略。
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

## 测试边界

- `tests/host` 存在，使用 fake `connectivity_manager`、`event_bus`、`nv_storage` 覆盖窗口、确认、撤销与 Wi-Fi adapter。
- 宿主不覆盖 NimBLE 回调时序、NVS 持久化、射频与 Android 互操作。