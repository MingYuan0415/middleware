# chore_service

通用有界后台 worker：在独立任务上执行短小、有界的后台作业（周期维护、快照格式化、小型存储查询等）。

## 对外 API

公共头 `include/chore_service.h`：

- `chore_service_init()` / `chore_service_deinit()`：初始化或停止单例 worker；deinit 取消全部作业并在返回前释放每个作业参数。
- `chore_service_submit()`：提交一次性或周期作业（`delay_ms`、`period_ms`）；成功后参数所有权转移给服务。
- `chore_service_cancel()`：请求协作式取消并等待作业静默。
- `chore_service_suspend()` / `chore_service_resume()`：轻睡眠前后暂停或恢复 worker。
- `chore_service_get_status()` / `chore_service_is_available()`：状态快照与可用性查询。
- `chore_service_cancel_pending()`：作业在 run 循环中轮询取消令牌。

## 配置与依赖

- Kconfig：`CHORE_SERVICE_TASK_STACK_SIZE`（默认 4096）、`CHORE_SERVICE_JOB_CAPACITY`（默认 8，1..16）。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：`PRIV_REQUIRES mt_log esp_timer freertos heap`。
- 运行时策略由 `chore_service_config_t`（worker 优先级、时长告警阈值）提供。

## 状态与并发

- 单例；一个 pinned worker 任务，固定容量作业池。
- 互斥锁保护队列，事件组做每槽取消确认；`run`/`release` 在 worker 上下文执行，不得调用 LVGL 或长时间阻塞。
- 作业句柄带世代，实例切换后旧句柄不会命中新作业。

## 测试边界

- `tests/host` 存在，覆盖提交、取消、暂停和 deinit 排空与并发语义。
- 宿主不覆盖真机任务调度时序与栈水位实际值。