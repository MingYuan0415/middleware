# system_pm

系统轻睡眠装配：配置 EXT1 GPIO 唤醒源，串行执行 prepare/complete hook，并提供睡眠提交校验与 CPU 最高频率锁。

## 对外 API

公共头 `include/system_pm.h`：

- `system_pm_init()` / `deinit()`：复制配置，创建 standby worker 与 CPU 频率锁。
- `system_pm_request_standby()`：排入一次异步待机事务。
- `system_pm_cancel_standby()`：取消排队中的准备或重试待机恢复。
- `system_pm_acquire_cpu_max_freq()` / `release_cpu_max_freq()`：获取或释放最高频率 PM 锁。
- 类型：`system_pm_config_t`、`system_pm_wake_source_t`、`system_pm_wake_event_t`、`system_pm_sleep_hook_t`、`system_pm_commit_guard_t`、`system_pm_wake_reason_t`。

## 配置与依赖

- Kconfig：`SYSTEM_PM_STANDBY_TASK_STACK`（默认 4096）。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：`PRIV_REQUIRES mt_log esp_pm esp_driver_gpio esp_hw_support`。
- 运行时由 `system_pm_config_t` 提供唤醒源、必需 prepare/complete hook、可选唤醒/提交回调与任务优先级。

## 状态与并发

- 单例；pinned standby worker 任务、互斥锁、临界区与事件组。
- hook 与唤醒回调在 worker 上下文运行；唤醒回调必须只入队或通知并立即返回。
- commit guard 在睡眠提交点做最终校验，commit callback 只做无阻塞记录。

## 测试边界

- 无 `tests/host`。
- 宿主不覆盖真实 light sleep 进入与唤醒、RTC GPIO 电气与功耗。