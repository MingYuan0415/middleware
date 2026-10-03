# imu_service

周期采样板载 IMU（QMI8658C），维护最新快照，并经事件总线发布快照、可用性与中断事件。

## 对外 API

公共头 `include/imu_service.h`：

- `imu_service_register_ops()` / `register_imu_ops()`：启动前注册板级操作表（`read` 必需，其余可选）。
- `imu_service_init()` / `start()`：创建采样 worker 并开始连续采样。
- `imu_service_stop()` / `deinit()`：停止 worker；deinit 释放 worker 同步对象并清空操作表。
- `imu_service_suspend()` / `resume()`：暂停或恢复采样，暂停时可选关闭传感器电源门。
- `imu_service_get_state()`：worker 生命周期状态。
- `imu_service_get_snapshot()`：复制缓存快照，不访问硬件。
- `imu_service_read()` / `read_sample()`：与 worker 读串行化的同步硬件读取。
- 发布 `IMU_SERVICE_MSG` 的 SNAPSHOT_UPDATE、AVAILABILITY_CHANGED、INTERRUPT 子类型。

## 配置与依赖

- Kconfig：`IMU_SERVICE_TASK_STACK`（默认 3072）。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：`REQUIRES event_bus`；`PRIV_REQUIRES mt_log esp_timer`。
- 运行时由 `imu_service_config_t`（采样率、任务优先级）与注册的 ops 表提供。

## 状态与并发

- 单例；pinned worker 任务、互斥锁、临界区与事件组。
- 同步读与 worker 读共用同一串行化边界；暂停、恢复、停止期间的读会被拒绝。
- 服务在接受样本时填充单调时间戳与序号。

## 测试边界

- `tests/host` 存在，覆盖 worker 生命周期、快照/读路径与暂停语义。
- 宿主不覆盖真机传感器 I2C、INT1 电平与 ODR 时序。