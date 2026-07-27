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
| `time_service` | 维护 `CST-8` 本地时区、RTC/日历 alarm 桥接、时钟可信度和异步 SNTP 同步 | `event_bus`、`nv_storage`、网络栈等（私有） |
| `wifi_service` | 以常驻 worker 串行处理扫描、连接、断开、挂起和恢复，发布缓存快照 | `event_bus`；ESP-IDF Wi-Fi/网络组件（私有） |
| `ble_service` | BLE 生命周期占位实现；初始化可用，启用、禁用和扫描当前返回 `ESP_ERR_NOT_SUPPORTED` | `event_bus` |

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
idf_component_register(SRCS "app.c" REQUIRES event_bus wifi_service)
```

构建依赖不会初始化运行时。应在单线程启动阶段初始化 `mt_log`、`nv_storage` 和 `event_bus`；在服务启动前分别调用 `power_service_register_power_ops()`、`imu_service_register_imu_ops()`、`time_service_register_rtc_ops()` 和 `sd_storage_service_register_mount_ops()`。`audio_service_init()` 直接取得已提交的 `bsp_audio_ops_t`，因此也必须在 `bsp_init()` 成功后调用；在 `wifi_service_init()` 前准备 ESP-NETIF 和默认事件循环。停止发布者并等待已接纳工作完成后，再按逆序反初始化。`event_bus` 是进程生命周期单例，没有反初始化接口。

## 新增服务 API 与事件

- `imu_service` 的板级表要求 `read`，并可提供 `is_available`、`configure`、`set_enabled` 和 `poll_interrupt`。初始化会在启用传感器前把 `IMU_SERVICE_SAMPLE_RATE_HZ` 传给 `configure`，使 worker 周期与硬件 ODR 使用同一请求值。公共 API 支持 init/start、stop/deinit、suspend/resume、缓存 `get_snapshot` 和同步 `read`；`IMU_SERVICE_MSG` 发布快照、可用性变化和 interrupt subtype。
- `audio_service` 支持 Kconfig 默认格式查询、configure、start/stop、全双工 read/write、0-100 音量、mute 和 NS4150B PA 控制。它不发布 event bus 消息。
- `sd_storage_service` 的 adapter 提供 mount/unmount/is_mounted；公共 API 支持 init/start、stop/deinit，以及 mount path、handle、active config 查询。默认挂载点是 `/sdcard`，`format_if_mount_failed` 默认关闭，挂载失败不会自动格式化。
- `power_service` 的 `poll_irq` 返回已消费的 AXP2101 latched status。非零状态以 `POWER_SERVICE_MSG_SUB_TYPE_IRQ` 和 `power_service_irq_event_t` 发布；该边沿事件使用 flags `0`，不会被 `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` 覆盖。遥测快照仍按独立周期更新。
- `time_service` 的 RTC 表现在要求 alarm 功能要么全部不提供，要么完整提供 configure/disable/get_status/clear/poll_interrupt。`time_service_alarm_*` 管理重复 UTC 日历 alarm；worker 以固定 100 ms 周期轮询低有效 RTC_INT，并用 flags `0` 发布 `TIME_SERVICE_MSG_SUB_TYPE_RTC_ALARM` sequence 事件。

显示 TE 同步不属于 middleware 服务 API。BSP 通过 `bsp_display_port_t.te` 导出 GPIO13 上升沿、所选 SPI 频率（项目经验默认 40 MHz；80 MHz 为超规格实验）、4 data lines 和当前 16 bpp 物理参数；`layers/app_manager` 据此启用 TE sync，并补充 adapter 默认 13/1 ms、66% 刷新窗口。

## 配置

使用 `idf.py menuconfig` 调整以下选项：

| 服务 | 配置项（默认值；范围） |
| --- | --- |
| 电源 | `POWER_SERVICE_TASK_STACK`（3072；2048-8192）、`POWER_SERVICE_TASK_PRIORITY`（4；1-24）、`POWER_SERVICE_POLL_INTERVAL_MS`（5000；1000-60000）、`POWER_SERVICE_IRQ_POLL_INTERVAL_MS`（100；10-1000） |
| IMU | `IMU_SERVICE_TASK_STACK`（3072；2048-8192）、`IMU_SERVICE_TASK_PRIORITY`（6；1-24）、`IMU_SERVICE_SAMPLE_RATE_HZ`（100；1-1000） |
| 音频 | 默认 16 kHz、16-bit、双声道、384x MCLK、streaming 时 PA 开启；`AUDIO_SERVICE_*` choice 提供 8-96 kHz、16/24/32-bit、mono/stereo 及受采样格式约束的 MCLK 倍频 |
| SD | `SD_STORAGE_SERVICE_ENABLE`（y）、`SD_STORAGE_SERVICE_MOUNT_PATH`（`/sdcard`）、`SD_STORAGE_SERVICE_FORMAT_IF_MOUNT_FAILED`（n）、`SD_STORAGE_SERVICE_MAX_FILES`（5；1-32）、`SD_STORAGE_SERVICE_ALLOCATION_UNIT_SIZE`（16384；0-65536） |
| 系统 PM | `SYSTEM_PM_STANDBY_TASK_STACK`（4096；2048-16384）、`SYSTEM_PM_STANDBY_TASK_PRIO`（5；1-24）、`SYSTEM_PM_DEVELOPMENT_MODE`（n；USB Serial/JTAG 连接时禁止 standby） |
| Wi-Fi | `WIFI_SERVICE_TASK_STACK`（4096；3072-8192）、`WIFI_SERVICE_TASK_PRIORITY`（4；1-20）、`WIFI_SERVICE_QUEUE_DEPTH`（16；8-32）、`WIFI_SERVICE_WORKER_POLL_MS`（20；5-100）、`WIFI_SERVICE_EVENT_DRAIN_TIMEOUT_MS`（1000；100-5000） |

时间服务当前固定使用 `CST-8`、`pool.ntp.org` 和 100 ms alarm IRQ 轮询，没有对应 Kconfig。修改配置后运行 `idf.py reconfigure && idf.py build`。

## 并发与资源边界

- `event_bus` API 仅限任务上下文，不支持 ISR。最多 24 个订阅、24 个待处理 UI 回调和 24 份 UI payload；匹配 UI 订阅时 payload 最大 256 字节。发布者回调同步执行，UI 回调异步执行；取消订阅不是静默屏障，销毁 `user_data` 前仍需停止发布者并排空 UI 工作。
- `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` 只用于可覆盖的状态快照，不得用于边沿、命令、审计或计数事件。事件 payload 只在回调期间有效。
- `nv_storage` 成功初始化后独占默认 NVS 分区生命周期。键最长 15 字节，Blob 注册池为 16 项；注册数据缓冲和回调必须存活到成功反初始化。Blob 加载会冻结注册表，但回调执行时不持锁。
- Wi-Fi 公共请求是非阻塞接纳操作，扫描快照最多保存 5 条记录；SSID 和个人网络密码上限分别为 32、63 字节。Wi-Fi、时间、电源、IMU、音频、SD 和系统 PM 的挂起、等待、I/O 或反初始化接口可能阻塞，生命周期调用必须由上层串行化。
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
