[English](README.md) | **简体中文**

# event_bus

线程安全的固定容量发布/订阅总线，支持发布者任务同步回调与 UI worker 异步回调两种分发上下文。

## 对外 API

公共头 `include/event_bus.h`：

- `event_bus_init()` / `event_bus_deinit()`：单例生命周期。仅在单线程启动阶段初始化一次；反初始化要求所有发布者与分发器已停止。
- `EVENT_BUS_DECLARE_ID()` / `EVENT_BUS_DEFINE_ID()` / `EVENT_BUS_ID()`：消息身份。身份是描述符地址，描述符同时携带稳定名称，供日志与统计使用。
- `event_bus_subscribe()` / `event_bus_unsubscribe()`：按消息与 subtype 订阅或退订，句柄带世代保护。
- `event_bus_unsubscribe_sync()`：取消单个订阅，并等待该订阅自己的发布者回调结束，再释放 `user_data`。
- `event_bus_flush_sync()`：等待整个总线不再有发布者回调运行。
- `event_bus_publish()` / `event_bus_publish_ex()`：发布消息。publisher 回调同步执行并接收调用方缓冲区；UI 回调收到共享不可变 payload 副本。
- `event_bus_register_ui_dispatch()` / `unregister_ui_dispatch()`：注册或注销非阻塞 UI worker 入队函数，并上报其队列深度。
- `event_bus_abandon_pending_ui()`：分发器队列销毁后的收尾钩子，用于释放未派发的 UI 批次。
- `event_bus_register_wake_requester()` / `unregister_wake_requester()`：注册或注销非阻塞唤醒请求。
- `event_bus_get_stats()`：读取分发计数器。
- `EVENT_BUS_PUBLISH_FLAG_WAKE_REQUEST` / `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` / `EVENT_BUS_PUBLISH_FLAG_ATOMIC`：唤醒、UI 快照合并、双路全有或全无。

## 投递模型

- **两条独立路径。** UI 入队失败不会抑制 publisher 回调；`event_bus_publish_ex()` 分别报告两路结果。`EVENT_BUS_PUBLISH_FLAG_ATOMIC` 为需要自行重试、且不能重复观察同一事件的调用方保留旧的“全有或全无”语义。
- **可见性。** 订阅者只接收其建立之后发生的发布。总线用 64 位单调序号表达该谓词，因此不再保存每次发布的订阅快照，长寿命订阅也不会因序号半空间回绕而失效。
- **合并。** `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` 按消息与 subtype 替换待派发批次的 payload，并由最新序号决定接收者。状态快照必须自包含；合并发布之后才出现的订阅者应在恢复时向状态所有者主动读取当前值。
- **顺序。** 每次待派发发布对应一个 UI 批次，除非它合并进更早的批次；回调按订阅槽顺序执行。跨消息不保证全局顺序。
- **生命周期。** `event_bus_unsubscribe()` 只做失效；`event_bus_unsubscribe_sync()` 额外等待该订阅自己的发布者回调排空。UI 回调与订阅在分发任务上串行执行，因此 UI 订阅必须在分发任务上取消。仍有发布者回调的槽位不会被复用，因此同步退订超时会暂时减少可用订阅容量；若回调仍在运行，用同一句柄再次调用会继续等待，回调排空后再次调用返回 `ESP_OK`。

## 配置与依赖

- Kconfig：`EVENT_BUS_SUBSCRIBER_CAPACITY`（默认 24）、`EVENT_BUS_UI_BATCH_CAPACITY`（默认 8）、`EVENT_BUS_UI_PAYLOAD_SIZE`（默认 320）、`EVENT_BUS_STATS`（默认 y）、`EVENT_BUS_TRACE`（默认 n）。`EVENT_BUS_TRACE` 会把发布 trace 编入总线并提升该文件的编译期日志上限；运行时仍需允许 `event_bus` 标签使用 `ESP_LOG_DEBUG`（初始化后调用 `esp_log_level_set("event_bus", ESP_LOG_DEBUG)`）。UI 批次容量不得超过注册分发器时上报的队列深度。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：`PRIV_REQUIRES mt_log`。
- UI 分发器与唤醒请求函数由装配层在运行时注册。
- 各发布方以 `_Static_assert` 将自身 payload 类型绑定到 `EVENT_BUS_MAX_UI_PAYLOAD_SIZE`：快照增长会在构建期失败，而不是运行期失败。

## 状态与并发

- 单例；一把互斥锁保护订阅表与待处理批次池，一把准入互斥锁串行化 UI 批次创建，一个二值信号量唤醒同步排空等待者。非 ISR 安全。
- UI 订阅在注册 dispatcher 之前会被拒绝；已存在 UI 订阅者时，dispatcher 被注销期间发布会返回 `ESP_ERR_INVALID_STATE`。
- 待处理 UI 元数据位于内部 RAM，只有 payload 池通过 `EXT_RAM_BSS_ATTR` 放入外部 RAM。
- `event_bus_abandon_pending_ui()` 只能在分发器队列销毁之后调用；若仍有批次回调可能执行，属于释放后使用。
- 被拒绝的 UI 发布会计入 `event_bus_get_stats()`，并以 warning 级别最多每 5 秒记录一次；`late_subscriber_skips` 统计分发时观察到、且在发布之后才建立的匹配订阅者。

## 测试边界

- `tests/host` 覆盖句柄世代、长寿命可见性、合并、双路隔离、容量、批次回收、按订阅与全局排空、迟到订阅者计数，以及基于共享 pthread FreeRTOS fake 的核心状态机；`-DEVENT_BUS_SANITIZER=thread` 提供竞态档位。
- 套件还会以 `EVENT_BUS_TRACE` 编译总线，并使用会求值参数的日志 fake，因此 trace 专属的构建错误会直接失败。
- 真实实现还会以 `HOST_TEST=1` 编入 `tests/integration`、`tests/connectivity` 与 `test_ble_wifi_recovery`。
- 宿主不覆盖真实 UI worker/LVGL 调度与唤醒硬件路径。
