# sd_storage_service

独立于板级引脚的可插拔 SD 文件系统挂载服务；默认不格式化，只有显式恢复挂载才允许格式化。

## 对外 API

公共头 `include/sd_storage_service.h`：

- `sd_storage_service_register_mount_ops()`：初始化前注册板级 mount/unmount/is_mounted 适配；服务不拥有引脚映射。
- `sd_storage_service_init()`：按普通模式挂载，挂载失败绝不格式化。
- `sd_storage_service_recover_and_mount()`：显式允许挂载失败后格式化恢复。
- `sd_storage_service_deinit()`：卸载并释放适配资源；`start` / `stop` 为别名。
- `sd_storage_service_is_mounted()` / `get_mount_path()` / `get_handle()` / `get_config()`：状态查询。
- `sd_storage_service_get_snapshot()`：一次性读取 mounted/generation/mount_path 快照；`generation` 仅在服务自身挂载/卸载迁移时递增，用于识别新的挂载纪元。当前板无卡检测信号，服务无法观测热插拔存在性。

## 配置与依赖

- CMake：`PRIV_REQUIRES mt_log freertos`。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- 无 Kconfig；运行时由 `sd_storage_service_config_t`（挂载路径、最大文件数、分配单元）提供。

## 状态与并发

- 单例；临界区保护挂载状态，无 worker 任务，挂载与卸载同步执行。
- 挂载错误时若非空 `out_handle` 会尝试卸载清理；清理失败则在 deinit 重试。

## 测试边界

- `tests/host` 存在，使用 fake 适配覆盖挂载模式、格式化策略与清理重试。
- 宿主不覆盖真实 SD 卡协议、时序与热插拔。