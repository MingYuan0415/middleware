# MicroTech Middleware

本仓库提供 MicroTech 固件的通用 ESP-IDF 服务组件。组件以单例服务为主，覆盖日志、事件分发、NVS、时间/RTC alarm、Wi-Fi、天气、电源采样与 IRQ、IMU、音频、可移除 SD 存储和系统轻睡眠；要求 ESP-IDF 5.0 或更高版本。

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
| `ble_runtime` | NimBLE host、静态 GATT、GAP/ADV、Device Link transport、不可变异步身份和 TX/deadline 调度的唯一底层 owner | `bt`；`device_link_security`、`freertos`、`mt_log`（私有） |
| `device_link_security` | 使用 ESP-IDF 官方 Protocomm Security 2 类型实现握手、bootstrap/long-term verifier 和授权记录 journal；这些 protobuf-c 类型仅属于 ESP-IDF 内部安全实现 | `nv_storage`、`protocomm`；`mbedtls`、`protobuf-c`（私有） |
| `device_link_service` | 串行拥有绑定窗口、Security 2 会话、授权/清理事务、应用状态和 factory-reset startup gate | `ble_runtime`、`device_link_security`、`event_bus` |
| `factory_reset_service` | 持有版本化恢复出厂 journal；marker 持久化后才重启，并在全部 reset domain 与广告前置条件收敛后清除 marker | `nv_storage`；`mt_log`（私有） |
| `weather_service` | 每个 IPv4 会话完成一次城市级定位（`/api/v1/location`），以服务端下发的 opaque `location_key` 作为位置作用域身份；顺序更新实时、预警、逐小时和逐日数据，并提供 PSRAM 不可变快照及 A/B 离线缓存 | `event_bus`；HTTP、cJSON、FreeRTOS、heap（私有） |
| `chore_service` | 单一后台杂活 worker：任意任务可提交一次性或周期 job（固定槽位池、协作式取消、挂起后不追补），用于不便在 GUI worker 或调用上下文执行的短时有界工作；禁止 LVGL 调用与长阻塞 | `mt_log`、`esp_timer`、`freertos`、`heap`（私有） |
| `device_link` | Device Link Core v2 应用层 Typed-TLV、固定分片头、静态领域描述符、路由、操作和 replay 原语；不拥有 NimBLE/Protocomm，不包含应用层 protobuf | 无 |

## 目录结构

每个 `components/<name>/` 都是独立 ESP-IDF 组件：`include/` 是公开 API，`src/` 是内部实现，`CMakeLists.txt` 声明构建依赖，`idf_component.yml` 声明最低 IDF 版本。可调服务带有 `Kconfig`；当前独立宿主测试还覆盖 `ble_runtime`、`device_link_security`、`device_link_service`、`factory_reset_service`、`device_link` 和 `chore_service`，入口均位于各组件的 `tests/host/`。

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
- `connectivity_manager` 是 Wi-Fi profile、自动连接和无感同步的唯一 owner。`connectivity_manager_request_sync_profile()` 以非零 client sync ID 接受凭据；相同 ID/相同凭据幂等，相同 ID/不同凭据冲突，新 profile 只有 IPv4 和 durable store 都成功后替换旧 profile。密码永不读回；存储结果不明确时保留旧 profile 并返回可恢复的 storage/internal 错误。
- `device_link_service` worker 是 ACL、Security 2 epoch、授权事务、`link_state` 投递和清理义务的逻辑 owner。生产队列和 TX completion 通过 task notification 唤醒；worker 按最近绝对 deadline 等待并在每个循环出口 sweep。所有迟到事件以 `{generation, security_epoch, flow_id, token, kind, conn_handle}` 过滤；ACL 终态以 generation/handle 清理该 ACL 的全部 epoch，会话级失败仍核对 epoch/flow。Cmd0 只分配一次 epoch，Cmd1 在同一 epoch 认证。`link_state` 的当前值与投递 stamp 在 GATT 锁下分离，认证/CCCD 变化立即要求 fresh 值，提交或异步 completion 失败在 100 ms cooldown 后由 worker retained retry。
- provisional/orphan/replacement cleanup 由 owner 按 100/200/400/800/1000 ms 退避保留，port 以固定 `4 + 1 overflow` 容量和物理目标合并避免队列满丢失。pending cleanup 拒绝新 ACL；live terminal cleanup 保留 session/control write fence 和 host-serialized terminate retry，公开 `link_state` 仍可读，广告仅在所有 cleanup 清空后恢复。peer-store 删除采用单次 explicit delete、逐类型 readback 和完整 host-run sticky error，持久化失败不能被同 run 的 RAM absence 冒充成功；deinit 通过双 host barrier 的 fixed-point drain 保留 revoke/cleanup 义务。
- `ble_runtime` 的 TX scheduler 以固定 `queue_depth + 1` credit 覆盖 queued、in-flight 和待投递 completion，每个成功提交只产生一个终态。ADV START/STOP 失败保留 generation-scoped obligation，并按 100/200/400/800/1000 ms 退避；普通窗口取消使用不受 ADV 队列容量影响的通知唤醒。pairing gate 的 requested-open 与 cleanup/rejected/revoke/drain hold 独立，effective open 仅在 hold mask 为空时成立；被拒绝 ACL 在 CONNECT host callback 内先关 gate 再保留终止义务。
- `device_link_security` 在 Cmd1 proof/Resp1 成功后立即标记认证；授权事务按 `PREPARED -> COMMIT_PROBED -> LOCALLY_CONFIRMED -> COMMITTED` 推进，本地确认同时核对 connection generation、security_epoch 与凭据 id，durable Commit 再以 boot-scoped token 探测幂等。durable Commit 的幂等结果在当前 ACL 内跨真实 long-term Security 2 重握手保留，并在 ACL 终态、replacement cutover、revoke/reset 时清除。Recovery Query 将确定不存在与 NVS/损坏记录的 ambiguous 失败分开映射。
- `time_service_set_network_ready()` 是非阻塞电平通知。每个 IPv4 联网周期只启动一次系统 SNTP，首次成功更新后立即停止；掉线和待机也会停止，唤醒后等待 Wi-Fi 重连取得新 IPv4 再同步。应用的“立即校时”可在在线时另行发起一次请求，页面关闭不取消系统请求。
- `weather_service` 将定位、HTTPS、JSON、重试和缓存全部留在 PSRAM worker 中。每个 IPv4 会话只请求一次定位；手动刷新不重复定位。天气响应携带的 `location_key` 是服务端按 0.1° 网格派生的不透明作用域标识：同一网格恒定、不暴露坐标，key 变化即清空旧数据集并按“实时优先”全量刷新，避免跨网格的陈旧或混合快照；可选的 `district` 区县名为显示字段（本地化成功时出现、永不从设备头回显），不参与位置身份判定；缓存不落盘 key 与 district，重启后由会话定位重新建立。UI 只 acquire/release 不可变快照，事件仅携带 generation、状态和 changed mask。
- `chore_service` 的 job 是短时有界回调：运行在单 worker 上，串行执行；`run` 须主动轮询取消令牌并及时返回，不得调用 LVGL、发起同步 HTTP 或无限等待。`submit` 成功后参数所有权转移给服务，`release` 在完成、取消或关闭后于 worker 上恰好执行一次；`cancel` 是协作式静默等待（返回即保证 `release` 已执行），拒绝 worker 自身调用。周期 job 从完成时刻固定延迟调度，挂起期间到期不追补，唤醒后最多立即补一次。`suspend`/`resume` 遵循仓库 PAUSE/RESUME 握手（超时回滚），整笔事务由独立生命周期锁串行，杜绝相反命令合并与 ack 互擦；停机会用 STOPPED 位唤醒在途挂起/恢复等待者，不会死锁。job 类 API 可在任意任务调用且与 `deinit` 并发安全：进程生命周期的接纳门先原子关闭、排空在途调用，新实例以新 epoch 重开接纳并沿用单调槽位代际，旧句柄永不指向新实例；仅 init 与并发 deinit 要求调用方串行化。休眠协调中该服务最先挂起、最后恢复。

显示 TE 同步不属于 middleware 服务 API。BSP 通过 `bsp_display_port_t.te` 导出 GPIO13 上升沿、所选 SPI 频率（项目经验默认 40 MHz；80 MHz 为超规格实验）、4 data lines 和当前 16 bpp 物理参数；`layers/app_manager` 据此启用 TE sync，并补充 adapter 默认 13/1 ms、66% 刷新窗口。

## 配置

Kconfig 只保留静态资源预算：Event Bus 三个池 24、payload 256 B，NVS blob pool 16，
IMU/Power stack 3072，Time stack 3072，Connectivity stack 4096 和 queue 8，Wi-Fi
stack 4096 和 queue 16，System PM stack 4096，Weather stack 8192 和最大临时响应
256 KiB，Chore stack 4096 和 job 容量 8。采样率、轮询周期、任务优先级、PCM、挂载点、时区和 SNTP server 都由根
`app_product_config_t` 在运行时传入。恢复出厂启动先清 Wi-Fi profile，再以 gated 模式
清 Device Link 授权、bond/CCCD 和易失状态，并在广告暂停时预取得 slow lease；全局 marker
清除后仅解除广告 pause，再允许平台及网络继续启动。marker 清除前任一持久化、擦除或广告
前置步骤失败均中止本次启动；清除后的物理 START 失败由 ADV owner 有界退避重试。`SYSTEM_PM_DEVELOPMENT_MODE` 是根产品开发 gate，
定义于 `main/Kconfig.projbuild`。修改 Kconfig 后运行
`idf.py reconfigure && idf.py save-defconfig && idf.py build`。

## 并发与资源边界

- `event_bus` API 仅限任务上下文，不支持 ISR。最多 24 个订阅、24 个待处理 UI 回调和 24 份 UI payload；匹配 UI 订阅时 payload 最大 256 字节。发布者回调同步执行，UI 回调异步执行；取消订阅不是静默屏障，销毁 `user_data` 前仍需停止发布者并排空 UI 工作。
- `EVENT_BUS_PUBLISH_FLAG_UI_LATEST` 只用于可覆盖的状态快照，不得用于边沿、命令、审计或计数事件。事件 payload 只在回调期间有效。
- `nv_storage` 成功初始化后独占默认 NVS 分区生命周期。键最长 15 字节，Blob 注册池为 16 项；注册数据缓冲和回调必须存活到成功反初始化。Blob 加载会冻结注册表，但回调执行时不持锁。
- Connectivity 公共请求是非阻塞接纳操作，扫描快照最多保存 5 条记录；SSID 上限 32 字节；个人网络密码上限 64 字节（契约边界，1..64 均接受，物理关联失败留作操作结果）。`wifi_service` 公共接口仅保留给 manager 和底层测试。Connectivity、Wi-Fi、天气、时间、电源、IMU、音频、SD、Chore 和系统 PM 的挂起、等待、I/O 或反初始化接口可能阻塞，生命周期调用必须由上层串行化。`chore_service` 的 job 回调本身运行在 worker 上，可在其中调用其他服务 API，但必须短时返回并遵守各服务的上下文限制。
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

运行 chore worker 套件（一次性/延时/周期调度、池满、协作取消、挂起无追补与超时回滚、deinit 释放、自调用拒绝、并发取消）：

```sh
cmake -S components/chore_service/tests/host -B /tmp/mt-chore -G Ninja \
    -DCHORE_SERVICE_SANITIZER=none
cmake --build /tmp/mt-chore
ctest --test-dir /tmp/mt-chore --output-on-failure
```

运行 Device Link Typed-TLV、router、operation 和 wire 套件：


```sh
cmake -S components/device_link/tests/host -B /tmp/mt-device-link -G Ninja \
    -DDEVICE_LINK_SANITIZER=none
cmake --build /tmp/mt-device-link
ctest --test-dir /tmp/mt-device-link --output-on-failure
```

运行 NimBLE/Device Link runtime、Security 2 和 service owner 套件（需要先导出
ESP-IDF v6.0.2 的 `IDF_PATH`）：

```sh
cmake -S components/ble_runtime/tests/host -B /tmp/mt-ble-runtime -G Ninja \
    -DBLE_RUNTIME_SANITIZER=none
cmake --build /tmp/mt-ble-runtime
ctest --test-dir /tmp/mt-ble-runtime --output-on-failure

cmake -S components/device_link_security/tests/host \
    -B /tmp/mt-device-link-security -G Ninja \
    -DDEVICE_LINK_SECURITY_SANITIZER=none
cmake --build /tmp/mt-device-link-security
ctest --test-dir /tmp/mt-device-link-security --output-on-failure

cmake -S components/device_link_service/tests/host \
    -B /tmp/mt-device-link-service -G Ninja \
    -DDEVICE_LINK_SERVICE_SANITIZER=none
cmake --build /tmp/mt-device-link-service
ctest --test-dir /tmp/mt-device-link-service --output-on-failure
```

三套 sanitizer 选项均接受 `address` 或 `thread`。`ble_runtime` 套件还执行固定
ESP-IDF v6.0.2 内部假设检查；失败时必须审查 NimBLE pairing/store/host-event 时序。

运行恢复出厂 journal 的持久化、故障注入和断电恢复套件：

```sh
cmake -S components/factory_reset_service/tests/host \
    -B /tmp/mt-factory-reset -G Ninja \
    -DFACTORY_RESET_SERVICE_SANITIZER=none
cmake --build /tmp/mt-factory-reset
ctest --test-dir /tmp/mt-factory-reset --output-on-failure
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

所有列出的 `*_SANITIZER` 选项支持 `none`、`address` 和 `thread`；切换 sanitizer 时使用独立构建目录。x86_64 上若存在 `setarch`，CTest 会为 TSan 测试自动关闭 ASLR。QMI8658C、SDSPI partial rollback、ES8311 板级生命周期、PCF85063 alarm 和 AXP2101 profile 由 `layers/bsp/tests/host` 覆盖；`layers/app_manager/app_core/tests/host` 校验 BSP TE 参数到 adapter TE sync 的映射。所有硬件行为仍需随整机工程构建并按风险上板验证。

## 许可证

本项目采用 [MIT License](LICENSE)。
