/**
 * @brief QQ Music account, catalog, and streaming playback service.
 *
 * The service owns all QQ Music network access and the audio stream. It
 * publishes immutable snapshots for the music App; callers must not perform
 * HTTP, JSON, image decode, or audio I/O themselves.
 */

#ifndef __MUSIC_SERVICE_H__
#define __MUSIC_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_SERVICE_WAIT_FOREVER          UINT32_MAX
#define MUSIC_SERVICE_MAX_PLAYLISTS         12U
#define MUSIC_SERVICE_MAX_SONGS             100U
#define MUSIC_SERVICE_MAX_QUEUE_TRACKS      65535U
#define MUSIC_SERVICE_MAX_LYRIC_LINES       256U
#define MUSIC_SERVICE_TITLE_BYTES           128U
#define MUSIC_SERVICE_ARTIST_BYTES          128U
#define MUSIC_SERVICE_ALBUM_BYTES           96U
#define MUSIC_SERVICE_PLAYLIST_TITLE_BYTES  96U
#define MUSIC_SERVICE_CREATOR_BYTES         64U
#define MUSIC_SERVICE_USER_ID_BYTES         24U
#define MUSIC_SERVICE_URL_BYTES             512U
#define MUSIC_SERVICE_SONG_COVER_BYTES      160U
#define MUSIC_SERVICE_PLAYLIST_COVER_BYTES  256U
#define MUSIC_SERVICE_SONGMID_BYTES         32U
#define MUSIC_SERVICE_LYRIC_TEXT_BYTES      192U

EVENT_BUS_DECLARE_ID(MUSIC_SERVICE_MSG);

/** @brief Service lifecycle and account state. */
typedef enum
{
    MUSIC_SERVICE_STATE_UNINITIALIZED = 0,
    MUSIC_SERVICE_STATE_WAITING_NETWORK,
    MUSIC_SERVICE_STATE_WAITING_TIME,
    MUSIC_SERVICE_STATE_LOGGED_OUT,
    MUSIC_SERVICE_STATE_QR_READY,
    MUSIC_SERVICE_STATE_QR_SCANNED,
    MUSIC_SERVICE_STATE_QR_EXPIRED,
    MUSIC_SERVICE_STATE_AUTHENTICATING,
    MUSIC_SERVICE_STATE_READY,
    MUSIC_SERVICE_STATE_AUTH_REQUIRED,
    MUSIC_SERVICE_STATE_ERROR,
} music_service_state_t;

/** @brief Stable failure classification for UI copy. */
typedef enum
{
    MUSIC_SERVICE_FAILURE_NONE = 0,
    MUSIC_SERVICE_FAILURE_NETWORK,
    MUSIC_SERVICE_FAILURE_TIME,
    MUSIC_SERVICE_FAILURE_AUTH,
    MUSIC_SERVICE_FAILURE_RATE_LIMIT,
    MUSIC_SERVICE_FAILURE_API,
    MUSIC_SERVICE_FAILURE_PARSE,
    MUSIC_SERVICE_FAILURE_NO_PERMISSION,
    MUSIC_SERVICE_FAILURE_AUDIO_BUSY,
    MUSIC_SERVICE_FAILURE_DECODE,
    MUSIC_SERVICE_FAILURE_STORAGE,
    MUSIC_SERVICE_FAILURE_INTERNAL,
} music_service_failure_t;

/** @brief QR login method. */
typedef enum
{
    MUSIC_LOGIN_TYPE_WECHAT = 0,
    MUSIC_LOGIN_TYPE_QQ,
} music_login_type_t;

/** @brief Requested audio quality. */
typedef enum
{
    MUSIC_QUALITY_STANDARD = 0,
    MUSIC_QUALITY_HIGH,
    MUSIC_QUALITY_LOSSLESS,
} music_quality_t;

/** @brief Playback state exposed to the music App. */
typedef enum
{
    MUSIC_PLAYBACK_IDLE = 0,
    MUSIC_PLAYBACK_BUFFERING,
    MUSIC_PLAYBACK_PLAYING,
    MUSIC_PLAYBACK_PAUSED,
    MUSIC_PLAYBACK_ERROR,
} music_playback_state_t;

/** @brief Queue play mode. */
typedef enum
{
    MUSIC_PLAY_MODE_SEQUENTIAL = 0,
    MUSIC_PLAY_MODE_REPEAT_ALL,
    MUSIC_PLAY_MODE_REPEAT_ONE,
    MUSIC_PLAY_MODE_SHUFFLE,
} music_play_mode_t;

/** @brief Playlist provenance used by the UI and API dispatcher. */
typedef enum
{
    MUSIC_PLAYLIST_KIND_MY_CREATED = 0,
    MUSIC_PLAYLIST_KIND_MY_FAVORITE, /**< dirId 201 "liked songs" playlist. */
    MUSIC_PLAYLIST_KIND_RECOMMENDED,
    MUSIC_PLAYLIST_KIND_MY_COLLECTED, /**< Collected playlist from the account. */
} music_playlist_kind_t;

/** @brief Stable identity of one playlist. */
typedef struct music_playlist_ref
{
    uint64_t id;       /**< Playlist tid/dissid used by CgiGetDiss. */
    uint32_t dirid;    /**< Directory ID for self-created playlists. */
    uint32_t tid;      /**< Playlist tid when the response carries it. */
    uint8_t kind;      /**< music_playlist_kind_t. */
} music_playlist_ref_t;

/** @brief Bounded playlist metadata copied into snapshots. */
typedef struct music_playlist
{
    music_playlist_ref_t ref;
    char title[MUSIC_SERVICE_PLAYLIST_TITLE_BYTES];
    char creator[MUSIC_SERVICE_CREATOR_BYTES];
    char cover_url[MUSIC_SERVICE_PLAYLIST_COVER_BYTES];
    uint32_t track_count;
} music_playlist_t;

/** @brief Bounded song metadata copied into snapshots and the queue. */
typedef struct music_song
{
    uint64_t songid;
    char songmid[MUSIC_SERVICE_SONGMID_BYTES];
    char media_mid[MUSIC_SERVICE_SONGMID_BYTES];
    char title[MUSIC_SERVICE_TITLE_BYTES];
    char artist[MUSIC_SERVICE_ARTIST_BYTES];
    char album[MUSIC_SERVICE_ALBUM_BYTES];
    char cover_url[MUSIC_SERVICE_SONG_COVER_BYTES];
    uint32_t duration_ms;
    uint32_t size_128;
    uint32_t size_320;
    uint32_t size_flac;
    uint8_t pay_flags;
} music_song_t;

/** @brief Immutable home-page snapshot. */
typedef struct music_home_snapshot
{
    uint64_t generation;
    size_t my_created_count;
    size_t my_favorite_count; /**< dirId 201 "liked songs" playlist. */
    size_t recommended_count;
    size_t my_collected_count; /**< Collected playlists from the account. */
    music_playlist_t my_created[MUSIC_SERVICE_MAX_PLAYLISTS];
    music_playlist_t my_favorite[MUSIC_SERVICE_MAX_PLAYLISTS];
    music_playlist_t recommended[MUSIC_SERVICE_MAX_PLAYLISTS];
    music_playlist_t my_collected[MUSIC_SERVICE_MAX_PLAYLISTS];
    bool login_required;
    bool recommended_ok;
    bool personal_ok;
    bool collected_ok;
} music_home_snapshot_t;

/** @brief Immutable playlist-detail snapshot. */
typedef struct music_playlist_snapshot
{
    uint64_t generation;
    music_playlist_t playlist;
    uint32_t total;
    uint16_t offset;
    uint16_t count;
    music_song_t songs[]; /**< count entries; allocation is exact. */
} music_playlist_snapshot_t;

/** @brief Immutable playback-queue window or queue-page view. */
typedef struct music_queue_snapshot
{
    uint64_t generation;
    music_playlist_ref_t ref; /**< Playlist that produced this queue. */
    uint16_t total;           /**< Logical queue length. */
    uint16_t offset;          /**< Global queue index of songs[0]. */
    uint16_t count;           /**< Entries materialized in this snapshot. */
    music_song_t songs[];     /**< count entries; allocation is exact. */
} music_queue_snapshot_t;

/** @brief One parsed LRC line. */
typedef struct music_lyric_line
{
    uint32_t offset_ms;
    char text[MUSIC_SERVICE_LYRIC_TEXT_BYTES];
} music_lyric_line_t;

/** @brief Immutable parsed-lyrics snapshot. */
typedef struct music_lyric_snapshot
{
    uint64_t generation;
    size_t line_count;
    music_lyric_line_t lines[]; /**< line_count entries; allocation is exact. */
} music_lyric_snapshot_t;

/** @brief Decoded RGB565 image owned by the service. */
typedef struct music_image
{
    uint16_t width;
    uint16_t height;
    uint16_t requested_width;
    uint16_t requested_height;
    size_t data_size;
    uint8_t *pixels;
    uint32_t token;
    char url[MUSIC_SERVICE_URL_BYTES];
} music_image_t;

/** @brief Small player status copied under the service lock. */
typedef struct music_player_status
{
    uint64_t generation;
    music_playback_state_t state;
    music_service_failure_t failure;
    music_play_mode_t mode;
    music_quality_t quality;
    uint16_t queue_index; /**< Absolute logical queue index, zero-based. */
    uint16_t queue_count; /**< Logical queue length. */
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t buffered_ms;
    uint32_t play_generation;
    char title[MUSIC_SERVICE_TITLE_BYTES];
    char artist[MUSIC_SERVICE_ARTIST_BYTES];
    char cover_url[MUSIC_SERVICE_URL_BYTES];
    char songmid[MUSIC_SERVICE_SONGMID_BYTES];
} music_player_status_t;

/** @brief Small account/service status copied under the service lock. */
typedef struct music_service_status
{
    uint64_t generation;
    music_service_state_t state;
    music_service_failure_t failure;
    bool network_ready;
    bool time_ready;
    bool logged_in;
    bool audio_available;
    bool qr_available;
    music_login_type_t login_type;
    char user_id[MUSIC_SERVICE_USER_ID_BYTES]; /**< Login account id. */
    char user_name[MUSIC_SERVICE_CREATOR_BYTES]; /**< Profile nickname. */
    char user_avatar[MUSIC_SERVICE_URL_BYTES]; /**< Profile avatar URL. */
    char qr_status[64];
} music_service_status_t;

/** @brief Lightweight service diagnostics for debugging and acceptance. */
typedef struct music_service_diagnostics
{
    uint32_t api_stack_high_water;
    uint32_t stream_stack_high_water;
    uint32_t api_queue_waiting;
    bool stream_initialized;
} music_service_diagnostics_t;

/** @brief Product-owned music service policy. */
typedef struct music_service_config
{
    uint32_t api_task_priority;
    uint32_t audio_task_priority;
    music_quality_t default_quality;
} music_service_config_t;

/** @brief Coalescible UI event; the full payload is acquired separately. */
typedef struct music_service_event
{
    uint64_t generation;
    uint32_t sub_type;
    uint32_t token;
    music_service_state_t state;
    music_service_failure_t failure;
} music_service_event_t;

/** @brief Event subtypes published by the service. */
typedef enum
{
    MUSIC_SERVICE_MSG_SUB_TYPE_STATUS = 1,
    MUSIC_SERVICE_MSG_SUB_TYPE_HOME,
    MUSIC_SERVICE_MSG_SUB_TYPE_PLAYLIST,
    MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC,
    MUSIC_SERVICE_MSG_SUB_TYPE_IMAGE,
    MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER,
    MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE,
} music_service_msg_sub_type_t;

/**
 * @brief Initialize the service and its worker tasks.
 *
 * Must be called after nv_storage, audio_service, event_bus, and chore_service
 * are initialized. A missing audio path disables playback but not catalog
 * browsing.
 */
esp_err_t music_service_init(const music_service_config_t *config);

/** @brief Stop workers and release all service-owned resources. */
esp_err_t music_service_deinit(void);

/** @brief Inform the service of IPv4 availability. */
esp_err_t music_service_set_network_ready(bool ready);

/** @brief Inform the service that the system clock is trustworthy. */
esp_err_t music_service_set_time_ready(bool ready);

/** @brief Clear a transient service error and re-evaluate account state. */
esp_err_t music_service_retry(void);

/** @brief Start a QR login flow. */
esp_err_t music_service_login_begin(music_login_type_t type);

/** @brief Acquire/release the current login QR image. */
esp_err_t music_service_login_qr_acquire(const music_image_t **image);
void music_service_login_qr_release(const music_image_t *image);

/** @brief Cancel a pending QR login flow. */
esp_err_t music_service_login_cancel(void);

/** @brief Forget the account and erase the persisted credential. */
esp_err_t music_service_logout(void);

/**
 * @brief Erase the persisted credential without requiring service init.
 *
 * Called by the factory-reset sequence after nv_storage_init().
 */
esp_err_t music_service_reset_persisted_state(void);

/**
 * @brief Request a home or missing-profile refresh.
 *
 * Cached data remains visible until replaced.
 */
esp_err_t music_service_refresh_home(bool force);

/** @brief Acquire/release the latest home snapshot. */
esp_err_t music_service_home_acquire(const music_home_snapshot_t **snapshot);
void music_service_home_release(const music_home_snapshot_t *snapshot);

/** @brief Load one playlist page and acquire its snapshot. */
esp_err_t music_service_playlist_acquire(const music_playlist_ref_t *ref,
        uint16_t offset, uint16_t count,
        const music_playlist_snapshot_t **snapshot);
void music_service_playlist_release(const music_playlist_snapshot_t *snapshot);

/**
 * @brief Acquire the materialized window around the current playback position.
 *
 * The queue is a logical playlist view backed by a bounded song window. The
 * returned snapshot covers at most MUSIC_SERVICE_MAX_SONGS entries, while
 * `total` remains the full logical queue length. A missing queue returns
 * ESP_ERR_NOT_FOUND.
 */
esp_err_t music_service_queue_acquire(const music_queue_snapshot_t **snapshot);

/** @brief Release a queue snapshot acquired from the service. */
void music_service_queue_release(const music_queue_snapshot_t *snapshot);

/**
 * @brief Acquire a paged view of the logical playback queue.
 *
 * A cached view at the same offset returns ESP_OK. Otherwise the request is
 * queued and MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE announces its result. A missing
 * queue returns ESP_ERR_INVALID_STATE.
 *
 * @param offset is the logical queue index of the first requested entry.
 * @param count is the maximum number of entries; must not exceed
 *              MUSIC_SERVICE_MAX_SONGS.
 * @param snapshot receives the retained view on success.
 */
esp_err_t music_service_queue_view_acquire(uint16_t offset, uint16_t count,
        const music_queue_snapshot_t **snapshot);

/** @brief Switch playback to one absolute index of the logical queue. */
esp_err_t music_service_queue_play(uint16_t index);

/**
 * @brief Remove one entry from the logical queue.
 *
 * `index` is an absolute logical queue index. Removing the current entry
 * advances to the same slot after the shift and starts the replacement when
 * playback is active. Removing the only entry stops playback. The change is
 * asynchronous; MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE marks the committed result.
 */
esp_err_t music_service_queue_remove(uint16_t index);

/** @brief Load and acquire parsed lyrics for one song. */
esp_err_t music_service_lyric_acquire(const char *songmid,
                                      const music_lyric_snapshot_t **snapshot);
void music_service_lyric_release(const music_lyric_snapshot_t *snapshot);

/**
 * @brief Whether the latest lyric request for one song completed without data.
 *
 * Returns false while the request is still pending or when the cached result
 * belongs to another song.
 */
bool music_service_lyric_failed(const char *songmid);

/** @brief Clear a completed lyric failure so a later acquire retries it. */
void music_service_lyric_retry(const char *songmid);

/** @brief Download and decode one cover into RGB565 PSRAM. */
esp_err_t music_service_image_acquire(const char *url, uint16_t width,
                                      uint16_t height, uint32_t token,
                                      const music_image_t **image);
void music_service_image_release(const music_image_t *image);

/** @brief Start a playlist queue at the absolute requested playlist index. */
esp_err_t music_service_play_playlist(const music_playlist_ref_t *ref,
                                      uint16_t start_index);

/**
 * @brief Start a queue from a playlist snapshot already held by the caller.
 *
 * The service retains the snapshot until the request is consumed, so the
 * caller may release it immediately after this call returns. `start_index` is
 * relative to the snapshot: the absolute playlist index is
 * `playlist->offset + start_index`.
 */
esp_err_t music_service_play_playlist_snapshot(
    const music_playlist_snapshot_t *playlist, uint16_t start_index);

/** @brief Toggle pause/resume of the current queue. */
esp_err_t music_service_play_pause(void);

/** @brief Skip to the next/previous queue entry. */
esp_err_t music_service_next(void);
esp_err_t music_service_prev(void);

/** @brief Seek within the current track in milliseconds.
 *
 * Reserved for range-seek support; the current implementation returns
 * ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t music_service_seek(uint32_t position_ms);

/** @brief Set the queue play mode. */
esp_err_t music_service_set_mode(music_play_mode_t mode);

/** @brief Stop playback and release the audio lease. */
esp_err_t music_service_stop(void);

/** @brief Copy the current account/service status. */
esp_err_t music_service_get_status(music_service_status_t *status);

/** @brief Copy the current player status. */
esp_err_t music_service_get_player_status(music_player_status_t *status);

/** @brief Copy stack/queue diagnostics for the music workers. */
esp_err_t music_service_get_diagnostics(music_service_diagnostics_t *diagnostics);

/** @brief Lock-free playback query used by the system PM guard. */
bool music_service_is_playing(void);

#ifdef __cplusplus
}
#endif

#endif /* __MUSIC_SERVICE_H__ */
