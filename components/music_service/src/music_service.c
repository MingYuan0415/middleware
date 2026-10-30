#define DBG_TAG "music_service"
#include "mt_log.h"

#include "music_service_internal.h"

#include "audio_service.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_random.h"
#include "event_bus.h"
#include "nv_storage.h"
#include "time_service.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

EVENT_BUS_DEFINE_ID(MUSIC_SERVICE_MSG);

#define MUSIC_SERVICE_PLAYLIST_PAGE_SIZE 30U

music_service_runtime_t s_music;

static atomic_bool s_playing;
static atomic_bool s_login_cancel;
static atomic_bool s_api_stop;
static atomic_bool s_api_exited;

static void _psram_free(void *pointer)
{
    heap_caps_free(pointer);
}

esp_err_t music_service_lock(void)
{
    if (!s_music.initialized || s_music.lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_music.lock, portMAX_DELAY) == pdTRUE ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

void music_service_unlock(void)
{
    if (s_music.lock != NULL)
    {
        (void)xSemaphoreGive(s_music.lock);
    }
}

void music_service_publish(uint32_t sub_type, uint32_t token)
{
    if (!s_music.initialized)
    {
        return;
    }
    music_service_event_t event = {0};
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    event.generation = s_music.generation;
    event.sub_type = sub_type;
    event.token = token;
    event.state = s_music.status.state;
    event.failure = s_music.status.failure;
    music_service_unlock();
    (void)event_bus_publish(MUSIC_SERVICE_MSG, sub_type, &event,
                            sizeof(event), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

void music_service_set_state_locked(music_service_state_t state,
                                    music_service_failure_t failure)
{
    s_music.status.state = state;
    s_music.status.failure = failure;
    s_music.player.failure = failure;
    s_music.status.generation = ++s_music.generation;
    s_music.player.generation = s_music.status.generation;
}

uint64_t music_service_next_generation(void)
{
    if (music_service_lock() != ESP_OK)
    {
        return 0U;
    }
    const uint64_t generation = ++s_music.generation;
    music_service_unlock();
    return generation;
}

void music_service_handle_network_result(esp_err_t result)
{
    music_service_failure_t failure = MUSIC_SERVICE_FAILURE_NONE;
    if (result == ESP_ERR_TIMEOUT)
    {
        failure = MUSIC_SERVICE_FAILURE_NETWORK;
    }
    else if (result == ESP_ERR_INVALID_RESPONSE)
    {
        failure = MUSIC_SERVICE_FAILURE_PARSE;
    }
    else if (result == ESP_ERR_NO_MEM)
    {
        failure = MUSIC_SERVICE_FAILURE_INTERNAL;
    }
    else if (result == ESP_ERR_NOT_FOUND)
    {
        failure = MUSIC_SERVICE_FAILURE_NO_PERMISSION;
    }
    else if (result != ESP_OK)
    {
        failure = MUSIC_SERVICE_FAILURE_API;
    }
    if (music_service_lock() == ESP_OK)
    {
        s_music.status.failure = failure;
        s_music.player.failure = failure;
        s_music.status.generation = ++s_music.generation;
        s_music.player.generation = s_music.status.generation;
        music_service_unlock();
    }
}

void music_service_replace_home(music_home_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }
    if (music_service_lock() != ESP_OK)
    {
        _psram_free(snapshot);
        return;
    }
    music_home_snapshot_t *old = s_music.home;
    s_music.home = snapshot;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.generation = ++s_music.generation;
    s_music.player.generation = s_music.status.generation;
    music_service_unlock();
    _psram_free(old);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
}

void music_service_replace_playlist(music_playlist_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }
    if (music_service_lock() != ESP_OK)
    {
        _psram_free(snapshot);
        return;
    }
    music_playlist_snapshot_t *old = s_music.playlist;
    s_music.playlist = snapshot;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.generation = ++s_music.generation;
    s_music.player.generation = s_music.status.generation;
    music_service_unlock();
    _psram_free(old);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYLIST, 0U);
}

void music_service_replace_lyric(music_lyric_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }
    if (music_service_lock() != ESP_OK)
    {
        _psram_free(snapshot);
        return;
    }
    music_lyric_snapshot_t *old = s_music.lyric;
    s_music.lyric = snapshot;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.generation = ++s_music.generation;
    s_music.player.generation = s_music.status.generation;
    music_service_unlock();
    _psram_free(old);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U);
}

void music_service_set_login_qr(music_image_t *image)
{
    if (image == NULL)
    {
        return;
    }
    if (atomic_load_explicit(&s_login_cancel, memory_order_acquire))
    {
        music_service_login_qr_release(image);
        return;
    }
    if (music_service_lock() != ESP_OK)
    {
        music_service_login_qr_release(image);
        return;
    }
    if (!s_music.login_active ||
            atomic_load_explicit(&s_login_cancel, memory_order_acquire))
    {
        music_service_unlock();
        music_service_login_qr_release(image);
        return;
    }
    music_image_t *old = s_music.login_qr;
    s_music.login_qr = image;
    s_music.status.state = MUSIC_SERVICE_STATE_QR_READY;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.qr_available = true;
    s_music.status.login_type = MUSIC_LOGIN_TYPE_WECHAT;
    (void)snprintf(s_music.status.qr_status,
                   sizeof(s_music.status.qr_status), "请使用微信扫码");
    s_music.status.generation = ++s_music.generation;
    music_service_unlock();
    music_service_login_qr_release(old);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
}

void music_service_set_image(music_image_t *image, uint32_t token)
{
    if (image == NULL)
    {
        return;
    }
    image->token = token;
    if (music_service_lock() != ESP_OK)
    {
        music_service_image_release(image);
        return;
    }
    music_image_t *old = s_music.image;
    s_music.image = image;
    s_music.image_token = token;
    s_music.status.generation = ++s_music.generation;
    music_service_unlock();
    _psram_free(old);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_IMAGE, token);
}

static void _music_service_recheck_time(void)
{
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    const bool time_ready = time_service_get_quality() !=
                            TIME_SERVICE_QUALITY_INVALID;
    bool changed = false;
    if (s_music.status.time_ready != time_ready)
    {
        s_music.status.time_ready = time_ready;
        changed = true;
    }
    if (s_music.status.state == MUSIC_SERVICE_STATE_WAITING_TIME &&
            s_music.status.network_ready && time_ready)
    {
        music_service_set_state_locked(s_music.credential_valid ?
                                       MUSIC_SERVICE_STATE_READY :
                                       MUSIC_SERVICE_STATE_LOGGED_OUT,
                                       MUSIC_SERVICE_FAILURE_NONE);
        changed = true;
    }
    music_service_unlock();
    if (!changed)
    {
        return;
    }
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    if (time_ready)
    {
        const music_api_command_t refresh =
        {
            .type = MUSIC_API_CMD_REFRESH_HOME,
        };
        (void)xQueueSend(s_music.api_queue, &refresh, pdMS_TO_TICKS(100U));
    }
}

static void _music_service_finish_login(bool success)
{
    music_service_lock();
    s_music.login_active = false;
    if (!success)
    {
        s_music.status.logged_in = s_music.credential_valid;
    }
    music_service_unlock();
}

static void _music_service_player_error(music_service_failure_t failure)
{
    (void)music_stream_stop();
    atomic_store_explicit(&s_playing, false, memory_order_release);
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    s_music.player.state = MUSIC_PLAYBACK_ERROR;
    s_music.player.failure = failure;
    s_music.play_valid = false;
    music_service_unlock();
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
}

static void _api_task(void *argument)
{
    (void)argument;
    for (;;)
    {
        if (atomic_load_explicit(&s_api_stop, memory_order_acquire))
        {
            break;
        }
        music_api_command_t command;
        if (xQueueReceive(s_music.api_queue, &command,
                          pdMS_TO_TICKS(1000U)) != pdTRUE)
        {
            _music_service_recheck_time();
            continue;
        }
        switch (command.type)
        {
        case MUSIC_API_CMD_LOGIN_BEGIN:
        {
            if (atomic_load_explicit(&s_login_cancel, memory_order_acquire))
            {
                _music_service_finish_login(false);
                break;
            }
            music_service_lock();
            s_music.login_active = true;
            s_music.status.login_type = command.login_type;
            s_music.status.qr_available = false;
            music_service_set_state_locked(MUSIC_SERVICE_STATE_QR_READY,
                                           MUSIC_SERVICE_FAILURE_NONE);
            (void)snprintf(s_music.status.qr_status,
                           sizeof(s_music.status.qr_status),
                           "正在获取二维码");
            music_service_unlock();
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
            esp_err_t result = music_net_login_begin(command.login_type);
            if (result != ESP_OK)
            {
                music_service_handle_network_result(result);
                music_service_lock();
                const music_service_failure_t failure = s_music.status.failure;
                music_service_set_state_locked(MUSIC_SERVICE_STATE_ERROR,
                                               failure);
                music_service_unlock();
                _music_service_finish_login(false);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            music_credential_blob_t credential;
            memset(&credential, 0, sizeof(credential));
            bool terminal_ok = false;
            while (!atomic_load_explicit(&s_login_cancel,
                                         memory_order_acquire) &&
                    !atomic_load_explicit(&s_api_stop,
                                          memory_order_acquire))
            {
                vTaskDelay(pdMS_TO_TICKS(1500U));
                music_login_poll_state_t poll_state = MUSIC_LOGIN_POLL_WAITING;
                const esp_err_t poll_result = music_net_login_poll(
                                                  &credential, &poll_state);
                if (poll_result != ESP_OK)
                {
                    music_service_handle_network_result(poll_result);
                    music_service_lock();
                    const music_service_failure_t failure =
                        s_music.status.failure;
                    music_service_set_state_locked(MUSIC_SERVICE_STATE_ERROR,
                                                   failure);
                    (void)snprintf(s_music.status.qr_status,
                                   sizeof(s_music.status.qr_status),
                                   "登录未完成");
                    music_service_unlock();
                    music_service_publish(
                        MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                    break;
                }
                if (poll_state == MUSIC_LOGIN_POLL_WAITING)
                {
                    continue;
                }
                if (poll_state == MUSIC_LOGIN_POLL_SCANNED)
                {
                    music_service_lock();
                    music_service_set_state_locked(
                        MUSIC_SERVICE_STATE_QR_SCANNED,
                        MUSIC_SERVICE_FAILURE_NONE);
                    (void)snprintf(s_music.status.qr_status,
                                   sizeof(s_music.status.qr_status),
                                   "已扫码，请在手机上确认");
                    music_service_unlock();
                    music_service_publish(
                        MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                    continue;
                }
                if (poll_state == MUSIC_LOGIN_POLL_FAILED)
                {
                    music_service_lock();
                    music_service_set_state_locked(
                        MUSIC_SERVICE_STATE_AUTH_REQUIRED,
                        MUSIC_SERVICE_FAILURE_AUTH);
                    (void)snprintf(s_music.status.qr_status,
                                   sizeof(s_music.status.qr_status),
                                   "二维码已过期，请重新登录");
                    music_service_unlock();
                    music_service_publish(
                        MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                    break;
                }
                if (atomic_load_explicit(&s_login_cancel,
                                         memory_order_acquire))
                {
                    break;
                }
                const esp_err_t save_result =
                    music_net_save_credential(&credential);
                if (save_result != ESP_OK)
                {
                    music_service_handle_network_result(save_result);
                    music_service_lock();
                    const music_service_failure_t failure =
                        MUSIC_SERVICE_FAILURE_STORAGE;
                    s_music.status.failure = failure;
                    s_music.player.failure = failure;
                    music_service_set_state_locked(
                        MUSIC_SERVICE_STATE_AUTH_REQUIRED, failure);
                    (void)snprintf(s_music.status.qr_status,
                                   sizeof(s_music.status.qr_status),
                                   "登录态保存失败");
                    music_service_unlock();
                    music_service_publish(
                        MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                    break;
                }
                music_service_lock();
                s_music.credential = credential;
                s_music.credential_valid = true;
                s_music.status.logged_in = true;
                s_music.status.login_type = MUSIC_LOGIN_TYPE_WECHAT;
                s_music.status.qr_available = false;
                s_music.status.user_name[0] = '\0';
                s_music.status.user_avatar[0] = '\0';
                music_service_set_state_locked(
                    MUSIC_SERVICE_STATE_READY,
                    MUSIC_SERVICE_FAILURE_NONE);
                (void)snprintf(s_music.status.qr_status,
                               sizeof(s_music.status.qr_status),
                               "登录成功");
                terminal_ok = true;
                music_service_unlock();
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                const music_api_command_t refresh =
                {
                    .type = MUSIC_API_CMD_REFRESH_HOME,
                };
                (void)xQueueSend(s_music.api_queue, &refresh,
                                 pdMS_TO_TICKS(100U));
                break;
            }
            music_net_cancel_login();
            _music_service_finish_login(terminal_ok);
            break;
        }
        case MUSIC_API_CMD_LOGOUT:
        {
            (void)music_stream_stop();
            atomic_store_explicit(&s_playing, false, memory_order_release);
            (void)music_net_erase_credential();
            music_home_snapshot_t *old_home = NULL;
            music_playlist_snapshot_t *old_playlist = NULL;
            music_lyric_snapshot_t *old_lyric = NULL;
            music_service_lock();
            memset(&s_music.credential, 0, sizeof(s_music.credential));
            s_music.credential_valid = false;
            s_music.login_active = false;
            s_music.status.logged_in = false;
            s_music.status.user_name[0] = '\0';
            s_music.status.user_avatar[0] = '\0';
            s_music.status.qr_status[0] = '\0';
            s_music.status.qr_available = false;
            s_music.status.login_type = MUSIC_LOGIN_TYPE_WECHAT;
            old_home = s_music.home;
            old_playlist = s_music.playlist;
            old_lyric = s_music.lyric;
            s_music.home = NULL;
            s_music.playlist = NULL;
            s_music.lyric = NULL;
            s_music.play_valid = false;
            s_music.player.state = MUSIC_PLAYBACK_IDLE;
            s_music.player.position_ms = 0U;
            s_music.player.duration_ms = 0U;
            s_music.player.title[0] = '\0';
            s_music.player.artist[0] = '\0';
            s_music.player.cover_url[0] = '\0';
            s_music.player.songmid[0] = '\0';
            music_service_set_state_locked(MUSIC_SERVICE_STATE_LOGGED_OUT,
                                           MUSIC_SERVICE_FAILURE_NONE);
            music_service_unlock();
            _psram_free(old_home);
            _psram_free(old_playlist);
            _psram_free(old_lyric);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
            break;
        }
        case MUSIC_API_CMD_REFRESH_HOME:
        {
            music_home_snapshot_t *snapshot = NULL;
            const esp_err_t result = music_net_fetch_home(&snapshot);
            if (result == ESP_OK)
            {
                music_service_replace_home(snapshot);
            }
            else
            {
                music_service_handle_network_result(result);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
            }
            break;
        }
        case MUSIC_API_CMD_PLAYLIST:
        {
            music_playlist_snapshot_t *snapshot = NULL;
            const esp_err_t result = music_net_fetch_playlist(
                                         &command.playlist, command.offset,
                                         command.count, &snapshot);
            if (result == ESP_OK)
            {
                music_service_replace_playlist(snapshot);
            }
            else
            {
                music_service_handle_network_result(result);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
            }
            break;
        }
        case MUSIC_API_CMD_LYRIC:
        {
            music_lyric_snapshot_t *snapshot = NULL;
            const esp_err_t result = music_net_fetch_lyric(
                                         command.songmid, &snapshot);
            if (result == ESP_OK)
            {
                music_service_lock();
                (void)snprintf(s_music.lyric_songmid,
                               sizeof(s_music.lyric_songmid), "%s",
                               command.songmid);
                music_service_unlock();
                music_service_replace_lyric(snapshot);
            }
            else
            {
                music_service_handle_network_result(result);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
            }
            break;
        }
        case MUSIC_API_CMD_IMAGE:
        {
            music_image_t *image = NULL;
            const esp_err_t result = music_net_fetch_image(
                                         command.url, command.image_width,
                                         command.image_height, command.token,
                                         &image);
            if (result == ESP_OK)
            {
                music_service_set_image(image, command.token);
            }
            else
            {
                music_service_handle_network_result(result);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_IMAGE,
                                      command.token);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
            }
            break;
        }
        case MUSIC_API_CMD_PLAY:
        {
            const uint32_t requested_index = command.offset;
            const uint32_t page_offset =
                (requested_index / MUSIC_SERVICE_PLAYLIST_PAGE_SIZE) *
                MUSIC_SERVICE_PLAYLIST_PAGE_SIZE;
            music_playlist_snapshot_t *playlist = NULL;
            const esp_err_t fetch_result = music_net_fetch_playlist(
                                               &command.playlist, (uint16_t)page_offset,
                                               MUSIC_SERVICE_PLAYLIST_PAGE_SIZE,
                                               &playlist);
            if (fetch_result != ESP_OK || playlist == NULL ||
                    playlist->count == 0U)
            {
                music_service_handle_network_result(
                    fetch_result == ESP_OK ? ESP_ERR_NOT_FOUND :
                    fetch_result);
                _psram_free(playlist);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                _music_service_player_error(s_music.status.failure);
                break;
            }
            const uint32_t local_index = requested_index - page_offset;
            if (local_index >= playlist->count)
            {
                _psram_free(playlist);
                music_service_handle_network_result(ESP_ERR_INVALID_ARG);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                break;
            }
            const music_song_t *song = &playlist->songs[local_index];
            char url[MUSIC_SERVICE_URL_BYTES];
            const esp_err_t url_result = music_net_fetch_song_url(
                                             song, s_music.config.default_quality,
                                             url, sizeof(url));
            if (url_result != ESP_OK)
            {
                music_service_handle_network_result(url_result);
                _psram_free(playlist);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                _music_service_player_error(s_music.status.failure);
                break;
            }
            const uint32_t total = playlist->total != 0U ?
                                   playlist->total :
                                   page_offset + playlist->count;
            music_service_lock();
            s_music.player.state = MUSIC_PLAYBACK_BUFFERING;
            s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
            (void)snprintf(s_music.player.title,
                           sizeof(s_music.player.title), "%s",
                           song->title);
            (void)snprintf(s_music.player.artist,
                           sizeof(s_music.player.artist), "%s",
                           song->artist);
            (void)snprintf(s_music.player.cover_url,
                           sizeof(s_music.player.cover_url), "%s",
                           song->cover_url);
            (void)snprintf(s_music.player.songmid,
                           sizeof(s_music.player.songmid), "%s",
                           song->songmid);
            s_music.player.duration_ms = song->duration_ms;
            s_music.player.queue_index = (uint16_t)(requested_index > UINT16_MAX ?
                                                    UINT16_MAX : requested_index);
            s_music.player.queue_count = (uint16_t)(total > UINT16_MAX ?
                                                    UINT16_MAX : total);
            s_music.play_ref = command.playlist;
            s_music.play_index = requested_index;
            s_music.play_count = total;
            s_music.play_valid = true;
            s_music.status.generation = ++s_music.generation;
            s_music.player.generation = s_music.status.generation;
            music_service_unlock();
            (void)music_stream_stop();
            const esp_err_t stream_result = music_stream_play(
                                                url, song->duration_ms);
            if (stream_result != ESP_OK)
            {
                music_service_handle_network_result(stream_result);
                music_service_lock();
                music_service_set_state_locked(s_music.status.state,
                                               MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
                music_service_unlock();
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                _music_service_player_error(MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
                _psram_free(playlist);
                break;
            }
            atomic_store_explicit(&s_playing, true, memory_order_release);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
            _psram_free(playlist);
            break;
        }
        default:
            break;
        }
    }
    atomic_store_explicit(&s_api_exited, true, memory_order_release);
    vTaskDelete(NULL);
}

esp_err_t music_service_init(const music_service_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_music.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_music, 0, sizeof(s_music));
    s_music.lock = xSemaphoreCreateMutex();
    s_music.api_queue = xQueueCreate(CONFIG_MUSIC_SERVICE_API_QUEUE_DEPTH,
                                     sizeof(music_api_command_t));
    if (s_music.lock == NULL || s_music.api_queue == NULL)
    {
        if (s_music.api_queue != NULL)
        {
            vQueueDelete(s_music.api_queue);
        }
        if (s_music.lock != NULL)
        {
            vSemaphoreDelete(s_music.lock);
        }
        memset(&s_music, 0, sizeof(s_music));
        return ESP_ERR_NO_MEM;
    }
    s_music.config = *config;
    s_music.status.state = MUSIC_SERVICE_STATE_WAITING_NETWORK;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.audio_available = audio_service_is_available();
    s_music.player.state = MUSIC_PLAYBACK_IDLE;
    s_music.player.quality = config->default_quality;
    s_music.initialized = true;
    atomic_store_explicit(&s_playing, false, memory_order_release);
    atomic_store_explicit(&s_login_cancel, false, memory_order_release);
    atomic_store_explicit(&s_api_stop, false, memory_order_release);
    atomic_store_explicit(&s_api_exited, false, memory_order_release);
    music_credential_blob_t credential;
    if (music_net_load_credential(&credential) == ESP_OK)
    {
        bool expired = false;
        if (credential.expires_at != 0U &&
                time_service_get_quality() != TIME_SERVICE_QUALITY_INVALID)
        {
            const time_t now = time(NULL);
            expired = now > 0 && (uint32_t)now >= credential.expires_at;
        }
        if (expired)
        {
            (void)music_net_erase_credential();
        }
        else
        {
            s_music.credential = credential;
            s_music.credential_valid = true;
            s_music.status.logged_in = true;
        }
    }
    if (music_stream_init(config->audio_task_priority) != ESP_OK)
    {
        s_music.initialized = false;
        vQueueDelete(s_music.api_queue);
        vSemaphoreDelete(s_music.lock);
        memset(&s_music, 0, sizeof(s_music));
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(_api_task, "music_api",
                                CONFIG_MUSIC_SERVICE_API_TASK_STACK_SIZE, NULL,
                                config->api_task_priority, &s_music.api_task,
                                0) != pdPASS)
    {
        (void)music_stream_deinit();
        s_music.initialized = false;
        vQueueDelete(s_music.api_queue);
        vSemaphoreDelete(s_music.lock);
        memset(&s_music, 0, sizeof(s_music));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t music_service_deinit(void)
{
    if (!s_music.initialized)
    {
        return ESP_OK;
    }
    atomic_store_explicit(&s_login_cancel, true, memory_order_release);
    atomic_store_explicit(&s_api_stop, true, memory_order_release);
    const esp_err_t stream_result = music_stream_deinit();
    if (stream_result != ESP_OK)
    {
        return stream_result;
    }
    for (uint32_t wait = 0U; wait < 300U; ++wait)
    {
        if (atomic_load_explicit(&s_api_exited, memory_order_acquire))
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50U));
    }
    if (!atomic_load_explicit(&s_api_exited, memory_order_acquire) &&
            s_music.api_task != NULL)
    {
        LOG_E("API task did not stop; forcing deletion");
        vTaskDelete(s_music.api_task);
    }
    s_music.api_task = NULL;
    if (s_music.api_queue != NULL)
    {
        vQueueDelete(s_music.api_queue);
        s_music.api_queue = NULL;
    }
    _psram_free(s_music.home);
    _psram_free(s_music.playlist);
    _psram_free(s_music.lyric);
    music_service_login_qr_release(s_music.login_qr);
    music_service_image_release(s_music.image);
    if (s_music.lock != NULL)
    {
        vSemaphoreDelete(s_music.lock);
        s_music.lock = NULL;
    }
    memset(&s_music, 0, sizeof(s_music));
    atomic_store_explicit(&s_playing, false, memory_order_release);
    return ESP_OK;
}

esp_err_t music_service_set_network_ready(bool ready)
{
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_music.status.network_ready = ready;
    const bool time_ready = time_service_get_quality() !=
                            TIME_SERVICE_QUALITY_INVALID;
    s_music.status.time_ready = time_ready;
    if (!ready)
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_WAITING_NETWORK,
                                       MUSIC_SERVICE_FAILURE_NETWORK);
    }
    else if (!time_ready)
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_WAITING_TIME,
                                       MUSIC_SERVICE_FAILURE_TIME);
    }
    else if (s_music.credential_valid)
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_READY,
                                       MUSIC_SERVICE_FAILURE_NONE);
    }
    else
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_LOGGED_OUT,
                                       MUSIC_SERVICE_FAILURE_NONE);
    }
    music_service_unlock();
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    if (ready && time_ready)
    {
        const music_api_command_t refresh =
        {
            .type = MUSIC_API_CMD_REFRESH_HOME,
        };
        (void)xQueueSend(s_music.api_queue, &refresh, pdMS_TO_TICKS(100U));
    }
    return ESP_OK;
}

esp_err_t music_service_login_begin(music_login_type_t type)
{
    if (type != MUSIC_LOGIN_TYPE_WECHAT && type != MUSIC_LOGIN_TYPE_QQ)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (type != MUSIC_LOGIN_TYPE_WECHAT)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_music.initialized || s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool allowed = !s_music.login_active &&
                         s_music.status.network_ready &&
                         s_music.status.time_ready;
    if (allowed)
    {
        s_music.login_active = true;
        atomic_store_explicit(&s_login_cancel, false, memory_order_release);
    }
    music_service_unlock();
    if (!allowed)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_LOGIN_BEGIN,
        .login_type = type,
    };
    if (xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        music_service_lock();
        s_music.login_active = false;
        music_service_unlock();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t music_service_login_qr_acquire(const music_image_t **image)
{
    if (image == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *image = NULL;
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    music_image_t *qr = s_music.login_qr;
    s_music.login_qr = NULL;
    music_service_unlock();
    if (qr == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *image = qr;
    return ESP_OK;
}

void music_service_login_qr_release(const music_image_t *image)
{
    if (image != NULL)
    {
        if (image->pixels != NULL)
        {
            jpeg_free_align(image->pixels);
        }
        _psram_free((void *)image);
    }
}

esp_err_t music_service_login_cancel(void)
{
    atomic_store_explicit(&s_login_cancel, true, memory_order_release);
    music_image_t *qr = NULL;
    if (music_service_lock() == ESP_OK)
    {
        qr = s_music.login_qr;
        s_music.login_qr = NULL;
        if (s_music.status.state == MUSIC_SERVICE_STATE_QR_READY ||
                s_music.status.state == MUSIC_SERVICE_STATE_QR_SCANNED ||
                s_music.status.state == MUSIC_SERVICE_STATE_AUTHENTICATING ||
                s_music.status.state == MUSIC_SERVICE_STATE_AUTH_REQUIRED)
        {
            music_service_set_state_locked(
                s_music.credential_valid ? MUSIC_SERVICE_STATE_READY :
                MUSIC_SERVICE_STATE_LOGGED_OUT,
                MUSIC_SERVICE_FAILURE_NONE);
            s_music.status.qr_status[0] = '\0';
            s_music.status.qr_available = false;
        }
        music_service_unlock();
    }
    music_service_login_qr_release(qr);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    return ESP_OK;
}

esp_err_t music_service_logout(void)
{
    if (s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_login_cancel, true, memory_order_release);
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_LOGOUT,
    };
    return xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) ==
           pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t music_service_reset_persisted_state(void)
{
    return music_net_erase_credential();
}

esp_err_t music_service_refresh_home(bool force)
{
    if (s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!force && music_service_lock() == ESP_OK)
    {
        const bool cached = s_music.home != NULL;
        music_service_unlock();
        if (cached)
        {
            return ESP_OK;
        }
    }
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_REFRESH_HOME,
    };
    return xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) ==
           pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t music_service_home_acquire(const music_home_snapshot_t **snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    music_home_snapshot_t *home = s_music.home;
    s_music.home = NULL;
    music_service_unlock();
    if (home == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = home;
    return ESP_OK;
}

void music_service_home_release(const music_home_snapshot_t *snapshot)
{
    _psram_free((void *)snapshot);
}

esp_err_t music_service_playlist_acquire(const music_playlist_ref_t *ref,
        uint16_t offset, uint16_t count,
        const music_playlist_snapshot_t **snapshot)
{
    if (ref == NULL || snapshot == NULL || count == 0U ||
            count > MUSIC_SERVICE_MAX_SONGS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    if (music_service_lock() == ESP_OK)
    {
        music_playlist_snapshot_t *playlist = s_music.playlist;
        if (playlist != NULL && playlist->playlist.ref.id == ref->id &&
                playlist->playlist.ref.dirid == ref->dirid &&
                playlist->playlist.ref.tid == ref->tid &&
                playlist->playlist.ref.kind == ref->kind &&
                playlist->offset == offset &&
                (playlist->count >= count ||
                 (playlist->total != 0U && playlist->total <= playlist->count)))
        {
            s_music.playlist = NULL;
            music_service_unlock();
            *snapshot = playlist;
            return ESP_OK;
        }
        music_service_unlock();
    }
    if (s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_PLAYLIST,
        .playlist = *ref,
        .offset = offset,
        .count = count,
    };
    return xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) ==
           pdTRUE ? ESP_ERR_NOT_FOUND : ESP_ERR_TIMEOUT;
}

void music_service_playlist_release(const music_playlist_snapshot_t *snapshot)
{
    _psram_free((void *)snapshot);
}

esp_err_t music_service_lyric_acquire(const char *songmid,
                                      const music_lyric_snapshot_t **snapshot)
{
    if (songmid == NULL || songmid[0] == '\0' || snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    if (music_service_lock() == ESP_OK)
    {
        music_lyric_snapshot_t *lyric = s_music.lyric;
        if (lyric != NULL && strcmp(s_music.lyric_songmid, songmid) == 0)
        {
            s_music.lyric = NULL;
            music_service_unlock();
            *snapshot = lyric;
            return ESP_OK;
        }
        music_service_unlock();
    }
    if (s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    music_api_command_t command =
    {
        .type = MUSIC_API_CMD_LYRIC,
    };
    (void)snprintf(command.songmid, sizeof(command.songmid), "%s", songmid);
    return xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) ==
           pdTRUE ? ESP_ERR_NOT_FOUND : ESP_ERR_TIMEOUT;
}

void music_service_lyric_release(const music_lyric_snapshot_t *snapshot)
{
    _psram_free((void *)snapshot);
}

esp_err_t music_service_image_acquire(const char *url, uint16_t width,
                                      uint16_t height, uint32_t token,
                                      const music_image_t **image)
{
    if (url == NULL || url[0] == '\0' || image == NULL ||
            width == 0U || height == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *image = NULL;
    if (music_service_lock() == ESP_OK)
    {
        music_image_t *current = s_music.image;
        if (current != NULL && current->token == token &&
                strcmp(current->url, url) == 0)
        {
            s_music.image = NULL;
            music_service_unlock();
            *image = current;
            return ESP_OK;
        }
        music_service_unlock();
    }
    if (s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    music_api_command_t command =
    {
        .type = MUSIC_API_CMD_IMAGE,
        .token = token,
        .image_width = width,
        .image_height = height,
    };
    (void)snprintf(command.url, sizeof(command.url), "%s", url);
    return xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) ==
           pdTRUE ? ESP_ERR_NOT_FOUND : ESP_ERR_TIMEOUT;
}

void music_service_image_release(const music_image_t *image)
{
    if (image != NULL)
    {
        if (image->pixels != NULL)
        {
            jpeg_free_align(image->pixels);
        }
        _psram_free((void *)image);
    }
}

esp_err_t music_service_play_playlist(const music_playlist_ref_t *ref,
                                      uint16_t start_index)
{
    if (ref == NULL || ref->id == 0U || s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_PLAY,
        .playlist = *ref,
        .offset = start_index,
    };
    if (xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    if (music_service_lock() == ESP_OK)
    {
        s_music.player.state = MUSIC_PLAYBACK_BUFFERING;
        s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
        music_service_unlock();
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    }
    return ESP_OK;
}

esp_err_t music_service_play_pause(void)
{
    music_stream_status_t status;
    if (music_stream_get_status(&status) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (status.state == MUSIC_PLAYBACK_PLAYING)
    {
        return music_stream_pause(true);
    }
    if (status.state == MUSIC_PLAYBACK_PAUSED)
    {
        return music_stream_pause(false);
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t music_service_next(void)
{
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool valid = s_music.play_valid;
    const music_playlist_ref_t ref = s_music.play_ref;
    const uint32_t count = s_music.play_count;
    const uint32_t index = s_music.play_index;
    const music_play_mode_t mode = s_music.player.mode;
    music_service_unlock();
    if (!valid || count == 0U || index >= UINT16_MAX)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t target = index + 1U;
    if (mode == MUSIC_PLAY_MODE_REPEAT_ONE)
    {
        target = index;
    }
    else if (mode == MUSIC_PLAY_MODE_SHUFFLE)
    {
        target = count > 1U ? esp_random() % count : index;
        if (target == index && count > 1U)
        {
            target = (target + 1U) % count;
        }
    }
    else if (target >= count)
    {
        if (mode != MUSIC_PLAY_MODE_REPEAT_ALL)
        {
            return ESP_ERR_INVALID_STATE;
        }
        target = 0U;
    }
    return music_service_play_playlist(&ref, (uint16_t)target);
}

esp_err_t music_service_prev(void)
{
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool valid = s_music.play_valid;
    const music_playlist_ref_t ref = s_music.play_ref;
    const uint32_t count = s_music.play_count;
    const uint32_t index = s_music.play_index;
    const music_play_mode_t mode = s_music.player.mode;
    music_service_unlock();
    if (!valid || count == 0U || index > UINT16_MAX)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t target = index == 0U ? count - 1U : index - 1U;
    if (mode == MUSIC_PLAY_MODE_REPEAT_ONE)
    {
        target = index;
    }
    else if (mode == MUSIC_PLAY_MODE_SHUFFLE)
    {
        target = count > 1U ? esp_random() % count : index;
        if (target == index && count > 1U)
        {
            target = target == 0U ? count - 1U : target - 1U;
        }
    }
    else if (index == 0U && mode != MUSIC_PLAY_MODE_REPEAT_ALL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return music_service_play_playlist(&ref, (uint16_t)target);
}

esp_err_t music_service_seek(uint32_t position_ms)
{
    (void)position_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t music_service_set_mode(music_play_mode_t mode)
{
    if ((int)mode < (int)MUSIC_PLAY_MODE_SEQUENTIAL ||
            mode > MUSIC_PLAY_MODE_SHUFFLE)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_music.player.mode = mode;
    music_service_unlock();
    return ESP_OK;
}

esp_err_t music_service_stop(void)
{
    (void)music_stream_stop();
    atomic_store_explicit(&s_playing, false, memory_order_release);
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_music.player.state = MUSIC_PLAYBACK_IDLE;
    s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.play_valid = false;
    music_service_unlock();
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    return ESP_OK;
}

esp_err_t music_service_get_status(music_service_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *status = s_music.status;
    music_service_unlock();
    return ESP_OK;
}

esp_err_t music_service_get_player_status(music_player_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *status = s_music.player;
    const bool play_valid = s_music.play_valid;
    music_service_unlock();
    music_stream_status_t stream_status;
    if (play_valid && music_stream_get_status(&stream_status) == ESP_OK)
    {
        status->state = stream_status.state;
        status->position_ms = stream_status.position_ms;
        status->duration_ms = stream_status.duration_ms != 0U ?
                              stream_status.duration_ms : status->duration_ms;
        if (stream_status.failure != MUSIC_SERVICE_FAILURE_NONE)
        {
            status->failure = stream_status.failure;
        }
    }
    return ESP_OK;
}

void music_service_stream_finished(void)
{
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    const bool valid = s_music.play_valid;
    music_service_unlock();
    if (!valid)
    {
        return;
    }
    if (music_service_next() != ESP_OK)
    {
        (void)music_service_stop();
    }
}

bool music_service_is_playing(void)
{
    return music_stream_is_playing();
}
