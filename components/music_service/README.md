**English** | [简体中文](README_ZH.md)

# music_service

QQ Music account, catalog, cover, lyric, and streaming playback service for
MicroTech. The service owns the QQ Music HTTPS client and the audio stream; the
music App consumes snapshots and never performs network, JSON, image decode, or
audio I/O itself.

## Public API

`include/music_service.h` provides:

- `music_service_init()` / `music_service_deinit()` and
  `music_service_set_network_ready()`.
- QR login: `music_service_login_begin()`, `music_service_login_qr_acquire()`,
  `music_service_login_cancel()`, `music_service_logout()`, and
  `music_service_reset_persisted_state()`.
- Catalog: `music_service_refresh_home()`, `music_service_home_acquire()` /
  `release()`, `music_service_playlist_acquire()` / `release()`, and
  `music_service_lyric_acquire()` / `release()`.
- Images: `music_service_image_acquire()` / `release()`.
- Playback: `music_service_play_playlist()`, `play_pause()`, `next()`,
  `prev()`, `set_mode()`, `stop()`, and
  `music_service_get_player_status()`. `seek()` is reserved and currently
  returns `ESP_ERR_NOT_SUPPORTED` until range seeking is implemented.
- `music_service_is_playing()` is the lock-free PM guard query.

All large snapshots and audio buffers are allocated in PSRAM. The API worker
uses an internal-RAM stack; the audio worker receives PCM through the
`audio_service` exclusive playback lease.

## Login and catalog

- WeChat QR login is implemented through the QQ Music web OAuth flow.
- QQ QR login and QQ Music App MQTT login are not implemented in this version.
- Credentials are stored in the `music_cred` NVS blob and erasing them is part
  of the factory-reset sequence.
- Home data combines the anonymous recommendation feed with the logged-in
  user's created and favorite playlists. Playlist detail and lyric requests are
  bounded by the public header limits.

## Audio

`music_stream.c` reads the CDN stream, uses `esp_audio_simple_dec` for
MP3/AAC/FLAC, maps the decoded sample rate/channel/bit depth to the BSP format
table, and writes PCM through `audio_service_write_leased()`. Audio decoding
and network reads run on the dedicated audio worker; no LVGL call is made from
either worker.

## Configuration

`Kconfig` defines the API/audio worker stack sizes, command queue depth, and
maximum JSON response size. The product supplies task priorities and the default
quality through `music_service_config_t`.

## Test Boundary

The service currently has no standalone host suite; protocol parsing,
credential handling, and playback require on-device validation. TLS, Wi-Fi,
I2S/DMA, ES8311, and battery behavior are outside host-test scope.
