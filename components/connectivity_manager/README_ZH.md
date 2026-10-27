[English](README.md) | **简体中文**

# connectivity_manager

Wi-Fi 连接策略 owner：策略 worker 负责扫描、连接、重连、配置持久化与自动连接状态机，低层 radio 操作委托给 `wifi_service`。

## 对外 API

公共头 `include/connectivity_manager.h`：

- `connectivity_manager_init()` / `deinit()` / `suspend()` / `resume()`：生命周期与待机。
- `connectivity_manager_clear_persisted_profile()`：初始化前幂等擦除持久化 profile（复位域）。
- 前台命令：`request_scan()`、`request_connect()`、`request_sync_profile()`、`request_save_profile()`、`request_connect_network()`、`request_forget_network()`、`request_disconnect()`、`request_reconnect_saved()`、`request_forget()`、`set_auto_connect()`、`cancel()`；均返回非零 `operation_id`。
- `connectivity_manager_get_status()` / `get_scan_snapshot()` / `get_profiles()`：读取不含密码的不可变状态、扫描或已保存网络快照。
- `connectivity_manager_is_available()`：公开 API 是否可用。
- 通过 `EVENT_BUS_DECLARE_ID(CONNECTIVITY_MANAGER_MSG)` 发布状态、扫描与已保存网络快照事件。

## 已保存网络

管理器最多保留 `CONNECTIVITY_MANAGER_MAX_PROFILES`（五个）已保存网络，并以单条记录整体持久化，
因此多份凭据可跨重启存活。该数量是固定的编译期常量而非配置项：持久化记录本就预留五个槽，
可调上限只会引入第二种宽度去推理。

- `request_save_profile()` 按 SSID upsert：已存在则替换，否则追加。当所有槽位都被其他网络占用时
  以 `ESP_ERR_NO_MEM` 拒绝，不会静默淘汰任一网络。保存同时启用自动连接，这正是"配网后下次开机
  即可用"的依据。
- 其中一个条目是被选中的档位。`request_connect_network()` 从指定条目开始尝试，
  `request_reconnect_saved()` 从被选中的条目开始；指定条目只有在链路成功后才成为选中档位。
  失败的尝试绝不改写选中档位，因此即使用户选择的那条在扫描中失败、其余网络也都试过，用户的选择
  依然保留。
- `request_forget_network()` 删除单个条目：删的是承载链路的条目时，由断开终态完成删除；否则
  保留已建立的链路、其状态与自动连接策略，且不会让射频进入手动离线保持。`request_forget()`
  是合同级 FORGET，清空全部条目，从而使 Device Link 的 `profile_ssid` 为空。
- 被拒绝的删除写入会在 RAM 中回滚，因此列表绝不会宣称一次未落到 flash 的删除。
- `status.profile_count`、`status.saved_profile` 与 Device Link 的 `profile_ssid` 投影描述的是
  已保存列表与被选中条目，而不是任意一条；扫描失败后两者仍指向被选中的条目。

### 尝试顺序

尝试顺序只存在于 RAM，仅在保存、删除或选择档位时改变；建立连接不会为记录它而写 flash。

已保存网络多于一个时，一次连接操作会扫描整个列表：先试起始条目，再按尝试顺序试其余条目。
分类为 `AP_NOT_FOUND`、`ASSOCIATION_TIMEOUT`、`DHCP_TIMEOUT` 或 `LINK_LOST` 时前进到下一个候选；
认证失败会结束扫描，让错误密码暴露出来而不是静默连到别处；射频级失败同样结束扫描，因为问题在
radio 而非候选。候选耗尽后进入长退避策略，不发布失败终态；取消则结束该操作。扫描使用自己的
游标，从不改写选中档位，因此候选全部耗尽后用户的选择与其持久化的 `active_index` 都保持不变。

因此 `status.ssid` 在整个扫描期间始终命名被选中的档位，这也正是 Device Link 的 `profile_ssid`
所投影的内容。真正在尝试的候选通过 `status.attempt_index` 与 `status.attempt_total` 单独上报
（未扫描时为 `UINT8_MAX` 与 `0`），使展示层可以说明当前尝试的是哪一条，而无需重新定义档位 SSID
的含义。

每次扫描尝试都带 `wifi_service_connect_options_t` 的零重试上限，使缺失的接入点立刻回到策略层，
而不消耗执行器内部的退避序列。只保存了一个网络时管理器不传选项块，执行器继续使用编译期重试预算。

## 配置与依赖

- Kconfig：`CONNECTIVITY_MANAGER_TASK_STACK`（默认 6144）、`CONNECTIVITY_MANAGER_QUEUE_DEPTH`（默认 8）、`CONNECTIVITY_MANAGER_MAX_PROFILES`（固定 5，不可配置）。
- CMake：`REQUIRES event_bus`；`PRIV_REQUIRES freertos mbedtls mt_log nvs_flash nv_storage wifi_service`。
- 运行时由 `connectivity_manager_config_t`（策略 worker 优先级、低层 Wi-Fi worker 优先级）配置。

## 状态与并发

- 单例；静态分配的策略 worker、低层 Wi-Fi worker、命令队列与信号量。
- 前台操作以 `operation_id` 世代标识；状态与扫描快照带 `generation`。
- 连接命令在准入时深拷贝候选凭据，调用返回后可释放调用方缓冲。
- pending 前台命令与立即执行命令共用唯一 dispatcher；SYNC_PROFILE 的候选凭据和 sync identity 采用相同深拷贝与持久化路径。
- CONNECT/DISCONNECT 只消费匹配当前 session 和非零 radio operation ID 的显式 terminal；link IP_READY 本身不能证明候选成功。只有成功 terminal 加当前有效 IPv4 才提交候选 profile，地址续租不会重复写 profile。
- terminal outbox 在每轮策略 phase 前排空；背压期间不消费 pending/retry/恢复意图。排空后由同一 worker 自主推进，不依赖额外驱动事件；STOPPING 不重开连接。
- 挂起中的显式 CONNECT/SYNC 保留原 manager operation、候选与 sync 元数据；恢复仅重启 executor，由 policy phase 使用新 radio ID/epoch 续接。取消和替代在本地发布一次终态，不对已退休的 radio ID0 发 cancel。
- retry 的绝对 deadline 跨待机保留；resume 控制失败不自动重试，也不丢弃候选。低层已实际 SUSPENDED 时，即使控制返回原 stop 错误，也保存 parked 意图供显式 resume 回滚。

## 持久化记录

`wifi_profile` NVS 键保存一条带版本的记录：头部（magic、version、数量、选中索引、自动连接策略、
revision 与 Device Link 同步窗口）之后是五个固定 103 字节的条目槽。RAM 列表与该记录宽度相同，
因此合法记录总是整条加载。数量超过五的记录属于结构无效，会被拒绝，而不是只加载其中一部分。

v1（112 字节）、v2（128 字节）或 v3（168 字节）的单档记录会在加载时升级为单条目列表并回写；
写失败时原字节保持不变，初始化报告存储失败。无效记录会被拒绝且不清除 flash；被拒绝的写入
也绝不会让 RAM 列表宣称一个尚未持久化的条目。写入方会拒绝编码选中索引不指向已保留条目的记录。

## 测试边界

- 无 `tests/host`。
- 宿主不覆盖 Wi-Fi 驱动、射频、关联/DHCP 时序与 NVS 掉电；这些由 `wifi_service` 和上板验证。
