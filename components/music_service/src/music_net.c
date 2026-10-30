#define DBG_TAG "music_net"
#include "mt_log.h"

#include "music_service_internal.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_jpeg_common.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_jpeg_dec.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nv_storage.h"

#include <ctype.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

#define MUSIC_HTTP_TIMEOUT_MS 10000
#define MUSIC_HTTP_MAX_REDIRECTS 5
#define MUSIC_COOKIE_MAX 16
#define MUSIC_QR_MAX_BYTES (256U * 1024U)
#define MUSIC_QR_POLL_MAX_BYTES (32U * 1024U)
#define MUSIC_JSON_MAX_BYTES CONFIG_MUSIC_SERVICE_MAX_RESPONSE_BYTES

typedef struct music_cookie
{
    char name[32];
    char value[512];
} music_cookie_t;

typedef struct music_http_context
{
    music_http_result_t *result;
    size_t limit;
    bool overflow;
    bool capture_cookies;
} music_http_context_t;

static music_cookie_t s_cookies[MUSIC_COOKIE_MAX];
static size_t s_cookie_count;

static void *_psram_alloc(size_t size)
{
    return heap_caps_calloc(1U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void _psram_free(void *pointer)
{
    heap_caps_free(pointer);
}

static void _cookie_clear(void)
{
    memset(s_cookies, 0, sizeof(s_cookies));
    s_cookie_count = 0U;
}

static void _cookie_set(const char *name, const char *value)
{
    if (name == NULL || value == NULL || name[0] == '\0')
    {
        return;
    }
    for (size_t index = 0U; index < s_cookie_count; ++index)
    {
        if (strcmp(s_cookies[index].name, name) == 0)
        {
            (void)snprintf(s_cookies[index].value,
                           sizeof(s_cookies[index].value), "%s", value);
            return;
        }
    }
    if (s_cookie_count == MUSIC_COOKIE_MAX)
    {
        return;
    }
    (void)snprintf(s_cookies[s_cookie_count].name,
                   sizeof(s_cookies[s_cookie_count].name), "%s", name);
    (void)snprintf(s_cookies[s_cookie_count].value,
                   sizeof(s_cookies[s_cookie_count].value), "%s", value);
    ++s_cookie_count;
}

static void _cookie_from_header(const char *header)
{
    if (header == NULL)
    {
        return;
    }
    const char *end = strchr(header, ';');
    const char *equals = strchr(header, '=');
    if (equals == NULL || (end != NULL && equals > end))
    {
        return;
    }
    char name[32];
    char value[512];
    const size_t name_length = (size_t)(equals - header);
    const size_t value_length = end == NULL ? strlen(equals + 1) :
                                (size_t)(end - equals - 1);
    if (name_length == 0U || name_length >= sizeof(name) ||
            value_length >= sizeof(value))
    {
        return;
    }
    memcpy(name, header, name_length);
    name[name_length] = '\0';
    memcpy(value, equals + 1, value_length);
    value[value_length] = '\0';
    _cookie_set(name, value);
}

static void _cookie_header(char *output, size_t output_size)
{
    output[0] = '\0';
    size_t used = 0U;
    for (size_t index = 0U; index < s_cookie_count; ++index)
    {
        const int written = snprintf(output + used, output_size - used,
                                     "%s%s=%s", used == 0U ? "" : "; ",
                                     s_cookies[index].name,
                                     s_cookies[index].value);
        if (written < 0 || (size_t)written >= output_size - used)
        {
            output[output_size - 1U] = '\0';
            return;
        }
        used += (size_t)written;
    }
}

static esp_err_t _http_event(esp_http_client_event_t *event)
{
    music_http_context_t *context = event->user_data;
    if (context == NULL || context->result == NULL)
    {
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER)
    {
        if (event->header_key != NULL && event->header_value != NULL)
        {
            if (context->capture_cookies &&
                    strcasecmp(event->header_key, "Set-Cookie") == 0)
            {
                _cookie_from_header(event->header_value);
            }
            else if (strcasecmp(event->header_key, "Location") == 0)
            {
                (void)snprintf(context->result->location,
                               sizeof(context->result->location), "%s",
                               event->header_value);
            }
            else if (strcasecmp(event->header_key, "Content-Type") == 0)
            {
                (void)snprintf(context->result->content_type,
                               sizeof(context->result->content_type), "%s",
                               event->header_value);
            }
        }
    }
    else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0)
    {
        if (context->result->length + (size_t)event->data_len >
                context->limit)
        {
            context->overflow = true;
            return ESP_OK;
        }
        memcpy(context->result->body + context->result->length,
               event->data, (size_t)event->data_len);
        context->result->length += (size_t)event->data_len;
        context->result->body[context->result->length] = '\0';
    }
    return ESP_OK;
}

static void _result_free(music_http_result_t *result);

static esp_err_t _http_once(esp_http_client_method_t method, const char *url,
                            const char *body, bool with_cookie,
                            music_http_result_t *result, size_t max_body)
{
    if (url == NULL || result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->body = _psram_alloc(max_body + 1U);
    if (result->body == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    music_http_context_t context =
    {
        .result = result,
        .limit = max_body,
        .capture_cookies = with_cookie,
    };
    const esp_http_client_config_t config =
    {
        .url = url,
        .method = method,
        .event_handler = _http_event,
        .user_data = &context,
        .timeout_ms = MUSIC_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        _psram_free(result->body);
        result->body = NULL;
        return ESP_ERR_NO_MEM;
    }
    (void)esp_http_client_set_header(client, "User-Agent",
                                     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                                     "Chrome/120.0.0.0 Safari/537.36");
    (void)esp_http_client_set_header(client, "Referer", "https://y.qq.com/");
    (void)esp_http_client_set_header(client, "Accept", "*/*");
    if (with_cookie)
    {
        char *cookie = _psram_alloc(4096U);
        if (cookie != NULL)
        {
            _cookie_header(cookie, 4096U);
            if (cookie[0] != '\0')
            {
                (void)esp_http_client_set_header(client, "Cookie", cookie);
            }
            _psram_free(cookie);
        }
    }
    if (body != NULL)
    {
        (void)esp_http_client_set_header(client, "Content-Type",
                                         "application/json");
        (void)esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t error = esp_http_client_perform(client);
    if (error == ESP_OK)
    {
        result->status = esp_http_client_get_status_code(client);
    }
    (void)esp_http_client_cleanup(client);
    if (error != ESP_OK || context.overflow)
    {
        _psram_free(result->body);
        result->body = NULL;
        result->length = 0U;
        return error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
    }
    return ESP_OK;
}

static esp_err_t _http_request(esp_http_client_method_t method, const char *url,
                               const char *body, bool with_cookie,
                               music_http_result_t *result, size_t max_body)
{
    char current_url[1024];
    (void)snprintf(current_url, sizeof(current_url), "%s", url);
    esp_http_client_method_t current_method = method;
    const char *current_body = body;
    for (int redirect = 0; redirect <= MUSIC_HTTP_MAX_REDIRECTS; ++redirect)
    {
        esp_err_t error = _http_once(current_method, current_url,
                                     current_body, with_cookie,
                                     result, max_body);
        if (error != ESP_OK)
        {
            return error;
        }
        if (result->status >= 300 && result->status < 400 &&
                result->location[0] != '\0')
        {
            if (strncmp(result->location, "https://", 8) != 0)
            {
                _result_free(result);
                return ESP_ERR_INVALID_RESPONSE;
            }
            (void)snprintf(current_url, sizeof(current_url), "%s",
                           result->location);
            if (result->status == 301 || result->status == 302 ||
                    result->status == 303)
            {
                current_method = HTTP_METHOD_GET;
                current_body = NULL;
            }
            _psram_free(result->body);
            result->body = NULL;
            result->length = 0U;
            result->location[0] = '\0';
            continue;
        }
        return ESP_OK;
    }
    _result_free(result);
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t _http_get(const char *url, bool with_cookie,
                           music_http_result_t *result, size_t max_body)
{
    return _http_request(HTTP_METHOD_GET, url, NULL, with_cookie, result, max_body);
}

static esp_err_t _http_post(const char *url, const char *body,
                            music_http_result_t *result, size_t max_body)
{
    return _http_request(HTTP_METHOD_POST, url, body, true, result, max_body);
}

static void _result_free(music_http_result_t *result)
{
    if (result != NULL && result->body != NULL)
    {
        _psram_free(result->body);
        result->body = NULL;
        result->length = 0U;
    }
}

static uint32_t _hash33(const char *value, uint32_t initial)
{
    uint32_t hash = initial;
    if (value == NULL)
    {
        return hash & 0x7FFFFFFFU;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
            *cursor != '\0'; ++cursor)
    {
        hash = (hash << 5) + hash + *cursor;
    }
    return hash & 0x7FFFFFFFU;
}

static void _json_string(const cJSON *object, const char *key,
                         char *output, size_t output_size)
{
    output[0] = '\0';
    if (object == NULL)
    {
        return;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        (void)snprintf(output, output_size, "%s", item->valuestring);
    }
}

static void _json_string_or_number(const cJSON *object, const char *key,
                                   char *output, size_t output_size)
{
    output[0] = '\0';
    if (object == NULL)
    {
        return;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        (void)snprintf(output, output_size, "%s", item->valuestring);
    }
    else if (cJSON_IsNumber(item))
    {
        (void)snprintf(output, output_size, "%.0f", item->valuedouble);
    }
}

static uint64_t _json_u64(const cJSON *object, const char *key)
{
    if (object == NULL)
    {
        return 0U;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item))
    {
        return (uint64_t)item->valuedouble;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        return strtoull(item->valuestring, NULL, 10);
    }
    return 0U;
}

static uint32_t _json_u32(const cJSON *object, const char *key)
{
    return (uint32_t)_json_u64(object, key);
}

static bool _json_bool(const cJSON *object, const char *key)
{
    const cJSON *item = object == NULL ? NULL :
                        cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint != 0);
}

static cJSON *_musicu_post(const char *module, const char *method,
                           cJSON *param, cJSON **root_out)
{
    *root_out = NULL;
    if (module == NULL || method == NULL || param == NULL)
    {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *comm = cJSON_CreateObject();
    cJSON *request = cJSON_CreateObject();
    if (root == NULL || comm == NULL || request == NULL)
    {
        cJSON_Delete(root);
        cJSON_Delete(comm);
        cJSON_Delete(request);
        return NULL;
    }
    const bool logged_in = s_music.credential_valid;
    cJSON_AddStringToObject(comm, "uin",
                            logged_in ? s_music.credential.musicid : "0");
    cJSON_AddStringToObject(comm, "format", "json");
    cJSON_AddNumberToObject(comm, "ct", 24);
    cJSON_AddNumberToObject(comm, "cv", 0);
    cJSON_AddStringToObject(comm, "platform", "yqq.json");
    if (strcmp(module, "music.login.LoginServer") == 0 &&
            strcmp(method, "Login") == 0)
    {
        cJSON_AddNumberToObject(comm, "tmeLoginType", 1);
    }
    if (logged_in)
    {
        char g_tk[24];
        (void)snprintf(g_tk, sizeof(g_tk), "%u",
                       (unsigned)_hash33(s_music.credential.musickey, 5381U));
        cJSON_AddStringToObject(comm, "g_tk", g_tk);
        cJSON_AddStringToObject(comm, "authst", s_music.credential.musickey);
    }
    cJSON_AddItemToObject(root, "comm", comm);
    cJSON_AddStringToObject(request, "module", module);
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddItemToObject(request, "param", param);
    cJSON_AddItemToObject(root, "req_0", request);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL)
    {
        return NULL;
    }
    music_http_result_t result;
    const esp_err_t error = _http_post(
                                "https://u.y.qq.com/cgi-bin/musicu.fcg", body, &result,
                                MUSIC_JSON_MAX_BYTES);
    cJSON_free(body);
    if (error != ESP_OK)
    {
        return NULL;
    }
    cJSON *response = cJSON_Parse(result.body);
    _result_free(&result);
    if (response == NULL)
    {
        return NULL;
    }
    cJSON *response_request = cJSON_GetObjectItemCaseSensitive(response,
        "req_0");
    cJSON *data = response_request == NULL ? NULL :
                  cJSON_GetObjectItemCaseSensitive(response_request, "data");
    if (data == NULL)
    {
        cJSON_Delete(response);
        return NULL;
    }
    *root_out = response;
    return data;
}

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
    output->ref.id = _json_u64(basic, "tid");
    if (output->ref.id == 0U)
    {
        output->ref.id = _json_u64(basic, "dissid");
    }
    output->ref.dirid = _json_u32(basic, "dirid");
    output->ref.tid = _json_u32(basic, "tid");
    _json_string(basic, "title", output->title, sizeof(output->title));
    const cJSON *creator = cJSON_GetObjectItemCaseSensitive(basic, "creator");
    _json_string(creator, "nick", output->creator, sizeof(output->creator));
    const cJSON *cover = cJSON_GetObjectItemCaseSensitive(basic, "cover");
    _json_string(cover, "small_url", output->cover_url,
                 sizeof(output->cover_url));
    output->track_count = _json_u32(basic, "song_cnt");
    if (output->track_count == 0U)
    {
        output->track_count = _json_u32(basic, "songnum");
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
            credential->musicid[0] == '\0' ||
            credential->musickey[0] == '\0')
    {
        memset(credential, 0, sizeof(*credential));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t music_net_save_credential(const music_credential_blob_t *credential)
{
    if (credential == NULL || credential->musicid[0] == '\0' ||
            credential->musickey[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    return nv_storage_set_blob(MUSIC_CREDENTIAL_KEY, credential,
                               sizeof(*credential));
}

esp_err_t music_net_erase_credential(void)
{
    music_net_clear_session();
    const esp_err_t result = nv_storage_erase_key(MUSIC_CREDENTIAL_KEY);
    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
}

void music_net_cancel_login(void)
{
    s_music.qr_uuid[0] = '\0';
    s_music.qr_qrsig[0] = '\0';
}

void music_net_clear_session(void)
{
    _cookie_clear();
    music_net_cancel_login();
}

static esp_err_t _decode_rgb565(const uint8_t *data, size_t data_size,
                                uint16_t requested_width,
                                uint16_t requested_height,
                                music_image_t **image_out)
{
    if (data == NULL || data_size == 0U || image_out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    jpeg_dec_config_t config =
    {
        .output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
        .scale = { 0, 0 },
        .clipper = { 0, 0 },
        .rotate = JPEG_ROTATE_0D,
        .block_enable = false,
    };
    if (requested_width >= 8U && requested_height >= 8U)
    {
        /* The decoder requires scale dimensions to be multiples of 8. */
        config.scale.width = (uint16_t)(requested_width & 0xFFF8U);
        config.scale.height = (uint16_t)(requested_height & 0xFFF8U);
    }
    jpeg_dec_handle_t decoder = NULL;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    jpeg_dec_io_t io =
    {
        .inbuf = (uint8_t *)data,
        .inbuf_len = (int)data_size,
        .inbuf_remain = 0,
    };
    jpeg_dec_header_info_t header;
    if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK)
    {
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (header.width == 0U || header.height == 0U ||
            header.width > 1024U || header.height > 1024U)
    {
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_SIZE;
    }
    int output_size = 0;
    if (jpeg_dec_get_outbuf_len(decoder, &output_size) != JPEG_ERR_OK ||
            output_size <= 0 || output_size > (2 * 1024 * 1024))
    {
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t *pixels = jpeg_calloc_align((size_t)output_size, 16);
    if (pixels == NULL)
    {
        jpeg_dec_close(decoder);
        return ESP_ERR_NO_MEM;
    }
    io.outbuf = pixels;
    io.out_size = output_size;
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK)
    {
        jpeg_free_align(pixels);
        jpeg_dec_close(decoder);
        return ESP_ERR_INVALID_RESPONSE;
    }
    jpeg_dec_close(decoder);
    music_image_t *image = _psram_alloc(sizeof(*image));
    if (image == NULL)
    {
        jpeg_free_align(pixels);
        return ESP_ERR_NO_MEM;
    }
    image->width = config.scale.width != 0U ? config.scale.width :
                   header.width;
    image->height = config.scale.height != 0U ? config.scale.height :
                    header.height;
    image->data_size = (size_t)output_size;
    image->pixels = pixels;
    image->token = 0U;
    *image_out = image;
    return ESP_OK;
}

esp_err_t music_net_fetch_image(const char *url, uint16_t width,
                                uint16_t height, uint32_t token,
                                music_image_t **image)
{
    if (url == NULL || url[0] == '\0' || image == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *image = NULL;
    music_http_result_t result;
    const esp_err_t error = _http_get(url, false, &result,
                                      MUSIC_QR_MAX_BYTES);
    if (error != ESP_OK)
    {
        return error;
    }
    music_image_t *decoded = NULL;
    const esp_err_t decode_result = _decode_rgb565(
                                        (const uint8_t *)result.body,
                                        result.length, width, height, &decoded);
    _result_free(&result);
    if (decode_result != ESP_OK)
    {
        return decode_result;
    }
    decoded->token = token;
    (void)snprintf(decoded->url, sizeof(decoded->url), "%s", url);
    *image = decoded;
    return ESP_OK;
}

esp_err_t music_net_login_begin(music_login_type_t type)
{
    _cookie_clear();
    music_net_cancel_login();
    if (type != MUSIC_LOGIN_TYPE_WECHAT)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    music_http_result_t result;
    const esp_err_t error = _http_get(
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
        _result_free(&result);
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
        _result_free(&result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(s_music.qr_uuid, uuid_key, uuid_length);
    s_music.qr_uuid[uuid_length] = '\0';
    _result_free(&result);

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
    music_service_set_login_qr(qr);
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
    cJSON *root = NULL;
    cJSON *data = _musicu_post("music.login.LoginServer", "Login",
                               param, &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    memset(credential, 0, sizeof(*credential));
    credential->magic = MUSIC_CREDENTIAL_MAGIC;
    credential->version = MUSIC_CREDENTIAL_VERSION;
    credential->login_type = 1U;
    _json_string_or_number(data, "musicid", credential->musicid,
                           sizeof(credential->musicid));
    if (credential->musicid[0] == '\0')
    {
        _json_string_or_number(data, "str_musicid", credential->musicid,
                               sizeof(credential->musicid));
    }
    _json_string(data, "musickey", credential->musickey,
                 sizeof(credential->musickey));
    _json_string(data, "refresh_key", credential->refresh_key,
                 sizeof(credential->refresh_key));
    _json_string(data, "refresh_token", credential->refresh_token,
                 sizeof(credential->refresh_token));
    credential->expires_at = _json_u32(data, "expired_at");
    if (credential->expires_at == 0U)
    {
        credential->expires_at = _json_u32(data, "expiredAt");
    }
    cJSON_Delete(root);
    return credential->musicid[0] != '\0' &&
           credential->musickey[0] != '\0' ? ESP_OK :
           ESP_ERR_INVALID_RESPONSE;
}

esp_err_t music_net_login_poll(music_credential_blob_t *credential,
                               music_login_poll_state_t *state)
{
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
    const esp_err_t error = _http_get(url, true, &result,
                                      MUSIC_QR_POLL_MAX_BYTES);
    if (error != ESP_OK)
    {
        return error;
    }
    const char *error_key = strstr(result.body, "wx_errcode=");
    if (error_key == NULL)
    {
        _result_free(&result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int error_code = atoi(error_key + strlen("wx_errcode="));
    if (error_code == 408)
    {
        _result_free(&result);
        return ESP_OK;
    }
    if (error_code == 404)
    {
        *state = MUSIC_LOGIN_POLL_SCANNED;
        _result_free(&result);
        return ESP_OK;
    }
    if (error_code != 405)
    {
        *state = MUSIC_LOGIN_POLL_FAILED;
        _result_free(&result);
        return ESP_OK;
    }
    const char *code_key = strstr(result.body, "wx_code='");
    if (code_key == NULL)
    {
        _result_free(&result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    code_key += strlen("wx_code='");
    const char *code_end = strchr(code_key, '\'');
    if (code_end == NULL || code_end == code_key)
    {
        _result_free(&result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    char code[256];
    const size_t code_length = (size_t)(code_end - code_key);
    if (code_length >= sizeof(code))
    {
        _result_free(&result);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(code, code_key, code_length);
    code[code_length] = '\0';
    const esp_err_t exchange = _exchange_wechat_code(code, credential);
    _result_free(&result);
    if (exchange != ESP_OK)
    {
        return exchange;
    }
    *state = MUSIC_LOGIN_POLL_SUCCESS;
    return ESP_OK;
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
    cJSON *data = _musicu_post("music.playlist.PlaylistSquare",
                               "GetRecommendFeed", param, &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
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
    cJSON *data = _musicu_post("music.musicasset.PlaylistBaseRead",
                               "GetPlaylistByUin", param, &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(data, "disslist");
    if (cJSON_IsArray(list))
    {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, list)
        {
            /* dirid 1 is a created playlist; dirid 201 is the favorite
             * playlist bucket exposed by QQ Music. */
            const uint32_t dirid = _json_u32(item, "dirid");
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
            slot->ref.id = _json_u64(item, "tid");
            slot->ref.dirid = dirid;
            slot->ref.tid = _json_u32(item, "tid");
            _json_string(item, "diss_name", slot->title, sizeof(slot->title));
            _json_string(item, "diss_cover", slot->cover_url,
                         sizeof(slot->cover_url));
            slot->track_count = _json_u32(item, "song_cnt");
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t music_net_fetch_home(music_home_snapshot_t **snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    music_home_snapshot_t *home = _psram_alloc(sizeof(*home));
    if (home == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    home->generation = music_service_next_generation();
    esp_err_t result = _home_recommended(home);
    if (result != ESP_OK)
    {
        _psram_free(home);
        return result;
    }
    /* A private/failed personal section must not hide the anonymous
     * recommendation feed. */
    if (_home_personal(home) != ESP_OK)
    {
        home->login_required = true;
    }
    *snapshot = home;
    return ESP_OK;
}

static void _parse_song(const cJSON *item, music_song_t *song)
{
    memset(song, 0, sizeof(*song));
    song->songid = _json_u64(item, "songid");
    _json_string(item, "songmid", song->songmid, sizeof(song->songmid));
    _json_string(item, "strMediaMid", song->media_mid,
                 sizeof(song->media_mid));
    if (song->media_mid[0] == '\0')
    {
        _json_string(item, "media_mid", song->media_mid,
                     sizeof(song->media_mid));
    }
    _json_string(item, "songname", song->title, sizeof(song->title));
    if (song->title[0] == '\0')
    {
        _json_string(item, "name", song->title, sizeof(song->title));
    }
    _json_string(item, "albumname", song->album, sizeof(song->album));
    const cJSON *singer = cJSON_GetObjectItemCaseSensitive(item, "singer");
    if (cJSON_IsArray(singer))
    {
        const cJSON *artist = cJSON_GetArrayItem(singer, 0);
        _json_string(artist, "name", song->artist, sizeof(song->artist));
    }
    song->duration_ms = _json_u32(item, "interval") * 1000U;
    song->size_128 = _json_u32(item, "size128");
    song->size_320 = _json_u32(item, "size320");
    song->size_flac = _json_u32(item, "sizeflac");
    const cJSON *album = cJSON_GetObjectItemCaseSensitive(item, "album");
    char album_mid[64] = {0};
    if (cJSON_IsObject(album))
    {
        _json_string(album, "mid", album_mid, sizeof(album_mid));
    }
    else
    {
        _json_string(item, "albummid", album_mid, sizeof(album_mid));
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
        if (_json_bool(pay, "payplay"))
        {
            song->pay_flags |= 1U;
        }
        if (_json_bool(pay, "paydownload"))
        {
            song->pay_flags |= 2U;
        }
    }
}

esp_err_t music_net_fetch_playlist(const music_playlist_ref_t *ref,
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
    cJSON_AddNumberToObject(param, "disstid", (double)ref->id);
    cJSON_AddNumberToObject(param, "dirid", (double)ref->dirid);
    cJSON_AddBoolToObject(param, "tag", true);
    cJSON_AddNumberToObject(param, "song_begin", offset);
    cJSON_AddNumberToObject(param, "song_num", count);
    cJSON_AddBoolToObject(param, "userinfo", true);
    cJSON_AddBoolToObject(param, "orderlist", true);
    cJSON_AddBoolToObject(param, "onlysonglist", false);
    cJSON *root = NULL;
    cJSON *data = _musicu_post("music.srfDissInfo.DissInfo", "CgiGetDiss",
                               param, &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    music_playlist_snapshot_t *playlist = _psram_alloc(sizeof(*playlist));
    if (playlist == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    playlist->generation = music_service_next_generation();
    playlist->playlist.ref = *ref;
    _json_string(data, "dissname", playlist->playlist.title,
                 sizeof(playlist->playlist.title));
    _json_string(data, "diss_cover", playlist->playlist.cover_url,
                 sizeof(playlist->playlist.cover_url));
    playlist->playlist.track_count = _json_u32(data, "songnum");
    playlist->offset = offset;
    playlist->total = _json_u32(data, "total_song_num");
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

static void _parse_lrc(const char *text, music_lyric_snapshot_t *lyric)
{
    if (text == NULL || lyric == NULL)
    {
        return;
    }
    const char *cursor = text;
    while (*cursor != '\0' && lyric->line_count < MUSIC_SERVICE_MAX_LYRIC_LINES)
    {
        const char *line_end = strchr(cursor, '\n');
        if (line_end == NULL)
        {
            line_end = cursor + strlen(cursor);
        }
        const char *open = cursor;
        while (open < line_end && *open != '[')
        {
            ++open;
        }
        if (open < line_end)
        {
            const char *close = strchr(open, ']');
            if (close != NULL && close < line_end)
            {
                unsigned minutes = 0U;
                unsigned seconds = 0U;
                unsigned hundredths = 0U;
                if (sscanf(open + 1, "%u:%u.%u", &minutes, &seconds,
                           &hundredths) >= 2)
                {
                    music_lyric_line_t *line =
                        &lyric->lines[lyric->line_count];
                    line->offset_ms = minutes * 60000U + seconds * 1000U +
                                      hundredths * 10U;
                    const char *text_start = close + 1;
                    while (text_start < line_end &&
                            (*text_start == ' ' || *text_start == '\r'))
                    {
                        ++text_start;
                    }
                    size_t length = (size_t)(line_end - text_start);
                    if (length >= sizeof(line->text))
                    {
                        length = sizeof(line->text) - 1U;
                    }
                    memcpy(line->text, text_start, length);
                    line->text[length] = '\0';
                    if (line->text[0] != '\0')
                    {
                        ++lyric->line_count;
                    }
                }
            }
        }
        cursor = *line_end == '\0' ? line_end : line_end + 1;
    }
}

esp_err_t music_net_fetch_lyric(const char *songmid,
                                music_lyric_snapshot_t **snapshot)
{
    if (songmid == NULL || songmid[0] == '\0' || snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    cJSON *param = cJSON_CreateObject();
    if (param == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(param, "songMid", songmid);
    cJSON_AddNumberToObject(param, "crypt", 1);
    cJSON_AddNumberToObject(param, "lrc_t", 0);
    cJSON_AddNumberToObject(param, "qrc", 0);
    cJSON_AddNumberToObject(param, "qrc_t", 0);
    cJSON_AddNumberToObject(param, "roma", 0);
    cJSON_AddNumberToObject(param, "trans", 0);
    cJSON_AddNumberToObject(param, "type", 1);
    cJSON *root = NULL;
    cJSON *data = _musicu_post("music.musichallSong.PlayLyricInfo",
                               "GetPlayLyricInfo", param, &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t encoded_size = 256U * 1024U;
    char *encoded = _psram_alloc(encoded_size);
    if (encoded == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    _json_string(data, "lyric", encoded, encoded_size);
    cJSON_Delete(root);
    if (encoded[0] == '\0')
    {
        _psram_free(encoded);
        return ESP_ERR_NOT_FOUND;
    }
    unsigned char *decoded = _psram_alloc(encoded_size);
    if (decoded == NULL)
    {
        _psram_free(encoded);
        return ESP_ERR_NO_MEM;
    }
    size_t decoded_length = 0U;
    const int base64_result = mbedtls_base64_decode(
                                  decoded, encoded_size,
                                  &decoded_length,
                                  (const unsigned char *)encoded,
                                  strlen(encoded));
    if (base64_result != 0)
    {
        _psram_free(encoded);
        _psram_free(decoded);
        return ESP_ERR_INVALID_RESPONSE;
    }
    _psram_free(encoded);
    decoded[decoded_length] = '\0';
    music_lyric_snapshot_t *lyric = _psram_alloc(sizeof(*lyric));
    if (lyric == NULL)
    {
        _psram_free(decoded);
        return ESP_ERR_NO_MEM;
    }
    lyric->generation = music_service_next_generation();
    _parse_lrc((const char *)decoded, lyric);
    _psram_free(decoded);
    if (lyric->line_count == 0U)
    {
        _psram_free(lyric);
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = lyric;
    return ESP_OK;
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

esp_err_t music_net_fetch_song_url(const music_song_t *song,
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
    cJSON_AddStringToObject(param, "songmid", song->songmid);
    cJSON_AddNumberToObject(param, "songtype", 0);
    cJSON_AddStringToObject(param, "uin", s_music.credential.musicid);
    cJSON_AddNumberToObject(param, "loginflag", 1);
    cJSON_AddStringToObject(param, "platform", "20");
    cJSON *filenames = cJSON_AddArrayToObject(param, "filename");
    if (filenames != NULL)
    {
        cJSON_AddItemToArray(filenames, cJSON_CreateString(filename));
    }
    cJSON *root = NULL;
    cJSON *data = _musicu_post("vkey.GetVkeyServer", "CgiGetVkey", param,
                               &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *midurlinfo = cJSON_GetObjectItemCaseSensitive(data,
        "midurlinfo");
    const cJSON *first = cJSON_IsArray(midurlinfo) ?
                         cJSON_GetArrayItem(midurlinfo, 0) : NULL;
    char purl[512];
    _json_string(first, "purl", purl, sizeof(purl));
    if (purl[0] == '\0')
    {
        cJSON_Delete(root);
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
