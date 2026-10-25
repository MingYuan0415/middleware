# recorder_service

在 `audio_service` 与 `sd_storage_service` 之上提供 WAV 录音、播放与文件管理，并维护状态快照。

## 对外 API

公共头 `include/recorder_service.h`：

- `recorder_service_init()` / `deinit()`：初始化或停止服务；目录是卷内相对路径
  （例如 `MicroTech/Recordings`），服务在每次使用时通过 `sd_storage_service`
  解析当前挂载点，不硬编码挂载路径。无卡时初始化同样成功，录音只是不可开始。
- `recorder_service_start()` / `pause()` / `resume()` / `stop()`：录音命令，完成通过快照报告。
- `recorder_service_play()` / `stop_playback()`：播放指定 WAV 并停止播放。
- `recorder_service_list()` / `delete()`：枚举已完成 WAV 与删除文件，不暴露目录句柄。
  列表按修改时间倒序（`has_mtime` 为假的行排在已知时间之后，同时间按文件名倒序），
  最多保留最新的 `RECORDER_SERVICE_MAX_FILES` 条；超出时快照的 `library_truncated`
  为真，UI 据此提示「仅显示最新 N 个」。
  列表每行的 `target` 保留完整 UTF-8 后端路径（192 字节，含终止符）及该次
  索引发布的挂载 generation / 内容 revision；播放与删除接收该 target 的指针，
  队列复制完整 target，不按同名路径重新选择文件。每行另带 `has_mtime` 与
  `modified_epoch_s`（来自 `stat`），供 UI 显示录制时间。
- `recorder_service_refresh()`：只读快照计数的调用方用它请求重新发布过期索引；
  与 `list()` 的隐式请求同样合并，索引新鲜时不排队。
- `recorder_service_get_snapshot()` / `is_busy()`：状态快照与忙碌查询。快照
  区分 `free_bytes`（`space_valid` 为真时才有效）、`space_error` 与
  `storage_ready`；容量查询失败不会被当作空间不足。当前 SD 的
  `storage_generation` / `content_revision` 在 recorder 锁外读取；
  `index_generation` / `index_revision` / `index_valid` / `file_count` /
  `library_truncated` 在锁内与索引一同复制；`max_duration_ms` 来自初始化配置，
  UI 用它显示上限与剩余录音时间。

`recorder_service_snapshot_t.name`、`recorder_service_file_t.target.name` 和播放/删除命令均保存
完整文件路径，容量为 `RECORDER_SERVICE_PATH_BYTES` 字节（含结尾 NUL）；路径拼接超出该容量的
录音文件不进入服务索引。

## 文件命名

新录音以 `REC_YYYYMMDD_HHMMSS` 命名（本地时间，系统时钟不早于 2020 时才采用）；
时钟未校准时回退为 `REC_<esp_timer 微秒>`，保持单调可排序。同一秒内的第二次录音
追加 `-2`、`-3` 后缀：创建用 `open(O_CREAT|O_EXCL)` 占位，并额外探测对应的
`.wav` 是否已存在，因此重启或在同一秒内连续录音都不会覆盖既有文件。

名字中出现连续两点（如 `REC_..a.wav`）不影响播放与删除：路径归属检查只要求文件名是
录音目录的直接子项且不含分隔符，`.`/`..` 条目本身仍被拒绝。此前按“含 `..` 即拒绝”
判定，会让扫描列出的这类录音长期可见却无法操作。

## 配置与依赖

- CMake：`PRIV_REQUIRES audio_service esp_timer fatfs freertos mt_log sd_storage_service`。
- `idf_component.yml`：`idf: ">=5.0"`，本地依赖 `audio_service`、`sd_storage_service`、`mt_log`。
- 无 Kconfig；运行时由 `recorder_service_config_t` 提供目录、最大录音时长与最小剩余空间。

## 状态与并发

- 单例；命令经有界队列交给 pinned worker 任务，录音与播放共用一个流任务。
  设备流任务的 4096 字节栈以 `SPIRAM | 8BIT` 显式分配，不回退内部 RAM；
  共用 4096 字节 PCM 位于外部 BSS。TCB、队列、锁和 6144 字节命令栈仍在内部。
  命令栈承载每次索引扫描向下的 VFS → FATFS → diskio → SDSPI 调用链；按 `-O2`
  实测该链（扫描 304、`vfs_fat_stat` 336、`f_stat` 96、`sdspi_host_start_command`
  272、`start_command_default` 96，未计 newlib printf 与扇区缓冲）约需 1.8 KB，
  加上命令任务自身 576 字节与打断余量，4096 不足，6144 留出约 3 KB 裕量。
  普通宿主与模拟器只运行命令 worker，不模拟音频流，SESSION 保留至显式停止；
  独立的设备分支宿主目标执行真实流任务以验证生命周期。
- 状态以 `generation` / `operation_id` 标识，命令异步完成；临界区与原子量保护共享字段。
  所有跨服务调用（包括 SD 状态、revision、租约和计时）均在 recorder 临界区之外执行。
- 状态机：IDLE / RECORDING / PAUSED / PLAYING / ERROR。
- 暂停期间采集设备仍在运行，流任务继续读取并丢弃输入；不读会让暂停区间积存在 I2S
  环形缓冲里，恢复后写入文件，产生用户没录过的音频且数据长度超出上报时长。
- 失败的录音（IO 错误、时长上限、写头失败）在关闭后删除 `.part` 工作文件；否则库永远
  不列出这些名字，用户也无法删除，只会缓慢占满存储卡。
- 音频流：录音取得 `AUDIO_SERVICE_SESSION_RECORD` 租约（读写），播放取得
  `AUDIO_SERVICE_SESSION_PLAYBACK` 租约（只写）；流任务用租约读写，收尾/错误路径
  只有 release 真正返回 `ESP_OK` 才清除同代句柄。任何错误（包括
  `ESP_ERR_INVALID_STATE`）均保留清理义务；stop、下一次 start/play 或 deinit
  可重试。start/play 在取得 SD SESSION 或打开/创建文件之前先清理旧租约，
  失败原样返回，不覆盖旧代次。音频被其它写者占用时启动回滚文件和 SD 使用权，
  不停止对方流。
- 占用：目录准备、文件创建、录制、暂停、关闭、WAV finalize 与索引刷新，以及
  播放从打开到关闭，全程持有 `sd_storage_service` 的业务使用权（SESSION）；
  删除取独占维护权。因此录音/播放期间可以浏览，但删除、卸载与格式化会被拒绝。
  播放取得 SESSION、删除取得 MAINTENANCE 后，先核对目标 generation 与租约、
  当前 SD generation / revision；过期目标在打开或删除之前失败并释放租约，
  不会触碰同路径的新文件。`ESP_OK` 仅表示命令接受，须等待同一 `operation_id`
  的 `operation_pending == false` 并检查 `last_error` 后才能判断成功。
- 索引：worker 在独立扫描前取得 ACCESS，从路径解析、容量查询到目录与每个
  WAV 头检查、关闭句柄全程持有；录音结束与删除后的刷新复用既有 SESSION / MAINTENANCE。
  扫描前后核对 SD generation 与内容 revision，变化则丢弃结果；UI 查询只读缓存，
  过期列表返回空并请求异步刷新，设备与宿主均不在查询中做文件/容量 I/O。
  失败或中断的目录扫描不发布有效索引；并发扫描用 publication version 防止旧结果覆盖
  新索引。有效索引变化递增快照 generation，列表验证与复制在同一临界区完成。
  录音目录由首次录音惰性创建，未录制过的卡上目录不存在属于空库而非扫描失败，
  `opendir()` 返回 `ENOENT`/`ENOTDIR` 时照常发布空索引（否则首次开机将永久停在读取中）。
  索引只在内容真正变化时按实际条数复制进运行时缓存；无条件复制 7 KB 会在关中断临界区里
  付出几十微秒的中断延迟，而读到的字节对任何读者都不可见。
  每次扫描的完整工作集（`RECORDER_SERVICE_MAX_FILES` 条元数据加两个路径缓冲，
  约 7.3 KB，类型 `recorder_scan_t`）独立分配在堆上，发布后或扫描失败时释放，不放在
  `recorder_cmd` 的任务栈上，也不与并发收尾扫描共用 scratch。扫描按字段
  （文件名、mtime、epoch）比较并直接写入目标槽位，不在栈上构造候选记录。
  分配失败使索引无效并释放本次 ACCESS；不会上报介质故障，后续 list 可请求重扫。
- 内容 revision 由 `sd_storage_service` 统一维护，无 `file_service` 依赖。
  录音先关闭并 finalize WAV，再通知内容变更、刷新索引并释放 SESSION，最后发布
  IDLE / ERROR 和允许下一次开始；播放关闭不修改内容 revision。
  缓存容量附带挂载 generation，拔卡/重挂载时不会返回旧容量为有效值。
- 仅文件系统调用确认的 `errno == EIO` 上报介质故障；任务内存不足、音频失败、
  权限错误或空间不足不会将健康 SD 标记为故障，仍保证文件关闭与租约释放。
- 流任务先完成音频停止、文件关闭/收尾、索引刷新和 SESSION 释放，最后锁内发布
  `task_running=false`，随后只循环 delay park，不自删除。命令任务是唯一创建/回收
  owner，在锁外用非空 handle 调用 `vTaskDeleteWithCaps`；有待回收流任务时以
  50 ms 周期接收命令，确保自然 EOF/错误无需新用户命令也能回收。STOP 完成和
  SHUTDOWN 退出前均先回收；deinit 超时不强杀 I/O 或释放仍在使用的资源。
  这些外部栈路径仅访问 SD/SDSPI 与 I2S，不执行 NVS、flash 文件系统、OTA 或显式 sleep。
  SDSPI 复用 SDK slot 的 516 字节内部 DMA staging；外移量不等于净 DMA 空闲增量。
- deinit 在全部清理成功前保持 initialized/stopping，拒绝新命令。队列存在且尚未
  成功发送 SHUTDOWN 时按 FIFO 发送 STOP、SHUTDOWN；入队失败可重试，成功发送
  SHUTDOWN 后的超时重试只等待退出，不再向无人消费的队列追加命令。
  命令任务退出后删除队列并清空指针；之后音频 release 即使失败，下一次 deinit
  仍直接重试剩余清理，不访问空队列。成功 deinit 后可重新 init、录音与播放。

## 内存归属

索引缓存（`RECORDER_SERVICE_MAX_FILES` × 每条元数据，32 条共 7,168 B）放在 PSRAM。
内部 DRAM 同时承担显示 SPI bounce 缓冲（10 行 × 368 × 2 B × 队列 2 = 14,720 B 连续块）
与 I2S 全双工 DMA 环（每方向 5,760 B）；索引若留在内部 DRAM，录音启动后单个显示分块
就会 `ESP_ERR_NO_MEM`，界面停止刷新。

## 测试边界

- `tests/host` 使用真实 pthread 命令 worker 与 audio/SD fake，覆盖异步状态迁移、
  无卡启动与重挂载、容量查询失败/空间不足、暂停与播放 SESSION、浏览 ACCESS 共存、
  扫描 ACCESS 阻止维护、扫描中 revision/epoch 变化丢弃、以及关闭/内容失效/租约释放
  先于 IDLE 发布和下一命令接受的顺序。阻塞容量查询时 UI 快照与列表仍只读缓存。
  `recorder_service_races_test` 用私有实现测试和确定性 barrier 覆盖通知、目录错误重试、
  旧/新扫描发布竞争、列表验证/复制交错，以及 EIO 与非存储错误的区分。
  `recorder_service_recovery_test` 用条件变量和仅测试侧的时钟/队列 seam 覆盖
  STOP/SHUTDOWN 入队失败、SHUTDOWN 已发送后的等待超时/重试、队列已删除但
  音频仍失败时多次 deinit，以及不重置 fixture 的重新初始化/录音/播放。
  音频 fake 用同一 mutex 校验 token/generation、独占 kind 和 RUNNING I/O，
  release 失败保留 ERROR 与 owner；races/stream 回归同时覆盖保留旧代的恢复。
  确定性租约 barrier 覆盖播放/删除接受后发生 revision 变化或同路径重挂载替换：
  原目标终态失败、替换文件逐字节保留、不启动音频且释放 SESSION / MAINTENANCE；
  同时覆盖当前目标的成功播放/删除及索引元数据与快照 epoch 对应。
  扫描缓冲 OOM 回归检查索引失效、ACCESS 释放、介质保持可用与后续恢复。
  `recorder_service_library_test` 覆盖上限截断标记、按修改时间倒序发布、
  删除至上限后清除截断标记，以及同一秒内两次录音仍取得不同文件名。
  `recorder_service_stream_test` 单独定义 `ESP_PLATFORM`，使用真实 pthread-backed
  FreeRTOS 流任务与条件变量控制音频读写。覆盖 WAV 长度/头、显式停止、最大时长、
  无命令的播放 EOF、读写错误、创建失败补偿、完成早于创建返回、立即 start/play、
  活跃录音 deinit 与 20 次重复录放，并核对 owner 删除和任务/文件句柄/SESSION 无累计。
  静态栈帧不含完整 libc/VFS/驱动调用链，不能替代真机栈高水位测量。
- 宿主不覆盖真实 I2S 音频、SD 卡 I/O 与文件系统掉电。

### 设备验证范围

带诊断候选 `095ce70ddb04083a6f95fa25c42ab21032c3846c5e9af8ab8cb827d6b1409ffa`
已完成天气 HTTPS 更新后的长录音、文件浏览、暂停/继续、停止和播放自然结束；
用户确认操作正常，并进一步确认 Wi-Fi/BLE 同时开启时连续录音正常。
串口确认流栈与 PCM 在 PSRAM，命令栈仍在内部 RAM；各次已观察流任务由 owner
回收，任务数从 28 回到 27，未出现目标 LCD DMA 分配失败或显示提交失败。

这些结果支持当前资源放置方案，不构成所有无线负载的内部堆余量保证，也不替代
冷启动、待机/唤醒或文件系统掉电验证。清理诊断后的固件与该候选分别保存；
候选的实机结果不能冒充清理后固件已重新上板验证。
