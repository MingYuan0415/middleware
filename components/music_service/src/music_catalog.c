#define DBG_TAG "music_catalog"
#include "mt_log.h"

#include "music_service_internal.h"
#include "music_api.h"
#include "music_http.h"
#include "music_profile.h"
#include "music_snapshot.h"

#include "cJSON.h"
#include "esp_random.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void _parse_playlist_basic(const cJSON *playlist,
                                  music_playlist_t *output,
                                  music_playlist_kind_t kind)
{
    memset(output, 0, sizeof(*output));
    output->ref.kind = (uint8_t)kind;
    const cJSON *basic = cJSON_GetObjectItemCaseSensitive(playlist, "basic");
    if (basic == NULL)
    {
        basic = playlist;
    }
    output->ref.id = music_json_u64(basic, "tid");
    if (output->ref.id == 0U)
    {
        output->ref.id = music_json_u64(basic, "dissid");
    }
    output->ref.dirid = music_json_u32(basic, "dirid");
    output->ref.tid = music_json_u32(basic, "tid");
    music_json_string(basic, "title", output->title, sizeof(output->title));
    const cJSON *creator = cJSON_GetObjectItemCaseSensitive(basic, "creator");
    music_json_string(creator, "nick", output->creator, sizeof(output->creator));
    const cJSON *cover = cJSON_GetObjectItemCaseSensitive(basic, "cover");
    music_json_string(cover, "small_url", output->cover_url,
                      sizeof(output->cover_url));
    output->track_count = music_json_u32(basic, "song_cnt");
    if (output->track_count == 0U)
    {
        output->track_count = music_json_u32(basic, "songnum");
    }
}

static music_playlist_t *_home_playlist_slot(music_home_snapshot_t *home,
        music_playlist_kind_t kind, size_t *count)
{
    if (*count >= MUSIC_SERVICE_MAX_PLAYLISTS)
    {
        return NULL;
    }
    music_playlist_t *slot = NULL;
    if (kind == MUSIC_PLAYLIST_KIND_MY_CREATED)
    {
        slot = &home->my_created[*count];
    }
    else if (kind == MUSIC_PLAYLIST_KIND_MY_FAVORITE)
    {
        slot = &home->my_favorite[*count];
    }
    else
    {
        slot = &home->recommended[*count];
    }
    ++(*count);
    return slot;
}

static esp_err_t _home_recommended(music_home_snapshot_t *home)
{
    cJSON *param = cJSON_CreateObject();
    if (param == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(param, "From", 0);
    cJSON_AddNumberToObject(param, "Size", MUSIC_SERVICE_MAX_PLAYLISTS);
    cJSON *root = NULL;
    cJSON *data = music_api_post("music.playlist.PlaylistSquare",
                                 "GetRecommendFeed", param, &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return music_api_last_error();
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(data, "List");
    if (cJSON_IsArray(list))
    {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, list)
        {
            const cJSON *playlist = cJSON_GetObjectItemCaseSensitive(item,
                "Playlist");
            music_playlist_t *slot = _home_playlist_slot(
                                         home, MUSIC_PLAYLIST_KIND_RECOMMENDED,
                                         &home->recommended_count);
            if (slot == NULL)
            {
                break;
            }
            _parse_playlist_basic(playlist, slot,
                                  MUSIC_PLAYLIST_KIND_RECOMMENDED);
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t _home_personal(music_home_snapshot_t *home)
{
    if (!s_music.credential_valid)
    {
        home->login_required = true;
        return ESP_OK;
    }
    cJSON *param = cJSON_CreateObject();
    if (param == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(param, "uin", s_music.credential.musicid);
    cJSON *root = NULL;
    cJSON *data = music_api_post("music.musicasset.PlaylistBaseRead",
                                 "GetPlaylistByUin", param, &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return music_api_last_error();
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(data, "v_playlist");
    if (!cJSON_IsArray(list))
    {
        list = cJSON_GetObjectItemCaseSensitive(data, "disslist");
    }
    if (cJSON_IsArray(list))
    {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, list)
        {
            /* dirid 1 is a created playlist; dirid 201 is the favorite
             * playlist bucket exposed by QQ Music. The current API uses
             * dirId; keep dirid as a fallback for older responses. */
            uint32_t dirid = music_json_u32(item, "dirId");
            if (dirid == 0U)
            {
                dirid = music_json_u32(item, "dirid");
            }
            const music_playlist_kind_t kind =
                dirid == 201U ? MUSIC_PLAYLIST_KIND_MY_FAVORITE :
                MUSIC_PLAYLIST_KIND_MY_CREATED;
            size_t *count = kind == MUSIC_PLAYLIST_KIND_MY_FAVORITE ?
                            &home->my_favorite_count :
                            &home->my_created_count;
            music_playlist_t *slot = _home_playlist_slot(home, kind, count);
            if (slot == NULL)
            {
                continue;
            }
            memset(slot, 0, sizeof(*slot));
            slot->ref.kind = (uint8_t)kind;
            slot->ref.id = music_json_u64(item, "tid");
            slot->ref.dirid = dirid;
            slot->ref.tid = music_json_u32(item, "tid");
            music_json_string(item, "dirName", slot->title,
                              sizeof(slot->title));
            if (slot->title[0] == '\0')
            {
                music_json_string(item, "diss_name", slot->title,
                                  sizeof(slot->title));
            }
            music_json_string(item, "picUrl", slot->cover_url,
                              sizeof(slot->cover_url));
            if (slot->cover_url[0] == '\0')
            {
                music_json_string(item, "bigpicUrl", slot->cover_url,
                                  sizeof(slot->cover_url));
            }
            if (slot->cover_url[0] == '\0')
            {
                music_json_string(item, "diss_cover", slot->cover_url,
                                  sizeof(slot->cover_url));
            }
            const cJSON *creator =
                cJSON_GetObjectItemCaseSensitive(item, "creator");
            music_json_string(creator, "nick", slot->creator,
                              sizeof(slot->creator));
            if (slot->creator[0] == '\0')
            {
                music_json_string(item, "nick", slot->creator,
                                  sizeof(slot->creator));
            }
            slot->track_count = music_json_u32(item, "songNum");
            if (slot->track_count == 0U)
            {
                slot->track_count = music_json_u32(item, "song_cnt");
            }
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t _home_collected(music_home_snapshot_t *home)
{
    if (!s_music.credential_valid)
    {
        home->collected_ok = true;
        return ESP_OK;
    }
    char url[384];
    (void)snprintf(url, sizeof(url),
                   "https://c.y.qq.com/fav/fcgi-bin/"
                   "fcg_get_profile_order_asset.fcg"
                   "?ct=20&cid=205360956&userid=%s&reqtype=3&sin=0&ein=%u"
                   "&format=json",
                   s_music.credential.musicid,
                   (unsigned)MUSIC_SERVICE_MAX_PLAYLISTS);
    music_http_result_t result;
    const esp_err_t get_result = music_http_get(
                                     url, true, &result,
                                     CONFIG_MUSIC_SERVICE_MAX_RESPONSE_BYTES);
    if (get_result != ESP_OK)
    {
        music_http_result_free(&result);
        return get_result;
    }
    cJSON *root = cJSON_Parse(result.body);
    music_http_result_free(&result);
    if (root == NULL)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *list = data == NULL ? NULL :
                        cJSON_GetObjectItemCaseSensitive(data, "cdlist");
    if (!cJSON_IsArray(list))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, list)
    {
        if (home->my_collected_count >= MUSIC_SERVICE_MAX_PLAYLISTS)
        {
            break;
        }
        music_playlist_t *slot =
            &home->my_collected[home->my_collected_count];
        memset(slot, 0, sizeof(*slot));
        slot->ref.kind = (uint8_t)MUSIC_PLAYLIST_KIND_MY_COLLECTED;
        slot->ref.id = music_json_u64(item, "dissid");
        if (slot->ref.id == 0U)
        {
            slot->ref.id = music_json_u64(item, "tid");
        }
        slot->ref.tid = music_json_u32(item, "tid");
        music_json_string(item, "dissname", slot->title,
                          sizeof(slot->title));
        if (slot->title[0] == '\0')
        {
            music_json_string(item, "title", slot->title,
                              sizeof(slot->title));
        }
        music_json_string(item, "logo", slot->cover_url,
                          sizeof(slot->cover_url));
        if (slot->cover_url[0] == '\0')
        {
            music_json_string(item, "imgurl", slot->cover_url,
                              sizeof(slot->cover_url));
        }
        slot->track_count = music_json_u32(item, "songnum");
        if (slot->track_count == 0U)
        {
            slot->track_count = music_json_u32(item, "song_cnt");
        }
        if (slot->ref.id == 0U || slot->title[0] == '\0')
        {
            continue;
        }
        ++home->my_collected_count;
    }
    cJSON_Delete(root);
    home->collected_ok = true;
    return ESP_OK;
}

esp_err_t music_net_fetch_home(music_home_snapshot_t **snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    music_home_snapshot_t *home = music_snapshot_alloc(sizeof(*home), NULL);
    if (home == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    home->generation = music_service_next_generation();
    const esp_err_t recommended_result = _home_recommended(home);
    home->recommended_ok = recommended_result == ESP_OK;
    const esp_err_t personal_result = _home_personal(home);
    home->personal_ok = personal_result == ESP_OK;
    const esp_err_t collected_result = _home_collected(home);
    home->collected_ok = collected_result == ESP_OK;
    if (personal_result == MUSIC_ERR_HTTP_AUTH)
    {
        music_snapshot_release(home);
        return personal_result;
    }
    if (!home->recommended_ok || !home->personal_ok)
    {
        home->login_required = true;
    }
    if (!home->recommended_ok && !home->personal_ok)
    {
        music_snapshot_release(home);
        return recommended_result != ESP_OK ? recommended_result :
               personal_result;
    }
    *snapshot = home;
    return ESP_OK;
}

esp_err_t music_net_fetch_profile(char *account_id, size_t account_id_size,
                                  char *name, size_t name_size,
                                  char *avatar, size_t avatar_size)
{
    if (account_id == NULL || account_id_size == 0U || name == NULL ||
            name_size == 0U || avatar == NULL || avatar_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    account_id[0] = '\0';
    name[0] = '\0';
    avatar[0] = '\0';
    char uin[sizeof(s_music.credential.musicid)] = {0};
    char musickey[sizeof(s_music.credential.musickey)] = {0};
    music_login_type_t login_type = MUSIC_LOGIN_TYPE_WECHAT;
    bool credential_valid = false;
    if (music_service_lock() == ESP_OK)
    {
        credential_valid = s_music.credential_valid;
        login_type = s_music.login_type;
        (void)snprintf(uin, sizeof(uin), "%s", s_music.credential.musicid);
        (void)snprintf(musickey, sizeof(musickey), "%s",
                       s_music.credential.musickey);
        music_service_unlock();
    }
    if (!credential_valid || uin[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }
    (void)snprintf(account_id, account_id_size, "%s", uin);

    cJSON *param = cJSON_CreateObject();
    if (param == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    cJSON *vec_uin = cJSON_AddArrayToObject(param, "vec_uin");
    cJSON *uin_item = vec_uin == NULL ? NULL : cJSON_CreateString(uin);
    if (vec_uin == NULL || uin_item == NULL)
    {
        cJSON_Delete(param);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(vec_uin, uin_item);
    cJSON_AddNumberToObject(param, "need_profile", 1);
    cJSON *root = NULL;
    cJSON *data = music_api_post_with_credentials(uin, musickey,
        "userInfo.BaseUserInfoServer",
        "get_user_baseinfo_v2", param, &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return music_api_last_error();
    }

    const bool found = music_profile_parse(data, uin,
                                           login_type == MUSIC_LOGIN_TYPE_QQ,
                                           name, name_size, avatar,
                                           avatar_size);
    cJSON_Delete(root);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void _parse_song(const cJSON *item, music_song_t *song)
{
    memset(song, 0, sizeof(*song));
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(item, "file");
    const cJSON *album = cJSON_GetObjectItemCaseSensitive(item, "album");
    song->songid = music_json_u64(item, "songid");
    if (song->songid == 0U)
    {
        song->songid = music_json_u64(item, "id");
    }
    music_json_string(item, "songmid", song->songmid, sizeof(song->songmid));
    if (song->songmid[0] == '\0')
    {
        music_json_string(item, "mid", song->songmid, sizeof(song->songmid));
    }
    music_json_string(item, "strMediaMid", song->media_mid,
                      sizeof(song->media_mid));
    if (song->media_mid[0] == '\0')
    {
        music_json_string(item, "media_mid", song->media_mid,
                          sizeof(song->media_mid));
    }
    if (song->media_mid[0] == '\0')
    {
        music_json_string(file, "media_mid", song->media_mid,
                          sizeof(song->media_mid));
    }
    music_json_string(item, "songname", song->title, sizeof(song->title));
    if (song->title[0] == '\0')
    {
        music_json_string(item, "name", song->title, sizeof(song->title));
    }
    music_json_string(item, "albumname", song->album, sizeof(song->album));
    if (song->album[0] == '\0')
    {
        music_json_string(album, "name", song->album, sizeof(song->album));
    }
    const cJSON *singer = cJSON_GetObjectItemCaseSensitive(item, "singer");
    if (cJSON_IsArray(singer))
    {
        const cJSON *artist = cJSON_GetArrayItem(singer, 0);
        music_json_string(artist, "name", song->artist, sizeof(song->artist));
    }
    song->duration_ms = music_json_u32(item, "interval") * 1000U;
    song->size_128 = music_json_u32(item, "size128");
    if (song->size_128 == 0U)
    {
        song->size_128 = music_json_u32(file, "size_128mp3");
    }
    song->size_320 = music_json_u32(item, "size320");
    if (song->size_320 == 0U)
    {
        song->size_320 = music_json_u32(file, "size_320mp3");
    }
    song->size_flac = music_json_u32(item, "sizeflac");
    if (song->size_flac == 0U)
    {
        song->size_flac = music_json_u32(file, "size_flac");
    }
    char album_mid[64] = {0};
    if (cJSON_IsObject(album))
    {
        music_json_string(album, "mid", album_mid, sizeof(album_mid));
    }
    if (album_mid[0] == '\0')
    {
        music_json_string(item, "albummid", album_mid, sizeof(album_mid));
    }
    if (album_mid[0] != '\0')
    {
        (void)snprintf(song->cover_url, sizeof(song->cover_url),
                       "https://y.gtimg.cn/music/photo_new/"
                       "T002R300x300M000%s.jpg", album_mid);
    }
    const cJSON *pay = cJSON_GetObjectItemCaseSensitive(item, "pay");
    if (cJSON_IsObject(pay))
    {
        if (music_json_bool(pay, "payplay") || music_json_bool(pay, "pay_play"))
        {
            song->pay_flags |= 1U;
        }
        if (music_json_bool(pay, "paydownload") || music_json_bool(pay, "pay_down"))
        {
            song->pay_flags |= 2U;
        }
    }
}

static uint64_t _playlist_disstid(const music_playlist_ref_t *ref)
{
    /* The liked-songs bucket is addressed by dirId 201; its tid is not a
     * playlist disstid. Sending the bucket tid makes CgiGetDiss return a
     * different or inaccessible list even though the metadata came from the
     * account catalog. */
    if (ref->kind == MUSIC_PLAYLIST_KIND_MY_FAVORITE ||
            ref->dirid == 201U)
    {
        return 0U;
    }
    return ref->id;
}

static esp_err_t _music_net_fetch_playlist_once(
    const music_playlist_ref_t *ref,
    uint16_t offset, uint16_t count,
    music_playlist_snapshot_t **snapshot)
{
    if (ref == NULL || snapshot == NULL || count == 0U ||
            count > MUSIC_SERVICE_MAX_SONGS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    cJSON *param = cJSON_CreateObject();
    if (param == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(param, "disstid", (double)_playlist_disstid(ref));
    cJSON_AddNumberToObject(param, "dirid", (double)ref->dirid);
    cJSON_AddBoolToObject(param, "tag", true);
    cJSON_AddNumberToObject(param, "song_begin", offset);
    cJSON_AddNumberToObject(param, "song_num", count);
    cJSON_AddBoolToObject(param, "userinfo", true);
    cJSON_AddBoolToObject(param, "orderlist", true);
    cJSON_AddBoolToObject(param, "onlysonglist", false);
    cJSON *root = NULL;
    cJSON *data = music_api_post("music.srfDissInfo.DissInfo", "CgiGetDiss",
                                 param, &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return music_api_last_error();
    }
    music_playlist_snapshot_t *playlist = music_snapshot_alloc(
            offsetof(music_playlist_snapshot_t, songs) +
            (size_t)count * sizeof(music_song_t), NULL);
    if (playlist == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    playlist->generation = music_service_next_generation();
    playlist->playlist.ref = *ref;
    const cJSON *dirinfo = cJSON_GetObjectItemCaseSensitive(data, "dirinfo");
    music_json_string(data, "dissname", playlist->playlist.title,
                      sizeof(playlist->playlist.title));
    if (playlist->playlist.title[0] == '\0')
    {
        music_json_string(dirinfo, "title", playlist->playlist.title,
                          sizeof(playlist->playlist.title));
    }
    music_json_string(data, "diss_cover", playlist->playlist.cover_url,
                      sizeof(playlist->playlist.cover_url));
    if (playlist->playlist.cover_url[0] == '\0')
    {
        music_json_string(dirinfo, "picurl", playlist->playlist.cover_url,
                          sizeof(playlist->playlist.cover_url));
    }
    if (playlist->playlist.cover_url[0] == '\0')
    {
        music_json_string(dirinfo, "bigpicUrl", playlist->playlist.cover_url,
                          sizeof(playlist->playlist.cover_url));
    }
    const cJSON *creator = cJSON_GetObjectItemCaseSensitive(dirinfo,
        "creator");
    music_json_string(creator, "nick", playlist->playlist.creator,
                      sizeof(playlist->playlist.creator));
    playlist->playlist.track_count = music_json_u32(data, "songnum");
    if (playlist->playlist.track_count == 0U)
    {
        playlist->playlist.track_count = music_json_u32(dirinfo, "songnum");
    }
    if (playlist->playlist.track_count == 0U)
    {
        playlist->playlist.track_count = music_json_u32(data, "songlist_size");
    }
    playlist->offset = offset;
    playlist->total = music_json_u32(data, "total_song_num");
    if (playlist->total == 0U)
    {
        playlist->total = music_json_u32(dirinfo, "songnum");
    }
    const cJSON *songs = cJSON_GetObjectItemCaseSensitive(data, "songlist");
    if (cJSON_IsArray(songs))
    {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, songs)
        {
            if (playlist->count >= count ||
                    playlist->count >= MUSIC_SERVICE_MAX_SONGS)
            {
                break;
            }
            _parse_song(item, &playlist->songs[playlist->count]);
            ++playlist->count;
        }
    }
    if (playlist->total == 0U)
    {
        playlist->total = (uint32_t)offset + playlist->count;
    }
    cJSON_Delete(root);
    *snapshot = playlist;
    return ESP_OK;
}

esp_err_t music_net_fetch_playlist(const music_playlist_ref_t *ref,
                                   uint16_t offset, uint16_t count,
                                   music_playlist_snapshot_t **snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt)
    {
        music_playlist_snapshot_t *page = NULL;
        result = _music_net_fetch_playlist_once(ref, offset, count, &page);
        if (result == ESP_OK && page != NULL &&
                (page->count > 0U || page->total <= offset))
        {
            *snapshot = page;
            return ESP_OK;
        }
        if (result == ESP_OK)
        {
            /* A missing song list for a non-empty playlist is a transient
             * provider response, not an empty playlist. */
            music_snapshot_release(page);
            result = ESP_ERR_INVALID_RESPONSE;
        }
        if (attempt == 0U &&
                (result == ESP_ERR_TIMEOUT ||
                 result == ESP_ERR_INVALID_RESPONSE))
        {
            LOG_W("playlist fetch retry after %s", esp_err_to_name(result));
            continue;
        }
        return result;
    }
    return result;
}

static void _song_filename(const music_song_t *song, music_quality_t quality,
                           char *filename, size_t filename_size,
                           const char **extension)
{
    const char *prefix = "M500";
    const char *suffix = ".mp3";
    if (quality == MUSIC_QUALITY_LOSSLESS && song->size_flac > 0U)
    {
        prefix = "F000";
        suffix = ".flac";
    }
    else if (quality >= MUSIC_QUALITY_HIGH && song->size_320 > 0U)
    {
        prefix = "M800";
    }
    const char *media = song->media_mid[0] != '\0' ? song->media_mid :
                        song->songmid;
    (void)snprintf(filename, filename_size, "%s%s%s", prefix, media, suffix);
    *extension = suffix;
}

static esp_err_t _music_net_fetch_song_url_one(const music_song_t *song,
        music_quality_t quality,
        char *url, size_t url_size)
{
    if (song == NULL || url == NULL || url_size == 0U ||
            song->songmid[0] == '\0' || !s_music.credential_valid)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char filename[96];
    const char *extension = NULL;
    _song_filename(song, quality, filename, sizeof(filename), &extension);
    (void)extension;
    cJSON *param = cJSON_CreateObject();
    if (param == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    char guid[24];
    (void)snprintf(guid, sizeof(guid), "%u", (unsigned)(esp_random() % 1000000000U));
    cJSON_AddStringToObject(param, "guid", guid);
    cJSON *songmids = cJSON_AddArrayToObject(param, "songmid");
    if (songmids != NULL)
    {
        cJSON_AddItemToArray(songmids,
                             cJSON_CreateString(song->songmid));
    }
    cJSON *songtypes = cJSON_AddArrayToObject(param, "songtype");
    if (songtypes != NULL)
    {
        cJSON_AddItemToArray(songtypes, cJSON_CreateNumber(0));
    }
    cJSON_AddStringToObject(param, "uin", s_music.credential.musicid);
    cJSON_AddNumberToObject(param, "loginflag", 1);
    cJSON_AddStringToObject(param, "platform", "20");
    cJSON *filenames = cJSON_AddArrayToObject(param, "filename");
    if (filenames != NULL)
    {
        cJSON_AddItemToArray(filenames, cJSON_CreateString(filename));
    }
    cJSON *root = NULL;
    cJSON *data = music_api_post("vkey.GetVkeyServer", "CgiGetVkey", param,
                                 &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return music_api_last_error();
    }
    const cJSON *midurlinfo = cJSON_GetObjectItemCaseSensitive(data,
        "midurlinfo");
    const cJSON *first = cJSON_IsArray(midurlinfo) ?
                         cJSON_GetArrayItem(midurlinfo, 0) : NULL;
    char purl[512];
    music_json_string(first, "purl", purl, sizeof(purl));
    if (purl[0] == '\0')
    {
        /* The newer vkey response exposes a relative wifiurl instead of
         * purl. Keep purl as the primary field and use wifiurl as fallback. */
        music_json_string(first, "wifiurl", purl, sizeof(purl));
        if (purl[0] != '\0')
        {
            if (strncmp(purl, "https://", 8) == 0)
            {
                (void)snprintf(url, url_size, "%s", purl);
                cJSON_Delete(root);
                return ESP_OK;
            }
            if (strncmp(purl, "http://", 7) == 0)
            {
                (void)snprintf(url, url_size, "https://%s", purl + 7);
                cJSON_Delete(root);
                return ESP_OK;
            }
            const char *path = purl[0] == '/' ? purl + 1 : purl;
            (void)snprintf(url, url_size,
                           "https://isure.stream.qqmusic.qq.com/%s", path);
            cJSON_Delete(root);
            return ESP_OK;
        }
        cJSON_Delete(root);
        LOG_W("no play url for %s", song->songmid);
        return ESP_ERR_NOT_FOUND;
    }
    if (strncmp(purl, "https://", 8) == 0)
    {
        (void)snprintf(url, url_size, "%s", purl);
        cJSON_Delete(root);
        return ESP_OK;
    }
    if (strncmp(purl, "http://", 7) == 0)
    {
        (void)snprintf(url, url_size, "https://%s", purl + 7);
        cJSON_Delete(root);
        return ESP_OK;
    }
    const cJSON *sip = cJSON_GetObjectItemCaseSensitive(data, "sip");
    const char *domain = NULL;
    if (cJSON_IsArray(sip))
    {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, sip)
        {
            if (cJSON_IsString(item) && item->valuestring != NULL &&
                    strncmp(item->valuestring, "http://ws", 9) != 0)
            {
                domain = item->valuestring;
                break;
            }
        }
        if (domain == NULL)
        {
            const cJSON *fallback = cJSON_GetArrayItem(sip, 0);
            if (cJSON_IsString(fallback))
            {
                domain = fallback->valuestring;
            }
        }
    }
    if (domain == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    (void)snprintf(url, url_size, "%s%s", domain, purl);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t music_net_fetch_song_url(const music_song_t *song,
                                   music_quality_t quality,
                                   char *url, size_t url_size,
                                   music_quality_t *actual_quality)
{
    if (song == NULL || url == NULL || url_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    music_quality_t attempts[3];
    size_t attempt_count = 0U;
    if (quality == MUSIC_QUALITY_LOSSLESS && song->size_flac > 0U)
    {
        attempts[attempt_count++] = MUSIC_QUALITY_LOSSLESS;
    }
    if (quality >= MUSIC_QUALITY_HIGH && song->size_320 > 0U)
    {
        attempts[attempt_count++] = MUSIC_QUALITY_HIGH;
    }
    attempts[attempt_count++] = MUSIC_QUALITY_STANDARD;

    esp_err_t last_error = ESP_ERR_NOT_FOUND;
    for (size_t index = 0U; index < attempt_count; ++index)
    {
        url[0] = '\0';
        last_error = _music_net_fetch_song_url_one(song, attempts[index],
            url, url_size);
        if (last_error == ESP_OK)
        {
            if (actual_quality != NULL)
            {
                *actual_quality = attempts[index];
            }
            return ESP_OK;
        }
        if (last_error != ESP_ERR_NOT_FOUND)
        {
            return last_error;
        }
    }
    return last_error;
}
