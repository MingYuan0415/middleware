# audio_service

在板级 BSP 音频设备之上提供全双工 PCM 播放与采集的单例服务，管理生命周期、音量、静音和 NS4150B 功放使能。

## 对外 API

公共头 `include/audio_service.h`：

- `audio_service_init()` / `audio_service_deinit()`：围绕 BSP 音频设备建立或释放服务状态；服务不申请或释放 BSP 设备所有权。
- `audio_service_configure()` / `audio_service_start()` / `audio_service_stop()`：设置 PCM 格式、启动或停止全双工 DMA。
- `audio_service_suspend()` / `audio_service_resume()`：系统休眠前在有界等待内静默流，恢复时按需重启；`resume_required` 指示原本是否处于运行态。
- `audio_service_write()` / `audio_service_read()`：写入或读取交织 PCM 字节。
- `audio_service_set_volume()` / `get_volume()`、`set_mute()` / `get_mute()`、`set_pa()` / `get_pa()`：音量、静音与功放控制。
- `audio_service_is_available()` / `audio_service_get_state()` / `audio_service_get_config()`：可用性、生命周期状态与当前 PCM 配置查询。

## 配置与依赖

- `idf_component.yml`：仅 `idf: ">=5.0"`。
- CMake：`REQUIRES bsp`，`PRIV_REQUIRES freertos mt_log`。
- 无 Kconfig；启动策略由 `audio_service_init_config_t`（PCM 格式、初始音量、静音、PA）提供。
- 板级设备必须已初始化，服务使用前由 BSP 持有其资源。

## 状态与并发

- 单例，无独立 worker 任务。
- 生命周期状态：UNINITIALIZED / READY / RUNNING / SUSPENDING / ERROR。
- 内部互斥锁与二值信号量保护生命周期与在途 I/O；生命周期控制 API 必须由调用方串行化。
- suspend/resume 与在途读写通过排空握手协调；失败转换可由 stop、resume 或 deinit 恢复。

## 测试边界

- `tests/host` 存在，使用宿主 BSP/音频设备 fake 覆盖生命周期、I/O 准入与 suspend/resume 握手。
- 宿主不覆盖真机 I2S/DMA 时序、编解码与功放电气行为、硬件时钟和功耗。