# MicroTech Middleware

本仓库提供 MicroTech 固件的通用 ESP-IDF 服务组件。组件以单例服务为主，覆盖日志、事件分发、NVS、时间/RTC alarm、Wi-Fi、电源采样与 IRQ、IMU、音频、可移除 SD 存储和系统轻睡眠；要求 ESP-IDF 5.0 或更高版本。

## 组件

| 组件 | 职责与当前状态 | 直接组件依赖 |
| --- | --- | --- |
| `mt_log` | 封装 ESP-IDF 日志级别，并提供统一 `exit` 错误处理宏 | 无 |
| `event_bus` | 固定容量、线程安全的发布/订阅总线，支持发布者任务和 UI worker 两种分发上下文 | `mt_log`（私有） |
| `nv_storage` | 管理默认 NVS 分区，提供标量 K-V API 和固定池 Blob 注册/校验/默认值回退 | `freertos`、`nvs_flash`、`mt_log`（私有） |
| `power_service` | 通过板级操作表采样 PMU，并以独立周期消费 latched IRQ；缓存快照并发布状态/IRQ 事件 | `event_bus`；`mt_log`、`esp_timer`（私有） |
| `imu_service` | 周期采样 QMI8658C 适配器，缓存带 sequence 的三轴快照并发布状态/中断事件 | `event_bus`；`mt_log`、`esp_timer`（私有） |
| `audio_service` | 管理 BSP ES8311/NS4150B 全双工 PCM、音量、静音和 PA 生命周期 | `bsp`；`freertos`、`mt_log`（私有） |
| `sd_storage_service` | 通过板级 mount adapter 管理可移除存储生命周期，不绑定 SDSPI/SDMMC 实现 | `freertos`、`mt_log`（私有） |
| `system_pm` | 串行执行外设休眠钩子、ESP32 轻睡眠与唤醒恢复，并管理 CPU 最高频率锁 | `mt_log` 和 ESP-IDF PM/GPIO/硬件支持组件（私有） |
| `time_service` | 维护 `CST-8` 本地时区、RTC/日历 alarm 桥接、时钟可信度和系统级异步 SNTP 同步 | `event_bus`、`nv_storage`、网络栈等（私有） |
| `connectivity_manager` | 生产 Wi-Fi 策略唯一所有者；管理 profile、自动连接、长退避、前台抢占和待机协调 | `event_bus`；`nv_storage`、`wifi_service`（私有） |
| `wifi_service` | 单射频异步执行层；串行处理扫描、连接、断开和射频挂起，不持久化 STA 凭据 | `event_bus`；ESP-IDF Wi-Fi/网络组件（私有） |
| `provisioning_service` | 手动开启的 Protocomm BLE Security 2 配网服务；实现 v1.0 轮询协议并将 Wi-Fi 操作交给 `connectivity_manager` | `connectivity_manager`, `event_bus` |

## 目录结构

每个 `components/<name>/` 都是独立 ESP-IDF 组件：`include/` 是公开 API，`src/` 是内部实现，`CMakeLists.txt` 声明构建依赖，`idf_component.yml` 声明最低 IDF 版本。可调服务带有 `Kconfig`；当前独立宿主测试位于 `audio_service`、`nv_storage`、`power_service`、`sd_storage_service` 和 `time_service` 的 `tests/host/`。

## 集成与初始化

在应用顶层 `CMakeLists.txt` 的 `project()` 之前加入组件目录：

```cmake
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/path/to/middleware/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
```

消费组件通过 `REQUIRES` 或 `PRIV_REQUIRES` 声明所需服务，例如：

```cmake
idf_component_register(SRCS "app.c" REQUIRES connectivity_manager event_bus)
```

构建依赖不会初始化运行时。应在单线程启动阶段初始化 `mt_log`、`nv_storage` 和 `event_bus`；在服务启动前分别调用 `power_service_register_power_ops()`、`imu_service_register_imu_ops()`、`time_service_register_rtc_ops()` 和 `sd_storage_service_register_mount_ops()`。Audio、SD、IMU、Power、Time、Connectivity 和 System PM 的 init 都接收类型化配置；`audio_service_init()` 直接取得已提交的 `bsp_audio_ops_t`，因此必须在 `bsp_init()` 成功后调用。启动 Wi-Fi 前先准备 ESP-NETIF 和默认事件循环，再调用 `connectivity_manager_init()`；manager 内部长期持有唯一的 `wifi_service` session。生产代码不得自行初始化或控制 `wifi_service`。相同配置重复初始化幂等，活动状态下不同配置返回 `ESP_ERR_INVALID_STATE`。停止发布者并等待已接纳工作完成后，再按逆序反初始化。`event_bus` 是进程生命周期单例，没有反初始化接口。

## 新增服务 API 与事件

- `imu_service` 的板级表要求 `read`，并可提供 `is_available`、`configure`、`set_enabled` 和 `poll_interrupt`。初始化会把运行时 `sample_rate_hz` 同时用于硬件 ODR 和 worker 周期。公共 API 支持 init/start、stop/deinit、suspend/resume、缓存 `get_snapshot` 和同步 `read`。
- `audio_service_init_config_t` 同时提供 PCM、初始音量、mute 和 PA；`audio_service_get_config()` 返回当前有效 PCM 格式。服务支持 configure、start/stop 和全双工 read/write，不发布 event bus 消息。
- `sd_storage_service` 的 adapter 提供 mount/unmount/is_mounted；普通 `init/start(config)` 从不格式化。破坏性恢复只能由显式 `sd_storage_service_recover_and_mount(config)` 发起。
- `power_service` 的 `poll_irq` 返回已消费的 AXP2101 latched status。非零状态以 `POWER_SERVICE_MSG_SUB_TYPE_IRQ` 和 `power_service_irq_event_t` 发布；该边沿事件使用 flags `0`，不会被 `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` 覆盖。遥测快照仍按独立周期更新。
- `time_service` 的 RTC 表现在要求 alarm 功能要么全部不提供，要么完整提供 configure/disable/get_status/clear/poll_interrupt。`time_service_alarm_*` 管理重复 UTC 日历 alarm；worker 以固定 100 ms 周期轮询低有效 RTC_INT，并用 flags `0` 发布 `TIME_SERVICE_MSG_SUB_TYPE_RTC_ALARM` sequence 事件。
- `connectivity_manager` 用 NVS 单键 `wifi_profile` 保存一个 Open/Personal IPv4 网络；仅在取得 IPv4 后提交新凭据。它发布不含密码的状态和扫描快照，统一分类认证、AP、关联、DHCP、链路、射频、存储和内部错误。长期自动重试为 30 秒、2 分钟、10 分钟、30 分钟并封顶；手动断开只在本次启动保持离线。
- `time_service_set_network_ready()` 是非阻塞电平通知。每个 IPv4 联网周期只启动一次系统 SNTP，首次成功更新后立即停止；掉线和待机也会停止，唤醒后等待 Wi-Fi 重连取得新 IPv4 再同步。应用的“立即校时”可在在线时另行发起一次请求，页面关闭不取消系统请求。

显示 TE 同步不属于 middleware 服务 API。BSP 通过 `bsp_display_port_t.te` 导出 GPIO13 上升沿、所选 SPI 频率（项目经验默认 40 MHz；80 MHz 为超规格实验）、4 data lines 和当前 16 bpp 物理参数；`layers/app_manager` 据此启用 TE sync，并补充 adapter 默认 13/1 ms、66% 刷新窗口。

## 配置

Kconfig 只保留静态资源预算：Event Bus 三个池 24、payload 256 B，NVS blob pool 16，
IMU/Power stack 3072，Time stack 3072，Connectivity stack 4096 和 queue 8，Wi-Fi
stack 4096 和 queue 16，System PM stack 4096。采样率、轮询周期、任务优先级、PCM、挂载点、时区和 SNTP server 都由根
`app_product_config_t` 在运行时传入。`SYSTEM_PM_DEVELOPMENT_MODE` 是根产品开发 gate，
定义于 `main/Kconfig.projbuild`。修改 Kconfig 后运行
`idf.py reconfigure && idf.py save-defconfig && idf.py build`。

## 并发与资源边界

- `event_bus` API 仅限任务上下文，不支持 ISR。最多 24 个订阅、24 个待处理 UI 回调和 24 份 UI payload；匹配 UI 订阅时 payload 最大 256 字节。发布者回调同步执行，UI 回调异步执行；取消订阅不是静默屏障，销毁 `user_data` 前仍需停止发布者并排空 UI 工作。
- `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` 只用于可覆盖的状态快照，不得用于边沿、命令、审计或计数事件。事件 payload 只在回调期间有效。
- `nv_storage` 成功初始化后独占默认 NVS 分区生命周期。键最长 15 字节，Blob 注册池为 16 项；注册数据缓冲和回调必须存活到成功反初始化。Blob 加载会冻结注册表，但回调执行时不持锁。
- Connectivity 公共请求是非阻塞接纳操作，扫描快照最多保存 5 条记录；SSID 和个人网络密码上限分别为 32、63 字节。`wifi_service` 公共接口仅保留给 manager 和底层测试。Connectivity、Wi-Fi、时间、电源、IMU、音频、SD 和系统 PM 的挂起、等待、I/O 或反初始化接口可能阻塞，生命周期调用必须由上层串行化。
- `system_pm` 接受 1 至 4 个唯一 RTC GPIO 唤醒源，且有效电平必须一致。唤醒回调应只通知其他 worker；外设准备和恢复钩子运行在 PM worker 中，可以阻塞但必须遵守配置超时。
- `SYSTEM_PM_DEVELOPMENT_MODE=y` 不是让 USB Serial/JTAG 在 light sleep 中继续工作；ESP32-S3 硬件不支持这一点。该模式在 USB 主机连接时跳过 app standby（显示仍可熄灭），并启用 IDF 的自动睡眠连接保护；拔出 USB 后恢复正常 light sleep。
- 当前板级 EXIO3/5/6 经过 TCA9554，只能由 time/power/IMU worker 轮询，不能成为 RTC GPIO 唤醒源。触摸唤醒尚未实现，GPIO21 未注册；实际 wake descriptor 仍只有 GPIO0 低电平。

## 宿主测试

需要 CMake 3.16+、Ninja、C11 编译器和 pthread。从本 middleware 子模块根目录运行 NVS 套件：

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

时间套件还覆盖 RTC alarm 的非合并发布、清除和重新触发。运行 power IRQ worker 套件：

```sh
cmake -S components/power_service/tests/host -B /tmp/mt-power -G Ninja \
    -DPOWER_SERVICE_SANITIZER=none
cmake --build /tmp/mt-power
ctest --test-dir /tmp/mt-power --output-on-failure
```

运行 audio 和 SD 生命周期套件：

```sh
cmake -S components/audio_service/tests/host -B /tmp/mt-audio -G Ninja
cmake --build /tmp/mt-audio
ctest --test-dir /tmp/mt-audio --output-on-failure

cmake -S components/sd_storage_service/tests/host -B /tmp/mt-sd -G Ninja
cmake --build /tmp/mt-sd
ctest --test-dir /tmp/mt-sd --output-on-failure
```

`NV_STORAGE_SANITIZER`、`TIME_SERVICE_SANITIZER` 和 `POWER_SERVICE_SANITIZER` 支持 `none`、`address` 和 `thread`；切换 sanitizer 时使用独立构建目录。x86_64 上若存在 `setarch`，CTest 会为 TSan 测试自动关闭 ASLR。QMI8658C、SDSPI partial rollback、ES8311 板级生命周期、PCF85063 alarm 和 AXP2101 profile 由 `layers/bsp/tests/host` 覆盖；`layers/app_manager/app_core/tests/host` 校验 BSP TE 参数到 adapter TE sync 的映射。所有硬件行为仍需随整机工程构建并按风险上板验证。

## 许可证

本项目采用 [MIT License](LICENSE)。
