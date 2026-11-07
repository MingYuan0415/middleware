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
#include "esp_timer.h"
#include "event_bus.h"
#include "freertos/idf_additions.h"
#include "nv_storage.h"
#include "time_service.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

EVENT_BUS_DEFINE_ID(MUSIC_SERVICE_MSG);

MUSIC_SERVICE_EXT_BSS music_service_runtime_t s_music;
static MUSIC_SERVICE_EXT_BSS music_api_command_t s_api_command;

static atomic_bool s_playing;
static atomic_bool s_login_cancel;
static atomic_uint s_login_generation;
static atomic_bool s_api_stop;
static atomic_bool s_api_exited;
static atomic_bool s_login_exited;

static esp_err_t _music_service_queue_play_internal(uint16_t index,
        bool reset_skip);
static bool _music_service_skip_unavailable(uint32_t play_generation);
static void _music_service_request_session_check(void);
static esp_err_t _music_service_ensure_login_worker(void);
static music_queue_snapshot_t *_music_service_detach_queue_view_locked(void);

static uint32_t _music_service_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static bool _login_session_current(uint32_t session)
{
    return atomic_load_explicit(&s_login_generation,
                                memory_order_acquire) == session;
}

bool music_service_login_session_current(uint32_t login_session)
{
    return _login_session_current(login_session);
}

esp_err_t music_service_credential_lock(void)
{
    if (!s_music.initialized || s_music.credential_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_music.credential_lock, portMAX_DELAY) == pdTRUE ?
           ESP_OK : ESP_ERR_INVALID_STATE;
}

void music_service_credential_unlock(void)
{
    if (s_music.credential_lock != NULL)
    {
        (void)xSemaphoreGive(s_music.credential_lock);
    }
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
    s_music.profile_requested = false;
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

static void _music_service_refresh_profile(bool from_command,
        uint32_t credential_generation)
{
    bool needed = false;
    if (music_service_lock() == ESP_OK)
    {
        if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
                s_music.credential_generation != credential_generation ||
                s_music.profile_loaded)
        {
            s_music.profile_requested = false;
        }
        else if (from_command || !s_music.profile_requested)
        {
            s_music.profile_requested = true;
            needed = true;
        }
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
                                 credential_generation,
                                 account, sizeof(account),
                                 name, sizeof(name), avatar, sizeof(avatar));
    if (result == MUSIC_ERR_HTTP_AUTH)
    {
        music_service_handle_auth_result(result, credential_generation);
        return;
    }
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND)
    {
        if (music_service_lock() == ESP_OK)
        {
            if (s_music.credential_generation == credential_generation)
            {
                s_music.profile_requested = false;
            }
            music_service_unlock();
        }
        return;
    }
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.credential_generation != credential_generation ||
            strcmp(s_music.credential.musicid, account) != 0)
    {
        s_music.profile_requested = false;
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
    s_music.profile_requested = false;
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

static void _music_service_clear_account_locked(
    music_home_snapshot_t **home,
    music_playlist_snapshot_t *playlists[MUSIC_PLAYLIST_CACHE_SLOTS],
    music_queue_snapshot_t **queue,
    music_queue_snapshot_t **queue_view,
    music_lyric_snapshot_t **lyric,
    music_image_t *images[MUSIC_IMAGE_CACHE_SLOTS])
{
    *home = s_music.home;
    memcpy(playlists, s_music.playlist, sizeof(s_music.playlist));
    *queue = s_music.queue;
    *queue_view = _music_service_detach_queue_view_locked();
    *lyric = s_music.lyric;
    memcpy(images, s_music.image, sizeof(s_music.image));

    s_music.credential_state = MUSIC_CREDENTIAL_NONE;
    s_music.status.logged_in = false;
    s_music.status.qr_available = false;
    s_music.profile_loaded = false;
    s_music.profile_requested = false;
    s_music.session_check_pending = false;
    s_music.last_session_check_ms = 0U;
    s_music.session_retry_ms = 0U;
    ++s_music.credential_generation;
    if (s_music.credential_generation == 0U)
    {
        ++s_music.credential_generation;
    }
    memset(&s_music.credential, 0, sizeof(s_music.credential));
    _music_service_set_account_locked(NULL);

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
    memset(s_music.image, 0, sizeof(s_music.image));
    memset(s_music.image_used, 0, sizeof(s_music.image_used));

    s_music.play_valid = false;
    ++s_music.play_generation;
    if (s_music.play_generation == 0U)
    {
        ++s_music.play_generation;
    }
    s_music.play_skip_count = 0U;
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
}

static void _music_service_release_account(
    music_home_snapshot_t *home,
    music_playlist_snapshot_t *playlists[MUSIC_PLAYLIST_CACHE_SLOTS],
    music_queue_snapshot_t *queue,
    music_queue_snapshot_t *queue_view,
    music_lyric_snapshot_t *lyric,
    music_image_t *images[MUSIC_IMAGE_CACHE_SLOTS])
{
    music_snapshot_release(home);
    for (size_t slot = 0U; slot < MUSIC_PLAYLIST_CACHE_SLOTS; ++slot)
    {
        music_snapshot_release(playlists[slot]);
    }
    music_snapshot_release(queue);
    music_snapshot_release(queue_view);
    music_snapshot_release(lyric);
    for (size_t slot = 0U; slot < MUSIC_IMAGE_CACHE_SLOTS; ++slot)
    {
        music_service_image_release(images[slot]);
    }
}

static bool _music_service_enter_auth_required(uint32_t expected_generation)
{
    if (expected_generation == 0U ||
            music_service_lock() != ESP_OK)
    {
        return false;
    }
    if (s_music.credential_state == MUSIC_CREDENTIAL_NONE ||
            s_music.credential_generation != expected_generation ||
            s_music.login_active ||
            s_music.status.state == MUSIC_SERVICE_STATE_AUTH_REQUIRED)
    {
        music_service_unlock();
        return false;
    }

    music_home_snapshot_t *home = NULL;
    music_playlist_snapshot_t *playlists[MUSIC_PLAYLIST_CACHE_SLOTS] = {0};
    music_queue_snapshot_t *queue = NULL;
    music_queue_snapshot_t *queue_view = NULL;
    music_lyric_snapshot_t *lyric = NULL;
    music_image_t *images[MUSIC_IMAGE_CACHE_SLOTS] = {0};
    music_image_t *login_qr = s_music.login_qr;
    s_music.login_qr = NULL;
    _music_service_clear_account_locked(&home, playlists, &queue,
                                        &queue_view, &lyric, images);
    music_service_set_state_locked(MUSIC_SERVICE_STATE_AUTH_REQUIRED,
                                   MUSIC_SERVICE_FAILURE_AUTH);
    music_service_unlock();

    (void)music_stream_stop();
    atomic_store_explicit(&s_playing, false, memory_order_release);
    music_net_clear_session();
    _music_service_release_account(home, playlists, queue, queue_view,
                                   lyric, images);
    music_service_login_qr_release(login_qr);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
    music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                 MUSIC_SERVICE_FAILURE_NONE);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U);
    return true;
}

void music_service_handle_auth_result(esp_err_t result,
                                      uint32_t credential_generation)
{
    if (result == MUSIC_ERR_HTTP_AUTH)
    {
        (void)_music_service_enter_auth_required(credential_generation);
    }
}

static void _music_service_handle_network_result(
    esp_err_t result, uint32_t credential_generation, bool check_generation)
{
    const music_service_failure_t failure =
        _music_service_failure_from_result(result);
    if (failure == MUSIC_SERVICE_FAILURE_AUTH)
    {
        return;
    }
    if (music_service_lock() == ESP_OK)
    {
        if (!check_generation ||
                s_music.credential_generation == credential_generation)
        {
            s_music.status.failure = failure;
            s_music.status.generation = ++s_music.generation;
            s_music.player.generation = s_music.status.generation;
        }
        music_service_unlock();
    }
}

void music_service_handle_network_result(esp_err_t result)
{
    _music_service_handle_network_result(result, 0U, false);
}

void music_service_handle_network_result_for_generation(
    esp_err_t result, uint32_t credential_generation)
{
    _music_service_handle_network_result(result, credential_generation, true);
}

void music_service_replace_home(music_home_snapshot_t *snapshot,
                                uint32_t credential_generation)
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.credential_generation != credential_generation)
    {
        music_service_unlock();
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

void music_service_replace_playlist(music_playlist_snapshot_t *snapshot,
                                    uint32_t credential_generation)
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.credential_generation != credential_generation)
    {
        music_service_unlock();
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

void music_service_set_image(music_image_t *image, uint32_t token,
                             uint32_t credential_generation)
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
    if (s_music.credential_generation != credential_generation ||
            (s_music.credential_state == MUSIC_CREDENTIAL_PENDING_VALIDATION) ||
            s_music.status.state == MUSIC_SERVICE_STATE_AUTH_REQUIRED)
    {
        music_service_unlock();
        music_service_image_release(image);
        return;
    }
    size_t slot = MUSIC_IMAGE_CACHE_SLOTS;
    uint32_t oldest = UINT32_MAX;
    for (size_t index = 0U; index < MUSIC_IMAGE_CACHE_SLOTS; ++index)
    {
        if (s_music.image[index] == NULL)
        {
            slot = index;
            break;
        }
        if (music_service_image_cache_matches(
                    s_music.image[index], image->url,
                    image->requested_width, image->requested_height, token))
        {
            slot = index;
            break;
        }
        if (s_music.image_used[index] < oldest)
        {
            oldest = s_music.image_used[index];
            slot = index;
        }
    }
    music_image_t *old = s_music.image[slot];
    s_music.image[slot] = image;
    s_music.image_used[slot] = ++s_music.image_clock;
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
    bool became_ready = false;
    if (s_music.status.time_ready != time_ready)
    {
        s_music.status.time_ready = time_ready;
        changed = true;
    }
    if (s_music.status.state == MUSIC_SERVICE_STATE_WAITING_TIME &&
            s_music.status.network_ready && time_ready)
    {
        const music_service_state_t state =
            s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED ?
            MUSIC_SERVICE_STATE_READY :
            s_music.credential_state == MUSIC_CREDENTIAL_PENDING_VALIDATION ?
            MUSIC_SERVICE_STATE_VALIDATING_SESSION :
            MUSIC_SERVICE_STATE_LOGGED_OUT;
        music_service_set_state_locked(state, MUSIC_SERVICE_FAILURE_NONE);
        became_ready =
            s_music.credential_state == MUSIC_CREDENTIAL_PENDING_VALIDATION;
        changed = true;
    }
    music_service_unlock();
    if (changed)
    {
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    }
    if (became_ready)
    {
        _music_service_request_session_check();
    }
}

static void _music_service_check_session(uint32_t credential_generation)
{
    char musicid[sizeof(s_music.credential.musicid)] = {0};
    char musickey[sizeof(s_music.credential.musickey)] = {0};
    music_credential_state_t started_state = MUSIC_CREDENTIAL_NONE;
    bool run = false;
    if (music_service_lock() == ESP_OK)
    {
        const bool current =
            (s_music.credential_state == MUSIC_CREDENTIAL_PENDING_VALIDATION ||
             s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED) &&
            s_music.credential_generation == credential_generation;
        if (current &&
                s_music.status.network_ready &&
                s_music.status.time_ready &&
                !s_music.login_active)
        {
            started_state = s_music.credential_state;
            (void)snprintf(musicid, sizeof(musicid), "%s",
                           s_music.credential.musicid);
            (void)snprintf(musickey, sizeof(musickey), "%s",
                           s_music.credential.musickey);
            s_music.last_session_check_ms = _music_service_now_ms();
            s_music.session_retry_ms = 0U;
            run = true;
        }
        else
        {
            s_music.session_check_pending = false;
        }
        music_service_unlock();
    }
    if (!run)
    {
        return;
    }

    const esp_err_t check_result = music_net_check_session(musicid, musickey);
    bool preserve_auth_required = false;
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    const bool same_probe =
        s_music.credential_generation == credential_generation &&
        strcmp(s_music.credential.musicid, musicid) == 0 &&
        strcmp(s_music.credential.musickey, musickey) == 0 &&
        s_music.credential_state == started_state;
    if (same_probe)
    {
        if (check_result == ESP_OK && s_music.status.network_ready &&
                s_music.status.time_ready && !s_music.login_active)
        {
            s_music.credential_state = MUSIC_CREDENTIAL_VALIDATED;
            s_music.status.logged_in = true;
            music_service_set_state_locked(MUSIC_SERVICE_STATE_READY,
                                           MUSIC_SERVICE_FAILURE_NONE);
            s_music.last_session_check_ms = _music_service_now_ms();
        }
        else if (check_result == ESP_OK)
        {
            s_music.session_retry_ms = _music_service_now_ms();
        }
        else if (check_result != MUSIC_ERR_HTTP_AUTH)
        {
            s_music.session_retry_ms = _music_service_now_ms();
            LOG_W("session check deferred: %s",
                  esp_err_to_name(check_result));
        }
        else
        {
            preserve_auth_required = true;
        }
        s_music.session_check_pending = false;
    }
    music_service_unlock();

    if (preserve_auth_required)
    {
        (void)_music_service_enter_auth_required(credential_generation);
        return;
    }
    if (check_result == ESP_OK)
    {
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    }
}

static bool _music_service_session_due_locked(uint32_t now)
{
    if (s_music.login_active || s_music.session_check_pending ||
            s_music.status.state == MUSIC_SERVICE_STATE_QR_READY ||
            s_music.status.state == MUSIC_SERVICE_STATE_QR_SCANNED ||
            s_music.status.state == MUSIC_SERVICE_STATE_QR_EXPIRED ||
            s_music.status.state == MUSIC_SERVICE_STATE_AUTHENTICATING ||
            !s_music.status.network_ready || !s_music.status.time_ready ||
            (s_music.credential_state != MUSIC_CREDENTIAL_PENDING_VALIDATION &&
             s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED))
    {
        return false;
    }
    if (s_music.credential_state == MUSIC_CREDENTIAL_PENDING_VALIDATION)
    {
        return s_music.session_retry_ms == 0U ||
               (uint32_t)(now - s_music.session_retry_ms) >=
               MUSIC_SERVICE_SESSION_RETRY_MS;
    }
    return s_music.last_session_check_ms == 0U ||
           (uint32_t)(now - s_music.last_session_check_ms) >=
           MUSIC_SERVICE_SESSION_CHECK_INTERVAL_MS;
}

static void _music_service_session_tick(void)
{
    uint32_t credential_generation = 0U;
    if (music_service_lock() == ESP_OK)
    {
        if (_music_service_session_due_locked(_music_service_now_ms()))
        {
            credential_generation = s_music.credential_generation;
            s_music.session_check_pending = true;
        }
        music_service_unlock();
    }
    if (credential_generation != 0U)
    {
        _music_service_check_session(credential_generation);
    }
}

static void _music_service_request_session_check(void)
{
    uint32_t credential_generation = 0U;
    if (music_service_lock() == ESP_OK)
    {
        if (!s_music.session_check_pending &&
                !s_music.login_active &&
                s_music.status.state != MUSIC_SERVICE_STATE_QR_READY &&
                s_music.status.state != MUSIC_SERVICE_STATE_QR_SCANNED &&
                s_music.status.state != MUSIC_SERVICE_STATE_QR_EXPIRED &&
                s_music.status.state != MUSIC_SERVICE_STATE_AUTHENTICATING &&
                s_music.status.network_ready &&
                s_music.status.time_ready &&
                (s_music.credential_state == MUSIC_CREDENTIAL_PENDING_VALIDATION ||
                 s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED))
        {
            s_music.session_check_pending = true;
            credential_generation = s_music.credential_generation;
        }
        music_service_unlock();
    }
    if (credential_generation == 0U || s_music.login_queue == NULL)
    {
        return;
    }
    const esp_err_t worker_result = _music_service_ensure_login_worker();
    if (worker_result != ESP_OK)
    {
        if (music_service_lock() == ESP_OK)
        {
            if (s_music.credential_generation == credential_generation)
            {
                s_music.session_check_pending = false;
            }
            music_service_unlock();
        }
        return;
    }
    const music_login_command_t command =
    {
        .command_type = MUSIC_LOGIN_COMMAND_CHECK_SESSION,
        .credential_generation = credential_generation,
    };
    if (xQueueSend(s_music.login_queue, &command, pdMS_TO_TICKS(100U)) !=
            pdTRUE)
    {
        if (music_service_lock() == ESP_OK)
        {
            if (s_music.credential_generation == credential_generation)
            {
                s_music.session_check_pending = false;
            }
            music_service_unlock();
        }
    }
}

static void _music_service_player_error(
    uint32_t credential_generation, uint32_t play_generation,
    music_service_failure_t failure)
{
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.credential_generation != credential_generation ||
            !s_music.play_valid || s_music.play_generation != play_generation)
    {
        music_service_unlock();
        return;
    }
    s_music.player.state = MUSIC_PLAYBACK_ERROR;
    s_music.player.failure = failure;
    s_music.play_valid = false;
    music_service_unlock();
    (void)music_stream_stop();
    atomic_store_explicit(&s_playing, false, memory_order_release);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
}

static void _music_log_api_stack(const char *phase)
{
    LOG_I("worker stack high water=%u B phase=%s",
          (unsigned)uxTaskGetStackHighWaterMark(NULL), phase);
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
    uint32_t credential_generation, const music_playlist_ref_t *ref,
    uint16_t source_total, const uint8_t *removed, uint16_t removed_count,
    uint16_t total, uint16_t offset, uint16_t count,
    music_queue_snapshot_t **snapshot)
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
                                           credential_generation, ref,
                                           source_cursor, (uint16_t)fetch,
                                           &page);
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

static bool _music_service_queue_install_source(
    music_queue_snapshot_t *queue, uint16_t source_total,
    uint32_t credential_generation, uint32_t play_generation)
{
    if (queue == NULL)
    {
        return false;
    }
    music_queue_snapshot_t *old_queue = NULL;
    music_queue_snapshot_t *old_view = NULL;
    if (music_service_lock() != ESP_OK)
    {
        music_snapshot_release(queue);
        return false;
    }
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.credential_generation != credential_generation ||
            s_music.play_generation != play_generation)
    {
        music_service_unlock();
        music_snapshot_release(queue);
        return false;
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
    s_music.play_valid = true;
    music_service_unlock();
    music_snapshot_release(old_queue);
    music_snapshot_release(old_view);
    music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                 MUSIC_SERVICE_FAILURE_NONE);
    return true;
}

void music_service_replace_queue_view(music_queue_snapshot_t *snapshot,
                                      uint32_t credential_generation)
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.credential_generation != credential_generation ||
            s_music.queue == NULL || snapshot->total != s_music.queue_total)
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

static bool _music_service_play_owner_current(uint32_t credential_generation,
        uint32_t play_generation)
{
    if (music_service_lock() != ESP_OK)
    {
        return false;
    }
    const bool current =
        s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED &&
        s_music.credential_generation == credential_generation &&
        s_music.play_valid && s_music.play_generation == play_generation;
    music_service_unlock();
    return current;
}

static esp_err_t _music_service_start_queue_index(
    const music_queue_snapshot_t *queue, uint16_t index,
    uint32_t credential_generation, uint32_t play_generation)
{
    if (queue == NULL || !music_queue_window_contains(queue->offset,
            queue->count, index) ||
            credential_generation == 0U || play_generation == 0U ||
            !_music_service_playback_current(play_generation))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool current =
        s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED &&
        s_music.credential_generation == credential_generation;
    music_service_unlock();
    if (!current)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t local_index = (uint16_t)(index - queue->offset);
    const music_song_t *song = &queue->songs[local_index];
    (void)music_stream_stop();
    _music_service_set_queue_player(song, index, queue->total,
                                    play_generation);
    char url[MUSIC_SERVICE_URL_BYTES];
    music_quality_t actual_quality = s_music.config.default_quality;
    const esp_err_t url_result = music_net_fetch_song_url(
                                     credential_generation, song,
                                     s_music.config.default_quality,
                                     url, sizeof(url), &actual_quality);
    if (url_result != ESP_OK)
    {
        if (!_music_service_play_owner_current(credential_generation,
                                               play_generation))
        {
            return ESP_ERR_INVALID_STATE;
        }
        if (url_result == MUSIC_ERR_HTTP_AUTH)
        {
            music_service_handle_auth_result(url_result,
                                             credential_generation);
            return url_result;
        }
        if (url_result == ESP_ERR_NOT_FOUND)
        {
            /* Let the caller try the next queue entry before reporting the
             * track as unplayable. */
            return url_result;
        }
        music_service_handle_network_result_for_generation(
            url_result, credential_generation);
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
        _music_service_player_error(
            credential_generation, play_generation,
            _music_service_failure_from_result(url_result));
        return url_result;
    }

    esp_err_t stream_result = _music_service_ensure_stream();
    if (stream_result == ESP_OK)
    {
        music_service_lock();
        if (!s_music.play_valid ||
                s_music.play_generation != play_generation ||
                s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
                s_music.credential_generation != credential_generation)
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
        if (!_music_service_play_owner_current(credential_generation,
                                               play_generation))
        {
            return ESP_ERR_INVALID_STATE;
        }
        music_service_handle_network_result_for_generation(
            stream_result, credential_generation);
        music_service_lock();
        if (s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED &&
                s_music.credential_generation == credential_generation &&
                s_music.play_valid &&
                s_music.play_generation == play_generation)
        {
            music_service_set_state_locked(s_music.status.state,
                                           MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
        }
        music_service_unlock();
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
        _music_service_player_error(credential_generation, play_generation,
                                    MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
        return stream_result;
    }
    _music_log_api_stack("play");
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
    return ESP_OK;
}

static void _music_service_report_start_failure(
    esp_err_t result, uint32_t credential_generation,
    uint32_t play_generation)
{
    if (result == ESP_OK ||
            !_music_service_play_owner_current(credential_generation,
                    play_generation))
    {
        return;
    }
    if (result == ESP_ERR_NOT_FOUND &&
            _music_service_skip_unavailable(play_generation))
    {
        return;
    }
    music_service_handle_network_result_for_generation(
        result, credential_generation);
    _music_service_player_error(credential_generation, play_generation,
                                _music_service_failure_from_result(result));
}

static bool _music_service_skip_unavailable(uint32_t play_generation)
{
    if (music_service_lock() != ESP_OK)
    {
        return false;
    }
    const bool current = s_music.play_valid &&
                         s_music.play_generation == play_generation &&
                         s_music.queue != NULL;
    const uint16_t count = s_music.player.queue_count;
    const uint16_t index = s_music.player.queue_index;
    const uint16_t skipped = s_music.play_skip_count;
    const music_play_mode_t mode = s_music.player.mode;
    music_service_unlock();
    if (!current || count < 2U || (uint16_t)(skipped + 1U) >= count)
    {
        return false;
    }
    uint16_t next = 0U;
    if (!music_queue_next_after_failure(count, index, mode, esp_random(),
                                        &next))
    {
        return false;
    }
    if (music_service_lock() != ESP_OK)
    {
        return false;
    }
    s_music.play_skip_count = (uint16_t)(skipped + 1U);
    music_service_unlock();
    return _music_service_queue_play_internal(next, false) == ESP_OK;
}

static void _music_service_replace_queue_if_current(
    music_queue_snapshot_t *queue, uint32_t credential_generation,
    uint64_t queue_epoch, uint32_t play_generation)
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.credential_generation != credential_generation ||
            !s_music.play_valid || s_music.play_generation != play_generation ||
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

static bool _music_service_commit_queue_window(
    music_queue_snapshot_t *queue, uint32_t credential_generation,
    uint64_t queue_epoch, uint32_t play_generation)
{
    if (queue == NULL)
    {
        return false;
    }
    if (music_service_lock() != ESP_OK)
    {
        music_snapshot_release(queue);
        return false;
    }
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.credential_generation != credential_generation ||
            s_music.play_generation != play_generation ||
            s_music.queue_epoch != queue_epoch)
    {
        music_service_unlock();
        music_snapshot_release(queue);
        return false;
    }
    music_queue_snapshot_t *old = s_music.queue;
    s_music.queue = queue;
    s_music.play_valid = true;
    music_service_unlock();
    music_snapshot_release(old);
    music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                 MUSIC_SERVICE_FAILURE_NONE);
    return true;
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
    const uint32_t credential_generation = s_music.credential_generation;
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED)
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
                                 credential_generation, &ref, source_total,
                                 removed_copy, removed_count, total, offset,
                                 count, &window);
    heap_caps_free(removed_copy);
    if (result != ESP_OK)
    {
        LOG_W("queue prefetch failed: %s", esp_err_to_name(result));
        return;
    }
    _music_service_replace_queue_if_current(
        window, credential_generation, queue_epoch, play_generation);
}

static bool _music_service_login_terminal(
    uint32_t session, music_service_state_t state,
    music_service_failure_t failure, const char *qr_status, bool keep_qr)
{
    music_image_t *qr = NULL;
    bool finalized = false;
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
    s_music.login_active = false;
    s_music.login_session = 0U;
    s_music.status.qr_available = keep_qr;
    if (!keep_qr)
    {
        qr = s_music.login_qr;
        s_music.login_qr = NULL;
    }
    if (qr_status != NULL)
    {
        (void)snprintf(s_music.status.qr_status,
                       sizeof(s_music.status.qr_status), "%s", qr_status);
    }
    finalized = true;
    music_service_unlock();
    music_service_login_qr_release(qr);
    if (finalized)
    {
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    }
    return finalized;
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

static bool _music_service_login_abort(uint32_t session, bool *expired)
{
    const bool finalized = _music_service_login_terminal(
                               session, MUSIC_SERVICE_STATE_QR_EXPIRED,
                               MUSIC_SERVICE_FAILURE_AUTH,
                               "二维码已过期，请重新登录", true);
    if (finalized && expired != NULL)
    {
        *expired = true;
    }
    return finalized;
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
        (void)_music_service_login_terminal(
            session, MUSIC_SERVICE_STATE_ERROR, failure, "登录未完成",
            false);
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
                poll_result == MUSIC_ERR_HTTP_AUTH ?
                MUSIC_SERVICE_FAILURE_API :
                _music_service_failure_from_result(poll_result);
            (void)_music_service_login_terminal(
                session, MUSIC_SERVICE_STATE_ERROR, failure, "登录未完成",
                false);
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
            (void)_music_service_login_abort(session, &expired);
            break;
        }

        if (atomic_load_explicit(&s_login_cancel, memory_order_acquire) ||
                !_login_session_current(session))
        {
            break;
        }
        esp_err_t save_result =
            music_net_save_credential_if_current(&credential, session);
        if (save_result == ESP_ERR_INVALID_STATE &&
                _login_session_current(session) &&
                !atomic_load_explicit(&s_login_cancel,
                                      memory_order_acquire))
        {
            /* The persistence helper rejected the session only if logout or
             * another terminal transition invalidated it. */
            break;
        }
        if (save_result != ESP_OK)
        {
            (void)_music_service_login_terminal(
                session, MUSIC_SERVICE_STATE_AUTH_REQUIRED,
                MUSIC_SERVICE_FAILURE_STORAGE, "登录态保存失败", false);
            break;
        }
        if (!_login_session_current(session) ||
                atomic_load_explicit(&s_login_cancel,
                                     memory_order_acquire))
        {
            (void)music_net_erase_credential();
            break;
        }
        music_home_snapshot_t *old_home = NULL;
        music_playlist_snapshot_t *old_playlists[MUSIC_PLAYLIST_CACHE_SLOTS] =
        {0};
        music_queue_snapshot_t *old_queue = NULL;
        music_queue_snapshot_t *old_queue_view = NULL;
        music_lyric_snapshot_t *old_lyric = NULL;
        music_image_t *old_images[MUSIC_IMAGE_CACHE_SLOTS] = {0};
        music_image_t *old_qr = NULL;
        if (music_service_lock() != ESP_OK)
        {
            (void)music_net_erase_credential();
            break;
        }
        if (!s_music.login_active || s_music.login_session != session ||
                !_login_session_current(session))
        {
            music_service_unlock();
            (void)music_net_erase_credential();
            break;
        }
        old_qr = s_music.login_qr;
        s_music.login_qr = NULL;
        _music_service_clear_account_locked(&old_home, old_playlists,
                                            &old_queue, &old_queue_view,
                                            &old_lyric, old_images);
        s_music.credential = credential;
        s_music.credential_state = MUSIC_CREDENTIAL_VALIDATED;
        s_music.login_active = false;
        s_music.session_check_pending = false;
        s_music.session_retry_ms = 0U;
        s_music.last_session_check_ms = _music_service_now_ms();
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
        const uint32_t credential_generation =
            s_music.credential_generation;
        music_service_unlock();
        _music_service_release_account(old_home, old_playlists, old_queue,
                                       old_queue_view, old_lyric, old_images);
        music_service_login_qr_release(old_qr);
        _music_log_api_stack("login");
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
        music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                     MUSIC_SERVICE_FAILURE_NONE);
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U);
        const music_api_command_t refresh =
        {
            .type = MUSIC_API_CMD_REFRESH_HOME,
            .credential_generation = credential_generation,
        };
        (void)xQueueSend(s_music.api_queue, &refresh,
                         pdMS_TO_TICKS(100U));
        terminal_ok = true;
        break;
    }

    music_service_lock();
    const bool owns_login = s_music.login_active &&
                            s_music.login_session == session;
    music_service_unlock();
    if (owns_login)
    {
        (void)_music_service_login_terminal(
            session, MUSIC_SERVICE_STATE_ERROR,
            MUSIC_SERVICE_FAILURE_INTERNAL, "登录未完成", false);
    }
    if (!terminal_ok && !expired)
    {
        music_net_cancel_login();
    }
}

static void _login_task(void *argument)
{
    (void)argument;
    for (;;)
    {
        music_login_command_t command = {0};
        if (xQueueReceive(s_music.login_queue, &command,
                          pdMS_TO_TICKS(MUSIC_SERVICE_SESSION_CHECK_POLL_MS)) !=
                pdTRUE)
        {
            if (atomic_load_explicit(&s_api_stop, memory_order_acquire))
            {
                break;
            }
            _music_service_session_tick();
            continue;
        }
        if (atomic_load_explicit(&s_api_stop, memory_order_acquire))
        {
            break;
        }
        if (command.command_type == MUSIC_LOGIN_COMMAND_CHECK_SESSION)
        {
            _music_service_check_session(command.credential_generation);
            continue;
        }
        if (command.command_type != MUSIC_LOGIN_COMMAND_LOGIN ||
                !_login_session_current(command.session) ||
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

static esp_err_t _music_service_ensure_login_worker(void)
{
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.login_task != NULL)
    {
        music_service_unlock();
        return ESP_OK;
    }
    atomic_store_explicit(&s_login_exited, false, memory_order_release);
    const BaseType_t created = xTaskCreatePinnedToCore(
                                   _login_task, "music_login",
                                   CONFIG_MUSIC_SERVICE_LOGIN_TASK_STACK_SIZE,
                                   NULL, s_music.config.api_task_priority,
                                   &s_music.login_task, 0);
    if (created != pdPASS)
    {
        s_music.login_task = NULL;
        atomic_store_explicit(&s_login_exited, true, memory_order_release);
    }
    music_service_unlock();
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
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
        music_api_command_t *const command = &s_api_command;
        if (xQueueReceive(s_music.api_queue, command,
                          portMAX_DELAY) != pdTRUE)
        {
            _music_service_recheck_time();
            continue;
        }
        if (command->type != MUSIC_API_CMD_LOGOUT &&
                command->type != MUSIC_API_CMD_WAKE)
        {
            bool current = false;
            if (music_service_lock() == ESP_OK)
            {
                if (command->type == MUSIC_API_CMD_IMAGE)
                {
                    current =
                        s_music.credential_generation ==
                        command->credential_generation &&
                        s_music.credential_state !=
                        MUSIC_CREDENTIAL_PENDING_VALIDATION &&
                        s_music.status.state !=
                        MUSIC_SERVICE_STATE_AUTH_REQUIRED;
                }
                else
                {
                    current =
                        s_music.credential_state ==
                        MUSIC_CREDENTIAL_VALIDATED &&
                        s_music.credential_generation ==
                        command->credential_generation;
                }
                music_service_unlock();
            }
            if (!current)
            {
                if (command->type == MUSIC_API_CMD_PLAY_SNAPSHOT)
                {
                    music_snapshot_release(command->playlist_snapshot);
                }
                continue;
            }
        }
        switch (command->type)
        {
        case MUSIC_API_CMD_LOGOUT:
        {
            /* Stop a pending QR session before touching storage. The account
             * state itself stays intact until the credential erase succeeds. */
            atomic_store_explicit(&s_login_cancel, true, memory_order_release);
            if (atomic_fetch_add_explicit(&s_login_generation, 1U,
                                          memory_order_acq_rel) == UINT32_MAX)
            {
                atomic_store_explicit(&s_login_generation, 1U,
                                      memory_order_release);
            }
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
            music_image_t *old_images[MUSIC_IMAGE_CACHE_SLOTS] = {0};
            music_image_t *old_qr = NULL;
            music_service_lock();
            _music_service_clear_account_locked(&old_home, old_playlists,
                                                &old_queue, &old_queue_view,
                                                &old_lyric, old_images);
            s_music.login_active = false;
            s_music.login_session = 0U;
            s_music.login_type = MUSIC_LOGIN_TYPE_WECHAT;
            s_music.status.qr_status[0] = '\0';
            s_music.status.login_type = MUSIC_LOGIN_TYPE_WECHAT;
            old_qr = s_music.login_qr;
            s_music.login_qr = NULL;
            music_service_set_state_locked(MUSIC_SERVICE_STATE_LOGGED_OUT,
                                           MUSIC_SERVICE_FAILURE_NONE);
            music_service_unlock();
            _music_service_release_account(old_home, old_playlists, old_queue,
                                           old_queue_view, old_lyric,
                                           old_images);
            music_service_login_qr_release(old_qr);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_HOME, 0U);
            music_service_publish_detail(MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                                         MUSIC_SERVICE_FAILURE_NONE);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
            music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_LYRIC, 0U);
            break;
        }
        case MUSIC_API_CMD_REFRESH_PROFILE:
        {
            _music_service_refresh_profile(true,
                                           command->credential_generation);
            break;
        }
        case MUSIC_API_CMD_REFRESH_HOME:
        {
            music_home_snapshot_t *snapshot = NULL;
            const esp_err_t result = music_net_fetch_home(&snapshot);
            if (result == ESP_OK)
            {
                music_service_replace_home(snapshot,
                                           command->credential_generation);
            }
            else
            {
                music_service_handle_auth_result(result,
                                                 command->credential_generation);
                music_service_handle_network_result_for_generation(
                    result, command->credential_generation);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
            }
            _music_service_refresh_profile(false,
                                           command->credential_generation);
            break;
        }
        case MUSIC_API_CMD_PLAYLIST:
        {
            music_playlist_snapshot_t *snapshot = NULL;
            const esp_err_t result = music_net_fetch_playlist(
                                         command->credential_generation,
                                         &command->playlist, command->offset,
                                         command->count, &snapshot);
            if (result == ESP_OK)
            {
                music_service_replace_playlist(snapshot,
                                               command->credential_generation);
            }
            else
            {
                const music_service_failure_t failure =
                    _music_service_failure_from_result(result);
                music_service_handle_auth_result(
                    result, command->credential_generation);
                music_service_handle_network_result_for_generation(
                    result, command->credential_generation);
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
                                         command->credential_generation,
                                         command->songmid, &snapshot);
            if (result == MUSIC_ERR_HTTP_AUTH)
            {
                music_snapshot_release(snapshot);
                music_service_handle_auth_result(
                    result, command->credential_generation);
                break;
            }
            const music_service_failure_t failure =
                _music_service_failure_from_result(result);
            music_lyric_snapshot_t *old_lyric = NULL;
            bool committed = false;
            music_service_lock();
            if (s_music.credential_generation ==
                    command->credential_generation &&
                    s_music.lyric_request_id == command->token &&
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
                music_service_set_image(image, command->token,
                                        command->credential_generation);
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
                                               command->credential_generation,
                                               &command->playlist, fetch_offset,
                                               MUSIC_SERVICE_QUEUE_WINDOW_SONGS,
                                               &playlist);
            if (fetch_result == MUSIC_ERR_HTTP_AUTH)
            {
                music_snapshot_release(playlist);
                music_service_handle_auth_result(
                    fetch_result, command->credential_generation);
                break;
            }
            if (fetch_result != ESP_OK || playlist == NULL ||
                    playlist->count == 0U)
            {
                music_service_handle_network_result_for_generation(
                    fetch_result == ESP_OK ? ESP_ERR_NOT_FOUND : fetch_result,
                    command->credential_generation);
                music_snapshot_release(playlist);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                const esp_err_t play_failure =
                    fetch_result == ESP_OK ? ESP_ERR_NOT_FOUND : fetch_result;
                _music_service_player_error(
                    command->credential_generation, play_generation,
                    _music_service_failure_from_result(play_failure));
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
                music_service_handle_network_result_for_generation(
                    ESP_ERR_INVALID_ARG, command->credential_generation);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                _music_service_player_error(command->credential_generation,
                                            play_generation,
                                            MUSIC_SERVICE_FAILURE_API);
                break;
            }
            music_queue_snapshot_t *queue =
                _music_service_queue_from_playlist_snapshot(playlist);
            music_snapshot_release(playlist);
            if (queue == NULL)
            {
                music_service_handle_network_result_for_generation(
                    ESP_ERR_NO_MEM, command->credential_generation);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                      0U);
                _music_service_player_error(command->credential_generation,
                                            play_generation,
                                            MUSIC_SERVICE_FAILURE_INTERNAL);
                break;
            }
            music_queue_snapshot_t *held_queue =
                music_snapshot_retain(queue);
            if (!_music_service_queue_install_source(
                        queue, (uint16_t)total, command->credential_generation,
                        play_generation))
            {
                music_snapshot_release(held_queue);
                break;
            }
            const esp_err_t start_result = _music_service_start_queue_index(
                                               held_queue, requested_index,
                                               command->credential_generation,
                                               play_generation);
            music_snapshot_release(held_queue);
            if (start_result == ESP_OK)
            {
                _music_service_queue_prefetch_window(play_generation);
            }
            else
            {
                _music_service_report_start_failure(start_result,
                                                    command->credential_generation,
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
                music_service_handle_network_result_for_generation(
                    ESP_ERR_INVALID_ARG, command->credential_generation);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                _music_service_player_error(command->credential_generation,
                                            play_generation,
                                            MUSIC_SERVICE_FAILURE_API);
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
                music_service_handle_network_result_for_generation(
                    ESP_ERR_INVALID_ARG, command->credential_generation);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                _music_service_player_error(command->credential_generation,
                                            play_generation,
                                            MUSIC_SERVICE_FAILURE_API);
                break;
            }
            music_queue_snapshot_t *queue =
                _music_service_queue_from_playlist_snapshot(playlist);
            music_snapshot_release(playlist);
            if (queue == NULL)
            {
                music_service_handle_network_result_for_generation(
                    ESP_ERR_NO_MEM, command->credential_generation);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                _music_service_player_error(command->credential_generation,
                                            play_generation,
                                            MUSIC_SERVICE_FAILURE_INTERNAL);
                break;
            }
            music_queue_snapshot_t *held_queue =
                music_snapshot_retain(queue);
            if (!_music_service_queue_install_source(
                        queue, (uint16_t)total, command->credential_generation,
                        play_generation))
            {
                music_snapshot_release(held_queue);
                break;
            }
            const esp_err_t start_result = _music_service_start_queue_index(
                                               held_queue, (uint16_t)absolute,
                                               command->credential_generation,
                                               play_generation);
            music_snapshot_release(held_queue);
            if (start_result == ESP_OK)
            {
                _music_service_queue_prefetch_window(play_generation);
            }
            else
            {
                _music_service_report_start_failure(start_result,
                                                    command->credential_generation,
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
            const bool current = s_music.credential_state ==
                                 MUSIC_CREDENTIAL_VALIDATED &&
                                 s_music.credential_generation ==
                                 command->credential_generation;
            if (!current)
            {
                music_service_unlock();
                break;
            }
            music_queue_snapshot_t *queue =
                music_snapshot_retain(s_music.queue);
            const uint16_t total = s_music.queue_total;
            const uint16_t source_total = s_music.queue_source_total;
            const uint16_t removed_count = s_music.queue_removed_count;
            const uint64_t queue_epoch = s_music.queue_epoch;
            const uint32_t credential_generation =
                command->credential_generation;
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
                music_service_handle_network_result_for_generation(
                    ESP_ERR_INVALID_STATE, command->credential_generation);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            if (!music_queue_window_contains(queue->offset, queue->count, index))
            {
                music_snapshot_release(queue);
                uint8_t *removed_copy = _music_service_queue_removed_copy();
                if (removed_copy == NULL)
                {
                    music_service_handle_network_result_for_generation(
                        ESP_ERR_NO_MEM, command->credential_generation);
                    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                          0U);
                    _music_service_player_error(
                        command->credential_generation, play_generation,
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
                        credential_generation, &ref, source_total,
                        removed_copy, removed_count, total, offset, count,
                        &window);
                heap_caps_free(removed_copy);
                if (fetch_result != ESP_OK)
                {
                    music_service_handle_network_result_for_generation(
                        fetch_result, command->credential_generation);
                    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS,
                                          0U);
                    _music_service_player_error(
                        command->credential_generation, play_generation,
                        _music_service_failure_from_result(fetch_result));
                    break;
                }
                music_queue_snapshot_t *held_window =
                    music_snapshot_retain(window);
                if (held_window == NULL ||
                        !_music_service_commit_queue_window(
                            window, credential_generation, queue_epoch,
                            play_generation))
                {
                    music_snapshot_release(held_window);
                    break;
                }
                queue = held_window;
            }
            const esp_err_t start_result = _music_service_start_queue_index(
                                               queue, index, credential_generation,
                                               play_generation);
            music_snapshot_release(queue);
            if (start_result == ESP_OK)
            {
                _music_service_queue_prefetch_window(play_generation);
            }
            else
            {
                _music_service_report_start_failure(start_result,
                                                    command->credential_generation,
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
            const bool current_account =
                s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED &&
                s_music.credential_generation ==
                command->credential_generation;
            if (!current_account)
            {
                music_service_unlock();
                break;
            }
            const uint16_t source_total = s_music.queue_source_total;
            const uint16_t old_total = s_music.queue_total;
            const uint16_t old_removed_count = s_music.queue_removed_count;
            const uint16_t current = s_music.player.queue_index;
            const bool play_valid = s_music.play_valid;
            const music_playback_state_t player_state = s_music.player.state;
            const uint64_t queue_epoch = s_music.queue_epoch;
            const uint32_t credential_generation =
                command->credential_generation;
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
                music_service_handle_network_result_for_generation(
                    ESP_ERR_NO_MEM, command->credential_generation);
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
                music_service_handle_network_result_for_generation(
                    ESP_ERR_INVALID_STATE, command->credential_generation);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            if (transition.count == 0U)
            {
                heap_caps_free(removed_snapshot);
                music_queue_snapshot_t *old_queue = NULL;
                music_queue_snapshot_t *old_view = NULL;
                music_service_lock();
                if (s_music.queue_epoch != queue_epoch ||
                        s_music.credential_generation !=
                        credential_generation)
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
                (void)music_stream_stop();
                atomic_store_explicit(&s_playing, false,
                                      memory_order_release);
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
                                               credential_generation, &ref,
                                               source_total, removed_map,
                                               (uint16_t)(old_removed_count + 1U),
                                               transition.count, offset, count,
                                               &next);
            if (build_result != ESP_OK)
            {
                heap_caps_free(removed_map);
                music_service_handle_network_result_for_generation(
                    build_result, command->credential_generation);
                music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
                break;
            }
            music_queue_snapshot_t *old_queue = NULL;
            music_queue_snapshot_t *old_view = NULL;
            music_service_lock();
            if (s_music.queue_epoch != queue_epoch ||
                    s_music.credential_generation != credential_generation)
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
                     player_state == MUSIC_PLAYBACK_BUFFERING) &&
                    _music_service_play_owner_current(credential_generation,
                            play_generation))
            {
                const esp_err_t start_result =
                    _music_service_start_queue_index(
                        next, transition.index, credential_generation,
                        play_generation);
                if (start_result == ESP_OK)
                {
                    _music_service_queue_prefetch_window(play_generation);
                }
                else
                {
                    _music_service_report_start_failure(start_result,
                                                        command->credential_generation,
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
                if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
                        s_music.credential_generation != credential_generation ||
                        !s_music.play_valid ||
                        s_music.play_generation != play_generation)
                {
                    music_service_unlock();
                    break;
                }
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
            uint32_t credential_generation = 0U;
            bool epoch_ok = false;
            music_service_lock();
            if (s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED &&
                    s_music.credential_generation ==
                    command->credential_generation &&
                    s_music.queue_epoch == queue_epoch)
            {
                epoch_ok = true;
                credential_generation = s_music.credential_generation;
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
                music_service_lock();
                const bool owned =
                    s_music.credential_generation ==
                    command->credential_generation &&
                    s_music.queue_epoch == queue_epoch;
                if (owned)
                {
                    s_music.queue_view_pending = false;
                }
                music_service_unlock();
                if (owned)
                {
                    music_service_publish_detail(
                        MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                        MUSIC_SERVICE_FAILURE_INTERNAL);
                }
                break;
            }
            music_queue_snapshot_t *view = NULL;
            const esp_err_t result = _music_service_queue_build_view(
                                         credential_generation, &ref,
                                         source_total, removed, removed_count,
                                         total, offset, count, &view);
            heap_caps_free(removed);
            if (result == ESP_OK)
            {
                if (music_service_lock() != ESP_OK ||
                        s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
                        s_music.credential_generation !=
                        command->credential_generation ||
                        s_music.queue_epoch != queue_epoch)
                {
                    music_service_unlock();
                    music_snapshot_release(view);
                    break;
                }
                music_service_unlock();
                music_service_replace_queue_view(
                    view, command->credential_generation);
            }
            else
            {
                bool owned = false;
                music_service_lock();
                owned = s_music.credential_generation ==
                        command->credential_generation &&
                        s_music.queue_epoch == queue_epoch;
                if (owned)
                {
                    s_music.queue_view_pending = false;
                }
                music_service_unlock();
                if (owned)
                {
                    music_service_handle_auth_result(
                        result, command->credential_generation);
                    music_service_publish_detail(
                        MUSIC_SERVICE_MSG_SUB_TYPE_QUEUE, 0U,
                        _music_service_failure_from_result(result));
                }
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
    s_music.credential_lock = xSemaphoreCreateMutex();
    /* PSRAM-backed queue; delete it with vQueueDeleteWithCaps(). */
    s_music.api_queue = xQueueCreateWithCaps(
                            CONFIG_MUSIC_SERVICE_API_QUEUE_DEPTH,
                            sizeof(music_api_command_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_music.login_queue = xQueueCreateWithCaps(
                              4U, sizeof(music_login_command_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const esp_err_t http_result = music_http_init();
    if (s_music.lock == NULL || s_music.credential_lock == NULL ||
            s_music.api_queue == NULL || s_music.login_queue == NULL ||
            http_result != ESP_OK)
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
        if (s_music.credential_lock != NULL)
        {
            vSemaphoreDelete(s_music.credential_lock);
            s_music.credential_lock = NULL;
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
        s_music.credential = credential;
        s_music.credential_state = MUSIC_CREDENTIAL_PENDING_VALIDATION;
        s_music.credential_generation = 1U;
        s_music.login_type = credential.login_type == 2U ?
                             MUSIC_LOGIN_TYPE_QQ :
                             MUSIC_LOGIN_TYPE_WECHAT;
        s_music.status.login_type = s_music.login_type;
        s_music.status.logged_in = false;
        _music_service_set_account_locked(NULL);
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
        if (s_music.credential_lock != NULL)
        {
            vSemaphoreDelete(s_music.credential_lock);
        }
        memset(&s_music, 0, sizeof(s_music));
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t worker_result = _music_service_ensure_login_worker();
    if (worker_result != ESP_OK)
    {
        atomic_store_explicit(&s_api_stop, true, memory_order_release);
        const music_api_command_t wake =
        {
            .type = MUSIC_API_CMD_WAKE,
        };
        (void)xQueueSend(s_music.api_queue, &wake, pdMS_TO_TICKS(100U));
        for (uint32_t wait = 0U;
                wait < 100U &&
                !atomic_load_explicit(&s_api_exited, memory_order_acquire);
                ++wait)
        {
            vTaskDelay(pdMS_TO_TICKS(10U));
        }
        s_music.initialized = false;
        vQueueDeleteWithCaps(s_music.api_queue);
        s_music.api_queue = NULL;
        vQueueDeleteWithCaps(s_music.login_queue);
        s_music.login_queue = NULL;
        music_http_deinit();
        vSemaphoreDelete(s_music.lock);
        s_music.lock = NULL;
        if (s_music.credential_lock != NULL)
        {
            vSemaphoreDelete(s_music.credential_lock);
            s_music.credential_lock = NULL;
        }
        memset(&s_music, 0, sizeof(s_music));
        return worker_result;
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
    if (atomic_fetch_add_explicit(&s_login_generation, 1U,
                                  memory_order_acq_rel) == UINT32_MAX)
    {
        atomic_store_explicit(&s_login_generation, 1U, memory_order_release);
    }
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
    for (size_t slot = 0U; slot < MUSIC_IMAGE_CACHE_SLOTS; ++slot)
    {
        music_service_image_release(s_music.image[slot]);
    }
    music_net_clear_session();
    music_http_deinit();
    if (s_music.lock != NULL)
    {
        vSemaphoreDelete(s_music.lock);
        s_music.lock = NULL;
    }
    if (s_music.credential_lock != NULL)
    {
        vSemaphoreDelete(s_music.credential_lock);
        s_music.credential_lock = NULL;
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
    bool should_validate = false;
    const bool was_ready =
        s_music.status.network_ready &&
        s_music.status.time_ready;
    s_music.status.network_ready = ready;
    s_music.status.time_ready =
        time_service_get_quality() != TIME_SERVICE_QUALITY_INVALID;

    const music_service_state_t state = s_music.status.state;
    const bool auth_required = state == MUSIC_SERVICE_STATE_AUTH_REQUIRED;
    const bool preserving_login =
        s_music.login_active ||
        state == MUSIC_SERVICE_STATE_QR_READY ||
        state == MUSIC_SERVICE_STATE_QR_SCANNED ||
        state == MUSIC_SERVICE_STATE_QR_EXPIRED ||
        state == MUSIC_SERVICE_STATE_AUTHENTICATING ||
        auth_required;

    if (preserving_login)
    {
        const music_service_failure_t failure =
            auth_required ? s_music.status.failure :
            (ready ? MUSIC_SERVICE_FAILURE_NONE :
             MUSIC_SERVICE_FAILURE_NETWORK);
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
    else if (!s_music.status.time_ready)
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_WAITING_TIME,
                                       MUSIC_SERVICE_FAILURE_TIME);
        changed = true;
    }
    else if (s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED)
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_READY,
                                       MUSIC_SERVICE_FAILURE_NONE);
        changed = true;
    }
    else if (s_music.credential_state ==
             MUSIC_CREDENTIAL_PENDING_VALIDATION)
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_VALIDATING_SESSION,
                                       MUSIC_SERVICE_FAILURE_NONE);
        changed = true;
    }
    else
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_LOGGED_OUT,
                                       MUSIC_SERVICE_FAILURE_NONE);
        changed = true;
    }
    const bool now_ready =
        s_music.status.network_ready && s_music.status.time_ready;
    should_validate = !was_ready && now_ready;
    music_service_unlock();
    if (changed)
    {
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    }
    if (should_validate)
    {
        _music_service_request_session_check();
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
    const bool was_ready =
        s_music.status.network_ready && s_music.status.time_ready;
    if (s_music.status.time_ready != ready)
    {
        s_music.status.time_ready = ready;
        changed = true;
    }
    if (!ready &&
            (s_music.status.state == MUSIC_SERVICE_STATE_READY ||
             s_music.status.state == MUSIC_SERVICE_STATE_LOGGED_OUT ||
             s_music.status.state == MUSIC_SERVICE_STATE_VALIDATING_SESSION))
    {
        music_service_set_state_locked(MUSIC_SERVICE_STATE_WAITING_TIME,
                                       MUSIC_SERVICE_FAILURE_TIME);
        changed = true;
    }
    else if (ready && s_music.status.network_ready &&
             s_music.status.state == MUSIC_SERVICE_STATE_WAITING_TIME)
    {
        const music_service_state_t resume_state =
            s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED ?
            MUSIC_SERVICE_STATE_READY :
            s_music.credential_state == MUSIC_CREDENTIAL_PENDING_VALIDATION ?
            MUSIC_SERVICE_STATE_VALIDATING_SESSION :
            MUSIC_SERVICE_STATE_LOGGED_OUT;
        music_service_set_state_locked(resume_state,
                                       MUSIC_SERVICE_FAILURE_NONE);
        changed = true;
    }
    const bool now_ready =
        s_music.status.network_ready && s_music.status.time_ready;
    const bool should_validate = !was_ready && now_ready;
    music_service_unlock();
    if (changed)
    {
        music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    }
    if (should_validate)
    {
        _music_service_request_session_check();
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
    music_service_failure_t failure = MUSIC_SERVICE_FAILURE_NONE;
    if (!s_music.status.network_ready)
    {
        state = MUSIC_SERVICE_STATE_WAITING_NETWORK;
    }
    else if (!s_music.status.time_ready)
    {
        state = MUSIC_SERVICE_STATE_WAITING_TIME;
    }
    else if (s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED)
    {
        state = MUSIC_SERVICE_STATE_READY;
    }
    else if (s_music.credential_state ==
             MUSIC_CREDENTIAL_PENDING_VALIDATION)
    {
        state = MUSIC_SERVICE_STATE_VALIDATING_SESSION;
    }
    else if (s_music.status.state == MUSIC_SERVICE_STATE_AUTH_REQUIRED)
    {
        state = MUSIC_SERVICE_STATE_AUTH_REQUIRED;
        failure = s_music.status.failure;
    }
    else
    {
        state = MUSIC_SERVICE_STATE_LOGGED_OUT;
    }
    music_service_set_state_locked(state, failure);
    music_service_unlock();
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_STATUS, 0U);
    _music_service_request_session_check();
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
    const esp_err_t worker_result = _music_service_ensure_login_worker();
    if (worker_result != ESP_OK)
    {
        return worker_result;
    }
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool allowed = !s_music.login_active &&
                         s_music.status.state !=
                         MUSIC_SERVICE_STATE_VALIDATING_SESSION &&
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
        const uint32_t previous_session = atomic_fetch_add_explicit(
                                              &s_login_generation, 1U,
                                              memory_order_acq_rel);
        login_session = previous_session == UINT32_MAX ? 1U :
                        previous_session + 1U;
        if (login_session == 1U && previous_session == UINT32_MAX)
        {
            atomic_store_explicit(&s_login_generation, 1U,
                                  memory_order_release);
        }
        s_music.login_session = login_session;
        atomic_store_explicit(&s_login_cancel, false, memory_order_release);
    }
    music_service_unlock();
    music_service_login_qr_release(old_qr);
    if (!allowed)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const music_login_command_t command =
    {
        .command_type = MUSIC_LOGIN_COMMAND_LOGIN,
        .type = type,
        .session = login_session,
    };
    if (xQueueSend(s_music.login_queue, &command, pdMS_TO_TICKS(100U)) !=
            pdTRUE)
    {
        atomic_store_explicit(&s_login_cancel, true, memory_order_release);
        if (atomic_fetch_add_explicit(&s_login_generation, 1U,
                                      memory_order_acq_rel) == UINT32_MAX)
        {
            atomic_store_explicit(&s_login_generation, 1U,
                                  memory_order_release);
        }
        music_image_t *qr = NULL;
        if (music_service_lock() == ESP_OK)
        {
            if (s_music.login_session == login_session)
            {
                s_music.login_active = false;
                s_music.login_session = 0U;
                qr = s_music.login_qr;
                s_music.login_qr = NULL;
                s_music.status.qr_available = false;
            }
            music_service_unlock();
        }
        music_service_login_qr_release(qr);
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
    if (!s_music.initialized || music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_login_cancel, true, memory_order_release);
    if (atomic_fetch_add_explicit(&s_login_generation, 1U,
                                  memory_order_acq_rel) == UINT32_MAX)
    {
        atomic_store_explicit(&s_login_generation, 1U,
                              memory_order_release);
    }
    music_image_t *qr = s_music.login_qr;
    s_music.login_qr = NULL;
    s_music.login_active = false;
    s_music.login_session = 0U;
    if (s_music.status.state != MUSIC_SERVICE_STATE_AUTH_REQUIRED &&
            (s_music.status.state == MUSIC_SERVICE_STATE_QR_READY ||
             s_music.status.state == MUSIC_SERVICE_STATE_QR_SCANNED ||
             s_music.status.state == MUSIC_SERVICE_STATE_QR_EXPIRED ||
             s_music.status.state == MUSIC_SERVICE_STATE_AUTHENTICATING))
    {
        const music_service_state_t resume_state =
            s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED ?
            MUSIC_SERVICE_STATE_READY :
            s_music.credential_state == MUSIC_CREDENTIAL_PENDING_VALIDATION ?
            MUSIC_SERVICE_STATE_VALIDATING_SESSION :
            MUSIC_SERVICE_STATE_LOGGED_OUT;
        music_service_set_state_locked(resume_state,
                                       MUSIC_SERVICE_FAILURE_NONE);
        s_music.status.qr_status[0] = '\0';
        s_music.status.qr_available = false;
    }
    music_service_unlock();
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
    if (s_music.api_queue == NULL || music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t credential_generation = s_music.credential_generation;
    const bool cached = s_music.home != NULL;
    const bool profile_missing = !s_music.profile_loaded &&
                                 !s_music.profile_requested;
    if (!force && cached && profile_missing)
    {
        s_music.profile_requested = true;
    }
    music_service_unlock();

    if (!force && cached)
    {
        if (!profile_missing)
        {
            return ESP_OK;
        }
        const music_api_command_t profile_command =
        {
            .type = MUSIC_API_CMD_REFRESH_PROFILE,
            .credential_generation = credential_generation,
        };
        if (xQueueSend(s_music.api_queue, &profile_command,
                       pdMS_TO_TICKS(100U)) == pdTRUE)
        {
            return ESP_OK;
        }
        if (music_service_lock() == ESP_OK)
        {
            if (s_music.credential_generation == credential_generation)
            {
                s_music.profile_requested = false;
            }
            music_service_unlock();
        }
        return ESP_ERR_TIMEOUT;
    }
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_REFRESH_HOME,
        .credential_generation = credential_generation,
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED)
    {
        music_service_unlock();
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
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.api_queue == NULL)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t credential_generation = s_music.credential_generation;
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
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_PLAYLIST,
        .credential_generation = credential_generation,
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t credential_generation = s_music.credential_generation;
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
        .credential_generation = credential_generation,
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
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.credential_state == MUSIC_CREDENTIAL_PENDING_VALIDATION ||
            s_music.status.state == MUSIC_SERVICE_STATE_AUTH_REQUIRED)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t credential_generation = s_music.credential_generation;
    for (size_t slot = 0U; slot < MUSIC_IMAGE_CACHE_SLOTS; ++slot)
    {
        music_image_t *current = s_music.image[slot];
        if (!music_service_image_cache_matches(current, url, width,
                                               height, token))
        {
            continue;
        }
        current = music_snapshot_retain(current);
        s_music.image_used[slot] = ++s_music.image_clock;
        music_service_unlock();
        *image = current;
        return ESP_OK;
    }
    if (s_music.api_queue == NULL)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    music_service_unlock();
    music_api_command_t command =
    {
        .type = MUSIC_API_CMD_IMAGE,
        .credential_generation = credential_generation,
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED)
    {
        music_service_unlock();
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.queue == NULL || s_music.queue_total == 0U)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t credential_generation = s_music.credential_generation;
    if (s_music.queue_view_pending &&
            s_music.queue_view_pending_offset == offset &&
            s_music.queue_view_pending_count == count)
    {
        music_service_unlock();
        return ESP_ERR_NOT_FOUND;
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
    const uint64_t queue_epoch = s_music.queue_epoch;
    s_music.queue_view_pending = true;
    s_music.queue_view_pending_offset = offset;
    s_music.queue_view_pending_count = count;
    music_service_unlock();
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_QUEUE_VIEW,
        .credential_generation = credential_generation,
        .queue_generation = queue_epoch,
        .offset = offset,
        .count = count,
    };
    if (s_music.api_queue == NULL ||
            xQueueSend(s_music.api_queue, &command, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        music_service_lock();
        if (s_music.credential_generation == credential_generation &&
                s_music.queue_epoch == queue_epoch)
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t credential_generation = s_music.credential_generation;
    const uint32_t previous_play_generation = s_music.play_generation;
    const uint32_t previous_player_generation =
        s_music.player.play_generation;
    const uint32_t play_generation =
        _music_service_next_play_generation_locked();
    s_music.player.play_generation = play_generation;
    s_music.play_skip_count = 0U;
    music_service_unlock();
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_PLAY,
        .credential_generation = credential_generation,
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED)
    {
        music_service_unlock();
        music_snapshot_release((void *)playlist);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t credential_generation = s_music.credential_generation;
    const uint32_t previous_play_generation = s_music.play_generation;
    const uint32_t previous_player_generation =
        s_music.player.play_generation;
    const uint32_t play_generation =
        _music_service_next_play_generation_locked();
    s_music.player.play_generation = play_generation;
    s_music.play_skip_count = 0U;
    music_service_unlock();
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_PLAY_SNAPSHOT,
        .credential_generation = credential_generation,
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

static esp_err_t _music_service_queue_play_internal(uint16_t index,
        bool reset_skip)
{
    if (s_music.api_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (music_service_lock() != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.queue == NULL || index >= s_music.queue_total)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (reset_skip)
    {
        s_music.play_skip_count = 0U;
    }
    const uint32_t credential_generation = s_music.credential_generation;
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
        .credential_generation = credential_generation,
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

esp_err_t music_service_queue_play(uint16_t index)
{
    return _music_service_queue_play_internal(index, true);
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            s_music.queue == NULL || index >= s_music.queue_total)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t credential_generation = s_music.credential_generation;
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
        .credential_generation = credential_generation,
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
    if (s_music.credential_state != MUSIC_CREDENTIAL_VALIDATED ||
            !s_music.play_valid)
    {
        music_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t credential_generation = s_music.credential_generation;
    const uint32_t play_generation = s_music.player.play_generation;
    music_service_unlock();
    const music_api_command_t command =
    {
        .type = MUSIC_API_CMD_PAUSE,
        .credential_generation = credential_generation,
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
    const bool valid =
        s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED &&
        s_music.play_valid;
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
    const bool valid =
        s_music.credential_state == MUSIC_CREDENTIAL_VALIDATED &&
        s_music.play_valid;
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
    s_music.play_skip_count = 0U;
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

void music_service_stream_failed(music_service_failure_t failure,
                                 uint32_t play_generation)
{
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    const bool current = s_music.play_valid &&
                         s_music.play_generation == play_generation;
    if (current)
    {
        s_music.player.state = MUSIC_PLAYBACK_ERROR;
        s_music.player.failure = failure;
        s_music.status.failure = failure;
        s_music.play_valid = false;
        s_music.player.play_generation = 0U;
    }
    music_service_unlock();
    if (!current)
    {
        return;
    }
    atomic_store_explicit(&s_playing, false, memory_order_release);
    music_service_publish(MUSIC_SERVICE_MSG_SUB_TYPE_PLAYER, 0U);
}

void music_service_stream_finished(uint32_t play_generation)
{
    if (music_service_lock() != ESP_OK)
    {
        return;
    }
    const bool valid = s_music.play_valid &&
                       s_music.play_generation == play_generation;
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
