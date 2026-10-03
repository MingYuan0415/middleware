# event_bus

线程安全的固定容量发布/订阅总线，支持发布者任务同步回调与 UI worker 异步回调两种分发上下文。

## 对外 API

公共头 `include/event_bus.h`：

- `event_bus_init()`：单例初始化，仅在单线程启动阶段调用一次。
- `EVENT_BUS_DECLARE_ID()` / `EVENT_BUS_DEFINE_ID()` / `EVENT_BUS_ID()`：消息身份声明与定义。
- `event_bus_subscribe()` / `event_bus_unsubscribe()`：按消息与 subtype 订阅或退订，句柄带世代保护。
- `event_bus_publish()`：发布消息；publisher 回调同步执行，UI 回调收到共享不可变 payload 副本。
- `event_bus_register_ui_dispatch()` / `unregister_ui_dispatch()`：注册或注销非阻塞 UI worker 入队函数。
- `event_bus_register_wake_requester()` / `unregister_wake_requester()`：注册或注销非阻塞唤醒请求。
- `EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST` / `EVENT_BUS_PUBLISH_FLAG_UI_LATEST`：唤醒与 UI 快照合并标志。

## 配置与依赖

- Kconfig：`EVENT_BUS_SUBSCRIBER_CAPACITY`、`EVENT_BUS_UI_CALLBACK_CAPACITY`、`EVENT_BUS_UI_PAYLOAD_CAPACITY`（默认 24）、`EVENT_BUS_UI_PAYLOAD_SIZE`（默认 256）。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：`PRIV_REQUIRES mt_log`。
- UI 分发器与唤醒请求函数由装配层在运行时注册。

## 状态与并发

- 单例；互斥锁保护订阅表和 UI 待处理池；非 ISR 安全。
- UI 订阅在注册 dispatcher 之前会被拒绝；退订不等待已通过存活检查的回调，调用方需自行同步 `user_data` 销毁。

## 测试边界

- 无 `tests/host`。
- 宿主不覆盖真实 UI worker/LVGL 调度与唤醒硬件路径。