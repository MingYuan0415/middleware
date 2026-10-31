[English](README.md) | **简体中文**

# connectivity_manager

Wi-Fi 连接策略 owner：策略 worker 持有已保存网络列表，并在每次打开 Wi-Fi 时执行一次
有界“扫描→匹配→连接”周期，底层射频操作委托给 `wifi_service`。

## 连接策略

`connectivity_manager_request_wifi_enable()` 启动一个周期：

1. 射频按需初始化，然后启动（或恢复）；
2. 扫描已保存网络，并由 `wifi_service` 返回每个保存档位的存在位图；
3. 只尝试扫描中实际存在的档位，先选中的档位，再按内存中的尝试顺序；
4. 每个存在的档位最多尝试 `CONNECTIVITY_MANAGER_MAX_CONNECT_FAILURES`（3）次；
   认证失败立即停止重试该档位，但周期继续尝试下一个档位；关联/DHCP/链路丢失等瞬时
   失败消耗次数；
5. 第一个获得有效 IPv4 的链路成为选中档位；
6. 没有任何档位连接成功，或没有任何匹配档位时，停止射频并清除 `wifi_enabled`，
   开关不会停在“打开但无连接”的状态；单个周期受
   `CONNECTIVITY_MANAGER_CYCLE_MAX_MS`（默认 360 秒，包含一次有限扫描重试）约束。

扫描错误在归类为射频失败前重试一次；扫描成功但匹配为空归类为 `AP_NOT_FOUND`
并结束周期。连接尝试期间提交的前台 `request_scan()` 会在该次尝试终态后立即执行，
然后恢复周期。

`connectivity_manager_request_wifi_disable()` 取消正在进行的扫描/连接周期、停止射频，
并保留已保存列表。`wifi_enabled` 是持久化用户意图；`manual_hold` 表示当前主动停止
射频，不是持久化设置。Wi-Fi 保持开启期间链路丢失会重新开始扫描周期。

原“自动连接”开关已删除。`wifi_enabled` 是唯一的持久化策略，随 profile 记录保存并在
启动时恢复；射频不在 boot 阶段初始化，持久化为开启的周期在首帧和 startup commit
之后才开始。`request_save_profile()` 只保存凭据，不会自动发起连接；
`request_forget()` 会清除所有保存档位和持久化记录。

## 对外 API

公共头 `include/connectivity_manager.h`：

- `connectivity_manager_init()` / `deinit()` / `suspend()` / `resume()`：生命周期与待机。
- `connectivity_manager_clear_persisted_profile()`：初始化前幂等擦除持久化 profile。
- 前台命令：`request_scan()`、`request_wifi_enable()`、`request_wifi_disable()`、
  `request_connect()`、`request_sync_profile()`、`request_save_profile()`、
  `request_connect_network()`、`request_forget_network()`、`request_forget()`、
  `cancel()`；均返回非零 `operation_id`。每个已接纳操作都有且只有一个终态，
  包括存储和射频失败路径。
- `connectivity_manager_get_status()` / `get_scan_snapshot()` / `get_profiles()`：
  读取不含密码的不可变状态、扫描或已保存网络快照。
- 通过 `EVENT_BUS_DECLARE_ID(CONNECTIVITY_MANAGER_MSG)` 发布状态、扫描和网络列表事件。

## 已保存网络

管理器最多保存 `CONNECTIVITY_MANAGER_MAX_PROFILES`（五个）网络，并以一条记录整体持久化。

- `request_save_profile()` 按 SSID upsert；所有槽位被其他网络占用时返回
  `ESP_ERR_NO_MEM`。保存不会打开 Wi-Fi。
- 一个条目是选中档位。`request_connect_network()` 优先从指定条目开始周期，
  `request_wifi_enable()` 从选中条目开始；失败周期不会改写选中档位。
- `request_forget_network()` 删除单个条目；承载链路的条目在断开终态后删除，否则保留链路和策略。
- `request_forget()` 清空所有条目并使 Wi-Fi 保持关闭。
- 写入失败会回滚 RAM，列表不会宣称未落盘的删除。

### 尝试顺序与扫描匹配

尝试顺序只存在于 RAM，仅在保存、删除或选择时改变。每个周期只扫描一次；扫描结果包含
所有已保存档位的存在位图，而不只是 UI 展示的五条记录。未出现在扫描中的档位直接跳过，
不消耗尝试次数。`status.ssid` 始终表示选中档位；`status.attempt_index` 是候选的
profile 下标，`status.attempt_ordinal` 是候选的 1-based 序号，
`status.attempt_total` 是匹配候选数。

## 配置与依赖

- Kconfig：`CONNECTIVITY_MANAGER_TASK_STACK`（默认 8192）、
  `CONNECTIVITY_MANAGER_QUEUE_DEPTH`（默认 8）。
- `CONNECTIVITY_MANAGER_MAX_CONNECT_FAILURES` 为编译期常量（3）。
- CMake：`REQUIRES event_bus`；`PRIV_REQUIRES freertos mbedtls mt_log nvs_flash nv_storage wifi_service`。
- 运行时由 `connectivity_manager_config_t` 配置。

## 状态与并发

- 单例；静态分配策略 worker、Wi-Fi worker、命令队列和信号量。
- 前台操作带非零 `operation_id`，且每个操作都有终态；失败周期只发布一次终态并清除操作，
  不会把前台操作停在长时间退避中。
- 策略持久化和射频停止是两个步骤；NVS 写失败报告 `STORAGE`，但不能跳过射频停止。
- `wifi_enabled` 是持久化意图，`manual_hold` 是当前主动停止状态，`radio_available`
  是执行器健康状态，三者不能互相替代。
- manager 由 Wi-Fi service 事件和命令提交唤醒；空闲等待使用 deadline 或
  `portMAX_DELAY`，不再固定 20 ms 轮询。
- 系统待机使用 `connectivity_manager_suspend()` / `resume()`，与用户关闭 Wi-Fi 分离；
  恢复时只有 `wifi_enabled` 为 true 才重新启动周期。

## 持久化记录

`wifi_profile` NVS 键保存带版本记录：magic、version、count、active index、`wifi_enabled`、
revision、Device Link 同步窗口以及五个固定 103 字节条目槽。v1/v2/v3 单档记录和 592 字节
v4 多档记录在加载时迁移到 v5；v4 的 `auto_connect` 字节映射为 `wifi_enabled`。

## 测试边界

- `tests/connectivity` 将真实 manager/executor 与受控射频边界链接，覆盖 profile 迁移、
  有界连接周期、扫描匹配、关闭/重开、suspend/resume 和取消。
- 宿主不覆盖 Wi-Fi 驱动、RF、关联/DHCP 时序和 NVS 掉电，需要上板验证。
