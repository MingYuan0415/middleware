#ifndef __MUSIC_SERVICE_INTERNAL_H__
#define __MUSIC_SERVICE_INTERNAL_H__

#include "music_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Service runtime and cookie state are large enough to keep in PSRAM. */
#if defined(ESP_PLATFORM) && !defined(HOST_TEST)
    #include "esp_attr.h"
    #define MUSIC_SERVICE_EXT_BSS EXT_RAM_BSS_ATTR
#else
    #define MUSIC_SERVICE_EXT_BSS
#endif

#define MUSIC_CREDENTIAL_KEY "music_cred"
#define MUSIC_CREDENTIAL_MAGIC 0x4D555331U
#define MUSIC_CREDENTIAL_VERSION 1U

/* Transport/API errors that do not have a distinct ESP-IDF code. */
#define MUSIC_ERR_HTTP_AUTH      ((esp_err_t)0x7101)
#define MUSIC_ERR_HTTP_RATE_LIMIT ((esp_err_t)0x7102)
#define MUSIC_ERR_HTTP_API       ((esp_err_t)0x7103)

typedef struct music_credential_blob
{
    uint32_t magic;
    uint16_t version;
    uint16_t login_type;
    char musicid[24];
    char musickey[512];
    char refresh_key[256];
    char refresh_token[256];
    uint32_t expires_at;
} music_credential_blob_t;

typedef enum
{
    MUSIC_API_CMD_LOGOUT = 0,
    MUSIC_API_CMD_REFRESH_HOME,
    MUSIC_API_CMD_PLAYLIST,
    MUSIC_API_CMD_LYRIC,
    MUSIC_API_CMD_IMAGE,
    MUSIC_API_CMD_PLAY,
    MUSIC_API_CMD_PLAY_SNAPSHOT,
    MUSIC_API_CMD_PAUSE,
    MUSIC_API_CMD_QUEUE_PLAY,
    MUSIC_API_CMD_QUEUE_REMOVE,
    MUSIC_API_CMD_ERASE_CREDENTIAL,
} music_api_cmd_type_t;

/** @brief One serialized QR-login request handled outside the API worker. */
typedef struct music_login_command
{
    music_login_type_t type;
    uint32_t session;
} music_login_command_t;

/** @brief Result of one QR-poll iteration. */
typedef enum
{
    MUSIC_LOGIN_POLL_WAITING = 0, /**< QR code is still waiting for a scan. */
    MUSIC_LOGIN_POLL_SCANNED,     /**< QR code was scanned and awaits confirmation. */
    MUSIC_LOGIN_POLL_SUCCESS,     /**< QR login exchanged a credential successfully. */
    MUSIC_LOGIN_POLL_FAILED,      /**< QR code expired or login was rejected. */
} music_login_poll_state_t;

typedef struct music_api_command
{
    music_api_cmd_type_t type;
    music_login_type_t login_type;
    music_playlist_ref_t playlist;
    music_playlist_snapshot_t *playlist_snapshot;
    uint64_t queue_generation;
    uint16_t offset;
    uint16_t count;
    uint32_t token;
    char songmid[MUSIC_SERVICE_SONGMID_BYTES];
    char url[MUSIC_SERVICE_URL_BYTES];
    uint16_t image_width;
    uint16_t image_height;
} music_api_command_t;

typedef struct music_stream_status
{
    music_playback_state_t state;
    music_service_failure_t failure;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint32_t generation;
} music_stream_status_t;

typedef struct music_http_result
{
    int status;
    char *body;
    size_t length;
    char location[512];
    char content_type[64];
} music_http_result_t;

typedef struct music_service_runtime
{
    bool initialized;
    music_service_config_t config;
    SemaphoreHandle_t lock;
    QueueHandle_t api_queue;
    TaskHandle_t api_task;
    QueueHandle_t login_queue;
    TaskHandle_t login_task;
    music_service_status_t status;
    music_player_status_t player;
    music_home_snapshot_t *home;
    music_playlist_snapshot_t *playlist;
    music_queue_snapshot_t *queue;
    music_lyric_snapshot_t *lyric;
    music_image_t *login_qr;
    music_image_t *image;
    music_credential_blob_t credential;
    music_credential_blob_t login_credential;
    bool credential_valid;
    music_login_type_t login_type;
    char qr_uuid[96];
    char qr_qrsig[256];
    char qq_login_sig[160];
    char lyric_songmid[MUSIC_SERVICE_SONGMID_BYTES];
    uint32_t image_token;
    uint32_t play_generation;
    uint32_t login_session;
    bool play_valid;
    bool login_active;
    bool credential_erase_pending;
    uint64_t generation;
} music_service_runtime_t;

extern music_service_runtime_t s_music;

esp_err_t music_service_lock(void);
void music_service_unlock(void);
void music_service_publish(uint32_t sub_type, uint32_t token);
void music_service_publish_detail(uint32_t sub_type, uint32_t token,
                                  music_service_failure_t failure);
void music_service_set_state_locked(music_service_state_t state,
                                    music_service_failure_t failure);
void music_service_replace_home(music_home_snapshot_t *snapshot);
void music_service_replace_playlist(music_playlist_snapshot_t *snapshot);
void music_service_replace_queue(music_queue_snapshot_t *snapshot);
void music_service_replace_lyric(music_lyric_snapshot_t *snapshot);
void music_service_set_login_qr(music_image_t *image);
void music_service_set_image(music_image_t *image, uint32_t token);
void music_service_handle_network_result(esp_err_t result);
uint64_t music_service_next_generation(void);
void music_service_stream_finished(void);
void music_service_stream_failed(music_service_failure_t failure);

/* music_catalog.c / music_auth.c */
esp_err_t music_net_load_credential(music_credential_blob_t *credential);
esp_err_t music_net_save_credential(const music_credential_blob_t *credential);
esp_err_t music_net_erase_credential(void);
esp_err_t music_net_login_begin(music_login_type_t type);
esp_err_t music_net_login_poll(music_login_type_t type,
                               music_credential_blob_t *credential,
                               music_login_poll_state_t *state);
esp_err_t music_net_fetch_home(music_home_snapshot_t **snapshot);
esp_err_t music_net_fetch_playlist(const music_playlist_ref_t *ref,
                                   uint16_t offset, uint16_t count,
                                   music_playlist_snapshot_t **snapshot);
esp_err_t music_net_fetch_lyric(const char *songmid,
                                music_lyric_snapshot_t **snapshot);
esp_err_t music_net_fetch_image(const char *url, uint16_t width,
                                uint16_t height, uint32_t token,
                                music_image_t **image);
esp_err_t music_net_fetch_song_url(const music_song_t *song,
                                   music_quality_t quality,
                                   char *url, size_t url_size,
                                   music_quality_t *actual_quality);

esp_err_t music_stream_init(uint32_t priority);
esp_err_t music_stream_deinit(void);
esp_err_t music_stream_play(const char *url, uint32_t duration_ms,
                            uint32_t generation);
esp_err_t music_stream_pause(bool pause);
esp_err_t music_stream_stop(void);
bool music_stream_is_playing(void);
uint32_t music_stream_get_stack_high_water(void);
esp_err_t music_stream_get_status(music_stream_status_t *status);
void music_net_cancel_login(void);
void music_net_clear_session(void);

#endif /* __MUSIC_SERVICE_INTERNAL_H__ */
