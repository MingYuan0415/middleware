[English](README.md) | **简体中文**

# weather_service

后台天气获取、缓存与不可变快照服务，覆盖位置、当前、逐小时、逐日与预警数据集。

## 对外 API

公共头 `include/weather_service.h`：

- `weather_service_init()` / `deinit()` / `suspend()` / `resume()`：生命周期与待机。
- `weather_service_set_network_ready()`：通知 IPv4 可用性；就绪边沿触发一次位置会话。
- `weather_service_request_refresh()`：请求一次用户发起的刷新，受本地与上游 Retry-After 限制。
- `weather_service_notify_time_changed()`：时间同步后通知 worker 重新评估快照新鲜度。
- `weather_service_get_status()`：小型状态快照。
- `weather_service_snapshot_acquire()` / `snapshot_release()`：引用计数获取或释放不可变完整快照。
- `weather_service_is_available()`：公开 API 是否可用。
- 发布 `WEATHER_SERVICE_MSG` 的 SNAPSHOT 子类型（可合并 UI 通知）。

## 配置与依赖

- Kconfig：`WEATHER_SERVICE_TASK_STACK_SIZE`（默认 8192）、`WEATHER_SERVICE_MAX_RESPONSE_BYTES`（默认 262144）。
- `idf_component.yml`：`idf: ">=5.5"`、`espressif/cjson: "^1.7.19"`。
- CMake：`REQUIRES event_bus`；`PRIV_REQUIRES mt_log esp_http_client esp_timer mbedtls espressif__cjson freertos heap`。
- 运行时由 `weather_service_config_t`（server URL、device token、缓存目录、刷新周期、`allow_private_http`）提供。

## 状态与并发

- 单例；worker 任务、生命周期互斥锁、状态互斥锁与事件组。
- 公开调用只在生命周期就绪时准入；销毁先关闭准入并等待在途调用，清理完成前拒绝新调用。`deinit(timeout_ms)` 的同一个总截止时间覆盖生命周期锁获取、状态锁获取、调用方静默和 worker 退出。deinit 超时会保留 cleanup-pending 所有权，下次 deinit 重试最终释放，而不是在存活资源上重新初始化。
- 快照通过 acquire/release 引用计数共享；状态机覆盖未配置、等网、定位、更新、就绪、降级、鉴权错误、限流、挂起与错误。
- 每轮网络周期携带生命周期 epoch 与会话身份；旧周期的响应、解析结果、缓存完成、状态写入或事件会被丢弃，不能提交到已恢复或重新初始化的服务。
- `fetched_at` 记录设备接收时间。本地过期截止时间不依赖网络刷新，断网时仍会生效。

## 测试边界

- `tests/host` 存在，覆盖解析、缓存与状态机。
- 宿主覆盖 async HTTPS 轮询与取消、私有 HTTP 模式选择、响应/解析生命周期竞态、离线过期与设备接收时间。取消会中断已连接的活动 client；DNS/TCP/TLS 连接完成前的 IDF 阶段无法取消，由 generation 失效丢弃其结果。
- 宿主不覆盖真实 HTTPS/TLS、DNS、定位与上游 API 行为。