[English](README.md) | **简体中文**

# music_service

MicroTech 的 QQ 音乐账号、曲库、封面、歌词与流媒体播放服务。服务独占 QQ
音乐 HTTPS 客户端与音频流；音乐 App 只消费快照，不执行网络、JSON、图片解码
或音频 I/O。

## 公共 API

`include/music_service.h` 提供：

- `music_service_init()` / `music_service_deinit()`、
  `music_service_set_network_ready()`、`music_service_set_time_ready()` 与
  `music_service_retry()`。
- 扫码登录：`music_service_login_begin()`、`music_service_login_qr_acquire()`、
  `music_service_login_cancel()`、`music_service_logout()` 与
  `music_service_reset_persisted_state()`。
- 曲库：`music_service_refresh_home()`、`music_service_home_acquire()` /
  `release()`、`music_service_playlist_acquire()` / `release()` 与
  `music_service_lyric_acquire()` / `release()`，并由
  `music_service_lyric_failed()` / `music_service_lyric_retry()` 查询或清除
  已完成的空结果。同一歌曲的待处理歌词请求会去重；空结果保持到调用方重试或
  歌曲切换。
- 图片：`music_service_image_acquire()` / `release()`。服务保留一个小型 LRU
  （播放器封面、歌单封面与账号头像），重新进入页面时复用已解码图片，避免重复
  下载。
- 播放：`music_service_play_playlist()`、
  `music_service_play_playlist_snapshot()`（复用调用方已持有的歌单快照）、
  `play_pause()`、`next()`、`prev()`、`set_mode()`、`stop()` 与
  `music_service_get_player_status()`。歌曲没有可用播放 URL 时，服务会尝试
  下一条队列项（单曲循环也会前进），最多尝试整个队列；全部不可用时报告
  `NO_PERMISSION`。`seek()` 为保留接口，范围跳转实现前返回
  `ESP_ERR_NOT_SUPPORTED`。
- 队列：`music_service_queue_acquire()` / `release()` 暴露当前播放位置附近
  有界物化窗口；快照携带来源歌单引用、逻辑 `total`、窗口 `offset` 与窗口
  `count`。`music_service_queue_view_acquire()` 按页物化完整逻辑队列视图；
  `music_service_queue_play()` 与 `music_service_queue_remove()` 使用逻辑队列
  绝对下标。逻辑队列上限为 `MUSIC_SERVICE_MAX_QUEUE_TRACKS`，单个物化窗口
  不超过 `MUSIC_SERVICE_MAX_SONGS`。
- `music_service_get_status()` 复制账号状态，包括 `user_id` 以及服务当前已知的
  `user_name` / `user_avatar`。
- `music_service_is_playing()` 是供系统 PM 使用的查询；它覆盖“已接受播放命令但
  流任务尚未启动”的窗口。
- `music_service_get_diagnostics()` 返回 API/音频任务栈水位与 API 队列深度，
  供调试与验收使用。

快照、图片与音频缓冲分配在 PSRAM。所有快照/图片的 `acquire()` 都是引用计数
借用：服务保留自己的引用，`release()` 只减少调用方引用；替换缓存项不会使仍
持有引用的页面失效。服务运行状态、Cookie 罐与两个命令队列也放在 PSRAM。
API 与登录 worker 栈保留在内部 RAM，因为凭证保存/删除要经过 NVS，而 NVS 写会
关闭 flash cache。登录 worker 同时是账号 worker，串行执行二维码登录与服务端
会话验证，因此探测不会占用目录/播放 worker 任务；HTTP 请求仍共享传输互斥锁。
凭证保存/删除使用独立持久化互斥锁，迟到的二维码保存不能覆盖退出登录。音频
worker 在首次播放时懒创建，使用 PSRAM 栈，并通过 `audio_service` 的独占播放
租约接收 PCM。Cookie 状态使用独立互斥锁；清理时会等待活动 HTTP 传输结束。

## 登录与曲库

- 微信扫码通过 QQ 音乐 Web OAuth 流程实现。
- QQ 账号扫码通过 `ptlogin2.qq.com`、Graph OAuth 授权和
  `QQConnectLogin.LoginServer/QQLogin` 完成。
- 二维码可能是 JPEG 或 PNG，两种格式都会解码为 RGB565；当前版本不实现
  QQ 音乐 App 的 MQTT 登录。
- 凭证保存在 NVS 的 `music_cred` Blob；删除凭证是工厂重置流程的一部分。
- 从 NVS 加载的凭证只作为待验证候选。网络与时间就绪后，服务先发布
  `VALIDATING_SESSION`，使用凭据 Cookie 向
  `fcg_get_profile_homepage.fcg` 校验当前会话，成功后才发布 `READY` 与
  `logged_in`。网络、HTTP、限流与解析失败会退避重试；已知的登录失效 code
  才进入 `AUTH_REQUIRED`。`READY` 期间每十分钟复查；网络就绪边沿与二维码登录
  成功会唤醒验证，重复连接快照不会重置计时。`expires_at` 仍保留在凭据 Blob
  中以兼容旧数据，但不再作为 `musickey` 有效期使用。
- 每个账号请求在入队时捕获凭证代次。worker 会拒绝过期命令，网络结果在写入
  快照或改变账号状态前还会再次校验代次。只有实际携带已验证凭证的请求才解释
  鉴权业务码；二维码 code exchange 的失败保持普通登录失败语义。会话失效会
  原子清除凭证、首页/歌单/队列/歌词/图片缓存与待处理账号工作，再进入
  `AUTH_REQUIRED`。凭证保存失败使用 `AUTH_REQUIRED + FAILURE_STORAGE`，与真正
  的登录过期严格区分。
- 凭证加载或登录完成后的下一次首页刷新中，服务通过
  `userInfo.BaseUserInfoServer/get_user_baseinfo_v2` 读取当前账号资料；昵称与
  头像通过 `music_service_get_status()` 暴露，`user_id` 保持为回退身份标识。
  同一凭证的资料成功读取后不再重复；首页快照已缓存但资料缺失时，
  `music_service_refresh_home()` 只排队资料刷新。
- 首页数据合并匿名推荐流、登录用户的自建歌单、dirId 201 的“我喜欢”歌单与
  `CgiGetPlaylistFavInfo` 兼容账号接口返回的收藏歌单。首页在 App 侧边栏挂载时
  按需请求，不在每次网络就绪时预取；歌单详情、歌词与队列请求受公共头文件上限
  约束。dirId 201 的“我喜欢”以 `disstid: 0`、`dirid: 201` 请求，普通歌单以数字
  playlist id 作为 `disstid`。歌单页按引用与 offset 缓存，预取相邻页不会挤掉当前
  正在显示的页面。歌单详情请求遇到瞬时超时或无效响应会重试一次；非空歌单返回
  空页面时按无效响应处理。
- 播放队列是来源歌单的逻辑视图：服务保存歌单引用、逻辑总数以及当前歌曲附近
  的有界物化窗口。播放接近窗口边界时预取服务端下一页；`next`、`prev`、自动
  续播和 `queue_play` 在需要时按需拉取缺失页。本地队列移除用位图记录，并在后续
  窗口中过滤；不会修改来源歌单。这样内存不随歌单总长度增长，同时可以跨服务端
  分页连续播放。
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

`Kconfig` 定义 API/登录/音频 worker 栈、命令队列深度与最大 JSON 响应；发布默认
为 API 12 KiB、登录 16 KiB，对应实测峰值约 8.2 KiB 与 10.7 KiB。产品通过
`music_service_config_t` 提供任务优先级与默认音质；发布产品默认请求标准音质
（128 kbps），以改善受限网络下的首次播放启动时间。

## 测试边界

QQ 登录协议解析、凭证语法校验、引用计数快照分配器、队列删除状态转移以及队列
窗口/分页/移除映射已有独立 host 测试。模拟器覆盖登录、会话过期、迟到页面清理，
以及待验证和保存失败两类 UI 状态。网络、TLS、NVS、真实服务端验证响应与播放
仍需真实账号或板端验证。Wi-Fi、I2S/DMA、ES8311 与电池行为不在 host 测试范围
内；模拟器真后端是集成辅助，不替代板端音频与功耗测量。
