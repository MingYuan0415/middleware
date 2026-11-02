#define DBG_TAG "music_service"
#include "mt_log.h"

#include "music_service_internal.h"
#include "music_service_cache.h"
#include "music_http.h"
#include "music_queue_model.h"
#include "music_snapshot.h"

#include "audio_service.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_random.h"
#include "event_bus.h"
#include "freertos/idf_additions.h"
#include "nv_storage.h"
#include "time_service.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

EVENT_BUS_DEFINE_ID(MUSIC_SERVICE_MSG);

MUSIC_SERVICE_EXT_BSS music_service_runtime_t s_music;
static MUSIC_SERVICE_EXT_BSS music_api_command_t s_api_command;

static atomic_bool s_playing;
static atomic_bool s_login_cancel;
static atomic_uint s_login_generation;
static atomic_bool s_api_stop;
static atomic_bool s_api_exited;
static atomic_bool s_login_exited;

static bool _login_session_current(uint32_t session)
{
    return atomic_load_explicit(&s_login_generation,
                                memory_order_acquire) == session;
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

static void _music_service_publish_with_failure(uint32_t sub_type,
        uint32_t token, music_service_failure_t failure)
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
    event.failure = failure;
    music_service_unlock();
    (void)event_bus_publish(MUSIC_SERVICE_MSG, sub_type, &event,
                            sizeof(event), EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

void music_service_publish(uint32_t sub_type, uint32_t token)
{
    music_service_failure_t failure = MUSIC_SERVICE_FAILURE_NONE;
    if (music_service_lock() == ESP_OK)
    {
        failure = s_music.status.failure;
        music_service_unlock();
    }
    _music_service_publish_with_failure(sub_type, token, failure);
}

void music_service_publish_detail(uint32_t sub_type, uint32_t token,
                                  music_service_failure_t failure)
{
    _music_service_publish_with_failure(sub_type, token, failure);
}

void music_service_set_state_locked(music_service_state_t state,
                                    music_service_failure_t failure)
{
    s_music.status.state = state;
    s_music.status.failure = failure;
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

static music_service_failure_t _music_service_failure_from_result(
    esp_err_t result)
{
    if (result == ESP_ERR_TIMEOUT)
    {
        return MUSIC_SERVICE_FAILURE_NETWORK;
    }
    if (result == ESP_ERR_INVALID_RESPONSE)
    {
        return MUSIC_SERVICE_FAILURE_PARSE;
    }
    if (result == ESP_ERR_NO_MEM)
    {
        return MUSIC_SERVICE_FAILURE_INTERNAL;
    }
    if (result == ESP_ERR_NOT_FOUND)
    {
        return MUSIC_SERVICE_FAILURE_NO_PERMISSION;
    }
    if (result == ESP_ERR_NOT_SUPPORTED)
    {
        return MUSIC_SERVICE_FAILURE_DECODE;
    }
    if (result == MUSIC_ERR_HTTP_AUTH)
    {
        return MUSIC_SERVICE_FAILURE_AUTH;
    }
    if (result == MUSIC_ERR_HTTP_RATE_LIMIT)
    {
        return MUSIC_SERVICE_FAILURE_RATE_LIMIT;
    }
    if (result != ESP_OK)
    {
        return MUSIC_SERVICE_FAILURE_API;
    }
    return MUSIC_SERVICE_FAILURE_NONE;
}

void music_service_handle_network_result(esp_err_t result)
{
    const music_service_failure_t failure =
        _music_service_failure_from_result(result);
    if (music_service_lock() == ESP_OK)
    {
        s_music.status.failure = failure;
        if (failure == MUSIC_SERVICE_FAILURE_AUTH &&
                s_music.status.state == MUSIC_SERVICE_STATE_READY)
        {
            s_music.credential_valid = false;
            s_music.status.logged_in = false;
            music_service_set_state_locked(MUSIC_SERVICE_STATE_AUTH_REQUIRED,
                                           failure);
        }
        else
        {
            s_music.status.generation = ++s_music.generation;
            s_music.player.generation = s_music.status.generation;
        }
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
        music_snapshot_release(snapshot);
        return;
    }
    music_home_snapshot_t *old = s_music.home;
    s_music.home = snapshot;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.generation = ++s_music.generation;
    s_music.player.generation = s_music.status.generation;
    music_service_unlock();
    music_snapshot_release(old);
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
        music_snapshot_release(snapshot);
        return;
    }
    music_playlist_snapshot_t *old = s_music.playlist;
    s_music.playlist = snapshot;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.generation = ++s_music.generation;
    s_music.player.generation = s_music.status.generation;
    music_service_unlock();
    music_snapshot_release(old);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYLIST, 0U);
}

void music_service_replace_queue(music_queue_snapshot_t *snapshot)
{
    if (music_service_lock() != ESP_OK)
    {
        music_snapshot_release(snapshot);
        return;
    }
    music_queue_snapshot_t *old = s_music.queue;
    s_music.queue = snapshot;
    music_service_unlock();
    music_snapshot_release(old);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U);
}

void music_service_replace_lyric(music_lyric_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }
    if (music_service_lock() != ESP_OK)
    {
        music_snapshot_release(snapshot);
        return;
    }
    music_lyric_snapshot_t *old = s_music.lyric;
    s_music.lyric = snapshot;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.generation = ++s_music.generation;
    s_music.player.generation = s_music.status.generation;
    music_service_unlock();
    music_snapshot_release(old);
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
    s_music.status.login_type = s_music.login_type;
    (void)snprintf(s_music.status.qr_status,
                   sizeof(s_music.status.qr_status),
                   s_music.login_type == MUSIC_LOGIN_TYPE_QQ ?
                   "请使用QQ扫码" : "请使用微信扫码");
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
    music_service_image_release(old);
    music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_IMAGE, token,
                                 MUSIC_SERVICE_FAILURE_NONE);
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
}

static void _music_service_finish_login(uint32_t session, bool success,
                                        bool keep_qr)
{
    music_image_t *qr = NULL;
    music_service_lock();
    if (s_music.login_session != session)
    {
        music_service_unlock();
        return;
    }
    s_music.login_active = false;
    s_music.status.qr_available = keep_qr;
    if (!success)
    {
        s_music.status.logged_in = s_music.credential_valid;
    }
    if (!keep_qr)
    {
        qr = s_music.login_qr;
        s_music.login_qr = NULL;
    }
    music_service_unlock();
    music_service_login_qr_release(qr);
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

static void _music_log_api_stack(const char *phase)
{
    LOG_I("api stack high water=%u B phase=%s",
          (unsigned)uxTaskGetStackHighWaterMark(NULL), phase);
}

static bool _credential_expired(const music_credential_blob_t *credential)
{
    if (credential == NULL || credential->expires_at == 0U)
    {
        return false;
    }
    const time_t now = time(NULL);
    return now > 0 && (uint32_t)now >= credential->expires_at;
}

static esp_err_t _music_service_ensure_stream(void)
{
    const esp_err_t result = music_stream_init(
                                 s_music.config.audio_task_priority);
    return result == ESP_ERR_INVALID_STATE ? ESP_OK : result;
}

static music_queue_snapshot_t *_music_service_queue_alloc(uint16_t count)
{
    const size_t bytes = offsetof(music_queue_snapshot_t, songs) +
                         (size_t)count * sizeof(music_song_t);
    music_queue_snapshot_t *queue = music_snapshot_alloc(bytes, NULL);
    if (queue != NULL)
    {
        queue->generation = music_service_next_generation();
        queue->count = count;
    }
    return queue;
}

static music_queue_snapshot_t *_music_service_queue_from_playlist(
    const music_playlist_snapshot_t *playlist, uint16_t count)
{
    if (playlist == NULL || count == 0U || count > playlist->count)
    {
        return NULL;
    }
    music_queue_snapshot_t *queue = _music_service_queue_alloc(count);
    if (queue != NULL)
    {
        memcpy(queue->songs, playlist->songs,
               (size_t)count * sizeof(queue->songs[0]));
    }
    return queue;
}

static music_queue_snapshot_t *_music_service_queue_without(
    const music_queue_snapshot_t *queue, uint16_t removed)
{
    if (queue == NULL || removed >= queue->count)
    {
        return NULL;
    }
    const uint16_t count = (uint16_t)(queue->count - 1U);
    music_queue_snapshot_t *next = _music_service_queue_alloc(count);
    if (next == NULL)
    {
        return NULL;
    }
    if (removed > 0U)
    {
        memcpy(next->songs, queue->songs,
               (size_t)removed * sizeof(next->songs[0]));
    }
    if (removed + 1U < queue->count)
    {
        memcpy(&next->songs[removed], &queue->songs[removed + 1U],
               (size_t)(queue->count - removed - 1U) *
               sizeof(next->songs[0]));
    }
    return next;
}

static void _music_service_set_queue_player(const music_song_t *song,
        uint16_t index, uint16_t count, uint32_t play_generation)
{
    music_service_lock();
    if (!s_music.play_valid ||
            s_music.play_generation != play_generation)
    {
        music_service_unlock();
        return;
    }
    s_music.player.state = MUSIC_PLAYBACK_BUFFERING;
    s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    (void)snprintf(s_music.player.title, sizeof(s_music.player.title), "%s",
                   song->title);
    (void)snprintf(s_music.player.artist, sizeof(s_music.player.artist), "%s",
                   song->artist);
    (void)snprintf(s_music.player.cover_url, sizeof(s_music.player.cover_url),
                   "%s", song->cover_url);
    (void)snprintf(s_music.player.songmid, sizeof(s_music.player.songmid),
                   "%s", song->songmid);
    s_music.player.duration_ms = song->duration_ms;
    s_music.player.quality = s_music.config.default_quality;
    s_music.player.queue_index = index;
    s_music.player.queue_count = count;
    s_music.player.play_generation = play_generation;
    s_music.status.generation = ++s_music.generation;
    s_music.player.generation = s_music.status.generation;
    music_service_unlock();
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
}

static bool _music_service_playback_current(uint32_t play_generation)
{
    if (music_service_lock() != ESP_OK)
    {
        return false;
    }
    const bool current = s_music.play_valid &&
                         s_music.play_generation == play_generation;
    music_service_unlock();
    return current;
}

static bool _music_service_play_generation_current(uint32_t play_generation)
{
    if (music_service_lock() != ESP_OK)
    {
        return false;
    }
    const bool current = s_music.play_generation == play_generation;
    music_service_unlock();
    return current;
}

static esp_err_t _music_service_start_queue_index(
    const music_queue_snapshot_t *queue, uint16_t index,
    uint32_t play_generation)
{
    if (queue == NULL || index >= queue->count ||
            play_generation == 0U || !_music_service_playback_current(play_generation))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const music_song_t *song = &queue->songs[index];
    (void)music_stream_stop();
    _music_service_set_queue_player(song, index, queue->count,
                                    play_generation);
    char url[MUSIC_SERVICE_URL_BYTES];
    music_quality_t actual_quality = s_music.config.default_quality;
    const esp_err_t url_result = music_net_fetch_song_url(
                                     song, s_music.config.default_quality,
                                     url, sizeof(url), &actual_quality);
    if (url_result != ESP_OK)
    {
        if (!_music_service_playback_current(play_generation))
        {
            return ESP_ERR_INVALID_STATE;
        }
        (void)music_service_handle_network_result(url_result);
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
        _music_service_player_error(s_music.status.failure);
        return url_result;
    }

    esp_err_t stream_result = _music_service_ensure_stream();
    if (stream_result == ESP_OK)
    {
        music_service_lock();
        if (!s_music.play_valid ||
                s_music.play_generation != play_generation)
        {
            music_service_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        s_music.player.quality = actual_quality;
        stream_result = music_stream_play(url, song->duration_ms,
                                          play_generation);
        if (stream_result == ESP_OK)
        {
            atomic_store_explicit(&s_playing, true,
                                  memory_order_release);
        }
        music_service_unlock();
    }
    if (stream_result != ESP_OK)
    {
        if (!_music_service_playback_current(play_generation))
        {
            return ESP_ERR_INVALID_STATE;
        }
        (void)music_service_handle_network_result(stream_result);
        music_service_lock();
        music_service_set_state_locked(s_music.status.state,
                                       MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
        music_service_unlock();
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
        _music_service_player_error(MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
        return stream_result;
    }
    _music_log_api_stack("play");
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    return ESP_OK;
}

static bool _music_service_login_set_state(uint32_t session,
        music_service_state_t state, music_service_failure_t failure,
        const char *qr_status)
{
    if (atomic_load_explicit(&s_login_cancel, memory_order_acquire) ||
            !_login_session_current(session))
    {
        return false;
    }
    if (music_service_lock() != ESP_OK)
    {
        return false;
    }
    if (!s_music.login_active || s_music.login_session != session ||
            !_login_session_current(session))
    {
        music_service_unlock();
        return false;
    }
    music_service_set_state_locked(state, failure);
    if (qr_status != NULL)
    {
        (void)snprintf(s_music.status.qr_status,
                       sizeof(s_music.status.qr_status), "%s", qr_status);
    }
    if (state == MUSIC_SERVICE_STATE_QR_EXPIRED)
    {
        s_music.status.qr_available = true;
    }
    music_service_unlock();
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    return true;
}

static void _music_service_run_login(music_login_type_t type, uint32_t session)
{
    if (!_music_service_login_set_state(session,
                                        MUSIC_SERVICE_STATE_QR_READY,
                                        MUSIC_SERVICE_FAILURE_NONE,
                                        "正在获取二维码"))
    {
        return;
    }

    const esp_err_t begin_result = music_net_login_begin(type);
    if (begin_result != ESP_OK)
    {
        const music_service_failure_t failure =
            _music_service_failure_from_result(begin_result);
        (void)_music_service_login_set_state(session,
                                             MUSIC_SERVICE_STATE_ERROR,
                                             failure, "登录未完成");
        _music_service_finish_login(session, false, false);
        return;
    }

    music_credential_blob_t credential = {0};
    bool terminal_ok = false;
    bool expired = false;
    while (!atomic_load_explicit(&s_login_cancel, memory_order_acquire) &&
            _login_session_current(session) &&
            !atomic_load_explicit(&s_api_stop, memory_order_acquire))
    {
        vTaskDelay(pdMS_TO_TICKS(1500U));
        if (atomic_load_explicit(&s_login_cancel, memory_order_acquire) ||
                !_login_session_current(session))
        {
            break;
        }

        music_login_poll_state_t poll_state = MUSIC_LOGIN_POLL_WAITING;
        const esp_err_t poll_result = music_net_login_poll(type, &credential,
            &poll_state);
        if (poll_result != ESP_OK)
        {
            if (atomic_load_explicit(&s_login_cancel,
                                     memory_order_acquire) ||
                    !_login_session_current(session))
            {
                break;
            }
            const music_service_failure_t failure =
                _music_service_failure_from_result(poll_result);
            (void)_music_service_login_set_state(session,
                                                 MUSIC_SERVICE_STATE_ERROR,
                                                 failure, "登录未完成");
            break;
        }
        if (poll_state == MUSIC_LOGIN_POLL_WAITING)
        {
            continue;
        }
        if (poll_state == MUSIC_LOGIN_POLL_SCANNED)
        {
            if (!_music_service_login_set_state(
                        session, MUSIC_SERVICE_STATE_QR_SCANNED,
                        MUSIC_SERVICE_FAILURE_NONE, "已扫码，请在手机上确认"))
            {
                break;
            }
            continue;
        }
        if (poll_state == MUSIC_LOGIN_POLL_FAILED)
        {
            expired = _music_service_login_set_state(
                          session, MUSIC_SERVICE_STATE_QR_EXPIRED,
                          MUSIC_SERVICE_FAILURE_AUTH,
                          "二维码已过期，请重新登录");
            break;
        }

        if (atomic_load_explicit(&s_login_cancel, memory_order_acquire) ||
                !_login_session_current(session))
        {
            break;
        }
        const esp_err_t save_result =
            music_net_save_credential(&credential);
        if (save_result != ESP_OK)
        {
            (void)_music_service_login_set_state(
                session, MUSIC_SERVICE_STATE_AUTH_REQUIRED,
                MUSIC_SERVICE_FAILURE_STORAGE, "登录态保存失败");
            break;
        }
        if (!_login_session_current(session) ||
                atomic_load_explicit(&s_login_cancel,
                                     memory_order_acquire))
        {
            break;
        }
        if (music_service_lock() != ESP_OK)
        {
            break;
        }
        if (!s_music.login_active || s_music.login_session != session ||
                !_login_session_current(session))
        {
            music_service_unlock();
            break;
        }
        s_music.credential = credential;
        s_music.credential_valid = true;
        s_music.status.logged_in = true;
        s_music.login_type = credential.login_type == 2U ?
                             MUSIC_LOGIN_TYPE_QQ : MUSIC_LOGIN_TYPE_WECHAT;
        s_music.status.login_type = s_music.login_type;
        s_music.status.qr_available = false;
        s_music.status.user_name[0] = '\0';
        s_music.status.user_avatar[0] = '\0';
        music_service_set_state_locked(MUSIC_SERVICE_STATE_READY,
                                       MUSIC_SERVICE_FAILURE_NONE);
        (void)snprintf(s_music.status.qr_status,
                       sizeof(s_music.status.qr_status), "登录成功");
        music_service_unlock();
        _music_log_api_stack("login");
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
        const music_api_command_t refresh =
        {
            .type = MUSIC_API_CMD_REFRESH_HOME,
        };
        (void)xQueueSend(s_music.api_queue, &refresh,
                         pdMS_TO_TICKS(100U));
        terminal_ok = true;
        break;
    }

    _music_service_finish_login(session, terminal_ok, expired);

    music_service_lock();
    const bool newer_login = s_music.login_active &&
                             s_music.login_session != session;
    music_service_unlock();
    if (!newer_login)
    {
        music_net_cancel_login();
    }
}

static void _login_task(void *argument)
{
    (void)argument;
    for (;;)
    {
        music_login_command_t command;
        if (xQueueReceive(s_music.login_queue, &command,
                          portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if (atomic_load_explicit(&s_api_stop, memory_order_acquire))
        {
            break;
        }
        if (!_login_session_current(command.session) ||
                atomic_load_explicit(&s_login_cancel,
                                     memory_order_acquire))
        {
            continue;
        }
        _music_service_run_login(command.type, command.session);
    }
    atomic_store_explicit(&s_login_exited, true, memory_order_release);
    vTaskDelete(NULL);
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
        bool erase_pending = false;
        if (music_service_lock() == ESP_OK)
        {
            erase_pending = s_music.credential_erase_pending;
            s_music.credential_erase_pending = false;
            music_service_unlock();
        }
        if (erase_pending)
        {
            const esp_err_t erase_result = music_net_erase_credential();
            if (erase_result != ESP_OK)
            {
                music_service_lock();
                s_music.credential_erase_pending = true;
                music_service_unlock();
                vTaskDelay(pdMS_TO_TICKS(1000U));
            }
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
            continue;
        }
        music_api_command_t *const command = &s_api_command;
        TickType_t wait_ticks = portMAX_DELAY;
        if (music_service_lock() == ESP_OK)
        {
            wait_ticks = s_music.status.state == MUSIC_SERVICE_STATE_WAITING_TIME ?
                         pdMS_TO_TICKS(1000U) : portMAX_DELAY;
            music_service_unlock();
        }
        if (xQueueReceive(s_music.api_queue, command,
                          wait_ticks) != pdTRUE)
        {
            _music_service_recheck_time();
            continue;
        }
        switch (command->type)
        {
        case MUSIC_API_CMD_ERASE_CREDENTIAL:
        {
            music_service_lock();
            s_music.credential_erase_pending = false;
            music_service_unlock();
            (void)music_net_erase_credential();
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
            break;
        }
        case MUSIC_API_CMD_LOGOUT:
        {
            const esp_err_t erase_result = music_net_erase_credential();
            if (erase_result != ESP_OK)
            {
                music_service_lock();
                music_service_set_state_locked(
                    s_music.status.state, MUSIC_SERVICE_FAILURE_STORAGE);
                music_service_unlock();
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            (void)music_stream_stop();
            atomic_store_explicit(&s_playing, false, memory_order_release);
            music_home_snapshot_t *old_home = NULL;
            music_playlist_snapshot_t *old_playlist = NULL;
            music_queue_snapshot_t *old_queue = NULL;
            music_lyric_snapshot_t *old_lyric = NULL;
            music_image_t *old_image = NULL;
            music_image_t *old_qr = NULL;
            music_service_lock();
            memset(&s_music.credential, 0, sizeof(s_music.credential));
            s_music.credential_valid = false;
            s_music.login_active = false;
            s_music.login_type = MUSIC_LOGIN_TYPE_WECHAT;
            s_music.status.logged_in = false;
            s_music.status.user_name[0] = '\0';
            s_music.status.user_avatar[0] = '\0';
            s_music.status.qr_status[0] = '\0';
            s_music.status.qr_available = false;
            s_music.status.login_type = MUSIC_LOGIN_TYPE_WECHAT;
            old_home = s_music.home;
            old_playlist = s_music.playlist;
            old_queue = s_music.queue;
            old_lyric = s_music.lyric;
            old_image = s_music.image;
            old_qr = s_music.login_qr;
            s_music.home = NULL;
            s_music.playlist = NULL;
            s_music.queue = NULL;
            s_music.lyric = NULL;
            s_music.image = NULL;
            s_music.login_qr = NULL;
            s_music.play_valid = false;
            s_music.player.state = MUSIC_PLAYBACK_IDLE;
            s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
            s_music.player.position_ms = 0U;
            s_music.player.duration_ms = 0U;
            s_music.player.queue_index = 0U;
            s_music.player.queue_count = 0U;
            s_music.player.title[0] = '\0';
            s_music.player.artist[0] = '\0';
            s_music.player.cover_url[0] = '\0';
            s_music.player.songmid[0] = '\0';
            s_music.player.play_generation = 0U;
            music_service_set_state_locked(MUSIC_SERVICE_STATE_LOGGED_OUT,
                                           MUSIC_SERVICE_FAILURE_NONE);
            music_service_unlock();
            music_snapshot_release(old_home);
            music_snapshot_release(old_playlist);
            music_snapshot_release(old_queue);
            music_snapshot_release(old_lyric);
            music_service_image_release(old_image);
            music_service_login_qr_release(old_qr);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U);
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
                                         &command->playlist, command->offset,
                                         command->count, &snapshot);
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
                                         command->songmid, &snapshot);
            if (result == ESP_OK)
            {
                music_service_lock();
                (void)snprintf(s_music.lyric_songmid,
                               sizeof(s_music.lyric_songmid), "%s",
                               command->songmid);
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
                                         command->url, command->image_width,
                                         command->image_height, command->token,
                                         &image);
            if (result == ESP_OK)
            {
                music_service_set_image(image, command->token);
            }
            else
            {
                music_service_publish_detail(
                    MUSIC_SERVICE_MSG_SUB_TYPE_IMAGE, command->token,
                    _music_service_failure_from_result(result));
            }
            break;
        }
        case MUSIC_API_CMD_PLAY:
        {
            const uint16_t requested_index = (uint16_t)command->offset;
            const uint32_t play_generation = command->token;
            if (!_music_service_play_generation_current(play_generation))
            {
                break;
            }
            music_playlist_snapshot_t *playlist = NULL;
            const esp_err_t fetch_result = music_net_fetch_playlist(
                                               &command->playlist, 0U,
                                               MUSIC_SERVICE_MAX_SONGS,
                                               &playlist);
            if (fetch_result != ESP_OK || playlist == NULL ||
                    playlist->count == 0U)
            {
                music_service_handle_network_result(
                    fetch_result == ESP_OK ? ESP_ERR_NOT_FOUND :
                    fetch_result);
                music_snapshot_release(playlist);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                _music_service_player_error(s_music.status.failure);
                break;
            }
            if (!_music_service_play_generation_current(play_generation))
            {
                music_snapshot_release(playlist);
                break;
            }
            if (requested_index >= playlist->count)
            {
                music_snapshot_release(playlist);
                music_service_handle_network_result(ESP_ERR_INVALID_ARG);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                _music_service_player_error(MUSIC_SERVICE_FAILURE_API);
                break;
            }
            music_queue_snapshot_t *queue =
                _music_service_queue_from_playlist(playlist, playlist->count);
            music_snapshot_release(playlist);
            if (queue == NULL)
            {
                music_service_handle_network_result(ESP_ERR_NO_MEM);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                _music_service_player_error(MUSIC_SERVICE_FAILURE_INTERNAL);
                break;
            }
            music_service_replace_queue(queue);
            music_service_lock();
            s_music.play_valid = true;
            music_service_unlock();
            (void)_music_service_start_queue_index(queue, requested_index,
                                                   play_generation);
            break;
        }
        case MUSIC_API_CMD_PLAY_SNAPSHOT:
        {
            const uint16_t requested_index = (uint16_t)command->offset;
            const uint32_t play_generation = command->token;
            music_playlist_snapshot_t *playlist = command->playlist_snapshot;
            if (!_music_service_play_generation_current(play_generation))
            {
                music_snapshot_release(playlist);
                break;
            }
            if (playlist == NULL || playlist->count == 0U ||
                    requested_index >= playlist->count)
            {
                music_snapshot_release(playlist);
                music_service_handle_network_result(ESP_ERR_INVALID_ARG);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                _music_service_player_error(MUSIC_SERVICE_FAILURE_API);
                break;
            }
            music_queue_snapshot_t *queue =
                _music_service_queue_from_playlist(playlist, playlist->count);
            music_snapshot_release(playlist);
            if (queue == NULL)
            {
                music_service_handle_network_result(ESP_ERR_NO_MEM);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                _music_service_player_error(MUSIC_SERVICE_FAILURE_INTERNAL);
                break;
            }
            music_service_replace_queue(queue);
            music_service_lock();
            s_music.play_valid = true;
            music_service_unlock();
            (void)_music_service_start_queue_index(queue, requested_index,
                                                   play_generation);
            break;
        }
        case MUSIC_API_CMD_QUEUE_PLAY:
        {
            const uint16_t index = (uint16_t)command->offset;
            const uint32_t play_generation = command->token;
            if (play_generation == 0U ||
                    !_music_service_play_generation_current(play_generation))
            {
                break;
            }
            music_service_lock();
            music_queue_snapshot_t *queue =
                music_snapshot_retain(s_music.queue);
            music_service_unlock();
            if (queue == NULL || index >= queue->count ||
                    (command->queue_generation != 0U &&
                     queue->generation != command->queue_generation))
            {
                music_snapshot_release(queue);
                music_service_handle_network_result(ESP_ERR_INVALID_STATE);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            music_service_lock();
            s_music.play_valid = true;
            music_service_unlock();
            (void)_music_service_start_queue_index(queue, index,
                                                   play_generation);
            music_snapshot_release(queue);
            break;
        }
        case MUSIC_API_CMD_QUEUE_REMOVE:
        {
            const uint16_t removed = (uint16_t)command->offset;
            const uint32_t play_generation = command->token;
            if (play_generation != 0U &&
                    !_music_service_play_generation_current(play_generation))
            {
                break;
            }
            music_service_lock();
            music_queue_snapshot_t *queue =
                music_snapshot_retain(s_music.queue);
            const uint16_t current = s_music.player.queue_index;
            const bool play_valid = s_music.play_valid;
            const music_playback_state_t player_state = s_music.player.state;
            music_service_unlock();
            music_queue_transition_t transition;
            if (queue == NULL ||
                    (command->queue_generation != 0U &&
                     queue->generation != command->queue_generation) ||
                    music_queue_after_remove(queue->count, current, removed,
                                             &transition) != ESP_OK)
            {
                music_snapshot_release(queue);
                music_service_handle_network_result(ESP_ERR_INVALID_STATE);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            music_queue_snapshot_t *next =
                _music_service_queue_without(queue, removed);
            music_snapshot_release(queue);
            if (next == NULL)
            {
                music_service_handle_network_result(ESP_ERR_NO_MEM);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                break;
            }
            music_service_replace_queue(next);
            if (transition.count == 0U)
            {
                (void)music_stream_stop();
                atomic_store_explicit(&s_playing, false,
                                      memory_order_release);
                music_service_lock();
                s_music.player.state = MUSIC_PLAYBACK_IDLE;
                s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
                s_music.player.position_ms = 0U;
                s_music.player.duration_ms = 0U;
                s_music.player.queue_index = 0U;
                s_music.player.queue_count = 0U;
                s_music.player.title[0] = '\0';
                s_music.player.artist[0] = '\0';
                s_music.player.cover_url[0] = '\0';
                s_music.player.songmid[0] = '\0';
                s_music.player.play_generation = 0U;
                s_music.play_valid = false;
                music_service_unlock();
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER,
                                      0U);
                break;
            }
            music_service_lock();
            s_music.player.queue_index = transition.index;
            s_music.player.queue_count = transition.count;
            music_service_unlock();
            if (transition.restart && play_valid &&
                    (player_state == MUSIC_PLAYBACK_PLAYING ||
                     player_state == MUSIC_PLAYBACK_BUFFERING))
            {
                (void)_music_service_start_queue_index(
                    next, transition.index, play_generation);
            }
            else if (transition.restart && play_valid &&
                     player_state == MUSIC_PLAYBACK_PAUSED)
            {
                (void)music_stream_stop();
                atomic_store_explicit(&s_playing, false,
                                      memory_order_release);
                music_service_lock();
                s_music.player.state = MUSIC_PLAYBACK_IDLE;
                s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
                s_music.player.position_ms = 0U;
                s_music.player.duration_ms = 0U;
                s_music.player.title[0] = '\0';
                s_music.player.artist[0] = '\0';
                s_music.player.cover_url[0] = '\0';
                s_music.player.songmid[0] = '\0';
                s_music.player.play_generation = 0U;
                s_music.play_valid = false;
                music_service_unlock();
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER,
                                      0U);
            }
            else
            {
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER,
                                      0U);
            }
            break;
        }
        case MUSIC_API_CMD_PAUSE:
        {
            music_stream_status_t stream_status;
            if (music_stream_get_status(&stream_status) == ESP_OK)
            {
                if (stream_status.state == MUSIC_PLAYBACK_PLAYING)
                {
                    if (music_stream_pause(true) == ESP_OK)
                    {
                        atomic_store_explicit(&s_playing, false,
                                              memory_order_release);
                    }
                }
                else if (stream_status.state == MUSIC_PLAYBACK_PAUSED)
                {
                    if (music_stream_pause(false) == ESP_OK)
                    {
                        atomic_store_explicit(&s_playing, true,
                                              memory_order_release);
                    }
                }
            }
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
    const size_t internal_before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    memset(&s_music, 0, sizeof(s_music));
    s_music.lock = xSemaphoreCreateMutex();
    /* PSRAM-backed queue; delete it with vQueueDeleteWithCaps(). */
    s_music.api_queue = xQueueCreateWithCaps(
                            CONFIG_MUSIC_SERVICE_API_QUEUE_DEPTH,
                            sizeof(music_api_command_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_music.login_queue = xQueueCreateWithCaps(
                              2U, sizeof(music_login_command_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const esp_err_t http_result = music_http_init();
    if (s_music.lock == NULL || s_music.api_queue == NULL ||
            s_music.login_queue == NULL || http_result != ESP_OK)
    {
        if (s_music.api_queue != NULL)
        {
            vQueueDeleteWithCaps(s_music.api_queue);
        }
        if (s_music.login_queue != NULL)
        {
            vQueueDeleteWithCaps(s_music.login_queue);
        }
        if (s_music.lock != NULL)
        {
            vSemaphoreDelete(s_music.lock);
        }
        music_http_deinit();
        memset(&s_music, 0, sizeof(s_music));
        return ESP_ERR_NO_MEM;
    }
    s_music.config = *config;
    s_music.status.state = MUSIC_SERVICE_STATE_WAITING_NETWORK;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.time_ready =
        time_service_get_quality() != TIME_SERVICE_QUALITY_INVALID;
    s_music.status.audio_available = audio_service_is_available();
    s_music.player.state = MUSIC_PLAYBACK_IDLE;
    s_music.player.quality = config->default_quality;
    s_music.initialized = true;
    atomic_store_explicit(&s_playing, false, memory_order_release);
    atomic_store_explicit(&s_login_cancel, false, memory_order_release);
    atomic_store_explicit(&s_login_generation, 0U, memory_order_release);
    atomic_store_explicit(&s_api_stop, false, memory_order_release);
    atomic_store_explicit(&s_api_exited, false, memory_order_release);
    atomic_store_explicit(&s_login_exited, true, memory_order_release);
    music_credential_blob_t credential;
    if (music_net_load_credential(&credential) == ESP_OK)
    {
        const bool expired =
            time_service_get_quality() != TIME_SERVICE_QUALITY_INVALID &&
            _credential_expired(&credential);
        if (expired)
        {
            (void)music_net_erase_credential();
        }
        else
        {
            s_music.credential = credential;
            s_music.credential_valid = true;
            s_music.login_type = credential.login_type == 2U ?
                                 MUSIC_LOGIN_TYPE_QQ :
                                 MUSIC_LOGIN_TYPE_WECHAT;
            s_music.status.login_type = s_music.login_type;
            s_music.status.logged_in = true;
        }
    }
    if (xTaskCreatePinnedToCore(_api_task, "music_api",
                                CONFIG_MUSIC_SERVICE_API_TASK_STACK_SIZE, NULL,
                                config->api_task_priority, &s_music.api_task,
                                0) != pdPASS)
    {
        (void)music_stream_deinit();
        s_music.initialized = false;
        vQueueDeleteWithCaps(s_music.api_queue);
        if (s_music.login_queue != NULL)
        {
            vQueueDeleteWithCaps(s_music.login_queue);
        }
        music_http_deinit();
        vSemaphoreDelete(s_music.lock);
        memset(&s_music, 0, sizeof(s_music));
        return ESP_ERR_NO_MEM;
    }
    const size_t internal_after =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    LOG_I("api queue item=%u B internal cost=%u B free=%u B largest=%u B",
          (unsigned)sizeof(music_api_command_t),
          (unsigned)(internal_before > internal_after ?
                     internal_before - internal_after : 0U),
          (unsigned)internal_after,
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

esp_err_t music_service_deinit(void)
{
    if (!s_music.initialized)
    {
        return ESP_OK;
    }
    atomic_store_explicit(&s_login_cancel, true, memory_order_release);
    (void)atomic_fetch_add_explicit(&s_login_generation, 1U,
                                    memory_order_acq_rel);
    atomic_store_explicit(&s_api_stop, true, memory_order_release);
    music_login_command_t login_wake = {0};
    if (s_music.login_queue != NULL)
    {
        (void)xQueueSend(s_music.login_queue, &login_wake,
                         pdMS_TO_TICKS(100U));
    }
    const esp_err_t stream_result = music_stream_deinit();
    if (stream_result != ESP_OK)
    {
        return stream_result;
    }
    for (uint32_t wait = 0U; wait < 300U; ++wait)
    {
        if (atomic_load_explicit(&s_api_exited, memory_order_acquire) &&
                atomic_load_explicit(&s_login_exited, memory_order_acquire))
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50U));
    }
    if (!atomic_load_explicit(&s_api_exited, memory_order_acquire))
    {
        LOG_E("API task did not stop cooperatively");
        return ESP_ERR_TIMEOUT;
    }
    if (!atomic_load_explicit(&s_login_exited, memory_order_acquire))
    {
        LOG_E("login task did not stop cooperatively");
        return ESP_ERR_TIMEOUT;
    }
    s_music.api_task = NULL;
    s_music.login_task = NULL;
    if (s_music.api_queue != NULL)
    {
        music_api_command_t pending;
        while (xQueueReceive(s_music.api_queue, &pending,
                             0U) == pdTRUE)
        {
            music_snapshot_release(pending.playlist_snapshot);
        }
        vQueueDeleteWithCaps(s_music.api_queue);
        s_music.api_queue = NULL;
    }
    if (s_music.login_queue != NULL)
    {
        vQueueDeleteWithCaps(s_music.login_queue);
        s_music.login_queue = NULL;
    }
    music_snapshot_release(s_music.home);
    music_snapshot_release(s_music.playlist);
    music_snapshot_release(s_music.queue);
    music_snapshot_release(s_music.lyric);
    music_service_login_qr_release(s_music.login_qr);
    music_service_image_release(s_music.image);
    music_net_clear_session();
    music_http_deinit();
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
    bool changed = false;
    s_music.status.network_ready = ready;
    const bool time_ready = time_service_get_quality() !=
                            TIME_SERVICE_QUALITY_INVALID;
    s_music.status.time_ready = time_ready;

    const music_service_state_t state = s_music.status.state;
    const bool preserving_login =
        s_music.login_active ||
        state == MUSIC_SERVICE_STATE_QR_READY ||
        state == MUSIC_SERVICE_STATE_QR_SCANNED ||
        state == MUSIC_SERVICE_STATE_QR_EXPIRED ||
        state == MUSIC_SERVICE_STATE_AUTHENTICATING;

    if (preserving_login)
    {
        const music_service_failure_t failure =
            ready ? MUSIC_SERVICE_FAILURE_NONE :
            MUSIC_SERVICE_FAILURE_NETWORK;
        if (s_music.status.failure != failure)
        {
            s_music.status.failure = failure;
            changed = true;
        }
    }
    else if (!ready)
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_WAITING_NETWORK,
                                       MUSIC_SERVICE_FAILURE_NETWORK);
        changed = true;
    }
    else if (!time_ready)
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_WAITING_TIME,
                                       MUSIC_SERVICE_FAILURE_TIME);
        changed = true;
    }
    else if (s_music.credential_valid)
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_READY,
                                       MUSIC_SERVICE_FAILURE_NONE);
        changed = true;
    }
    else
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_LOGGED_OUT,
                                       MUSIC_SERVICE_FAILURE_NONE);
        changed = true;
    }
    music_service_unlock();
    if (changed)
    {
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    }
    return ESP_OK;
}

esp_err_t music_service_set_time_ready(bool ready)
{
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    bool changed = false;
    if (s_music.status.time_ready != ready)
    {
        s_music.status.time_ready = ready;
        changed = true;
    }
    if (!ready &&
            (s_music.status.state == MUSIC_SERVICE_STATE_READY ||
             s_music.status.state == MUSIC_SERVICE_STATE_LOGGED_OUT))
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_WAITING_TIME,
                                       MUSIC_SERVICE_FAILURE_TIME);
        changed = true;
    }
    else if (ready && s_music.status.network_ready &&
             s_music.status.state == MUSIC_SERVICE_STATE_WAITING_TIME)
    {
        music_service_set_state_locked(s_music.credential_valid ?
                                       MUSIC_SERVICE_STATE_READY :
                                       MUSIC_SERVICE_STATE_LOGGED_OUT,
                                       MUSIC_SERVICE_FAILURE_NONE);
        changed = true;
    }
    bool expired = false;
    if (ready && s_music.credential_valid && !s_music.login_active &&
            _credential_expired(&s_music.credential))
    {
        s_music.credential_valid = false;
        s_music.status.logged_in = false;
        music_service_set_state_locked(MUSIC_SERVICE_STATE_AUTH_REQUIRED,
                                       MUSIC_SERVICE_FAILURE_AUTH);
        expired = true;
        changed = true;
    }
    music_service_unlock();
    if (expired)
    {
        const music_api_command_t erase =
        {
            .type = MUSIC_API_CMD_ERASE_CREDENTIAL,
        };
        if (s_music.api_queue == NULL ||
                xQueueSend(s_music.api_queue, &erase,
                           pdMS_TO_TICKS(100U)) != pdTRUE)
        {
            music_service_lock();
            s_music.credential_erase_pending = true;
            music_service_unlock();
        }
    }
    if (changed)
    {
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    }
    return ESP_OK;
}

esp_err_t music_service_retry(void)
{
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.login_active)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    music_service_state_t state;
    if (!s_music.status.network_ready)
    {
        state = MUSIC_SERVICE_STATE_WAITING_NETWORK;
    }
    else if (!s_music.status.time_ready)
    {
        state = MUSIC_SERVICE_STATE_WAITING_TIME;
    }
    else if (s_music.credential_valid)
    {
        state = MUSIC_SERVICE_STATE_READY;
    }
    else
    {
        state = MUSIC_SERVICE_STATE_LOGGED_OUT;
    }
    music_service_set_state_locked(state, MUSIC_SERVICE_FAILURE_NONE);
    music_service_unlock();
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    return ESP_OK;
}

esp_err_t music_service_login_begin(music_login_type_t type)
{
    if (type != MUSIC_LOGIN_TYPE_WECHAT && type != MUSIC_LOGIN_TYPE_QQ)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_music.initialized || s_music.login_queue == NULL)
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
    uint32_t login_session = 0U;
    music_image_t *old_qr = NULL;
    if (allowed)
    {
        s_music.login_active = true;
        s_music.login_type = type;
        s_music.status.login_type = type;
        old_qr = s_music.login_qr;
        s_music.login_qr = NULL;
        s_music.status.qr_available = false;
        login_session = atomic_fetch_add_explicit(
                            &s_login_generation, 1U,
                            memory_order_acq_rel) + 1U;
        s_music.login_session = login_session;
        atomic_store_explicit(&s_login_cancel, false, memory_order_release);
    }
    music_service_unlock();
    music_service_login_qr_release(old_qr);
    if (!allowed)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_music.login_task == NULL)
    {
        atomic_store_explicit(&s_login_exited, false, memory_order_release);
        if (xTaskCreatePinnedToCore(_login_task, "music_login",
                                    CONFIG_MUSIC_SERVICE_API_TASK_STACK_SIZE,
                                    NULL, s_music.config.api_task_priority,
                                    &s_music.login_task, 0) != pdPASS)
        {
            atomic_store_explicit(&s_login_exited, true,
                                  memory_order_release);
            music_service_lock();
            s_music.login_active = false;
            music_service_unlock();
            return ESP_ERR_NO_MEM;
        }
    }

    const music_login_command_t command =
    {
        .type = type,
        .session = login_session,
    };
    if (xQueueSend(s_music.login_queue, &command, pdMS_TO_TICKS(100U)) !=
            pdTRUE)
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
    music_image_t *qr = music_snapshot_retain(s_music.login_qr);
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
    music_snapshot_release((void *)image);
}

esp_err_t music_service_login_cancel(void)
{
    atomic_store_explicit(&s_login_cancel, true, memory_order_release);
    (void)atomic_fetch_add_explicit(&s_login_generation, 1U,
                                    memory_order_acq_rel);
    music_image_t *qr = NULL;
    if (music_service_lock() == ESP_OK)
    {
        qr = s_music.login_qr;
        s_music.login_qr = NULL;
        s_music.login_active = false;
        if (s_music.status.state == MUSIC_SERVICE_STATE_QR_READY ||
                s_music.status.state == MUSIC_SERVICE_STATE_QR_SCANNED ||
                s_music.status.state == MUSIC_SERVICE_STATE_QR_EXPIRED ||
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
    (void)atomic_fetch_add_explicit(&s_login_generation, 1U,
                                    memory_order_acq_rel);
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
    music_home_snapshot_t *home = music_snapshot_retain(s_music.home);
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
    music_snapshot_release((void *)snapshot);
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
        if (music_service_playlist_cache_matches(playlist, ref, offset,
                count))
        {
            playlist = music_snapshot_retain(playlist);
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
    music_snapshot_release((void *)snapshot);
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
            lyric = music_snapshot_retain(lyric);
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
    music_snapshot_release((void *)snapshot);
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
        if (music_service_image_cache_matches(current, url, width, height,
                                              token))
        {
            current = music_snapshot_retain(current);
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
    music_snapshot_release((void *)image);
}

esp_err_t music_service_queue_acquire(const music_queue_snapshot_t **snapshot)
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
    const music_queue_snapshot_t *queue =
        music_snapshot_retain(s_music.queue);
    music_service_unlock();
    if (queue == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = queue;
    return ESP_OK;
}

void music_service_queue_release(const music_queue_snapshot_t *snapshot)
{
    music_snapshot_release((void *)snapshot);
}

static uint32_t _music_service_next_play_generation_locked(void)
{
    uint32_t generation = ++s_music.play_generation;
    if (generation == 0U)
    {
        generation = ++s_music.play_generation;
    }
    return generation;
}

esp_err_t music_service_play_playlist(const music_playlist_ref_t *ref,
                                      uint16_t start_index)
{
    if (ref == NULL || ref->id == 0U || s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t previous_play_generation = s_music.play_generation;
    const uint32_t previous_player_generation =
        s_music.player.play_generation;
    const uint32_t play_generation =
        _music_service_next_play_generation_locked();
    s_music.player.play_generation = play_generation;
    music_service_unlock();
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_PLAY,
        .playlist = *ref,
        .offset = start_index,
        .token = play_generation,
    };
    if (xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        music_service_lock();
        s_music.play_generation = previous_play_generation;
        s_music.player.play_generation = previous_player_generation;
        music_service_unlock();
        return ESP_ERR_TIMEOUT;
    }
    music_service_lock();
    if (s_music.play_generation == play_generation)
    {
        s_music.player.state = MUSIC_PLAYBACK_BUFFERING;
        s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
        atomic_store_explicit(&s_playing, true, memory_order_release);
        music_service_unlock();
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    }
    else
    {
        music_service_unlock();
    }
    return ESP_OK;
}

esp_err_t music_service_play_playlist_snapshot(
    const music_playlist_snapshot_t *playlist, uint16_t start_index)
{
    if (playlist == NULL || playlist->count == 0U ||
            start_index >= playlist->count || s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (music_snapshot_retain((void *)playlist) == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (music_service_lock() != ESP_OK)
    {
        music_snapshot_release((void *)playlist);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t previous_play_generation = s_music.play_generation;
    const uint32_t previous_player_generation =
        s_music.player.play_generation;
    const uint32_t play_generation =
        _music_service_next_play_generation_locked();
    s_music.player.play_generation = play_generation;
    music_service_unlock();
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_PLAY_SNAPSHOT,
        .playlist_snapshot = (music_playlist_snapshot_t *)playlist,
        .offset = start_index,
        .token = play_generation,
    };
    if (xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        music_snapshot_release((void *)playlist);
        music_service_lock();
        s_music.play_generation = previous_play_generation;
        s_music.player.play_generation = previous_player_generation;
        music_service_unlock();
        return ESP_ERR_TIMEOUT;
    }
    music_service_lock();
    if (s_music.play_generation == play_generation)
    {
        s_music.player.state = MUSIC_PLAYBACK_BUFFERING;
        s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
        atomic_store_explicit(&s_playing, true, memory_order_release);
        music_service_unlock();
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    }
    else
    {
        music_service_unlock();
    }
    return ESP_OK;
}

esp_err_t music_service_queue_play(uint16_t index)
{
    if (s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.queue == NULL || index >= s_music.queue->count)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t queue_generation = s_music.queue->generation;
    const uint32_t previous_play_generation = s_music.play_generation;
    const uint32_t previous_player_generation =
        s_music.player.play_generation;
    const uint32_t play_generation =
        _music_service_next_play_generation_locked();
    s_music.player.play_generation = play_generation;
    music_service_unlock();
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_QUEUE_PLAY,
        .queue_generation = queue_generation,
        .offset = index,
        .token = play_generation,
    };
    if (xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        music_service_lock();
        s_music.play_generation = previous_play_generation;
        s_music.player.play_generation = previous_player_generation;
        music_service_unlock();
        return ESP_ERR_TIMEOUT;
    }
    music_service_lock();
    if (s_music.play_generation == play_generation)
    {
        s_music.player.state = MUSIC_PLAYBACK_BUFFERING;
        s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
        atomic_store_explicit(&s_playing, true, memory_order_release);
        music_service_unlock();
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    }
    else
    {
        music_service_unlock();
    }
    return ESP_OK;
}

esp_err_t music_service_queue_remove(uint16_t index)
{
    if (s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.queue == NULL || index >= s_music.queue->count)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t queue_generation = s_music.queue->generation;
    const uint32_t previous_play_generation = s_music.play_generation;
    const uint32_t previous_player_generation =
        s_music.player.play_generation;
    uint32_t play_generation = s_music.player.play_generation;
    if (index == s_music.player.queue_index)
    {
        play_generation = _music_service_next_play_generation_locked();
        s_music.player.play_generation = play_generation;
    }
    music_service_unlock();
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_QUEUE_REMOVE,
        .queue_generation = queue_generation,
        .offset = index,
        .token = play_generation,
    };
    if (xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        music_service_lock();
        s_music.play_generation = previous_play_generation;
        s_music.player.play_generation = previous_player_generation;
        music_service_unlock();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t music_service_play_pause(void)
{
    if (s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_PAUSE,
    };
    return xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) ==
           pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t music_service_next(void)
{
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool valid = s_music.play_valid;
    const uint32_t count = s_music.player.queue_count;
    const uint32_t index = s_music.player.queue_index;
    const music_play_mode_t mode = s_music.player.mode;
    music_service_unlock();
    if (!valid || count == 0U || index >= count || index >= UINT16_MAX)
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
    return music_service_queue_play((uint16_t)target);
}

esp_err_t music_service_prev(void)
{
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool valid = s_music.play_valid;
    const uint32_t count = s_music.player.queue_count;
    const uint32_t index = s_music.player.queue_index;
    const music_play_mode_t mode = s_music.player.mode;
    music_service_unlock();
    if (!valid || count == 0U || index >= count || index > UINT16_MAX)
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
    return music_service_queue_play((uint16_t)target);
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
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t next_generation = ++s_music.play_generation;
    if (next_generation == 0U)
    {
        next_generation = ++s_music.play_generation;
    }
    s_music.play_valid = false;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.player.play_generation = 0U;
    s_music.player.state = MUSIC_PLAYBACK_IDLE;
    s_music.player.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.player.position_ms = 0U;
    s_music.player.duration_ms = 0U;
    s_music.player.title[0] = '\0';
    s_music.player.artist[0] = '\0';
    s_music.player.cover_url[0] = '\0';
    s_music.player.songmid[0] = '\0';
    s_music.player.queue_index = 0U;
    s_music.player.queue_count = 0U;
    music_service_unlock();
    (void)music_stream_stop();
    atomic_store_explicit(&s_playing, false, memory_order_release);
    music_service_replace_queue(NULL);
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
    const uint32_t play_generation = s_music.player.play_generation;
    music_service_unlock();
    music_stream_status_t stream_status;
    if (play_valid && music_stream_get_status(&stream_status) == ESP_OK &&
            stream_status.generation == play_generation)
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


esp_err_t music_service_get_diagnostics(music_service_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.api_task != NULL)
    {
        diagnostics->api_stack_high_water =
            (uint32_t)uxTaskGetStackHighWaterMark(s_music.api_task);
    }
    if (s_music.api_queue != NULL)
    {
        diagnostics->api_queue_waiting =
            (uint32_t)uxQueueMessagesWaiting(s_music.api_queue);
    }
    music_service_unlock();
    diagnostics->stream_stack_high_water =
        music_stream_get_stack_high_water();
    diagnostics->stream_initialized = music_stream_is_playing() ||
                                      diagnostics->stream_stack_high_water != 0U;
    return ESP_OK;
}

void music_service_stream_failed(music_service_failure_t failure)
{
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    s_music.player.state = MUSIC_PLAYBACK_ERROR;
    s_music.player.failure = failure;
    s_music.status.failure = failure;
    s_music.play_valid = false;
    s_music.player.play_generation = 0U;
    music_service_unlock();
    atomic_store_explicit(&s_playing, false, memory_order_release);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
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
    return atomic_load_explicit(&s_playing, memory_order_acquire) ||
           music_stream_is_playing();
}
