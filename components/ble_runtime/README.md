# ble_runtime

`ble_runtime` 是 MicroTech 中 NimBLE host 生命周期和 `ble_hs_cfg` 的唯一 owner。
GAP、ADV、静态 GATT registry、Device Link framing/TX 和 host-store 协调均建立在这个
生命周期上；只有 NimBLE adapter 可以调用 `nimble_port_*` 或修改 `ble_hs_cfg`。

上层 `device_link_service` worker 是 ACL、Numeric Comparison 确认和持久化清理的逻辑
owner。NimBLE callback、timer callback 和 GATT access callback 只产生带身份的事件或工作，
不直接完成跨组件状态迁移。

## 生命周期与 host port

`ble_runtime_host_port_t` 注入 init/start、同步 pairing gate、peer-store reset、stop 和
deinit。工厂重置 gated 启动会在 host sync 后确认 bond/CCCD store 已清；窗口开关则通过
持久 NimBLE host event 同步应用 `sm_sec_lvl`，返回成功前不会恢复广告。

adapter 自己创建 host task，因此 task 创建失败可见。`nimble_port_stop()` 在专用 worker
上执行并有有界等待；wedged host、stop timeout 或 `nimble_port_deinit()` 失败会锁存 terminal
fault，只能重启恢复。发生 terminal fault 时保留相关资源，避免失败 teardown 产生
use-after-free；只有无 fault 的重试才释放资源。

独立的 `ble_link_timer` owner 使用 4096B stack。它的 deadline sweep 可以推进
TX scheduler 并保留 provisional cleanup，因此不是只执行轻量 timer callback 的任务。

clean deinit 先取得独立 shutdown pause，通过 NimBLE host barrier 关闭 pairing gate，随后
持续推进 revoke、cleanup、terminal fence 和 accepted/rejected terminate。owner 仅在第一次
empty observation 后再通过一次 host barrier，并得到第二次 empty observation 时退出，避免
DISCONNECT、SMP store 或 replacement producer 在最终检查后补入义务。

## 异步身份与连接事实

所有可跨连接代际到达的工作使用 `ble_link_operation_identity_t`：

```text
{generation, security_epoch, flow_id, token, kind, conn_handle}
```

DISCONNECT、MTU、ENC_CHANGE、SUBSCRIBE、TX completion、reassembly deadline、
TERMINATE 和 cleanup 均核对适用的完整身份。旧 generation 或旧 epoch 的事件只产生
no-op，conn_handle 复用不会把新连接误认为旧操作目标。CONNECT reducer 还会读取当前
connection descriptor，补放 encrypted、bond、identity 事实，覆盖 ENC_CHANGE 早于
CONNECT 分发的时序。

ESP-IDF v6.0.2 只通过 `ble_gap_adv_start()` 捕获的 per-connection GAP callback 投递
`IDENTITY_RESOLVED` 和 `REPEAT_PAIRING`。每次 ADV START 都注册该 callback，并只转发这两个
事件；其他连接事件继续由 global listener 处理，避免同一事件重复 reduction。identity event
可以先更新当前 ACL 的 normalized identity；fresh public/static identity 不一定产生
`IDENTITY_RESOLVED`，因此最终 ENC_CHANGE 路径会重新读取 connection descriptor 和 bond
store，以当前 identity、bonded、verified facts 收敛同一 ACL 的准入。无旧 bond 的 CONNECT
只登记 provisional candidate；只有 reducer 观察到当前 ACL 新生成且 verified 的 bond 后才
形成 provisional cleanup 义务，SMP 完成前断连不会制造全量 bond 删除。

DISCONNECT、RESET 和 TERMINATE 是 ACL 终态：匹配 generation/conn_handle 后统一清理该
ACL 的全部 Security 2 epoch，即使 Cmd0 在事件等待 service mutex 时已推进 epoch 也不会
遗留会话。accepted ingress 同时保存 generation/conn_handle；终态在 worker execute 前清除
queued reservation、推进 ingress epoch 并标记该代 retired，因此已入队但未执行的 work 和
同代后续 ingress 都是 no-op。同一终态重复执行为 no-op。ENC_CHANGE drop、TX failure 和
timeout 等会话级 teardown 仍核对 security_epoch 及适用的 flow/token，不能退休更新的握手。

provisional/orphan/replacement cleanup 在 Device Link owner 中保留；port callback 暂时失败时
按 100/200/400/800/1000 ms 封顶退避，失败本身不产生新的 wake。port 使用 4 个固定 slot 和
1 个 fail-closed overflow，按同一物理 bond 目标合并重复请求，并在旧 snapshot 执行期间保留
后来增强的 delete-all、authorization invalidation 和 terminate 要求。cleanup 或 terminal
fence 未清空时 GAP 拒绝新 ACL；live ACL 的 terminal cleanup 立即拒绝 session/control 写入并
保留 terminate retry，但公开 `link_state` 仍可读。fence 只由匹配 generation/handle 的
DISCONNECT/RESET 释放；广告只在全部 cleanup slot 清空后恢复。

定向 peer-store cleanup 只执行一次显式 `ble_store_util_delete_peer()`，并逐类 readback
OUR_SEC、PEER_SEC、CCCD；RPA_REC 使用 identity/RPA 精确 key readback。ESP-IDF store 在
NVS persist 前先删除 RAM entry，
因此任一 store/枚举错误会在完整 host run 内粘滞；同一 host run 后续看到 RAM absence 不能
退休义务。port 同时包装 NimBLE store write callback：容量溢出仍交给 replacement owner，
其他写失败立即粘滞，bond verification 只有在 guard 保持成功时才接受 RAM mirror。当前
ESP32-S3 构建使用 controller privacy；NimBLE privacy startup 的
`ble_hs_pvcy_set_default_irk()` 路径会在 initial sync 和 reset resync 前恢复 IDF writer。cold
boot callback 仍为 NULL 时只 arm guard；首次 sync 捕获 IDF writer，后续 resync 只对精确原
writer 重装 wrapper。未知 callback 不会被覆盖，并使当前 host run fail closed。

ESP-IDF v6.0.2 的 NVS loader 会记录却吞掉 store restore 错误。非 journaled revoke 的每次
sync 都在同一 storage lock 下、destructive reconciliation 前，对当前 controller-privacy 构建
可加载的六个 store family 执行有界审计：OUR_SEC、PEER_SEC、CCCD、CSFC 和 LOCAL_IRK
比较 durable key presence count 与 public RAM-store count；RPA_REC 因固定 IDF 的
PEER_ADDR iterator 不支持 wildcard，改为逐个读取 `rpa_rec_N` 并与 RAM 中完整
`{peer_rpa_addr, peer_addr}` 精确匹配。NVS 访问、RAM count、blob 尺寸/identity 或匹配不一致
都会粘滞 storage error，不发布 SYNC，也不开放 SMP/ADV。完整 host reload 从 durable NVS
重建状态后才允许 reconciliation 重试。

local revoke、factory reset 和 unresolved single-bond cleanup 使用完整 peer-store reset，而非
定向 peer cleanup。reset 先捕获当前可加载 RPA_REC 的精确 key，再固定执行
`durable namespace erase -> controller/RAM cleanup -> durable namespace erase -> empty audit`；
第一次 erase 先于任何 IDF RAM persistence helper，
因此 malformed blob 不能阻塞删除，第二次 erase 覆盖 runtime cleanup 重新写入的数据，并清除
旧配置超出当前上限的遗留 key。local revoke/factory reset journal 位于独立 namespace，只有
整个 `nimble_bond` namespace 和 RAM mirror 都确认为空后才清除；journaled revoke 不依赖一次
成功的 RAM restore。
accepted/rejected terminate 在 NimBLE host event 中做最终身份核对和 handle-only HCI 副作用，
成功提交仍保留到 exact DISCONNECT/RESET，而不是把 HCI command admission 当作物理终态。

`link_state` 在 GATT 状态锁下将当前事实与
`{generation, auth_epoch, cccd_epoch}` 投递 stamp 分开保存，由 Device Link worker 独占
transport submit。只有授权后提交成功才更新 stamp；并发 epoch/事实变化不会被旧 submit
覆盖。Cmd1 认证成功或 CCCD 开启会清除旧 cooldown、立即唤醒 owner 并要求 fresh
`PublicLinkState`。同步/异步 notification 失败保留 dirty obligation，owner 在 100 ms
`retry_not_before` 后重试，因而不会丢失 fresh 值或形成失败忙循环。

`GetLinkSnapshot` 的 `event_sequence` 是当前 boot 的原子 baseline。新 `boot_id` 建立时固定
初始化为 `1`，首个增量事件分配 `2`；同一 boot 内的 NimBLE/GATT runtime restart、连接终态
和 Security 2 teardown 都保留当前值。`0` 只作为未初始化或耗尽后的内部哨兵，snapshot
encoder 会拒绝将其写入 wire，service 遇到该不变量错误时返回 `LINK_ERROR_INTERNAL`。

每个完整 Cmd0 只分配一次 security epoch，Cmd1 仅把该 epoch 从 `HANDSHAKING` 推进到
`AUTHENTICATED`。durable Commit 的终态 replay 以 generation/handle、peer identity、
transaction/credential 为键，在当前 ACL 内跨真实 long-term Cmd0/Cmd1 重握手保留；ACL
终态、generation 变化、remote replacement cutover、local revoke 或 factory reset 会清除
它，旧成功不能跨授权所有权边界重放。

## Framing 与 TX

reassembler 的结果是 `NEW_PARTIAL`、`DUPLICATE` 或 `COMPLETE`。frame ID 仅约束活动
消息；完成后可被后续消息复用。slot 保存最终片段的精确字节用于去重，重复片段不会刷新
5 秒 idle deadline。gap、overlap、offset/flag 错误和非精确重传都会被拒绝。

TX scheduler 串行 notification/indication，最多一帧 in-flight。queued、in-flight 和待
投递 completion 共享固定 `queue_depth + 1` credit；completion buffer 在 init 时分配，终态
路径不扩容。每个成功提交严格产生一次 completion，包括同步失败、异步失败、timeout、
disconnect、reset 和 deinit。

响应 indication 失败只退休对应 flow/epoch；不会设置跨会话 sticky failure。best-effort
snapshot notification 失败只使 snapshot dirty。2 秒 indication deadline 使用完整身份，
timeout 退休 scheduler operation，但把对应 NimBLE raw callback tuple 保留为 retired tracker
tombstone。迟到 terminal callback 消费该 tombstone；disconnect/reset 也会清空它。在此之前
相同 `{conn_handle, value_handle, indication}` tuple 的新 indication 被拒绝，避免无应用 token
的迟到 confirmation 冒充新 operation。

## ADV 收敛

ADV manager 把 slow non-bindable 和 fast bindable lease 收敛成唯一目标。普通 arm/cancel
wake 使用 task notification，不占深度有限的 ADV command queue；最后一个 fast lease
释放或窗口取消会立即重新查询目标。bindable lease 拒绝全零 discriminator。

每次 START 提交都分配新的非零 generation；每个新逻辑 STOP obligation 分配新的非零
generation，同一 STOP 的同步/异步重试复用该值。两个计数在 boot 内跨 manager reinit 不
复用，耗尽时 fail closed。generation 随 NimBLE command 进入 ADV owner，并由
`ADV_STARTED`/`ADV_STOPPED` completion 原样返回。manager 只接受当前 STARTING/STOPPING
transition 的匹配 completion，迟到旧代结果为 no-op。异步失败保留
`{action, generation, retry_not_before}`，采用 100/200/400/800/1000 ms 封顶退避。
冷却期内 `ble_adv_manager_poll()` 是 no-op；到期时只重试仍匹配当前 generation 的义务，
错代 retry 被清除，成功或目标变化会清除当前 retry。这既让 START 自动恢复，也避免持续
STOP 失败形成忙循环。

production ADV owner 根据 fast-window 和 retry 的最近绝对 deadline 等待，并由 task
notification 响应普通目标变化。deadline 仍是 owner state，不依赖可能丢失的队列 tick。

## SMP admission

NimBLE 固定启用 Secure Connections only、16-byte LTK 和单 bond 所需 key distribution。
闭窗 gate 为 `sm_sec_lvl=1`，开窗为 `sm_sec_lvl=0`。未知 peer 闭窗时可以保持 public-only
ACL 和读取 `link_state`，但 Pair Request 不进入 store；session access 仍要求 encrypted、
SC bond verified、LTK 与 identity facts 全部匹配。

pairing gate 保存 requested-open 与独立 cleanup、rejected-ACL、drain、revoke hold；effective
open 只在 requested-open 且 hold mask 为空时成立，旧 queued OPEN event 执行时也读取最新
effective 值。admission 拒绝新 ACL 时，CONNECT host callback 在尝试 terminate 前同步取得
gate/ADV hold 并物理关 gate，临时 STOP/HCI 失败不会留下无 tracking 的 SMP 配对机会。

根构建仅针对 ESP-IDF v6.0.2 设置 `MYNEWT_VAL_BLE_RESTART_PAIR=0`，避免 LTK lookup 失败
时闭窗主动重启 pairing。`scripts/check_idf_assumptions.sh` 锁定对应 GATT、GAP、ATT、SM、
store、connection 和 host-event 源码假设；脚本失败必须触发人工时序审查。

## 宿主测试

测试需要导出 ESP-IDF v6.0.2 的 `IDF_PATH`：

```sh
cmake -S tests/host -B /tmp/mt-ble-runtime -G Ninja \
    -DBLE_RUNTIME_SANITIZER=none
cmake --build /tmp/mt-ble-runtime
ctest --test-dir /tmp/mt-ble-runtime --output-on-failure
```

`BLE_RUNTIME_SANITIZER` 还接受 `address` 和 `thread`。套件覆盖 lifecycle、GATT registry、
GAP identity/admission、ADV retry/cancel、TX credit/completion/tombstone、codec/dispatcher、
reassembly、deadline、cleanup capacity/fence/promote、pairing-gate hold、sticky store guard、
session/security state、GATT bridge 和 IDF assumptions。x86_64 上发现
`setarch` 时，CTest 会为 TSan 自动关闭 ASLR。

宿主测试不替代 ESP32-S3 的 NimBLE host event、NVS store、射频、late indication、
handle reuse、reset/cold-cycle 和 BLE soak 验证。
