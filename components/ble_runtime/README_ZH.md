[English](README.md) | **简体中文**

# ble_runtime

`ble_runtime` 是 ESP-IDF NimBLE 主机生命周期与 `ble_hs_cfg` 的唯一所有者。
它提供 GAP 接纳、广播、静态 GATT 数据库、Device Link v1 传输、数字比较
（Numeric Comparison）以及持久 bond 存储的协调。其他组件必须使用其公共 API，
而不是直接调用 `nimble_port_*` 或修改 `ble_hs_cfg`。

`device_link_service` 任务拥有产品策略：120 秒本地配对窗口、用户确认命令、
状态事件、蓝牙启用以及本地 bond 撤销。NimBLE 回调保留以连接为限定的事实
（connection-qualified facts）并唤醒该所有者；它们不直接更新 LVGL，也不直接
发布应用事件。

## 宿主生命周期

`ble_runtime_host_port_t` 注入 init/start、配对闸门同步、peer 存储重置、stop 与
deinit。宿主同步在允许广播或配对之前审计已恢复的 NVS 记录。stop 在专用 worker
上运行，并带有有界确认。超时会保留同一 worker 与每一项资源，直到其结果与
host 退出均被确认。部分 SDK deinit 失败会保持锁存直到重启，因为 SDK 拆除
无法重试。

`ble_link_timer` 任务拥有 indication 截止时间、保留 peer 清理、本地撤销以及
已接受/已拒绝 ACL 的终止。绝对义务存储在其命令队列之外，因此一次丢失的唤醒
提示不会丢弃工作。干净关闭会关闭接纳与广播、穿过一次 host 队列屏障、排空
全部保留义务，然后在退出前穿过第二道屏障。

## 连接身份

异步工作携带 `ble_link_operation_identity_t`：

```text
{generation, flow_id, token, kind, conn_handle}
```

断开、重置、MTU、加密、订阅、TX 完成、终止与清理路径都会校验适用的字段。
陈旧的 generation 或被复用的连接句柄无法退役更新的 ACL。CONNECT 与最终
ENC_CHANGE 路径都会读取当前 NimBLE 描述符，使身份解析与加密事件无论回调顺序
如何都能收敛。

连接 generation 与接纳预留 token 在整个设备启动周期内跨 SDK 重启单调递增。
零被保留；计数器耗尽时拒绝新连接，而不是复用某个身份。

数字比较确认 token 同样在整个启动周期内递增，跨服务初始化、重置与 ACL 变更。
分配不会发布提示或唤醒其所有者；发布是独立步骤，因此宿主可以在暴露之前安装
确切的提议。耗尽会拒绝新分配，而不覆盖待处理的确认。

CONNECT、MTU 与订阅回调在端口锁下捕获限定事实，并在释放锁后同步应用。
服务提交可以获取端口锁，但宿主回调绝不在持有该锁时进入服务。早期的
MTU/CCCD 事实不要求加密；命令接纳仍要求既有的 bond 传输安全。重复 CONNECT
保留已协商的 MTU、CCCD 接纳与活动 TX；新的 ACL 从 MTU 23 开始。

安全归约器区分已恢复的 bond 与由当前 ACL 创建的 bond。数字比较开始时，新配对
成为未提交的候选。已验证的 Secure Connections、MITM、16 字节 bond 是 v1 的
持久绑定边界；授权成功后它被提交并在断开后保留。取消、重置或断开只为已开始
但从未到达该边界的全新配对保留清理。格式错误或窗口不合格的 bond 会被删除，
其 ACL 被终止。

## Device Link v1 GATT

数据库包含一个服务与两个特征：

- `command_rx`：已认证的 Write Request。
- `server_tx`：已认证的 Indicate，CCCD 值为 `02 00`。

没有应用层 Security 2 握手，v1 中也没有旧的 `link_state`、`control_rx` 或
`control_tx` 特征。链路认证完全由已绑定的 Secure Connections 传输提供。

首选且必需的 ATT MTU 为 498，产生 495 字节的特征值。每次 Write 包含一个完整
的 v1 帧；没有应用层分片重组。路由闸门在分派应用工作之前检查加密、已验证的
SC/MITM bond、CCCD 状态、尺寸以及唯一未完成的 indication。所有者在排队的
工作执行之前立即重复当前连接与 `server_tx` CCCD 检查，因此关闭 indication
不会启动一个结果没有返回路径的 Wi-Fi 操作。ATT 安全错误对客户端保持可见，
以便其发起配对。

TX 调度器允许一个 indication 在途。两秒绝对截止时间在超时时退役确切的操作，
同时保留原始回调墓碑，直到迟到的 NimBLE 完成或 ACL 拆除将其消耗。这防止
一个没有应用 token 的完成去确认一个复用了相同 handle 元组的新 indication。

服务在输出之前冻结 generation、handle 与非复用 flow；调度器分配完成 token。
SENT 不是终态结果。匹配的确认、错误、超时或拆除会恰好退役该 flow 一次。
同步提交失败只拒绝仍归其所有的选定 TX，包括输出前的失败与输出期间的重入
完成。

选择一个普通状态会消耗待处理槽位，而不是后续更新。帧在途时重复 pump 不提交
任何内容；较新的观察会合并进最新的待处理状态，并在旧帧被确认或拒绝后保留。
失败的终态/ACK indication 保留操作记录以供 GET_OPERATION/ACK 恢复；失败的
ACCEPTED 绝不启用终态事件。

## 数字比较

NimBLE 配置为 DisplayYesNo、bonding、MITM、仅 Secure Connections、16 字节
密钥，以及 ENC 加身份密钥分发。只接受 `BLE_SM_IOACT_NUMCMP`。端口记录当前
ACL handle 与一个 epoch，获取服务 token，并唤醒 Device Link 所有者，且不记录
六位数值。

本地接受/拒绝会校验确切的 token 并调用 `ble_sm_inject_io()`。宿主拥有一个
提议/应答槽位：在既有提议或其应答墓碑仍存活时拒绝新的 passkey 提议，而不是
替换该元组。排队的超时与失效会恰好退役旧的服务提示一次。成功消耗待处理动作；
失败仅当 handle 与 epoch 未改变时才恢复它。断开、重置、窗口关闭、不支持的
passkey 动作以及 SMP 失败会清除本地待处理状态并唤醒所有者，使 Setup 页面
依据新的状态 generation 隐藏或恢复其控件。

重复配对绝不替换已验证的已存储 bond。仅当设备可绑定且不存在持久 bond 时，
才允许对不完整材料重试。因此绑定替换必须走本地撤销流程。

## 广播

管理器将慢速非可绑定与快速可绑定租约归约为一个目标。`bindable` 是内部控制
状态：即使它不在空口上序列化，其状态转换也会重启广播并同步配对闸门。编码器
只输出 Flags、完整的 128 位 Device Link 服务 UUID 以及可选的缩短本地名称。
它在调用 NimBLE 之前拒绝超过 31 字节传统广播上限的负载。

生产环境使用 `BLE_OWN_ADDR_PUBLIC`。它不发布 Service Data、discriminator、
实例 ID 或地址轮换元数据。

START 与 STOP 操作携带非零 generation。只有当前转换的完成可以变更管理器状态；
陈旧完成是空操作。广播任务在唤醒服务所有者之前提交每个结果，因此空闲所有者
无需另一个 GAP 事件就能发布已完成的物理状态。失败会保留确切的 action 与
generation 并采用有界指数重试，所有者等待最近的绝对截止时间，而不是周期性
轮询。

只有 CONNECT、DISCONNECT、SYNC、RESET 与广播生命周期事件会进入广播消费者，
并在管理器应用后唤醒所有者。TX、MTU、安全与订阅事件不产生广播唤醒提示。

## 配对闸门与存储

未绑定 peer 只有在本地窗口打开时才能配对。已验证的已绑定 peer 可以在窗口之外
重连。配对闸门占用覆盖清理、被拒绝的 ACL 终止、排空与撤销；仅当窗口被请求
且无剩余占用时，有效配对才会启用。在必须的清理仍被保留期间，GAP 接纳关闭。

Peer 清理会验证 OUR_SEC、PEER_SEC、CCCD 及适用隐私记录的移除。在本次 host
运行内，存储写入与恢复错误是粘滞的；持久化失败后，RAM 镜像中的缺失不能退役
一次清理。本地撤销与恢复出厂使用带日志的整库重置，并且只有在持久 NVS 与 RAM
镜像均为空之后才重新打开广播。该日志为
`ble_nimble_port_revoke_journal`（`nv_storage` 中带版本的 `ble.revoke` blob）：
`begin` 持久化意图，`pending` 对格式错误的 blob 采取 fail-closed，`end` 只在
验证为空的清扫之后清除它。撤销请求只排队 host 核心工作；Device Link 在声明
完成之前观察日志不存在，并在标记仍存在时重试。`begin` 会重写格式错误的标记，
包括大于当前版本的 blob。

根构建固定所需的 ESP-IDF v6.1 行为，包括
`MYNEWT_VAL_BLE_RESTART_PAIR=0`。`scripts/check_idf_assumptions.sh` 验证此处
使用的 GAP、GATT、ATT、SM、store 与 host 事件源码假设。

## 宿主测试

```sh
cmake -S tests/host -B /tmp/mt-ble-runtime -G Ninja \
    -DBLE_RUNTIME_SANITIZER=none
cmake --build /tmp/mt-ble-runtime
ctest --test-dir /tmp/mt-ble-runtime --output-on-failure
```

`BLE_RUNTIME_SANITIZER` 也接受 `address` 与 `thread`。该测试集覆盖生命周期、
GATT 注册表与闸门行为、GAP 接纳、广播负载与收敛、TX 信用/完成/墓碑、
indication 截止时间、清理容量与栅栏、数字比较策略、粘滞存储防护、会话安全
状态以及固定的 IDF 假设。`test_ble_link_delivery` 演练真实的
GATT/service/engine/scheduler 扫描投递与保留结果恢复；
`test_ble_nimble_link_events` 用真实生产辅助函数演练捕获/应用锁屏障、陈旧 ACL、
早期 MTU/CCCD 以及广播反馈/恢复。
`test_ble_wifi_recovery` 将产品事件消费者、适配器、事件总线、GATT、service、
engine 与调度器一起运行。它覆盖同 handle 重连下的断开/重置、陈旧断开的隔离、
完整保留扫描结果与 ACK 退役、不可变终态重试、蓝牙关闭的 worker 截止时间，
以及产品 deinit 时的桥接退役。其 fake host 在 stop 时关闭 ACL，并在蓝牙重启
之间保留 GATT 注册；SDK 回调与射频硬件仍在此 harness 之外。
这些辅助函数不模拟 `ble_nimble_port.c` 或 SDK 的全部内容。在 x86_64 上，当
`setarch` 可用时 CTest 会为 TSan 禁用 ASLR。

宿主测试不证明 ESP32-S3 的 NimBLE 回调时序、NVS 持久化、射频行为、重置/
冷启动行为，或与 Android 的互操作性。
