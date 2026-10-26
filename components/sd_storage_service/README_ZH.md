[English](README.md) | **简体中文**

# sd_storage_service

独立于板级引脚的可插拔 SD 文件系统挂载服务；默认不格式化，只有显式恢复挂载
或主动格式化才允许擦除介质。同时拥有卷的使用权与介质级故障状态。

## 对外 API

公共头 `include/sd_storage_service.h`：

- `sd_storage_service_register_mount_ops()`：初始化前注册板级
  mount/unmount/is_mounted/format/get_space 适配；服务不拥有引脚映射。
- `sd_storage_service_init()`：按普通模式挂载，挂载失败绝不格式化。
- `sd_storage_service_recover_and_mount(config, expected_generation)`：显式允许
  挂载失败后格式化恢复；健康挂载不擦除。故障/清理未完成时先真实释放旧适配器。
- `sd_storage_service_unmount(expected_generation)`：安全卸载，重复清理成功后幂等。
- `sd_storage_service_format(config, expected_generation)`：擦除已挂载介质并
  重新挂载；开始时即令旧纪元失效，失败保留资源清理义务，不再公开可用状态。
- 三个用户确认操作在同一临界区检查 expected_generation 与无活动 lease；
  旧确认不得操作后来的卷。普通 init 不请求格式化。
- `sd_storage_service_get_space(usage, total, free)`：在调用者已经持有的有效
  ACCESS/SESSION/MAINTENANCE lease 下查询；不额外占用 lease 槽，失败或查询期间
  纪元失效时输出保持不变，绝不冒充剩余空间为零。
- `sd_storage_service_deinit()`：卸载并释放适配资源；`start` / `stop` 为别名。
- `sd_storage_service_get_snapshot()`：一个加锁读取的快照，包含
  mounted、phase、generation、revision、mount_path、last_error、使用权计数、
  maintenance 与 faulted。
- 使用权：`sd_storage_service_usage_acquire/release/report_fault`，分为
  ACCESS（浏览/查询）、SESSION（录音、暂停、播放）与 MAINTENANCE
  （卸载/格式化）。
- `sd_storage_service_content_revision()` / `note_content_change(usage)`：
  由 SD 服务共享并原子更新内容版本，发布成功的文件/目录变更须持有效 lease。

## 阶段与代际

`sd_storage_service_phase_t` 对外区分 `UNMOUNTED`、`MOUNTING`、`MOUNTED`、
`UNMOUNTING`、`FORMATTING` 与 `ERROR`。普通挂载失败只进入 `ERROR`，不会被
推断成“无卡”或“格式错误”。

`generation` 在挂载、卸载、主动格式化、确认的格式化恢复尝试与故障失效时
变化。即使擦除后重新挂载失败，旧确认和旧结果也已失效。使用权与故障上报
携带代际；迟到故障不会污染后续挂载。`revision` 是跨消费者的内容版本；
成功格式化也更新该版本，普通文件变更在原 SD 临界区中递增。

## 使用权规则

- ACCESS 与 SESSION 在介质已挂载且未故障时可并存：浏览可以与录音并行。
- MAINTENANCE 只有在没有任何其他使用权时授予；反过来，持有维护权时新的
  ACCESS/SESSION 会被拒绝，避免“先查 busy、再操作”的竞争窗口。
- 确认介质故障后立即 mounted=false、phase=ERROR、mount_path=NULL，并推进
  generation；所有容量/变更操作与新使用权都拒绝，直到真实清理并重新挂载。
  原 token + 原 generation 仍能释放，所有生命周期在原消费者关闭句柄并释放
  前均拒绝执行，不将其当作已消失的计数。
- 文件不存在、权限或非空目录等业务错误不属于介质故障。
- 固定八个 lease 槽。容量查询复用传入 lease；即使表已满也不额外申请，
  慢查询在锁外执行，快照读取、其他 lease 的释放与非冲突获取保持响应。

## 配置与依赖

- CMake：`PRIV_REQUIRES mt_log freertos`。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- 无 Kconfig；运行时由 `sd_storage_service_config_t`（挂载路径、最大文件数、
  分配单元）提供。

## 并发

- 单例；临界区保护状态与使用权表，**临界区内不做文件 I/O**。
- 无 worker 任务；挂载、卸载与格式化由调用者（`file_service` worker）同步
  执行。

## 归档与格式化要求

- 适配器 `format()` 必须重新挂载并确认容量可读、非零，只有全部成功才返回
  `ESP_OK`。板级后端自行持有 card/SDSPI/diskio/FatFs/VFS 资源并用公共
  `f_mount` / `f_mkfs` 组合实现，不使用 SDK sdcard 格式化组合 helper。
- 清理逐阶段提交资源状态；失败保留未完成阶段。mount/unmount/deinit 均可重试
  旧适配器清理，成功阶段不再释放，mount 不叠加旧资源。
- 容量适配器使用 `esp_vfs_fat_info()`（宿主为 `statvfs()`）；服务在锁外查询，
  返回后重新核验传入 lease/纪元，成功才发布输出。

## 测试边界

- `tests/host` 覆盖代际绑定的恢复/卸载/格式化、全表容量查询、共享内容版本、
  故障即时失效与原 lease 释放、失败清理重试；pthread 阻塞容量回调验证状态锁
  不被 I/O 占用且生命周期拒绝运行。所有适配回调均断言没有持有状态锁。
- 宿主不覆盖真实 SD 卡协议、时序与热插拔。
