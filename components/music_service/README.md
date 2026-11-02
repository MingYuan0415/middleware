**English** | [简体中文](README_ZH.md)

# music_service

QQ Music account, catalog, cover, lyric, and streaming playback service for
MicroTech. The service owns the QQ Music HTTPS client and the audio stream; the
music App consumes snapshots and never performs network, JSON, image decode, or
audio I/O itself.

## Public API

`include/music_service.h` provides:

- `music_service_init()` / `music_service_deinit()`,
  `music_service_set_network_ready()`, `music_service_set_time_ready()`, and
  `music_service_retry()`.
- QR login: `music_service_login_begin()`, `music_service_login_qr_acquire()`,
  `music_service_login_cancel()`, `music_service_logout()`, and
  `music_service_reset_persisted_state()`.
- Catalog: `music_service_refresh_home()`, `music_service_home_acquire()` /
  `release()`, `music_service_playlist_acquire()` / `release()`, and
  `music_service_lyric_acquire()` / `release()` with
  `music_service_lyric_failed()` / `music_service_lyric_retry()`. A pending
  lyric request is deduplicated per song; a completed empty result is reported
  until the caller retries or the song changes.
- Images: `music_service_image_acquire()` / `release()`.
- Playback: `music_service_play_playlist()`, `music_service_play_playlist_snapshot()`
  (uses a playlist snapshot already held by the caller), `play_pause()`, `next()`,
  `prev()`, `set_mode()`, `stop()`, and
  `music_service_get_player_status()`. `seek()` is reserved and currently
  returns `ESP_ERR_NOT_SUPPORTED` until range seeking is implemented.
- Queue: `music_service_queue_acquire()` / `release()` expose the bounded
  materialized window around the current playback position. The snapshot
  carries the source playlist reference, logical `total`, window `offset`,
  and window `count`; `music_service_queue_view_acquire()` materializes a
  paged view of the full logical queue. `music_service_queue_play()` and
  `music_service_queue_remove()` take absolute logical queue indices. The
  logical queue is capped by `MUSIC_SERVICE_MAX_QUEUE_TRACKS`, while a single
  materialized window stays bounded by `MUSIC_SERVICE_MAX_SONGS`.
- `music_service_is_playing()` is the PM guard query; it includes the interval
  between accepting a play command and the stream task actually starting.
- `music_service_get_diagnostics()` returns API/stream stack watermarks and API
  queue depth for acceptance and debugging.

Snapshots, images, and audio buffers live in PSRAM. All snapshot/image
`acquire()` calls are reference-counted borrows: the service keeps its own
reference, `release()` only drops the caller reference, and replacing a cache
entry never invalidates a page that still holds a reference. The service
runtime state, cookie jar, and both command queues also live in PSRAM. The API
and login worker stacks stay in internal RAM because credential save/erase
goes through NVS, which disables the flash cache; the login worker is separate
from the catalog/playback API worker and uses a larger stack for the HTTPS/OAuth
and QR image path. The audio worker is created lazily on the first playback and
runs from a PSRAM stack through the `audio_service` exclusive playback lease.
HTTP/cookie state is serialized by a dedicated mutex.

## Login and catalog

- WeChat QR login is implemented through the QQ Music web OAuth flow.
- QQ account QR login is implemented through `ptlogin2.qq.com`, the Graph
  OAuth authorization step, and `QQConnectLogin.LoginServer/QQLogin`.
- QR payloads may be JPEG or PNG; both are decoded to RGB565. QQ Music App
  MQTT login is not implemented in this version.
- Credentials are stored in the `music_cred` NVS blob and erasing them is part
  of the factory-reset sequence.
- Home data combines the anonymous recommendation feed with the logged-in
  user's created playlists, dirId 201 liked playlist, and collected playlists
  from `CgiGetPlaylistFavInfo`-compatible account catalog. Home is requested
  lazily when the App sidebar mounts, not on every network-ready transition.
  Playlist detail, lyric and queue requests are bounded by the public header
  limits. The dirId 201 liked bucket is requested with `disstid: 0` and
  `dirid: 201`; ordinary playlists send their numeric playlist id as
  `disstid`. Playlist pages are cached by reference and offset, so prefetching
  a neighbouring page does not evict the page currently displayed.
- The playback queue is a logical view of the source playlist: the service
  stores the playlist reference, logical total, and a bounded materialized
  window around the current track. When playback approaches a window edge it
  prefetches the next provider page; `next`, `prev`, auto-advance, and
  `queue_play` fetch a missing page on demand. Local queue removals are tracked
  as a bitmap and filtered from later windows; the source playlist is not
  modified. This keeps memory independent of playlist length while allowing
  playback to cross provider pages.
- Catalog parsers accept current (`mid`, `file.media_mid`, `dirinfo`) and legacy
  (`songmid`, `strMediaMid`, `dissname`) response names. Song URL requests use
  `CgiGetVkey` array parameters and `media_mid` filenames; lyrics use `crypt: 0`
  plain LRC. When the requested quality is unavailable, playback falls back to
  the next lower quality and reports the actual quality used.

## Audio

`music_stream.c` reads the CDN stream, uses `esp_audio_simple_dec` for
MP3/FLAC, maps the decoded sample rate/channel/bit depth to the BSP format
table, and writes PCM through `audio_service_write_leased()`. Partial PCM
writes are completed with bounded retries; a repeated write failure is reported
as a playback error instead of silently dropping audio. When the BSP has no
24-bit clock pair for the decoded rate, PCM is converted to 16-bit before the
write. Pause releases the exclusive audio lease and closes the codec so other
audio consumers can use the device; resume reconfigures the saved PCM format and
acquires the lease again. Audio decoding and network reads run on the dedicated
audio worker; no LVGL call is made from either worker.

## Configuration

`Kconfig` defines the API/login/audio worker stack sizes, command queue depth,
and maximum JSON response size. The shipped defaults are 12 KiB for the API
worker and 16 KiB for the login worker; they reflect measured peaks of about
8.2 KiB and 10.7 KiB. The product supplies task priorities and the default
quality through `music_service_config_t`.

## Test Boundary

The pure QQ login protocol parsers, the reference-counted snapshot allocator,
the queue removal transition table, and the queue window/page/removal mapping
helpers have standalone host tests. Network, TLS, credential storage, and
playback still require on-device validation; Wi-Fi, I2S/DMA, ES8311, and battery behavior are
outside host-test scope. The simulator real backend is an integration aid, not
a replacement for device audio and power measurements.
