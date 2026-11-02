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

static void _music_service_set_account_locked(
    const music_credential_blob_t *credential)
{
    s_music.profile_loaded = false;
    s_music.status.user_id[0] = '\0';
    s_music.status.user_name[0] = '\0';
    s_music.status.user_avatar[0] = '\0';
    if (credential != NULL)
    {
        (void)snprintf(s_music.status.user_id,
                       sizeof(s_music.status.user_id), "%s",
                       credential->musicid);
        if (credential->login_type == 2U)
        {
            (void)snprintf(s_music.status.user_avatar,
                           sizeof(s_music.status.user_avatar),
                           "https://q.qlogo.cn/headimg_dl?dst_uin=%s&spec=140",
                           credential->musicid);
        }
    }
}

static void _music_service_refresh_profile(void)
{
    bool needed = false;
    if (music_service_lock() == ESP_OK)
    {
        needed = s_music.credential_valid && !s_music.profile_loaded;
        music_service_unlock();
    }
    if (!needed)
    {
        return;
    }
    char account[MUSIC_SERVICE_USER_ID_BYTES] = {0};
    char name[MUSIC_SERVICE_CREATOR_BYTES] = {0};
    char avatar[MUSIC_SERVICE_URL_BYTES] = {0};
    const esp_err_t result = music_net_fetch_profile(
                                 account, sizeof(account),
                                 name, sizeof(name), avatar, sizeof(avatar));
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND)
    {
        return;
    }
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    if (!s_music.credential_valid ||
            strcmp(s_music.credential.musicid, account) != 0)
    {
        music_service_unlock();
        return;
    }
    bool changed = false;
    if (result == ESP_OK)
    {
        if (name[0] != '\0' &&
                strcmp(s_music.status.user_name, name) != 0)
        {
            (void)snprintf(s_music.status.user_name,
                           sizeof(s_music.status.user_name), "%s", name);
            changed = true;
        }
        if (avatar[0] != '\0' &&
                strcmp(s_music.status.user_avatar, avatar) != 0)
        {
            (void)snprintf(s_music.status.user_avatar,
                           sizeof(s_music.status.user_avatar), "%s", avatar);
            changed = true;
        }
    }
    s_music.profile_loaded = true;
    if (changed)
    {
        s_music.status.generation = ++s_music.generation;
    }
    music_service_unlock();
    if (changed)
    {
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    }
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
            _music_service_set_account_locked(NULL);
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

static size_t _music_service_playlist_cache_slot_locked(
    const music_playlist_snapshot_t *snapshot)
{
    size_t victim = 0U;
    for (size_t slot = 0U; slot < MUSIC_PLAYLIST_CACHE_SLOTS; ++slot)
    {
        if (s_music.playlist[slot] == NULL)
        {
            victim = slot;
            continue;
        }
        if (music_service_playlist_cache_matches(
                    s_music.playlist[slot], &snapshot->playlist.ref,
                    snapshot->offset, snapshot->count))
        {
            return slot;
        }
    }
    return victim;
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
    size_t slot = _music_service_playlist_cache_slot_locked(snapshot);
    const music_playlist_snapshot_t *old = s_music.playlist[slot];
    s_music.playlist[slot] = snapshot;
    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
    s_music.status.generation = ++s_music.generation;
    s_music.player.generation = s_music.status.generation;
    music_service_unlock();
    music_snapshot_release((void *)old);
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
    music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                 MUSIC_SERVICE_FAILURE_NONE);
}

void music_service_set_login_qr(music_image_t *image, uint32_t session)
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
    if (!s_music.login_active || s_music.login_session != session ||
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
    LOG_I("worker stack high water=%u B phase=%s",
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

static music_queue_snapshot_t *_music_service_queue_alloc(
    const music_playlist_ref_t *ref, uint16_t total, uint16_t offset,
    uint16_t count)
{
    if (ref == NULL || count == 0U || total == 0U || offset >= total ||
            count > (uint16_t)(total - offset))
    {
        return NULL;
    }
    const size_t bytes = offsetof(music_queue_snapshot_t, songs) +
                         (size_t)count * sizeof(music_song_t);
    music_queue_snapshot_t *queue = music_snapshot_alloc(bytes, NULL);
    if (queue != NULL)
    {
        queue->generation = music_service_next_generation();
        queue->ref = *ref;
        queue->total = total;
        queue->offset = offset;
        queue->count = count;
    }
    return queue;
}

static uint8_t *_music_service_queue_removed_copy(void)
{
    uint8_t *copy = heap_caps_malloc(MUSIC_SERVICE_QUEUE_REMOVAL_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL)
    {
        return NULL;
    }
    if (music_service_lock() != ESP_OK)
    {
        heap_caps_free(copy);
        return NULL;
    }
    memcpy(copy, s_music.queue_removed, MUSIC_SERVICE_QUEUE_REMOVAL_BYTES);
    music_service_unlock();
    return copy;
}

static music_queue_snapshot_t *_music_service_queue_from_playlist_snapshot(
    const music_playlist_snapshot_t *playlist)
{
    if (playlist == NULL || playlist->count == 0U)
    {
        return NULL;
    }
    uint32_t total = playlist->total;
    if (total == 0U)
    {
        total = (uint32_t)playlist->offset + playlist->count;
    }
    if (total > MUSIC_SERVICE_MAX_QUEUE_TRACKS ||
            playlist->offset >= total)
    {
        return NULL;
    }
    uint16_t count = playlist->count;
    if ((uint32_t)playlist->offset + count > total)
    {
        count = (uint16_t)(total - playlist->offset);
    }
    music_queue_snapshot_t *queue = _music_service_queue_alloc(
                                        &playlist->playlist.ref,
                                        (uint16_t)total, playlist->offset,
                                        count);
    if (queue != NULL)
    {
        memcpy(queue->songs, playlist->songs,
               (size_t)count * sizeof(queue->songs[0]));
    }
    return queue;
}

/**
 * @brief Materialize a logical queue range from the source playlist.
 *
 * The provider is paged by source playlist offset. Removed source entries are
 * filtered before they enter the returned snapshot.
 */
static esp_err_t _music_service_queue_build_view(
    const music_playlist_ref_t *ref, uint16_t source_total,
    const uint8_t *removed, uint16_t removed_count, uint16_t total,
    uint16_t offset, uint16_t count, music_queue_snapshot_t **snapshot)
{
    if (ref == NULL || removed == NULL || snapshot == NULL || count == 0U ||
            offset >= total || total > source_total)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    if (count > (uint16_t)(total - offset))
    {
        count = (uint16_t)(total - offset);
    }
    uint16_t source_cursor = 0U;
    if (!music_queue_logical_to_source(source_total, removed, offset,
                                       &source_cursor))
    {
        return ESP_ERR_INVALID_ARG;
    }
    music_queue_snapshot_t *view = _music_service_queue_alloc(
                                       ref, total, offset, count);
    if (view == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    uint16_t produced = 0U;
    uint32_t pages = 0U;
    while (produced < count && source_cursor < source_total)
    {
        uint32_t fetch = (uint32_t)(count - produced) + removed_count;
        if (fetch > MUSIC_SERVICE_MAX_SONGS)
        {
            fetch = MUSIC_SERVICE_MAX_SONGS;
        }
        music_playlist_snapshot_t *page = NULL;
        const esp_err_t fetch_result = music_net_fetch_playlist(
                                           ref, source_cursor,
                                           (uint16_t)fetch, &page);
        if (fetch_result != ESP_OK)
        {
            music_snapshot_release(view);
            return fetch_result;
        }
        if (page == NULL || page->count == 0U)
        {
            const uint32_t page_total = page != NULL ? page->total : 0U;
            music_snapshot_release(page);
            if (page_total <= source_cursor ||
                    source_cursor >= source_total)
            {
                break;
            }
            music_snapshot_release(view);
            return ESP_ERR_INVALID_RESPONSE;
        }
        for (uint16_t index = 0U; index < page->count &&
                produced < count; ++index)
        {
            const uint32_t source_index = (uint32_t)page->offset + index;
            if (source_index >= source_total)
            {
                break;
            }
            if (music_queue_source_is_removed(removed, source_total,
                                              (uint16_t)source_index))
            {
                continue;
            }
            view->songs[produced++] = page->songs[index];
        }
        source_cursor = (uint16_t)(page->offset + page->count);
        music_snapshot_release(page);
        if (++pages > 32U)
        {
            music_snapshot_release(view);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (produced == 0U ||
            ((uint32_t)offset + produced < total &&
             source_cursor >= source_total))
    {
        music_snapshot_release(view);
        return ESP_ERR_INVALID_RESPONSE;
    }
    view->count = produced;
    *snapshot = view;
    return ESP_OK;
}

static music_queue_snapshot_t *_music_service_detach_queue_view_locked(void)
{
    music_queue_snapshot_t *old = s_music.queue_view;
    s_music.queue_view = NULL;
    s_music.queue_view_offset = 0U;
    s_music.queue_view_count = 0U;
    s_music.queue_view_pending_offset = 0U;
    s_music.queue_view_pending_count = 0U;
    s_music.queue_view_epoch = 0U;
    s_music.queue_view_pending = false;
    return old;
}

static void _music_service_queue_install_source(music_queue_snapshot_t *queue,
        uint16_t source_total)
{
    if (queue == NULL)
    {
        return;
    }
    music_queue_snapshot_t *old_queue = NULL;
    music_queue_snapshot_t *old_view = NULL;
    if (music_service_lock() != ESP_OK)
    {
        music_snapshot_release(queue);
        return;
    }
    old_queue = s_music.queue;
    old_view = _music_service_detach_queue_view_locked();
    s_music.queue = queue;
    s_music.queue_source_total = source_total;
    s_music.queue_total = source_total;
    s_music.queue_removed_count = 0U;
    memset(s_music.queue_removed, 0, sizeof(s_music.queue_removed));
    ++s_music.queue_epoch;
    if (s_music.queue_epoch == 0U)
    {
        ++s_music.queue_epoch;
    }
    music_service_unlock();
    music_snapshot_release(old_queue);
    music_snapshot_release(old_view);
    music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                 MUSIC_SERVICE_FAILURE_NONE);
}

void music_service_replace_queue_view(music_queue_snapshot_t *snapshot)
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
    if (s_music.queue == NULL || snapshot->total != s_music.queue_total)
    {
        music_service_unlock();
        music_snapshot_release(snapshot);
        return;
    }
    music_queue_snapshot_t *old = s_music.queue_view;
    s_music.queue_view = snapshot;
    s_music.queue_view_offset = snapshot->offset;
    s_music.queue_view_count = snapshot->count;
    s_music.queue_view_pending_offset = 0U;
    s_music.queue_view_pending_count = 0U;
    s_music.queue_view_epoch = s_music.queue_epoch;
    s_music.queue_view_pending = false;
    music_service_unlock();
    music_snapshot_release(old);
    music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                 MUSIC_SERVICE_FAILURE_NONE);
}

static void _music_service_set_queue_player(const music_song_t *song,
        uint16_t index, uint16_t total, uint32_t play_generation)
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
    s_music.player.queue_count = total;
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
    if (queue == NULL || !music_queue_window_contains(queue->offset,
            queue->count, index) ||
            play_generation == 0U || !_music_service_playback_current(play_generation))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t local_index = (uint16_t)(index - queue->offset);
    const music_song_t *song = &queue->songs[local_index];
    (void)music_stream_stop();
    _music_service_set_queue_player(song, index, queue->total,
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

static void _music_service_report_start_failure(esp_err_t result,
        uint32_t play_generation)
{
    if (result == ESP_OK ||
            !_music_service_play_generation_current(play_generation))
    {
        return;
    }
    music_service_handle_network_result(result);
    _music_service_player_error(_music_service_failure_from_result(result));
}

static void _music_service_replace_queue_if_current(
    music_queue_snapshot_t *queue, uint64_t queue_epoch,
    uint32_t play_generation)
{
    if (queue == NULL)
    {
        return;
    }
    if (music_service_lock() != ESP_OK)
    {
        music_snapshot_release(queue);
        return;
    }
    if (!s_music.play_valid || s_music.play_generation != play_generation ||
            s_music.queue_epoch != queue_epoch)
    {
        music_service_unlock();
        music_snapshot_release(queue);
        return;
    }
    music_queue_snapshot_t *old = s_music.queue;
    s_music.queue = queue;
    music_service_unlock();
    music_snapshot_release(old);
    music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                 MUSIC_SERVICE_FAILURE_NONE);
}

static void _music_service_queue_prefetch_window(uint32_t play_generation)
{
    music_playlist_ref_t ref = {0};
    uint16_t source_total = 0U;
    uint16_t total = 0U;
    uint16_t removed_count = 0U;
    uint16_t index = 0U;
    uint64_t queue_epoch = 0U;
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    if (!s_music.play_valid || s_music.queue == NULL ||
            s_music.play_generation != play_generation)
    {
        music_service_unlock();
        return;
    }
    ref = s_music.queue->ref;
    source_total = s_music.queue_source_total;
    total = s_music.queue_total;
    removed_count = s_music.queue_removed_count;
    index = s_music.player.queue_index;
    queue_epoch = s_music.queue_epoch;
    const uint16_t window_end = (uint16_t)(s_music.queue->offset +
                                           s_music.queue->count);
    if (window_end >= total || index + 5U < window_end)
    {
        music_service_unlock();
        return;
    }
    music_service_unlock();

    uint8_t *removed_copy = _music_service_queue_removed_copy();
    if (removed_copy == NULL)
    {
        LOG_W("queue prefetch removal snapshot unavailable");
        return;
    }
    const uint16_t focus = index + 5U < total ? (uint16_t)(index + 5U) : index;
    const uint16_t offset = music_queue_window_start(
                                focus, total, MUSIC_SERVICE_QUEUE_WINDOW_SONGS,
                                MUSIC_SERVICE_QUEUE_FETCH_PAGE);
    const uint16_t count = music_queue_window_count(
                               total, offset, MUSIC_SERVICE_QUEUE_WINDOW_SONGS);
    music_queue_snapshot_t *window = NULL;
    const esp_err_t result = _music_service_queue_build_view(
                                 &ref, source_total, removed_copy,
                                 removed_count, total, offset, count, &window);
    heap_caps_free(removed_copy);
    if (result != ESP_OK)
    {
        LOG_W("queue prefetch failed: %s", esp_err_to_name(result));
        return;
    }
    _music_service_replace_queue_if_current(window, queue_epoch,
                                            play_generation);
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

    const esp_err_t begin_result = music_net_login_begin(type, session);
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
        _music_service_set_account_locked(&credential);
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
    LOG_I("login task stack high water=%u B",
          (unsigned)uxTaskGetStackHighWaterMark(NULL));
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
            music_playlist_snapshot_t *old_playlists[MUSIC_PLAYLIST_CACHE_SLOTS] =
            {0};
            music_queue_snapshot_t *old_queue = NULL;
            music_queue_snapshot_t *old_queue_view = NULL;
            music_lyric_snapshot_t *old_lyric = NULL;
            music_image_t *old_image = NULL;
            music_image_t *old_qr = NULL;
            music_service_lock();
            memset(&s_music.credential, 0, sizeof(s_music.credential));
            s_music.credential_valid = false;
            s_music.login_active = false;
            s_music.login_type = MUSIC_LOGIN_TYPE_WECHAT;
            s_music.status.logged_in = false;
            _music_service_set_account_locked(NULL);
            s_music.status.qr_status[0] = '\0';
            s_music.status.qr_available = false;
            s_music.status.login_type = MUSIC_LOGIN_TYPE_WECHAT;
            old_home = s_music.home;
            memcpy(old_playlists, s_music.playlist, sizeof(old_playlists));
            old_queue = s_music.queue;
            old_queue_view = _music_service_detach_queue_view_locked();
            old_lyric = s_music.lyric;
            old_image = s_music.image;
            old_qr = s_music.login_qr;
            s_music.home = NULL;
            memset(s_music.playlist, 0, sizeof(s_music.playlist));
            s_music.queue = NULL;
            s_music.queue_total = 0U;
            s_music.queue_source_total = 0U;
            s_music.queue_removed_count = 0U;
            memset(s_music.queue_removed, 0, sizeof(s_music.queue_removed));
            ++s_music.queue_epoch;
            if (s_music.queue_epoch == 0U)
            {
                ++s_music.queue_epoch;
            }
            s_music.lyric = NULL;
            s_music.lyric_songmid[0] = '\0';
            s_music.lyric_request_songmid[0] = '\0';
            s_music.lyric_request_id = 0U;
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
            for (size_t slot = 0U; slot < MUSIC_PLAYLIST_CACHE_SLOTS; ++slot)
            {
                music_snapshot_release(old_playlists[slot]);
            }
            music_snapshot_release(old_queue);
            music_snapshot_release(old_queue_view);
            music_snapshot_release(old_lyric);
            music_service_image_release(old_image);
            music_service_login_qr_release(old_qr);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
            music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                         MUSIC_SERVICE_FAILURE_NONE);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
            break;
        }
        case MUSIC_API_CMD_REFRESH_PROFILE:
        {
            _music_service_refresh_profile();
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
            _music_service_refresh_profile();
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
                const music_service_failure_t failure =
                    _music_service_failure_from_result(result);
                music_service_handle_network_result(result);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                music_service_publish_detail(
                    MUSIC_SERVICE_MSG_SUB_TYPE_PLAYLIST, 0U, failure);
            }
            break;
        }
        case MUSIC_API_CMD_LYRIC:
        {
            music_lyric_snapshot_t *snapshot = NULL;
            const esp_err_t result = music_net_fetch_lyric(
                                         command->songmid, &snapshot);
            const music_service_failure_t failure =
                _music_service_failure_from_result(result);
            music_lyric_snapshot_t *old_lyric = NULL;
            bool committed = false;
            music_service_lock();
            if (s_music.lyric_request_id == command->token &&
                    strcmp(s_music.lyric_request_songmid,
                           command->songmid) == 0)
            {
                committed = true;
                old_lyric = s_music.lyric;
                if (result == ESP_OK)
                {
                    s_music.lyric = snapshot;
                    snapshot = NULL;
                    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
                }
                else
                {
                    s_music.lyric = NULL;
                    s_music.status.failure = MUSIC_SERVICE_FAILURE_NONE;
                }
                (void)snprintf(s_music.lyric_songmid,
                               sizeof(s_music.lyric_songmid), "%s",
                               command->songmid);
                s_music.lyric_request_songmid[0] = '\0';
                s_music.lyric_request_id = 0U;
                s_music.status.generation = ++s_music.generation;
                s_music.player.generation = s_music.status.generation;
            }
            music_service_unlock();
            music_snapshot_release(old_lyric);
            music_snapshot_release(snapshot);
            if (!committed)
            {
                break;
            }
            if (result == ESP_OK)
            {
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U);
            }
            else
            {
                music_service_publish_detail(
                    MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U, failure);
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
            const uint16_t fetch_offset = (uint16_t)(
                                              requested_index -
                                              requested_index %
                                              MUSIC_SERVICE_QUEUE_FETCH_PAGE);
            music_playlist_snapshot_t *playlist = NULL;
            const esp_err_t fetch_result = music_net_fetch_playlist(
                                               &command->playlist, fetch_offset,
                                               MUSIC_SERVICE_QUEUE_WINDOW_SONGS,
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
            const uint32_t total = playlist->total != 0U ? playlist->total :
                                   (uint32_t)playlist->offset +
                                   playlist->count;
            if (requested_index >= total ||
                    total > MUSIC_SERVICE_MAX_QUEUE_TRACKS)
            {
                music_snapshot_release(playlist);
                music_service_handle_network_result(ESP_ERR_INVALID_ARG);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                _music_service_player_error(MUSIC_SERVICE_FAILURE_API);
                break;
            }
            music_queue_snapshot_t *queue =
                _music_service_queue_from_playlist_snapshot(playlist);
            music_snapshot_release(playlist);
            if (queue == NULL)
            {
                music_service_handle_network_result(ESP_ERR_NO_MEM);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                _music_service_player_error(MUSIC_SERVICE_FAILURE_INTERNAL);
                break;
            }
            music_queue_snapshot_t *held_queue =
                music_snapshot_retain(queue);
            _music_service_queue_install_source(queue, (uint16_t)total);
            music_service_lock();
            s_music.play_valid = true;
            music_service_unlock();
            const esp_err_t start_result = _music_service_start_queue_index(
                                               held_queue, requested_index,
                                               play_generation);
            music_snapshot_release(held_queue);
            if (start_result == ESP_OK)
            {
                _music_service_queue_prefetch_window(play_generation);
            }
            else
            {
                _music_service_report_start_failure(start_result,
                                                    play_generation);
            }
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
            const uint32_t total = playlist->total != 0U ? playlist->total :
                                   (uint32_t)playlist->offset +
                                   playlist->count;
            const uint32_t absolute = (uint32_t)playlist->offset +
                                      requested_index;
            if (total > MUSIC_SERVICE_MAX_QUEUE_TRACKS || absolute >= total)
            {
                music_snapshot_release(playlist);
                music_service_handle_network_result(ESP_ERR_INVALID_ARG);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                _music_service_player_error(MUSIC_SERVICE_FAILURE_API);
                break;
            }
            music_queue_snapshot_t *queue =
                _music_service_queue_from_playlist_snapshot(playlist);
            music_snapshot_release(playlist);
            if (queue == NULL)
            {
                music_service_handle_network_result(ESP_ERR_NO_MEM);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                _music_service_player_error(MUSIC_SERVICE_FAILURE_INTERNAL);
                break;
            }
            music_queue_snapshot_t *held_queue =
                music_snapshot_retain(queue);
            _music_service_queue_install_source(queue, (uint16_t)total);
            music_service_lock();
            s_music.play_valid = true;
            music_service_unlock();
            const esp_err_t start_result = _music_service_start_queue_index(
                                               held_queue, (uint16_t)absolute,
                                               play_generation);
            music_snapshot_release(held_queue);
            if (start_result == ESP_OK)
            {
                _music_service_queue_prefetch_window(play_generation);
            }
            else
            {
                _music_service_report_start_failure(start_result,
                                                    play_generation);
            }
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
            const uint16_t total = s_music.queue_total;
            const uint16_t source_total = s_music.queue_source_total;
            const uint16_t removed_count = s_music.queue_removed_count;
            const uint64_t queue_epoch = s_music.queue_epoch;
            music_playlist_ref_t ref = {0};
            if (queue != NULL)
            {
                ref = queue->ref;
            }
            music_service_unlock();
            if (queue == NULL || index >= total ||
                    (command->queue_generation != 0U &&
                     command->queue_generation != queue_epoch))
            {
                music_snapshot_release(queue);
                music_service_handle_network_result(ESP_ERR_INVALID_STATE);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            if (!music_queue_window_contains(queue->offset, queue->count, index))
            {
                music_snapshot_release(queue);
                uint8_t *removed_copy = _music_service_queue_removed_copy();
                if (removed_copy == NULL)
                {
                    music_service_handle_network_result(ESP_ERR_NO_MEM);
                    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                          0U);
                    _music_service_player_error(
                        MUSIC_SERVICE_FAILURE_INTERNAL);
                    break;
                }
                const uint16_t offset = music_queue_window_start(
                                            index, total,
                                            MUSIC_SERVICE_QUEUE_WINDOW_SONGS,
                                            MUSIC_SERVICE_QUEUE_FETCH_PAGE);
                const uint16_t count = music_queue_window_count(
                                           total, offset,
                                           MUSIC_SERVICE_QUEUE_WINDOW_SONGS);
                music_queue_snapshot_t *window = NULL;
                const esp_err_t fetch_result =
                    _music_service_queue_build_view(
                        &ref, source_total, removed_copy, removed_count,
                        total, offset, count, &window);
                heap_caps_free(removed_copy);
                if (fetch_result != ESP_OK)
                {
                    music_service_handle_network_result(fetch_result);
                    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                          0U);
                    _music_service_player_error(
                        _music_service_failure_from_result(fetch_result));
                    break;
                }
                if (music_service_lock() != ESP_OK ||
                        s_music.queue_epoch != queue_epoch)
                {
                    music_service_unlock();
                    music_snapshot_release(window);
                    break;
                }
                music_service_unlock();
                queue = music_snapshot_retain(window);
                music_service_replace_queue(window);
            }
            music_service_lock();
            s_music.play_valid = true;
            music_service_unlock();
            const esp_err_t start_result = _music_service_start_queue_index(
                                               queue, index, play_generation);
            music_snapshot_release(queue);
            if (start_result == ESP_OK)
            {
                _music_service_queue_prefetch_window(play_generation);
            }
            else
            {
                _music_service_report_start_failure(start_result,
                                                    play_generation);
            }
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
            const uint16_t source_total = s_music.queue_source_total;
            const uint16_t old_total = s_music.queue_total;
            const uint16_t old_removed_count = s_music.queue_removed_count;
            const uint16_t current = s_music.player.queue_index;
            const bool play_valid = s_music.play_valid;
            const music_playback_state_t player_state = s_music.player.state;
            const uint64_t queue_epoch = s_music.queue_epoch;
            music_playlist_ref_t ref = {0};
            if (s_music.queue != NULL)
            {
                ref = s_music.queue->ref;
            }
            music_service_unlock();
            music_queue_transition_t transition;
            uint8_t *removed_snapshot = _music_service_queue_removed_copy();
            uint16_t removed_source = 0U;
            if (removed_snapshot == NULL)
            {
                music_service_handle_network_result(ESP_ERR_NO_MEM);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            if ((command->queue_generation != 0U &&
                    command->queue_generation != queue_epoch) ||
                    removed >= old_total ||
                    !music_queue_logical_to_source(source_total,
                                                   removed_snapshot, removed,
                                                   &removed_source) ||
                    music_queue_after_remove(old_total, current, removed,
                                             &transition) != ESP_OK)
            {
                heap_caps_free(removed_snapshot);
                music_service_handle_network_result(ESP_ERR_INVALID_STATE);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            if (transition.count == 0U)
            {
                heap_caps_free(removed_snapshot);
                (void)music_stream_stop();
                atomic_store_explicit(&s_playing, false,
                                      memory_order_release);
                music_queue_snapshot_t *old_queue = NULL;
                music_queue_snapshot_t *old_view = NULL;
                music_service_lock();
                if (s_music.queue_epoch != queue_epoch)
                {
                    music_service_unlock();
                    break;
                }
                old_queue = s_music.queue;
                old_view = _music_service_detach_queue_view_locked();
                s_music.queue = NULL;
                s_music.queue_total = 0U;
                s_music.queue_source_total = 0U;
                s_music.queue_removed_count = 0U;
                memset(s_music.queue_removed, 0, sizeof(s_music.queue_removed));
                ++s_music.queue_epoch;
                if (s_music.queue_epoch == 0U)
                {
                    ++s_music.queue_epoch;
                }
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
                music_service_unlock();
                music_snapshot_release(old_queue);
                music_snapshot_release(old_view);
                music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                             MUSIC_SERVICE_FAILURE_NONE);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
                break;
            }

            uint8_t *removed_map = removed_snapshot;
            removed_snapshot = NULL;
            removed_map[removed_source / 8U] |=
                (uint8_t)(1U << (removed_source % 8U));
            const uint16_t offset = music_queue_window_start(
                                        transition.index, transition.count,
                                        MUSIC_SERVICE_QUEUE_WINDOW_SONGS,
                                        MUSIC_SERVICE_QUEUE_FETCH_PAGE);
            const uint16_t count = music_queue_window_count(
                                       transition.count, offset,
                                       MUSIC_SERVICE_QUEUE_WINDOW_SONGS);
            music_queue_snapshot_t *next = NULL;
            const esp_err_t build_result = _music_service_queue_build_view(
                                               &ref, source_total, removed_map,
                                               (uint16_t)(old_removed_count + 1U),
                                               transition.count, offset, count,
                                               &next);
            if (build_result != ESP_OK)
            {
                heap_caps_free(removed_map);
                music_service_handle_network_result(build_result);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            music_queue_snapshot_t *old_queue = NULL;
            music_queue_snapshot_t *old_view = NULL;
            music_service_lock();
            if (s_music.queue_epoch != queue_epoch)
            {
                music_service_unlock();
                heap_caps_free(removed_map);
                music_snapshot_release(next);
                break;
            }
            memcpy(s_music.queue_removed, removed_map,
                   sizeof(s_music.queue_removed));
            s_music.queue_removed_count = (uint16_t)(old_removed_count + 1U);
            s_music.queue_total = transition.count;
            ++s_music.queue_epoch;
            if (s_music.queue_epoch == 0U)
            {
                ++s_music.queue_epoch;
            }
            old_queue = s_music.queue;
            old_view = _music_service_detach_queue_view_locked();
            s_music.queue = next;
            s_music.player.queue_index = transition.index;
            s_music.player.queue_count = transition.count;
            music_service_unlock();
            heap_caps_free(removed_map);
            music_snapshot_release(old_queue);
            music_snapshot_release(old_view);
            music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                         MUSIC_SERVICE_FAILURE_NONE);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
            if (transition.restart && play_valid &&
                    (player_state == MUSIC_PLAYBACK_PLAYING ||
                     player_state == MUSIC_PLAYBACK_BUFFERING))
            {
                const esp_err_t start_result =
                    _music_service_start_queue_index(next, transition.index,
                                                     play_generation);
                if (start_result == ESP_OK)
                {
                    _music_service_queue_prefetch_window(play_generation);
                }
                else
                {
                    _music_service_report_start_failure(start_result,
                                                        play_generation);
                }
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
            break;
        }
        case MUSIC_API_CMD_QUEUE_VIEW:
        {
            const uint16_t offset = (uint16_t)command->offset;
            const uint16_t count = (uint16_t)command->count;
            const uint64_t queue_epoch = command->queue_generation;
            music_playlist_ref_t ref = {0};
            uint8_t *removed = NULL;
            uint16_t source_total = 0U;
            uint16_t removed_count = 0U;
            uint16_t total = 0U;
            bool epoch_ok = false;
            music_service_lock();
            if (s_music.queue_epoch == queue_epoch)
            {
                epoch_ok = true;
                if (s_music.queue != NULL)
                {
                    ref = s_music.queue->ref;
                    source_total = s_music.queue_source_total;
                    removed_count = s_music.queue_removed_count;
                    total = s_music.queue_total;
                }
            }
            music_service_unlock();
            if (!epoch_ok)
            {
                break;
            }
            removed = _music_service_queue_removed_copy();
            if (removed == NULL || offset >= total)
            {
                heap_caps_free(removed);
                music_service_publish_detail(
                    MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                    MUSIC_SERVICE_FAILURE_INTERNAL);
                break;
            }
            music_queue_snapshot_t *view = NULL;
            const esp_err_t result = _music_service_queue_build_view(
                                         &ref, source_total, removed,
                                         removed_count, total, offset, count,
                                         &view);
            heap_caps_free(removed);
            if (result == ESP_OK)
            {
                if (music_service_lock() != ESP_OK ||
                        s_music.queue_epoch != queue_epoch)
                {
                    music_service_unlock();
                    music_snapshot_release(view);
                    break;
                }
                music_service_unlock();
                music_service_replace_queue_view(view);
            }
            else
            {
                music_service_lock();
                if (s_music.queue_epoch == queue_epoch)
                {
                    s_music.queue_view_pending = false;
                }
                music_service_unlock();
                music_service_publish_detail(
                    MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                    _music_service_failure_from_result(result));
            }
            break;
        }
        case MUSIC_API_CMD_PAUSE:
        {
            if (command->token != 0U)
            {
                music_service_lock();
                const bool current = s_music.play_valid &&
                                     s_music.player.play_generation ==
                                     command->token;
                music_service_unlock();
                if (!current)
                {
                    break;
                }
            }
            music_stream_status_t stream_status;
            if (music_stream_get_status(&stream_status) == ESP_OK)
            {
                if (stream_status.state == MUSIC_PLAYBACK_PLAYING)
                {
                    if (music_stream_pause(true) == ESP_OK)
                    {
                        atomic_store_explicit(&s_playing, false,
                                              memory_order_release);
                        music_service_lock();
                        s_music.player.state = MUSIC_PLAYBACK_PAUSED;
                        s_music.player.failure =
                            MUSIC_SERVICE_FAILURE_NONE;
                        music_service_unlock();
                        music_service_publish(
                            MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
                    }
                }
                else if (stream_status.state == MUSIC_PLAYBACK_PAUSED)
                {
                    if (music_stream_pause(false) == ESP_OK)
                    {
                        atomic_store_explicit(&s_playing, true,
                                              memory_order_release);
                        music_service_lock();
                        s_music.player.state = MUSIC_PLAYBACK_BUFFERING;
                        s_music.player.failure =
                            MUSIC_SERVICE_FAILURE_NONE;
                        music_service_unlock();
                        music_service_publish(
                            MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
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
            _music_service_set_account_locked(&credential);
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
    if (s_music.api_queue != NULL)
    {
        const music_api_command_t api_wake =
        {
            .type = MUSIC_API_CMD_WAKE,
        };
        for (uint32_t attempt = 0U; attempt < 10U; ++attempt)
        {
            if (xQueueSend(s_music.api_queue, &api_wake,
                           pdMS_TO_TICKS(20U)) == pdTRUE)
            {
                break;
            }
        }
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
    const esp_err_t stream_result = music_stream_deinit();
    if (stream_result != ESP_OK)
    {
        return stream_result;
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
    for (size_t slot = 0U; slot < MUSIC_PLAYLIST_CACHE_SLOTS; ++slot)
    {
        music_snapshot_release(s_music.playlist[slot]);
    }
    music_snapshot_release(s_music.queue);
    music_snapshot_release(s_music.queue_view);
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
        _music_service_set_account_locked(NULL);
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
                                    CONFIG_MUSIC_SERVICE_LOGIN_TASK_STACK_SIZE,
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
        const bool profile_missing = s_music.credential_valid &&
                                     !s_music.profile_loaded;
        music_service_unlock();
        if (cached)
        {
            if (!profile_missing)
            {
                return ESP_OK;
            }
            const music_api_command_t profile_command =
            {
                .type = MUSIC_API_CMD_REFRESH_PROFILE,
            };
            return xQueueSend(s_music.api_queue, &profile_command,
                              pdMS_TO_TICKS(100U)) == pdTRUE ?
                   ESP_OK : ESP_ERR_TIMEOUT;
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
        for (size_t slot = 0U; slot < MUSIC_PLAYLIST_CACHE_SLOTS; ++slot)
        {
            music_playlist_snapshot_t *playlist = s_music.playlist[slot];
            if (music_service_playlist_cache_matches(playlist, ref, offset,
                    count))
            {
                playlist = music_snapshot_retain(playlist);
                music_service_unlock();
                *snapshot = playlist;
                return ESP_OK;
            }
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
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    music_lyric_snapshot_t *lyric = s_music.lyric;
    if (lyric != NULL && strcmp(s_music.lyric_songmid, songmid) == 0)
    {
        lyric = music_snapshot_retain(lyric);
        music_service_unlock();
        *snapshot = lyric;
        return ESP_OK;
    }
    if (lyric == NULL && s_music.lyric_songmid[0] != '\0' &&
            strcmp(s_music.lyric_songmid, songmid) == 0)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (s_music.lyric_request_songmid[0] != '\0' &&
            strcmp(s_music.lyric_request_songmid, songmid) == 0)
    {
        music_service_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (s_music.api_queue == NULL)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t request_id = s_music.lyric_request_id + 1U;
    if (request_id == 0U)
    {
        request_id = 1U;
    }
    s_music.lyric_request_id = request_id;
    (void)snprintf(s_music.lyric_request_songmid,
                   sizeof(s_music.lyric_request_songmid), "%s", songmid);
    music_service_unlock();
    music_api_command_t command =
    {
        .type = MUSIC_API_CMD_LYRIC,
        .token = request_id,
    };
    (void)snprintf(command.songmid, sizeof(command.songmid), "%s", songmid);
    if (xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        music_service_lock();
        if (s_music.lyric_request_id == request_id)
        {
            s_music.lyric_request_id = 0U;
            s_music.lyric_request_songmid[0] = '\0';
        }
        music_service_unlock();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_ERR_NOT_FOUND;
}

bool music_service_lyric_failed(const char *songmid)
{
    if (songmid == NULL || songmid[0] == '\0' ||
            music_service_lock() != ESP_OK)
    {
        return false;
    }
    const bool failed = s_music.lyric == NULL &&
                        s_music.lyric_songmid[0] != '\0' &&
                        s_music.lyric_request_id == 0U &&
                        strcmp(s_music.lyric_songmid, songmid) == 0;
    music_service_unlock();
    return failed;
}

void music_service_lyric_retry(const char *songmid)
{
    if (songmid == NULL || songmid[0] == '\0' ||
            music_service_lock() != ESP_OK)
    {
        return;
    }
    if (s_music.lyric == NULL && s_music.lyric_request_id == 0U &&
            strcmp(s_music.lyric_songmid, songmid) == 0)
    {
        s_music.lyric_songmid[0] = '\0';
    }
    music_service_unlock();
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

esp_err_t music_service_queue_view_acquire(uint16_t offset, uint16_t count,
        const music_queue_snapshot_t **snapshot)
{
    if (snapshot == NULL || count == 0U || count > MUSIC_SERVICE_MAX_SONGS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.queue == NULL || s_music.queue_total == 0U)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (offset >= s_music.queue_total)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (s_music.queue_view != NULL &&
            s_music.queue_view_epoch == s_music.queue_epoch &&
            s_music.queue_view_offset == offset &&
            s_music.queue_view_count <= count &&
            (s_music.queue_view_count == count ||
             (uint32_t)s_music.queue_view_offset +
             s_music.queue_view_count >= s_music.queue_total))
    {
        music_queue_snapshot_t *view = music_snapshot_retain(
                                           s_music.queue_view);
        music_service_unlock();
        *snapshot = view;
        return view != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (s_music.queue_view_pending &&
            s_music.queue_view_pending_offset == offset &&
            s_music.queue_view_pending_count == count)
    {
        music_service_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const uint64_t queue_epoch = s_music.queue_epoch;
    s_music.queue_view_pending = true;
    s_music.queue_view_pending_offset = offset;
    s_music.queue_view_pending_count = count;
    music_service_unlock();
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_QUEUE_VIEW,
        .queue_generation = queue_epoch,
        .offset = offset,
        .count = count,
    };
    if (s_music.api_queue == NULL ||
            xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        music_service_lock();
        if (s_music.queue_epoch == queue_epoch)
        {
            s_music.queue_view_pending = false;
        }
        music_service_unlock();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_ERR_NOT_FOUND;
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
    const bool liked = ref != NULL &&
                       ref->kind == MUSIC_PLAYLIST_KIND_MY_FAVORITE &&
                       ref->dirid == 201U;
    if (ref == NULL || (ref->id == 0U && !liked) ||
            s_music.api_queue == NULL)
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
    if (s_music.queue == NULL || index >= s_music.queue_total)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t queue_generation = s_music.queue_epoch;
    const uint32_t previous_play_generation = s_music.play_generation;
    const uint32_t previous_player_generation =
        s_music.player.play_generation;
    const uint16_t previous_queue_index = s_music.player.queue_index;
    const uint32_t play_generation =
        _music_service_next_play_generation_locked();
    s_music.player.play_generation = play_generation;
    s_music.player.queue_index = index;
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
        s_music.player.queue_index = previous_queue_index;
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
    if (s_music.queue == NULL || index >= s_music.queue_total)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t queue_generation = s_music.queue_epoch;
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
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_music.play_valid)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t play_generation = s_music.player.play_generation;
    music_service_unlock();
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_PAUSE,
        .token = play_generation,
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
    music_queue_snapshot_t *old_queue = s_music.queue;
    music_queue_snapshot_t *old_view = _music_service_detach_queue_view_locked();
    music_lyric_snapshot_t *old_lyric = s_music.lyric;
    s_music.queue = NULL;
    s_music.queue_total = 0U;
    s_music.queue_source_total = 0U;
    s_music.queue_removed_count = 0U;
    memset(s_music.queue_removed, 0, sizeof(s_music.queue_removed));
    s_music.lyric = NULL;
    s_music.lyric_songmid[0] = '\0';
    s_music.lyric_request_songmid[0] = '\0';
    s_music.lyric_request_id = 0U;
    ++s_music.queue_epoch;
    if (s_music.queue_epoch == 0U)
    {
        ++s_music.queue_epoch;
    }
    music_service_unlock();
    (void)music_stream_stop();
    atomic_store_explicit(&s_playing, false, memory_order_release);
    music_snapshot_release(old_queue);
    music_snapshot_release(old_view);
    music_snapshot_release(old_lyric);
    music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                 MUSIC_SERVICE_FAILURE_NONE);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U);
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
