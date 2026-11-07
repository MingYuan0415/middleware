#define DBG_TAG "music_auth"
#include "mt_log.h"

#include "music_service_internal.h"
#include "music_api.h"
#include "music_auth.h"
#include "music_http.h"
#include "music_media.h"
#include "music_login_protocol.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nv_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* QQ Music web login endpoints are unofficial and can change independently of
 * the public OpenAPI. Re-verify fields and status codes when behavior drifts. */
#define MUSIC_QQ_REFERER "https://xui.ptlogin2.qq.com/"
#define MUSIC_QQ_XLOGIN_URL \
    "https://xui.ptlogin2.qq.com/cgi-bin/xlogin" \
    "?appid=716027609&daid=383&style=33" \
    "&login_text=%E7%99%BB%E5%BD%95" \
    "&hide_title_bar=1&hide_border=1&target=self" \
    "&s_url=https%3A%2F%2Fgraph.qq.com%2Foauth2.0%2Flogin_jump" \
    "&pt_3rd_aid=100497308" \
    "&pt_feedback_link=https%3A%2F%2Fsupport.qq.com%2Fproducts%2F77942" \
    "%3FcustomInfo%3D.appid100497308&theme=2&verify_theme="
#define MUSIC_QQ_AUTHORIZE_URL "https://graph.qq.com/oauth2.0/authorize"
#define MUSIC_QQ_AUTHORIZE_REFERER \
    "https://graph.qq.com/oauth2.0/show?which=Login&display=pc" \
    "&response_type=code&client_id=100497308" \
    "&redirect_uri=https://y.qq.com/portal/wx_redirect.html" \
    "?login_type=1&surl=https://y.qq.com/portal/profile.html" \
    "#stat=y_new.top.user_pic&stat=y_new.top.pop.logout" \
    "&use_customer_cb=0&state=state&display=pc"
#define MUSIC_QR_MAX_BYTES (256U * 1024U)
#define MUSIC_QR_POLL_MAX_BYTES (32U * 1024U)
#define MUSIC_SESSION_CHECK_MAX_BYTES (64U * 1024U)
#define MUSIC_SESSION_COOKIE_BYTES 1536U

static void *_psram_alloc(size_t size)
{
    return heap_caps_calloc(1U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void _psram_free(void *pointer)
{
    heap_caps_free(pointer);
}

static bool _credential_string_ok(const char *value, size_t capacity)
{
    return memchr(value, '\0', capacity) != NULL;
}

static bool _credential_token_ok(const char *value, size_t capacity,
                                 bool numeric_only)
{
    if (!_credential_string_ok(value, capacity) || value[0] == '\0')
    {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
            *cursor != '\0'; ++cursor)
    {
        if (*cursor <= 0x20U || *cursor >= 0x7FU ||
                *cursor == ';' || *cursor == '&' || *cursor == '=' ||
                *cursor == '\'' || *cursor == '"')
        {
            return false;
        }
        if (numeric_only && (*cursor < '0' || *cursor > '9'))
        {
            return false;
        }
    }
    return true;
}

esp_err_t music_net_load_credential(music_credential_blob_t *credential)
{
    if (credential == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    size_t size = sizeof(*credential);
    const esp_err_t result = nv_storage_get_blob(MUSIC_CREDENTIAL_KEY,
        credential, &size);
    if (result != ESP_OK)
    {
        return result;
    }
    if (size != sizeof(*credential) ||
            credential->magic != MUSIC_CREDENTIAL_MAGIC ||
            credential->version != MUSIC_CREDENTIAL_VERSION ||
            !_credential_token_ok(credential->musicid,
                                  sizeof(credential->musicid), true) ||
            !_credential_token_ok(credential->musickey,
                                  sizeof(credential->musickey), false) ||
            !_credential_string_ok(credential->refresh_key,
                                   sizeof(credential->refresh_key)) ||
            !_credential_string_ok(credential->refresh_token,
                                   sizeof(credential->refresh_token)))
    {
        memset(credential, 0, sizeof(*credential));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t music_net_save_credential(const music_credential_blob_t *credential)
{
    if (credential == NULL ||
            !_credential_token_ok(credential->musicid,
                                  sizeof(credential->musicid), true) ||
            !_credential_token_ok(credential->musickey,
                                  sizeof(credential->musickey), false))
    {
        return ESP_ERR_INVALID_ARG;
    }
    return nv_storage_set_blob(MUSIC_CREDENTIAL_KEY, credential,
                               sizeof(*credential));
}

esp_err_t music_net_save_credential_if_current(
    const music_credential_blob_t *credential, uint32_t login_session)
{
    esp_err_t result = music_service_credential_lock();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!music_service_login_session_current(login_session))
    {
        music_service_credential_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (music_service_lock() != ESP_OK)
    {
        music_service_credential_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const bool current = s_music.login_active &&
                         s_music.login_session == login_session &&
                         music_service_login_session_current(login_session);
    music_service_unlock();
    if (!current)
    {
        music_service_credential_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    result = music_net_save_credential(credential);
    music_service_credential_unlock();
    if (result == ESP_OK)
    {
        if (music_service_lock() == ESP_OK)
        {
            const bool still_current = s_music.login_active &&
                                       s_music.login_session == login_session;
            music_service_unlock();
            if (!still_current)
            {
                /* Logout won the race after NVS write; remove the orphan. */
                esp_err_t erase_result = music_service_credential_lock();
                if (erase_result == ESP_OK)
                {
                    erase_result = nv_storage_erase_key(MUSIC_CREDENTIAL_KEY);
                    music_service_credential_unlock();
                }
                if (erase_result == ESP_OK ||
                        erase_result == ESP_ERR_NVS_NOT_FOUND)
                {
                    return ESP_ERR_INVALID_STATE;
                }
                return erase_result;
            }
        }
        else
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return result;
}

esp_err_t music_net_erase_credential(void)
{
    esp_err_t result = music_service_credential_lock();
    if (result != ESP_OK)
    {
        return result;
    }
    result = nv_storage_erase_key(MUSIC_CREDENTIAL_KEY);
    music_service_credential_unlock();
    if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND)
    {
        music_net_clear_session();
        return ESP_OK;
    }
    return result;
}

esp_err_t music_net_check_session(const char *musicid, const char *musickey)
{
    if (musicid == NULL || musicid[0] == '\0' ||
            musickey == NULL || musickey[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = ESP_OK;
    char *url = _psram_alloc(MUSIC_SERVICE_URL_BYTES);
    char *cookie = _psram_alloc(MUSIC_SESSION_COOKIE_BYTES);
    if (url == NULL || cookie == NULL)
    {
        error = ESP_ERR_NO_MEM;
        goto done;
    }
    if (!music_login_build_session_check_url(url, MUSIC_SERVICE_URL_BYTES,
            musicid, musickey) ||
            !music_login_build_credential_cookie(cookie,
                    MUSIC_SESSION_COOKIE_BYTES, musicid, musickey))
    {
        error = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    music_http_result_t result;
    error = music_http_get_with_cookie_header(url, cookie, &result,
        MUSIC_SESSION_CHECK_MAX_BYTES);
    if (error != ESP_OK)
    {
        LOG_W("session check request failed: %s", esp_err_to_name(error));
        goto done;
    }
    if (result.status == 401)
    {
        music_http_result_free(&result);
        error = MUSIC_ERR_HTTP_AUTH;
        goto done;
    }
    if (result.status == 403)
    {
        cJSON *root = cJSON_Parse(result.body);
        const cJSON *code = root == NULL ? NULL :
                            cJSON_GetObjectItemCaseSensitive(root, "code");
        const bool explicit_auth =
            cJSON_IsNumber(code) &&
            music_login_code_is_auth_expired(code->valueint);
        cJSON_Delete(root);
        music_http_result_free(&result);
        error = explicit_auth ? MUSIC_ERR_HTTP_AUTH :
                ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    if (result.status == 429)
    {
        music_http_result_free(&result);
        error = MUSIC_ERR_HTTP_RATE_LIMIT;
        goto done;
    }
    if (result.status < 200 || result.status >= 300)
    {
        LOG_W("session check HTTP status=%d", result.status);
        music_http_result_free(&result);
        error = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    cJSON *root = cJSON_Parse(result.body);
    music_http_result_free(&result);
    if (root == NULL)
    {
        error = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    bool has_code = false;
    int code_value = 0;
    if (cJSON_IsNumber(code))
    {
        code_value = code->valueint;
        has_code = true;
    }
    else if (cJSON_IsString(code) && code->valuestring != NULL)
    {
        char *end = NULL;
        const long parsed = strtol(code->valuestring, &end, 10);
        if (end != NULL && end != code->valuestring && *end == '\0')
        {
            code_value = (int)parsed;
            has_code = true;
        }
    }
    cJSON_Delete(root);
    if (!has_code)
    {
        error = ESP_ERR_INVALID_RESPONSE;
    }
    else if (code_value == 0)
    {
        error = ESP_OK;
    }
    else if (music_login_code_is_auth_expired(code_value))
    {
        error = MUSIC_ERR_HTTP_AUTH;
    }
    else
    {
        LOG_W("session check returned code=%d", code_value);
        error = ESP_ERR_INVALID_RESPONSE;
    }

done:
    _psram_free(url);
    _psram_free(cookie);
    return error;
}

void music_net_cancel_login(void)
{
    s_music.qr_uuid[0] = '\0';
    s_music.qr_qrsig[0] = '\0';
    s_music.qq_login_sig[0] = '\0';
}

void music_net_clear_session(void)
{
    music_http_cookie_clear();
    music_net_cancel_login();
}

static void _random_uuid(char output[37])
{
    const uint32_t first = esp_random();
    const uint32_t second = esp_random();
    const uint32_t third = esp_random();
    const uint32_t fourth = esp_random();
    (void)snprintf(output, 37U,
                   "%08x-%04x-4%03x-%04x-%08x%04x",
                   (unsigned)first,
                   (unsigned)(second >> 16),
                   (unsigned)(second & 0x0FFFU),
                   (unsigned)(((third >> 16) & 0x3FFFU) | 0x8000U),
                   (unsigned)fourth,
                   (unsigned)(third & 0xFFFFU));
}

static esp_err_t _credential_from_data(const cJSON *data, uint16_t login_type,
                                       music_credential_blob_t *credential)
{
    if (data == NULL || credential == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(credential, 0, sizeof(*credential));
    credential->magic = MUSIC_CREDENTIAL_MAGIC;
    credential->version = MUSIC_CREDENTIAL_VERSION;
    credential->login_type = login_type;
    music_json_string_or_number(data, "musicid", credential->musicid,
                                sizeof(credential->musicid));
    if (credential->musicid[0] == '\0')
    {
        music_json_string_or_number(data, "str_musicid", credential->musicid,
                                    sizeof(credential->musicid));
    }
    music_json_string(data, "musickey", credential->musickey,
                      sizeof(credential->musickey));
    music_json_string(data, "refresh_key", credential->refresh_key,
                      sizeof(credential->refresh_key));
    music_json_string(data, "refresh_token", credential->refresh_token,
                      sizeof(credential->refresh_token));
    credential->expires_at = music_json_u32(data, "expired_at");
    if (credential->expires_at == 0U)
    {
        credential->expires_at = music_json_u32(data, "expiredAt");
    }
    return credential->musicid[0] != '\0' &&
           credential->musickey[0] != '\0' ? ESP_OK :
           ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t _exchange_qq_code(const char *code,
                                   music_credential_blob_t *credential)
{
    if (code == NULL || code[0] == '\0' || credential == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *param = cJSON_CreateObject();
    if (param == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(param, "code", code);
    music_api_result_t result;
    const esp_err_t request_result = music_api_post(
                                         MUSIC_API_SCOPE_PUBLIC, 0U,
                                         "QQConnectLogin.LoginServer",
                                         "QQLogin", param, &result);
    if (request_result != ESP_OK)
    {
        return request_result;
    }
    const esp_err_t parse_result = _credential_from_data(
                                       result.data, 2U, credential);
    music_api_result_release(&result);
    return parse_result;
}

static esp_err_t _qq_authorize(const char *auth_url,
                               music_credential_blob_t *credential)
{
    if (auth_url == NULL || auth_url[0] == '\0' || credential == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_OK;
    char *p_skey = _psram_alloc(512U);
    char *body = _psram_alloc(2048U);
    if (p_skey == NULL || body == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto done;
    }

    music_http_result_t auth_result;
    result = music_http_get_headers(auth_url, true, MUSIC_QQ_REFERER,
                                    &auth_result);
    if (result != ESP_OK)
    {
        goto done;
    }
    const int status = auth_result.status;
    music_http_result_free(&auth_result);
    if (status < 200 || status >= 300)
    {
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    if (!music_http_cookie_get("p_skey", p_skey, 512U))
    {
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    size_t used = 0U;
    body[0] = '\0';
    char number[32];
    (void)snprintf(number, sizeof(number), "%u",
                   (unsigned)music_login_hash33(p_skey, 5381U));
    char auth_time[32];
    (void)snprintf(auth_time, sizeof(auth_time), "%lld",
                   (long long)time(NULL));
    char uuid[37];
    _random_uuid(uuid);
    const bool fields_ok =
        music_login_form_add(body, 2048U, &used,
                             "response_type", "code") &&
        music_login_form_add(body, 2048U, &used,
                             "client_id", "100497308") &&
        music_login_form_add(body, 2048U, &used, "redirect_uri",
                             "https://y.qq.com/portal/wx_redirect.html"
                             "?login_type=1&surl=https://y.qq.com/"
                             "#&use_customer_cb=0") &&
        music_login_form_add(body, 2048U, &used, "scope", "") &&
        music_login_form_add(body, 2048U, &used, "state", "state") &&
        music_login_form_add(body, 2048U, &used, "switch", "") &&
        music_login_form_add(body, 2048U, &used,
                             "from_ptlogin", "1") &&
        music_login_form_add(body, 2048U, &used, "src", "1") &&
        music_login_form_add(body, 2048U, &used,
                             "update_auth", "1") &&
        music_login_form_add(body, 2048U, &used,
                             "openapi", "80901010") &&
        music_login_form_add(body, 2048U, &used, "g_tk", number) &&
        music_login_form_add(body, 2048U, &used,
                             "auth_time", auth_time) &&
        music_login_form_add(body, 2048U, &used, "ui", uuid);
    if (!fields_ok)
    {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    music_http_result_t code_result;
    result = music_http_post_form_no_redirect(MUSIC_QQ_AUTHORIZE_URL, body,
        MUSIC_QQ_AUTHORIZE_REFERER,
        &code_result,
        MUSIC_QR_POLL_MAX_BYTES);
    if (result != ESP_OK)
    {
        goto done;
    }
    char code[128];
    bool code_ok = music_login_query_value(code_result.location, "code",
                                           code, sizeof(code));
    if (!code_ok && code_result.body != NULL)
    {
        code_ok = music_login_query_value(code_result.body, "code",
                                          code, sizeof(code));
    }
    music_http_result_free(&code_result);
    if (!code_ok)
    {
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    result = _exchange_qq_code(code, credential);

done:
    _psram_free(p_skey);
    _psram_free(body);
    return result;
}

static esp_err_t _qq_login_begin(uint32_t session)
{
    music_http_result_t result;
    esp_err_t error = music_http_get_headers(MUSIC_QQ_XLOGIN_URL, true,
        MUSIC_QQ_REFERER, &result);
    if (error != ESP_OK)
    {
        LOG_W("qq xlogin failed: %s", esp_err_to_name(error));
        return error;
    }
    const int status = result.status;
    music_http_result_free(&result);
    if (status < 200 || status >= 300)
    {
        LOG_W("qq xlogin status=%d", status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!music_http_cookie_get("pt_login_sig", s_music.qq_login_sig,
                               sizeof(s_music.qq_login_sig)))
    {
        LOG_W("qq xlogin missing pt_login_sig");
        return ESP_ERR_INVALID_RESPONSE;
    }

    char qr_url[512];
    (void)snprintf(qr_url, sizeof(qr_url),
                   "https://ssl.ptlogin2.qq.com/ptqrshow"
                   "?appid=716027609&e=2&l=M&s=3&d=72&v=4&t=0.%06u"
                   "&daid=383&pt_3rd_aid=100497308",
                   (unsigned)(esp_random() % 1000000U));
    music_image_t *qr = NULL;
    error = music_media_fetch_image(qr_url, 240U, 240U, 1U, true,
                                    MUSIC_QQ_REFERER, &qr);
    if (error != ESP_OK)
    {
        LOG_W("qq qr image failed: %s", esp_err_to_name(error));
        return error;
    }
    if (!music_http_cookie_get("qrsig", s_music.qr_qrsig,
                               sizeof(s_music.qr_qrsig)))
    {
        LOG_W("qq qr response missing qrsig");
        music_service_login_qr_release(qr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    music_service_set_login_qr(qr, session);
    return ESP_OK;
}

static esp_err_t _qq_login_poll(music_credential_blob_t *credential,
                                music_login_poll_state_t *state)
{
    if (credential == NULL || state == NULL ||
            s_music.qr_qrsig[0] == '\0' ||
            s_music.qq_login_sig[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    char *query = _psram_alloc(1024U);
    char *url = _psram_alloc(1536U);
    char *auth_url = _psram_alloc(1024U);
    if (query == NULL || url == NULL || auth_url == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto done;
    }

    size_t used = 0U;
    query[0] = '\0';
    char number[32];
    (void)snprintf(number, sizeof(number), "%u",
                   (unsigned)music_login_hash33(s_music.qr_qrsig, 0U));
    char action[32];
    (void)snprintf(action, sizeof(action), "0-0-%lld",
                   (long long)time(NULL) * 1000LL);
    const bool fields_ok =
        music_login_form_add(query, 1024U, &used, "u1",
                             "https://graph.qq.com/oauth2.0/login_jump") &&
        music_login_form_add(query, 1024U, &used, "ptqrtoken", number) &&
        music_login_form_add(query, 1024U, &used, "ptredirect", "0") &&
        music_login_form_add(query, 1024U, &used, "h", "1") &&
        music_login_form_add(query, 1024U, &used, "t", "1") &&
        music_login_form_add(query, 1024U, &used, "g", "1") &&
        music_login_form_add(query, 1024U, &used, "from_ui", "1") &&
        music_login_form_add(query, 1024U, &used, "ptlang", "2052") &&
        music_login_form_add(query, 1024U, &used, "action", action) &&
        music_login_form_add(query, 1024U, &used, "js_ver", "20102616") &&
        music_login_form_add(query, 1024U, &used, "js_type", "1") &&
        music_login_form_add(query, 1024U, &used, "login_sig",
                             s_music.qq_login_sig) &&
        music_login_form_add(query, 1024U, &used, "pt_uistyle", "40") &&
        music_login_form_add(query, 1024U, &used, "aid", "716027609") &&
        music_login_form_add(query, 1024U, &used, "daid", "383") &&
        music_login_form_add(query, 1024U, &used, "pt_3rd_aid",
                             "100497308") &&
        music_login_form_add(query, 1024U, &used, "has_onekey", "1");
    if (!fields_ok)
    {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    (void)snprintf(url, 1536U, "https://ssl.ptlogin2.qq.com/ptqrlogin?%s",
                   query);
    music_http_result_t http_result;
    result = music_http_get_referer(url, true, MUSIC_QQ_REFERER, &http_result,
                                    MUSIC_QR_POLL_MAX_BYTES);
    if (result != ESP_OK)
    {
        goto done;
    }
    char musicid[32];
    const music_login_qq_state_t qq_state = music_login_parse_ptui_cb(
            http_result.body, musicid,
            sizeof(musicid), auth_url,
            1024U);
    music_http_result_free(&http_result);
    if (qq_state == MUSIC_LOGIN_QQ_WAITING)
    {
        *state = MUSIC_LOGIN_POLL_WAITING;
        goto done;
    }
    if (qq_state == MUSIC_LOGIN_QQ_SCANNED)
    {
        *state = MUSIC_LOGIN_POLL_SCANNED;
        goto done;
    }
    if (qq_state != MUSIC_LOGIN_QQ_SUCCESS)
    {
        *state = MUSIC_LOGIN_POLL_FAILED;
        goto done;
    }
    result = _qq_authorize(auth_url, credential);
    if (result != ESP_OK)
    {
        goto done;
    }
    music_http_cookie_clear();
    *state = MUSIC_LOGIN_POLL_SUCCESS;

done:
    _psram_free(query);
    _psram_free(url);
    _psram_free(auth_url);
    return result;
}

esp_err_t music_net_login_begin(music_login_type_t type, uint32_t session)
{
    music_http_cookie_clear();
    music_net_cancel_login();
    if (type == MUSIC_LOGIN_TYPE_QQ)
    {
        return _qq_login_begin(session);
    }
    if (type != MUSIC_LOGIN_TYPE_WECHAT)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    music_http_result_t result;
    const esp_err_t error = music_http_get(
                                "https://open.weixin.qq.com/connect/qrconnect"
                                "?appid=wx48db31d50e334801"
                                "&redirect_uri=https%3A%2F%2Fy.qq.com%2Fportal"
                                "%2Fwx_redirect.html%3Flogin_type%3D2%26surl"
                                "%3Dhttps%3A%2F%2Fy.qq.com%2F"
                                "&response_type=code&scope=snsapi_login"
                                "&state=STATE&href=https%3A%2F%2Fy.qq.com"
                                "%2Fmediastyle%2Fmusic_v17%2Fsrc%2Fcss"
                                "%2Fpopup_wechat.css%23wechat_redirect",
                                true, &result, MUSIC_QR_MAX_BYTES);
    if (error != ESP_OK)
    {
        return error;
    }
    const char *uuid_key = strstr(result.body, "uuid=");
    if (uuid_key == NULL)
    {
        music_http_result_free(&result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uuid_key += strlen("uuid=");
    const char *uuid_end = uuid_key;
    while (*uuid_end != '\0' && *uuid_end != '"' && *uuid_end != '&' &&
            *uuid_end != '\'' && *uuid_end != '\n' && *uuid_end != '\r')
    {
        ++uuid_end;
    }
    const size_t uuid_length = (size_t)(uuid_end - uuid_key);
    if (uuid_length == 0U || uuid_length >= sizeof(s_music.qr_uuid))
    {
        music_http_result_free(&result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(s_music.qr_uuid, uuid_key, uuid_length);
    s_music.qr_uuid[uuid_length] = '\0';
    music_http_result_free(&result);

    char qr_url[256];
    (void)snprintf(qr_url, sizeof(qr_url),
                   "https://open.weixin.qq.com/connect/qrcode/%s",
                   s_music.qr_uuid);
    music_image_t *qr = NULL;
    const esp_err_t qr_result = music_net_fetch_image(qr_url, 240U, 240U,
        1U, &qr);
    if (qr_result != ESP_OK)
    {
        return qr_result;
    }
    music_service_set_login_qr(qr, session);
    return ESP_OK;
}

static esp_err_t _exchange_wechat_code(const char *code,
                                       music_credential_blob_t *credential)
{
    if (code == NULL || code[0] == '\0' || credential == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *param = cJSON_CreateObject();
    if (param == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(param, "code", code);
    cJSON_AddStringToObject(param, "strAppid", "wx48db31d50e334801");
    music_api_result_t result;
    const esp_err_t request_result = music_api_post(
                                         MUSIC_API_SCOPE_PUBLIC, 0U,
                                         "music.login.LoginServer", "Login",
                                         param, &result);
    if (request_result != ESP_OK)
    {
        return request_result;
    }
    const esp_err_t parse_result = _credential_from_data(
                                       result.data, 1U, credential);
    music_api_result_release(&result);
    return parse_result;
}

esp_err_t music_net_login_poll(music_login_type_t type,
                               music_credential_blob_t *credential,
                               music_login_poll_state_t *state)
{
    if (type == MUSIC_LOGIN_TYPE_QQ)
    {
        return _qq_login_poll(credential, state);
    }
    if (type != MUSIC_LOGIN_TYPE_WECHAT)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (credential == NULL || state == NULL || s_music.qr_uuid[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }
    *state = MUSIC_LOGIN_POLL_WAITING;
    char url[512];
    (void)snprintf(url, sizeof(url),
                   "https://lp.open.weixin.qq.com/connect/l/qrconnect"
                   "?uuid=%s&_=%lld", s_music.qr_uuid,
                   (long long)(esp_timer_get_time() / 1000LL));
    music_http_result_t result;
    const esp_err_t error = music_http_get(url, true, &result,
                                           MUSIC_QR_POLL_MAX_BYTES);
    if (error != ESP_OK)
    {
        return error;
    }
    const char *error_key = strstr(result.body, "wx_errcode=");
    if (error_key == NULL)
    {
        music_http_result_free(&result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int error_code = atoi(error_key + strlen("wx_errcode="));
    /* 408: waiting; 404: scanned; 405: confirmed and carries wx_code. */
    if (error_code == 408)
    {
        music_http_result_free(&result);
        return ESP_OK;
    }
    if (error_code == 404)
    {
        *state = MUSIC_LOGIN_POLL_SCANNED;
        music_http_result_free(&result);
        return ESP_OK;
    }
    if (error_code != 405)
    {
        *state = MUSIC_LOGIN_POLL_FAILED;
        music_http_result_free(&result);
        return ESP_OK;
    }
    const char *code_key = strstr(result.body, "wx_code='");
    if (code_key == NULL)
    {
        music_http_result_free(&result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    code_key += strlen("wx_code='");
    const char *code_end = strchr(code_key, '\'');
    if (code_end == NULL || code_end == code_key)
    {
        music_http_result_free(&result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    char code[256];
    const size_t code_length = (size_t)(code_end - code_key);
    if (code_length >= sizeof(code))
    {
        music_http_result_free(&result);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(code, code_key, code_length);
    code[code_length] = '\0';
    const esp_err_t exchange = _exchange_wechat_code(code, credential);
    music_http_result_free(&result);
    if (exchange != ESP_OK)
    {
        return exchange;
    }
    *state = MUSIC_LOGIN_POLL_SUCCESS;
    return ESP_OK;
}

