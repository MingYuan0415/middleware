[English](README.md) | **简体中文**

# file_service

异步存储控制与文件浏览服务。独立 worker 执行所有文件 I/O；卷生命周期、
使用权、挂载 generation 和内容 revision 均由 `sd_storage_service` 拥有。

## 公共接口

- `init/deinit`、`suspend/resume`、`get_snapshot`、`is_busy` 管理生命周期。
- `request_storage(action, expected_generation, result)` 接受普通挂载、显式
  格式化恢复、安全卸载、主动格式化或容量刷新。generation 必须来自用户动作
  当时的快照，不能在提交时重新获取以替换确认依据。
- 最多四个浏览会话；`session_list` 使用相对路径和可选游标。
- `session_detail/delete` 使用 `file_service_target_t`，携带用户选择项目时的
  相对路径、generation、revision。提交和执行均检查版本，陈旧目标不会删除
  后来同名的文件。
- `session_get_batch/detail` 将上次成功发布的数据复制到调用者缓冲；不借出
  内部指针。调用者将大批次/详情缓冲独立分配在堆，不放入 Page arena 或任务栈。
- `session_get_result/cursor` 复制状态/游标。内容变更通知和 revision 查询直接
  使用持有有效 lease 的 SD 服务 API，本组件没有第二份 revision。

## 有界内存与所有权

初始化在状态锁外分配一个 PSRAM 优先、普通 8-bit 堆回退的固定池，包含四个
会话及各自请求槽、一个存储请求槽、已发布批次/详情和独立 worker scratch。
工作期间不分配或释放这些对象；队列只复制槽指针。worker 的 6144 字节栈以
`SPIRAM | 8BIT` 显式分配，不回退内部 RAM；TCB、队列和锁仍在内部 RAM。
worker、初始化和提交链没有 1 KiB 及以上的栈对象。任务路径仅操作 SD，不能从
该外部栈执行 NVS、flash 文件系统、OTA 或显式 sleep 调用。
初始化失败会释放全部部分分配，调用方不取得服务所有权，也无需调用 `deinit`；
后续仍可重新初始化。
worker 的 CPU affinity 使用 `CONFIG_MAIN_PROJECT_TASK_CORE_ID`，与项目 CPU0、
CPU1 或 no-affinity 配置一致；宿主回归以 CPU1 配置校验任务创建参数。

每会话同时只能有一个在途操作。非零 session ID 与 operation ID 独立分配，
不会因槽重用或挂载切换立即复用。关闭会话立即撤销读接口；在途请求的槽仍然
保留，直到唯一完成路径关闭句柄、释放 lease、结算 pending 后才可再次打开。
取消不会终止正在执行的系统调用。删除在进入 `unlink/rmdir` 前原子检查取消并
标记 mutation-started；此后保留真实成功或失败结果，不把已经执行的删除报告成
取消。关闭页面仍撤销读接口，不把结果交给后来重开的会话。状态锁内仅做有界
状态/结果复制和非阻塞指针入队，不做文件 I/O、SD 查询或内存分配/释放。

目录每批最多 16 项，先目录后普通文件，同类保持文件系统顺序。游标带有
stream offset、generation 和 revision；每批关闭目录，续读重新打开/定位。
运行中的批次只写 scratch，copy getter 仍可读取先前成功发布的完整批次。
满 16 项后继续前瞻下一个合格项目；只有确实还有项目时才发布 `has_more`，
游标停在该项目之前，恰好 16 项的目录不会出现空的续页。

## 路径与错误

- 相对路径 `""` 表示浏览根；根不能作为详情或删除目标。
- 拒绝绝对路径、`.`、`..`、空段、反斜杠及超长路径，不静默截断。
- 名称缓冲为 1021 字节（255 个四字节 UTF-8 Unicode 标量及 NUL）；完整
  backend 路径仍限 1024 字节，包括 mount/root 前缀与 NUL。
- 宿主检查每个路径组件，包括配置的根，拒绝所有符号链接（即使指向根内），
  并用 `PATH_MAX` 大小的独立 scratch 调用 `realpath`，再检查公开路径预算。
  设备 FAT 文件系统没有符号链接。
- 删除仅用 `unlink` 和 `rmdir`，从不递归。对 FAT 可能返回的 EACCES，检查
  目录是否确实非空，以区分 NOT_EMPTY 与 PERMISSION。
- 接纳失败和异步终态均提供 `error`、结构化 `reason` 和已保存的
  `system_errno`。不存在、非空、权限、路径过长、空间不足、内存不足、陈旧、
  未挂载、忙、取消和 I/O 错误不混为同类；未挂载且未故障的卷报告 UNAVAILABLE，
  已挂载但租约冲突报告 BUSY，介质故障报告 IO。
  `readdir` 的错误与正常 EOF 明确区分，清理不会覆盖原始 errno。

## 挂载、容量与停止

初始普通挂载是计入 pending 的正常存储请求，具有非零 operation ID，失败不
影响服务读快照和接受重试。普通挂载不格式化；格式化恢复必须由显式动作到达。
卸载、格式化和恢复的 epoch 检查由 SD owner 与无活跃 lease 检查原子完成。
容量查询传入当前 ACCESS lease；查询结果在锁外采集、锁内发布。挂载、恢复或
格式化后的容量查询失败也使该存储请求 FAILED，不发布误导性的 DONE。
`space.valid` 为假时不能把数值当成可用容量。快照中的 faulted/last_error 直接
镜像 SD owner，不把正常卸载与介质故障混淆。
容量查询保存失败时的 errno；只有 EIO 在释放 ACCESS lease 前报告介质故障，
使 generation 失效并要求普通挂载完成旧资源清理后重新探卡。其他容量错误只使
容量无效，不把健康卷标记为故障。

`suspend` 关闭接纳并等待 pending 清零；超时仍关闭接纳，调用者应在待机回滚
时显式 `resume`。`deinit` 永久关闭接纳，关闭会话并排空已有请求，不中断维护。
worker 最后一次访问服务状态后发送完成 semaphore，然后循环 delay park；owner
收到握手后用非空 handle 调用 `vTaskDeleteWithCaps`，再释放队列、锁和内存。
超时保留任务、栈、完整资源及停止状态，`resume` 不会重新开启正在停止的服务，
再次 `deinit` 可安全完成释放。

## 宿主回归

`tests/host` 编译真实服务 worker 和 pthread-backed FreeRTOS queue/task shim，
使用真实 POSIX 文件及脚本化 SD owner；链接包装器控制阻塞 I/O、queue-full 和
errno。覆盖关闭/重开、排队取消、copy publication、挂起、初始挂载 pending、
停止超时重试、陈旧删除、结构化错误、readdir 失败、EACCES 非空目录和符号链接。
回归覆盖中文名称的 LIST/DETAIL 往返、1100 个目录的目录优先分批枚举与有界
服务分配、阻塞删除后的取消/关闭真实结果以及维护后容量查询失败。真实 SD
时序、拔卡中断及 FatFs 差异仍需设备验证。

worker 的任务栈为 6 KiB。带诊断候选的串口已确认 worker 栈位于 PSRAM，
用户完成录放期间文件浏览并反馈正常。宿主回归覆盖 caps worker 创建失败补偿、
owner 回收，以及阻塞 I/O 下停止超时保留任务/lease、解除阻塞后重试释放。
这些结果不替代真实 SD/FatFs 调用链的栈高水位，也不代表最深路径、卸载、
主动/恢复格式化或故障介质场景均已通过设备验证。

```sh
cmake -S layers/middleware/components/file_service/tests/host -B /tmp/mt-file-service -G Ninja
cmake --build /tmp/mt-file-service
ctest --test-dir /tmp/mt-file-service --output-on-failure
```
