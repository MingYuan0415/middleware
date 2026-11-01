[English](README.md) | **简体中文**

# music_service

MicroTech 的 QQ 音乐账号、曲库、封面、歌词与流媒体播放服务。服务独占 QQ
音乐 HTTPS 客户端与音频流；音乐 App 只消费快照，不执行网络、JSON、图片解码
或音频 I/O。

## 公共 API

`include/music_service.h` 提供：

- `music_service_init()` / `music_service_deinit()`、
  `music_service_set_network_ready()` 与 `music_service_set_time_ready()`。
- 扫码登录：`music_service_login_begin()`、`music_service_login_qr_acquire()`、
  `music_service_login_cancel()`、`music_service_logout()` 与
  `music_service_reset_persisted_state()`。
- 曲库：`music_service_refresh_home()`、`music_service_home_acquire()` /
  `release()`、`music_service_playlist_acquire()` / `release()` 与
  `music_service_lyric_acquire()` / `release()`。
- 图片：`music_service_image_acquire()` / `release()`。
- 播放：`music_service_play_playlist()`、`play_pause()`、`next()`、`prev()`、
  `set_mode()`、`stop()` 与 `music_service_get_player_status()`。`seek()` 为保留
  接口，范围跳转实现前返回 `ESP_ERR_NOT_SUPPORTED`。
- 队列：`music_service_queue_acquire()` / `release()` 暴露不可变当前队列
  （上限 `MUSIC_SERVICE_MAX_SONGS`）；`music_service_queue_play()` 切换队列项，
  `music_service_queue_remove()` 提交删除并发布
  `MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE`。
- `music_service_is_playing()` 是供系统 PM 使用的查询；它覆盖“已接受播放命令但
  流任务尚未启动”的窗口。
- `music_service_get_diagnostics()` 返回 API/音频任务栈水位与 API 队列深度，
  供调试与验收使用。

快照、图片与音频缓冲分配在 PSRAM。所有快照/图片的 `acquire()` 都是引用计数
借用：服务保留自己的引用，`release()` 只减少调用方引用；替换缓存项不会使仍
持有引用的页面失效。服务运行状态、Cookie 罐与两个命令队列也放在 PSRAM。
API worker 栈保留在内部 RAM，因为凭证保存/删除要经过 NVS，而 NVS 写会关闭
flash cache；音频 worker 在首次播放时懒创建，使用 PSRAM 栈，并通过
`audio_service` 的独占播放租约接收 PCM。

## 登录与曲库

- 微信扫码通过 QQ 音乐 Web OAuth 流程实现。
- QQ 账号扫码通过 `ptlogin2.qq.com`、Graph OAuth 授权和
  `QQConnectLogin.LoginServer/QQLogin` 完成。
- 二维码可能是 JPEG 或 PNG，两种格式都会解码为 RGB565；当前版本不实现
  QQ 音乐 App 的 MQTT 登录。
- 凭证保存在 NVS 的 `music_cred` Blob；删除凭证是工厂重置流程的一部分。
- 首页数据合并匿名推荐流、登录用户的自建歌单、dirId 201 的“我喜欢”歌单与
  `CgiGetPlaylistFavInfo` 兼容账号接口返回的收藏歌单。首页在 App 侧边栏挂载时
  按需请求，不在每次网络就绪时预取；歌单详情、歌词与队列请求受公共头文件上限
  约束。
- 曲库解析同时兼容当前字段（`mid`、`file.media_mid`、`dirinfo`）与旧字段
  （`songmid`、`strMediaMid`、`dissname`）；歌曲 URL 请求使用 `CgiGetVkey`
  数组参数和 `media_mid` 文件名，歌词使用 `crypt: 0` 的普通 LRC。请求音质
  不可用时逐级回退到较低音质，并记录实际使用的音质。

## 音频

`music_stream.c` 读取 CDN 流，使用 `esp_audio_simple_dec` 解码 MP3/FLAC，
把解码采样率/声道/位深映射到 BSP 支持格式，再通过
`audio_service_write_leased()` 写入 PCM。PCM 短写会在有界重试内补齐；连续
写入失败会报告播放错误，而不是静默丢弃音频。若 BSP 没有对应采样率的
24-bit clock pair，则在写入前把 PCM 转为 16-bit。暂停会释放独占音频租约并
关闭 codec，让其他音频消费者使用设备；恢复时用保存的 PCM 格式重新申请租约。
网络读取与解码在专用音频 worker 执行；两个 worker 都不调用 LVGL。

## 配置

`Kconfig` 定义 API/音频 worker 栈、命令队列深度与最大 JSON 响应；产品通过
`music_service_config_t` 提供任务优先级与默认音质。

## 测试边界

QQ 登录协议解析、引用计数快照分配器与队列删除状态转移已有独立 host 测试；网络、TLS、凭证
存储与播放仍需在板端验证。Wi-Fi、I2S/DMA、ES8311 与电池行为不在 host
测试范围内；模拟器真后端是集成辅助，不替代板端音频与功耗测量。
