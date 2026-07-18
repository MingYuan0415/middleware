# MicroTech Middleware

本仓库提供 MicroTech 固件的通用 ESP-IDF 服务组件。组件以单例服务为主，覆盖日志、事件分发、NVS、时间、Wi-Fi、电源采样和系统轻睡眠；要求 ESP-IDF 5.0 或更高版本。

## 组件

| 组件 | 职责与当前状态 | 直接组件依赖 |
| --- | --- | --- |
| `mt_log` | 封装 ESP-IDF 日志级别，并提供统一 `exit` 错误处理宏 | 无 |
| `event_bus` | 固定容量、线程安全的发布/订阅总线，支持发布者任务和 UI worker 两种分发上下文 | `mt_log`（私有） |
| `nv_storage` | 管理默认 NVS 分区，提供标量 K-V API 和固定池 Blob 注册/校验/默认值回退 | `freertos`、`nvs_flash`、`mt_log`（私有） |
| `power_service` | 通过板级操作表轮询 PMU，缓存快照并发布状态事件 | `event_bus`；`mt_log`、`esp_timer`（私有） |
| `system_pm` | 串行执行外设休眠钩子、ESP32 轻睡眠与唤醒恢复，并管理 CPU 最高频率锁 | `mt_log` 和 ESP-IDF PM/GPIO/硬件支持组件（私有） |
| `time_service` | 维护 `CST-8` 本地时区、RTC 桥接、时钟可信度和异步 SNTP 同步 | `event_bus`、`nv_storage`、网络栈等（私有） |
| `wifi_service` | 以常驻 worker 串行处理扫描、连接、断开、挂起和恢复，发布缓存快照 | `event_bus`；ESP-IDF Wi-Fi/网络组件（私有） |
| `ble_service` | BLE 生命周期占位实现；初始化可用，启用、禁用和扫描当前返回 `ESP_ERR_NOT_SUPPORTED` | `event_bus` |

## 目录结构

每个 `components/<name>/` 都是独立 ESP-IDF 组件：`include/` 是公开 API，`src/` 是内部实现，`CMakeLists.txt` 声明构建依赖，`idf_component.yml` 声明最低 IDF 版本。可调服务带有 `Kconfig`；当前宿主测试位于 `nv_storage/tests/host/` 和 `time_service/tests/host/`。

## 集成与初始化

在应用顶层 `CMakeLists.txt` 的 `project()` 之前加入组件目录：

```cmake
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/path/to/middleware/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
```

消费组件通过 `REQUIRES` 或 `PRIV_REQUIRES` 声明所需服务，例如：

```cmake
idf_component_register(SRCS "app.c" REQUIRES event_bus wifi_service)
```

构建依赖不会初始化运行时。应在单线程启动阶段初始化 `mt_log`、`nv_storage` 和 `event_bus`；在 `power_service_init()`、`time_service_init()` 前分别注册板级 PMU、RTC 操作；在 `wifi_service_init()` 前准备 ESP-NETIF 和默认事件循环。停止发布者并等待已接纳工作完成后，再按逆序反初始化。`event_bus` 是进程生命周期单例，没有反初始化接口。

## 配置

使用 `idf.py menuconfig` 调整以下选项：

| 服务 | 配置项（默认值；范围） |
| --- | --- |
| 电源 | `POWER_SERVICE_TASK_STACK`（3072；2048-8192）、`POWER_SERVICE_TASK_PRIORITY`（4；1-24）、`POWER_SERVICE_POLL_INTERVAL_MS`（5000；1000-60000） |
| 系统 PM | `SYSTEM_PM_STANDBY_TASK_STACK`（4096；2048-16384）、`SYSTEM_PM_STANDBY_TASK_PRIO`（5；1-24） |
| Wi-Fi | `WIFI_SERVICE_TASK_STACK`（4096；3072-8192）、`WIFI_SERVICE_TASK_PRIORITY`（4；1-20）、`WIFI_SERVICE_QUEUE_DEPTH`（16；8-32）、`WIFI_SERVICE_WORKER_POLL_MS`（20；5-100）、`WIFI_SERVICE_EVENT_DRAIN_TIMEOUT_MS`（1000；100-5000） |

时间服务当前固定使用 `CST-8` 和 `pool.ntp.org`，没有对应 Kconfig。修改配置后运行 `idf.py reconfigure && idf.py build`。

## 并发与资源边界

- `event_bus` API 仅限任务上下文，不支持 ISR。最多 24 个订阅、24 个待处理 UI 回调和 24 份 UI payload；匹配 UI 订阅时 payload 最大 256 字节。发布者回调同步执行，UI 回调异步执行；取消订阅不是静默屏障，销毁 `user_data` 前仍需停止发布者并排空 UI 工作。
- `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` 只用于可覆盖的状态快照，不得用于边沿、命令、审计或计数事件。事件 payload 只在回调期间有效。
- `nv_storage` 成功初始化后独占默认 NVS 分区生命周期。键最长 15 字节，Blob 注册池为 16 项；注册数据缓冲和回调必须存活到成功反初始化。Blob 加载会冻结注册表，但回调执行时不持锁。
- Wi-Fi 公共请求是非阻塞接纳操作，扫描快照最多保存 5 条记录；SSID 和个人网络密码上限分别为 32、63 字节。Wi-Fi、时间、电源和系统 PM 的挂起、等待或反初始化接口可能阻塞，生命周期调用必须由上层串行化。
- `system_pm` 接受 1 至 4 个唯一 RTC GPIO 唤醒源，且有效电平必须一致。唤醒回调应只通知其他 worker；外设准备和恢复钩子运行在 PM worker 中，可以阻塞但必须遵守配置超时。

## 宿主测试

需要 CMake 3.16+、Ninja、C11 编译器和 pthread。从本仓库根目录运行 NVS 套件：

```sh
cmake -S components/nv_storage/tests/host -B /tmp/mt-nv -G Ninja \
    -DNV_STORAGE_SANITIZER=none
cmake --build /tmp/mt-nv
ctest --test-dir /tmp/mt-nv --output-on-failure
```

运行时间服务的回调代际门和端口排空屏障套件：

```sh
cmake -S components/time_service/tests/host -B /tmp/mt-time -G Ninja \
    -DTIME_SERVICE_SANITIZER=none
cmake --build /tmp/mt-time
ctest --test-dir /tmp/mt-time --output-on-failure
```

两个缓存变量都支持 `none`、`address` 和 `thread`；切换 sanitizer 时使用独立构建目录。x86_64 上若存在 `setarch`，CTest 会为 TSan 测试自动关闭 ASLR。其余组件目前没有独立宿主测试，相关改动需随整机工程构建并按硬件风险上板验证。

## 许可证

本项目采用 [MIT License](LICENSE)。
