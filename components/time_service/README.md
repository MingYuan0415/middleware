# time_service

系统时钟、RTC 桥接与 SNTP 同步的单例服务，并管理 RTC 日历闹钟与闹钟中断事件。

## 对外 API

公共头 `include/time_service.h`：

- `time_service_register_rtc_ops()`：初始化前注册板级 RTC 操作（读、写、闹钟、中断轮询，均可选）。
- `time_service_init()` / `deinit()` / `suspend()` / `resume()`：生命周期与待机；挂起期间拒绝 RTC/SNTP 控制操作。
- `time_service_get_utc()` / `get_local()` / `set_local()`：读取 UTC 或本地时间，设置本地时间并尽力写回 RTC。
- `time_service_get_quality()` / `get_last_rtc_error()`：时钟可信度与最近 RTC 结果。
- `time_service_alarm_configure()` / `alarm_disable()` / `alarm_get_status()` / `alarm_clear()`：循环日历闹钟控制。
- `time_service_set_network_ready()` / `request_sync()` / `cancel_sync()` / `wait_sync()` / `sync_ntp()`：SNTP 同步控制；`get_time` / `set_time` 为兼容包装。
- 发布 `TIME_SERVICE_MSG` 的 RTC_ALARM 子类型。

## 配置与依赖

- Kconfig：`TIME_SERVICE_SYNC_WORKER_STACK`（默认 3072）。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：`REQUIRES event_bus`；`PRIV_REQUIRES mt_log nv_storage esp_netif lwip`。
- 运行时由 `time_service_config_t`（POSIX 时区、SNTP 服务器、任务优先级）提供。

## 状态与并发

- 单例；pinned SNTP worker、互斥锁与事件组。
- 多个 RTC 操作可能阻塞 I2C；生命周期控制调用必须串行化。
- 在线、离线与挂起会停止或排队 SNTP 请求；时钟可信度由 `time_service_quality_t` 表达。

## 测试边界

- `tests/host` 存在，覆盖时钟、闹钟与 SNTP 状态机。
- 宿主不覆盖 RTC I2C、真实网络与 SNTP 服务器行为。