[English](README.md) | **简体中文**

# audio_service

在板级 BSP 音频设备之上提供全双工 PCM 播放与采集的单例服务，管理独占租约、生命周期、音量、静音和 NS4150B 功放使能。

## 对外 API

公共头 `include/audio_service.h`：

- `audio_service_init()` / `audio_service_deinit()`：围绕 BSP 音频设备建立或释放服务状态；服务不申请或释放 BSP 设备所有权。存在活动租约时 deinit 返回 `ESP_ERR_INVALID_STATE`。
- `audio_service_acquire()` / `audio_service_release()`：取得/释放独占流租约。initialized 且无 owner 的 READY/ERROR 可取得；ERROR 先 stop 清理。已租或 reservation 冲突返回 `ESP_ERR_INVALID_STATE`，不排队、不抢占。释放只接受当前 token/generation，失配不会停止 DMA；失败保留租约供同代重试。类型为 RECORD（读写）、PLAYBACK（只写）、MAINTENANCE（读写，基准路径）。
- `audio_service_configure()`：在 READY 且无租约时设置 PCM 格式。
- `audio_service_suspend()` / `audio_service_resume()`：系统休眠前在有界等待内静默流；租约跨 suspend 保留，恢复时按需重启；`resume_required` 指示原本是否处于运行态。
- `audio_service_write_leased()` / `audio_service_read_leased()`：在同一临界区校验租约、状态、准入及权限并登记在途 I/O，再锁外调用 BSP；服务拒绝时非空 transferred 输出为零。BSP 返回错误时仍可能有部分进度，调用方须处理已传输尾部位置。
- `audio_service_set_volume()` / `get_volume()`、`set_mute()` / `get_mute()`、`set_pa()` / `get_pa()`：系统级音量、静音与功放控制，不参与租约。
- `audio_service_set_volume_persisted()` / `audio_service_restore_volume()` / `audio_service_reset_persisted_state()`：持久化的扬声器电平。存储归属本组件，与所有自持配置的模块一致，`volume` 这个 key 不外泄给调用方。纯 `set_volume()` 只应用、从不碰 flash，因此拖动预览与基准测试的临时覆盖都是干净的；`set_volume_persisted()` 同步应用电平并排入一次提交，用于已落定的选择。提交不在调用方任务上执行：松手时调用它的 LVGL worker 栈位于 PSRAM，flash 写会关闭 cache 并触发栈断言，因此这一字节的 NVS 写由 chore worker 的内部 RAM 栈执行。返回的是入队结果；任务内的存储失败只记录日志，不回滚已经可听的电平。存储从不在服务锁内执行——NVS 提交可能阻塞在 flash 上，否则会拖住所有流与静默路径。`restore_volume()` 在无记录时保留 `audio_service_init_config_t` 的默认值，遇到损坏记录则拒绝；`reset_persisted_state()` 供工厂复位清除该 key，key 不存在视为成功。
- `audio_service_is_available()` / `audio_service_get_state()` / `audio_service_get_config()` / `audio_service_is_leased()` / `audio_service_get_lease()`：可用性、状态、配置与租约诊断查询。

## 配置与依赖

- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：`REQUIRES bsp`，`PRIV_REQUIRES freertos mt_log nvs_flash nv_storage chore_service`。
- 首次音量持久化提交前必须先初始化 `chore_service`，一字节的存储任务排入该 worker；`main` 在 `audio_service_init()` 之前完成其初始化。
- 无 Kconfig；启动策略由 `audio_service_init_config_t`（PCM 格式、初始音量、静音、PA）提供。初始化成功后调用 `audio_service_restore_volume()`，即可用持久化电平覆盖该默认值。
- 板级设备必须已初始化，服务使用前由 BSP 持有其资源。

## 状态与并发

- 单例，无独立 worker 任务；排队的音量提交运行在共享的 chore worker 上，其栈位于内部 RAM。
- 生命周期状态：UNINITIALIZED / READY / RUNNING / SUSPENDING / ERROR。
- RUNNING 有有效租约；READY 既可能 idle 无 owner，也可能是 suspend 后保留 owner。generation 跨 deinit/init 保留，新租约使旧 handle 失效。
- 首次 init（含同步对象创建）由启动 owner 串行。初始化后的生命周期事务由内部互斥锁与 SUSPENDING reservation 隔离，冲突控制返回 `ESP_ERR_INVALID_STATE`，调用方不必另建生命周期锁。
- release/suspend/deinit 先关闭准入；只有在途 I/O 非零才解锁等待。stop 与状态提交持锁完成，零 I/O 不开放 acquire 窗口。排空超时恢复原状态/准入；stop 失败保留 ERROR 和 owner/清理信息。READY 有 owner 的 release 不重复 stop。
- acquire start 失败立即 stop 补偿，成功回 READY，失败留 ERROR；输出句柄为零且返回原 start 错误。ownerless ERROR 的下次 acquire 先清理；partial-init ERROR 由 deinit/init 恢复。
- suspend 保留租约；READY suspend 为空操作。resume 在 READY 无 owner 时 OK 且不开 DMA，RUNNING 有 owner 时幂等；READY/ERROR 有 owner 时恢复原代次，ERROR 无 owner 不启动。
- 生产 `main/app_runtime_pm.c` 在休眠与回滚/唤醒中调用 suspend/resume。录音/播放、飞鸟和显示基准均持独占租约；清理失败必须保留所有权重试，不能抢停后台消费者或覆盖旧句柄。系统音量/静音/PA 请求不随 release 清零。

完整消费端与错误恢复契约见 [`doc/audio-session-lease.md`](../../../../doc/audio-session-lease.md)。

## 测试边界

- `tests/host` 存在，使用宿主 BSP/音频设备 fake 覆盖生命周期、租约互斥、陈旧 handle、I/O 准入与 suspend/resume 握手。
- 宿主不覆盖真机 I2S/DMA 时序、编解码与功放电气行为、硬件时钟和功耗。
