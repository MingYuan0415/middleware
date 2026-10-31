[English](README.md) | **简体中文**

# music_service

MicroTech 的 QQ 音乐账号、曲库、封面、歌词与流媒体播放服务。服务独占 QQ
音乐 HTTPS 客户端与音频流；音乐 App 只消费快照，不执行网络、JSON、图片解码
或音频 I/O。

## 公共 API

`include/music_service.h` 提供：

- `music_service_init()` / `music_service_deinit()` 与
  `music_service_set_network_ready()`。
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
- `music_service_is_playing()` 是供系统 PM 使用的无锁查询。

快照、图片与音频缓冲分配在 PSRAM。服务运行状态、Cookie 罐与两个命令队列也
放在 PSRAM。API worker 栈保留在内部 RAM，因为凭证保存/删除要经过 NVS，而
NVS 写会关闭 flash cache；音频 worker 使用 PSRAM 栈，并通过 `audio_service`
的独占播放租约接收 PCM。

## 登录与曲库

- 微信扫码通过 QQ 音乐 Web OAuth 流程实现。
- QQ 账号扫码通过 `ptlogin2.qq.com`、Graph OAuth 授权和
  `QQConnectLogin.LoginServer/QQLogin` 完成。
- 二维码可能是 JPEG 或 PNG，两种格式都会解码为 RGB565；当前版本不实现
  QQ 音乐 App 的 MQTT 登录。
- 凭证保存在 NVS 的 `music_cred` Blob；删除凭证是工厂重置流程的一部分。
- 首页数据合并匿名推荐流与登录用户的自建/收藏歌单。歌单详情与歌词请求受
  公共头文件上限约束。
- 曲库解析同时兼容当前字段（`mid`、`file.media_mid`、`dirinfo`）与旧字段
  （`songmid`、`strMediaMid`、`dissname`）；歌曲 URL 请求使用 `CgiGetVkey`
  数组参数和 `media_mid` 文件名，歌词使用 `crypt: 0` 的普通 LRC。

## 音频

`music_stream.c` 读取 CDN 流，使用 `esp_audio_simple_dec` 解码 MP3/FLAC，
把解码采样率/声道/位深映射到 BSP 支持格式，再通过
`audio_service_write_leased()` 写入 PCM。网络读取与解码在专用音频 worker
执行；两个 worker 都不调用 LVGL。

## 配置

`Kconfig` 定义 API/音频 worker 栈、命令队列深度与最大 JSON 响应；产品通过
`music_service_config_t` 提供任务优先级与默认音质。

## 测试边界

QQ 登录协议解析已有独立 host 测试套件；网络、TLS、凭证存储与播放仍需在板端
验证。Wi-Fi、I2S/DMA、ES8311 与电池行为不在 host 测试范围内。
