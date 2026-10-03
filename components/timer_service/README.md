# timer_service

倒计时、秒表与专注（工作/休息）周期计时的单例服务，完成边沿通过可选回调发布。

## 对外 API

公共头 `include/timer_service.h`：

- `timer_service_init()` / `deinit()`：初始化或停止服务；配置提供 `monotonic_time_us` 与可选完成回调。
- `timer_service_get_snapshot()`：读取统一快照（三种计时器状态、剩余或已用时间、专注周期数）。
- `timer_service_countdown_start()` / `pause()` / `resume()` / `reset()`：倒计时控制。
- `timer_service_stopwatch_start()` / `pause()` / `reset()`：秒表控制。
- `timer_service_focus_start()` / `pause()` / `resume()` / `reset()`：专注周期控制。
- 完成事件类型 `timer_service_completion_event_t`（倒计时或专注周期）。

## 配置与依赖

- CMake：`REQUIRES esp_common freertos`。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- 无 Kconfig；时间源由装配层注入的 `monotonic_time_us` 提供。

## 状态与并发

- 单例静态运行时；原子自旋锁保护快照与状态。
- 可选完成回调在锁外下发，可由 worker 或 API 调用方触发；状态机为 IDLE / RUNNING / PAUSED / COMPLETED。

## 测试边界

- `tests/host` 存在，覆盖倒计时、秒表、专注状态机与完成边沿。
- 宿主不覆盖真机时钟漂移与任务调度时序。