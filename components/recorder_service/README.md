# recorder_service

在 `audio_service` 与 `sd_storage_service` 之上提供 WAV 录音、播放与文件管理，并维护状态快照。

## 对外 API

公共头 `include/recorder_service.h`：

- `recorder_service_init()` / `deinit()`：初始化或停止服务；配置包含目录、最大时长与最小剩余字节。
- `recorder_service_start()` / `pause()` / `resume()` / `stop()`：录音命令，完成通过快照报告。
- `recorder_service_play()` / `stop_playback()`：播放指定 WAV 并停止播放。
- `recorder_service_list()` / `delete()`：枚举已完成 WAV 与删除文件，不暴露目录句柄。
- `recorder_service_get_snapshot()` / `is_busy()`：状态快照与忙碌查询。

## 配置与依赖

- CMake：`PRIV_REQUIRES audio_service sd_storage_service esp_timer fatfs freertos mt_log`。
- `idf_component.yml`：仅 `idf: ">=5.0"`。
- 无 Kconfig；运行时由 `recorder_service_config_t` 提供目录、最大录音时长与最小剩余空间。

## 状态与并发

- 单例；命令经队列交给 pinned worker 任务，录音与播放使用独立任务。
- 状态以 `generation` / `operation_id` 标识，命令异步完成；临界区与原子量保护共享字段。
- 状态机：IDLE / RECORDING / PAUSED / PLAYING / ERROR。

## 测试边界

- `tests/host` 存在，使用 audio/SD fake 覆盖命令队列、状态迁移与文件枚举。
- 宿主不覆盖真实 I2S 音频、SD 卡 I/O 与文件系统掉电。