# power_service

周期轮询板载 PMU（AXP2101）的电池与充电信息及中断状态，维护缓存快照并经事件总线发布。

## 对外 API

公共头 `include/power_service.h`：

- `power_service_register_power_ops()`：初始化前注册板级操作表（`get_info` 必需，可用性与 IRQ 轮询可选）。
- `power_service_init()` / `deinit()`：启动或停止采样 worker。
- `power_service_suspend()` / `resume()`：待机前后暂停或恢复采样。
- `power_service_get_snapshot()`：复制缓存快照（含有效性）。
- `power_service_get_info()`：复制最近一次有效电力信息。
- 发布 `POWER_SERVICE_MSG` 的 SNAPSHOT_UPDATE、AVAILABILITY_CHANGED、IRQ 子类型。

## 配置与依赖

- Kconfig：`POWER_SERVICE_TASK_STACK`（默认 3072）。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：`REQUIRES event_bus`；`PRIV_REQUIRES mt_log esp_timer`。
- 运行时由 `power_service_config_t`（遥测周期、IRQ 轮询周期、任务优先级）与注册 ops 表提供。

## 状态与并发

- 单例；pinned worker 任务、互斥锁、临界区与事件组。
- 快照读取不访问 PMU；IRQ 边沿以非合并事件发布。

## 测试边界

- `tests/host` 存在，覆盖快照缓存、可用性边沿与 IRQ 路径。
- 宿主不覆盖 PMU I2C、中断线与电池实际电气特性。