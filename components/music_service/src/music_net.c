#define DBG_TAG "music_net"
#include "mt_log.h"

#include "music_service_internal.h"
#include "music_login_protocol.h"

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
#include "png.h"

#include <ctype.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MUSIC_HTTP_TIMEOUT_MS 10000
#define MUSIC_HTTP_MAX_REDIRECTS 8
#define MUSIC_COOKIE_MAX 24
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

static MUSIC_SERVICE_EXT_BSS music_cookie_t s_cookies[MUSIC_COOKIE_MAX];
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

static bool _cookie_get(const char *name, char *value, size_t value_size)
{
    if (name == NULL || value == NULL || value_size == 0U)
    {
        return false;
    }
    value[0] = '\0';
    for (size_t index = 0U; index < s_cookie_count; ++index)
    {
        if (strcmp(s_cookies[index].name, name) == 0)
        {
            (void)snprintf(value, value_size, "%s", s_cookies[index].value);
            return true;
        }
    }
    return false;
}

static void _cookie_from_header(const char *header)
{
    char name[32];
    char value[512];
    if (music_login_cookie_parse(header, name, sizeof(name),
                                 value, sizeof(value)))
    {
        _cookie_set(name, value);
    }
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
        if (context->limit == 0U)
        {
            return ESP_OK;
        }
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
                            const char *body, const char *content_type,
                            const char *referer, bool with_cookie,
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
    (void)esp_http_client_set_header(client, "Referer",
                                     referer == NULL ? "https://y.qq.com/" :
                                     referer);
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
                                         content_type == NULL ?
                                         "application/json" : content_type);
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
                               const char *body, const char *content_type,
                               const char *referer, bool with_cookie,
                               music_http_result_t *result, size_t max_body)
{
    char current_url[1024];
    (void)snprintf(current_url, sizeof(current_url), "%s", url);
    esp_http_client_method_t current_method = method;
    const char *current_body = body;
    const char *current_content_type = content_type;
    for (int redirect = 0; redirect <= MUSIC_HTTP_MAX_REDIRECTS; ++redirect)
    {
        esp_err_t error = _http_once(current_method, current_url,
                                     current_body, current_content_type,
                                     referer, with_cookie, result, max_body);
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
                current_content_type = NULL;
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

static esp_err_t _http_get_referer(const char *url, bool with_cookie,
                                   const char *referer,
                                   music_http_result_t *result,
                                   size_t max_body)
{
    return _http_request(HTTP_METHOD_GET, url, NULL, NULL, referer,
                         with_cookie, result, max_body);
}

static esp_err_t _http_get(const char *url, bool with_cookie,
                           music_http_result_t *result, size_t max_body)
{
    return _http_get_referer(url, with_cookie, NULL, result, max_body);
}

static esp_err_t _http_get_headers(const char *url, bool with_cookie,
                                   const char *referer,
                                   music_http_result_t *result)
{
    return _http_request(HTTP_METHOD_GET, url, NULL, NULL, referer,
                         with_cookie, result, 0U);
}

static esp_err_t _http_post(const char *url, const char *body,
                            music_http_result_t *result, size_t max_body)
{
    return _http_request(HTTP_METHOD_POST, url, body, "application/json",
                         NULL, true, result, max_body);
}

static esp_err_t _http_post_form_no_redirect(const char *url,
        const char *body, const char *referer,
        music_http_result_t *result, size_t max_body)
{
    return _http_once(HTTP_METHOD_POST, url, body,
                      "application/x-www-form-urlencoded", referer,
                      true, result, max_body);
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
    else if (strcmp(module, "QQConnectLogin.LoginServer") == 0 &&
             strcmp(method, "QQLogin") == 0)
    {
        cJSON_AddStringToObject(comm, "tmeLoginType", "2");
    }
    if (logged_in)
    {
        char g_tk[24];
        (void)snprintf(g_tk, sizeof(g_tk), "%u",
                       (unsigned)music_login_hash33(
                           s_music.credential.musickey, 5381U));
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
        const cJSON *code = response_request == NULL ? NULL :
                            cJSON_GetObjectItemCaseSensitive(response_request,
                                "code");
        LOG_W("%s/%s failed: code=%d", module, method,
              cJSON_IsNumber(code) ? code->valueint : -1);
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
    s_music.qq_login_sig[0] = '\0';
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

static esp_err_t _decode_png_rgb565(const uint8_t *data, size_t data_size,
                                    music_image_t **image_out)
{
    if (data == NULL || data_size == 0U || image_out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    png_image png;
    memset(&png, 0, sizeof(png));
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, data, data_size))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (png.width == 0U || png.height == 0U ||
            png.width > 1024U || png.height > 1024U ||
            (uint64_t)png.width * png.height > 1024U * 1024U)
    {
        png_image_free(&png);
        return ESP_ERR_INVALID_SIZE;
    }
    png.format = PNG_FORMAT_RGBA;
    const size_t rgba_size = PNG_IMAGE_SIZE(png);
    if (rgba_size == 0U || rgba_size > (2U * 1024U * 1024U))
    {
        png_image_free(&png);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *rgba = _psram_alloc(rgba_size);
    if (rgba == NULL)
    {
        png_image_free(&png);
        return ESP_ERR_NO_MEM;
    }
    if (!png_image_finish_read(&png, NULL, rgba, 0, NULL))
    {
        _psram_free(rgba);
        png_image_free(&png);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint16_t width = (uint16_t)png.width;
    const uint16_t height = (uint16_t)png.height;
    png_image_free(&png);

    const size_t pixel_bytes = (size_t)width * (size_t)height * 2U;
    uint8_t *pixels = jpeg_calloc_align(pixel_bytes, 16);
    if (pixels == NULL)
    {
        _psram_free(rgba);
        return ESP_ERR_NO_MEM;
    }
    uint16_t *output = (uint16_t *)pixels;
    for (size_t index = 0U; index < (size_t)width * height; ++index)
    {
        const uint32_t red = rgba[index * 4U];
        const uint32_t green = rgba[index * 4U + 1U];
        const uint32_t blue = rgba[index * 4U + 2U];
        const uint32_t alpha = rgba[index * 4U + 3U];
        const uint32_t inverse = 255U - alpha;
        const uint8_t mixed_red =
            (uint8_t)((red * alpha + 255U * inverse) / 255U);
        const uint8_t mixed_green =
            (uint8_t)((green * alpha + 255U * inverse) / 255U);
        const uint8_t mixed_blue =
            (uint8_t)((blue * alpha + 255U * inverse) / 255U);
        output[index] = (uint16_t)(((uint16_t)mixed_red & 0xF8U) << 8) |
                        (uint16_t)(((uint16_t)mixed_green & 0xFCU) << 3) |
                        (uint16_t)(mixed_blue >> 3);
    }
    _psram_free(rgba);

    music_image_t *image = _psram_alloc(sizeof(*image));
    if (image == NULL)
    {
        jpeg_free_align(pixels);
        return ESP_ERR_NO_MEM;
    }
    image->width = width;
    image->height = height;
    image->data_size = pixel_bytes;
    image->pixels = pixels;
    image->token = 0U;
    *image_out = image;
    return ESP_OK;
}

static esp_err_t _fetch_image(const char *url, uint16_t width,
                              uint16_t height, uint32_t token,
                              bool with_cookie, const char *referer,
                              music_image_t **image)
{
    if (url == NULL || url[0] == '\0' || image == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *image = NULL;
    music_http_result_t result;
    const esp_err_t error = _http_get_referer(url, with_cookie, referer,
        &result, MUSIC_QR_MAX_BYTES);
    if (error != ESP_OK)
    {
        return error;
    }
    music_image_t *decoded = NULL;
    esp_err_t decode_result;
    if (music_login_png_signature((const uint8_t *)result.body,
                                  result.length))
    {
        decode_result = _decode_png_rgb565((const uint8_t *)result.body,
                                           result.length, &decoded);
    }
    else
    {
        decode_result = _decode_rgb565((const uint8_t *)result.body,
                                       result.length, width, height,
                                       &decoded);
    }
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

esp_err_t music_net_fetch_image(const char *url, uint16_t width,
                                uint16_t height, uint32_t token,
                                music_image_t **image)
{
    return _fetch_image(url, width, height, token, false, NULL, image);
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
    cJSON *root = NULL;
    cJSON *data = _musicu_post("QQConnectLogin.LoginServer", "QQLogin",
                               param, &root);
    if (data == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const esp_err_t result = _credential_from_data(data, 2U, credential);
    cJSON_Delete(root);
    return result;
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
    result = _http_get_headers(auth_url, true, MUSIC_QQ_REFERER,
                               &auth_result);
    if (result != ESP_OK)
    {
        goto done;
    }
    const int status = auth_result.status;
    _result_free(&auth_result);
    if (status < 200 || status >= 300)
    {
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    if (!_cookie_get("p_skey", p_skey, 512U))
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
    result = _http_post_form_no_redirect(MUSIC_QQ_AUTHORIZE_URL, body,
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
    _result_free(&code_result);
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

static esp_err_t _qq_login_begin(void)
{
    music_http_result_t result;
    esp_err_t error = _http_get_headers(MUSIC_QQ_XLOGIN_URL, true,
                                        MUSIC_QQ_REFERER, &result);
    if (error != ESP_OK)
    {
        LOG_W("qq xlogin failed: %s", esp_err_to_name(error));
        return error;
    }
    const int status = result.status;
    _result_free(&result);
    if (status < 200 || status >= 300)
    {
        LOG_W("qq xlogin status=%d", status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!_cookie_get("pt_login_sig", s_music.qq_login_sig,
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
    error = _fetch_image(qr_url, 240U, 240U, 1U, true,
                         MUSIC_QQ_REFERER, &qr);
    if (error != ESP_OK)
    {
        LOG_W("qq qr image failed: %s", esp_err_to_name(error));
        return error;
    }
    if (!_cookie_get("qrsig", s_music.qr_qrsig,
                     sizeof(s_music.qr_qrsig)))
    {
        LOG_W("qq qr response missing qrsig");
        music_service_login_qr_release(qr);
        return ESP_ERR_INVALID_RESPONSE;
    }
    music_service_set_login_qr(qr);
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
    result = _http_get_referer(url, true, MUSIC_QQ_REFERER, &http_result,
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
    _result_free(&http_result);
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
    _cookie_clear();
    *state = MUSIC_LOGIN_POLL_SUCCESS;

done:
    _psram_free(query);
    _psram_free(url);
    _psram_free(auth_url);
    return result;
}

esp_err_t music_net_login_begin(music_login_type_t type)
{
    _cookie_clear();
    music_net_cancel_login();
    if (type == MUSIC_LOGIN_TYPE_QQ)
    {
        return _qq_login_begin();
    }
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
    const esp_err_t result = _credential_from_data(data, 1U, credential);
    cJSON_Delete(root);
    return result;
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
            uint32_t dirid = _json_u32(item, "dirId");
            if (dirid == 0U)
            {
                dirid = _json_u32(item, "dirid");
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
            slot->ref.id = _json_u64(item, "tid");
            slot->ref.dirid = dirid;
            slot->ref.tid = _json_u32(item, "tid");
            _json_string(item, "dirName", slot->title,
                         sizeof(slot->title));
            if (slot->title[0] == '\0')
            {
                _json_string(item, "diss_name", slot->title,
                             sizeof(slot->title));
            }
            _json_string(item, "picUrl", slot->cover_url,
                         sizeof(slot->cover_url));
            if (slot->cover_url[0] == '\0')
            {
                _json_string(item, "bigpicUrl", slot->cover_url,
                             sizeof(slot->cover_url));
            }
            if (slot->cover_url[0] == '\0')
            {
                _json_string(item, "diss_cover", slot->cover_url,
                             sizeof(slot->cover_url));
            }
            const cJSON *creator =
                cJSON_GetObjectItemCaseSensitive(item, "creator");
            _json_string(creator, "nick", slot->creator,
                         sizeof(slot->creator));
            if (slot->creator[0] == '\0')
            {
                _json_string(item, "nick", slot->creator,
                             sizeof(slot->creator));
            }
            slot->track_count = _json_u32(item, "songNum");
            if (slot->track_count == 0U)
            {
                slot->track_count = _json_u32(item, "song_cnt");
            }
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
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(item, "file");
    const cJSON *album = cJSON_GetObjectItemCaseSensitive(item, "album");
    song->songid = _json_u64(item, "songid");
    if (song->songid == 0U)
    {
        song->songid = _json_u64(item, "id");
    }
    _json_string(item, "songmid", song->songmid, sizeof(song->songmid));
    if (song->songmid[0] == '\0')
    {
        _json_string(item, "mid", song->songmid, sizeof(song->songmid));
    }
    _json_string(item, "strMediaMid", song->media_mid,
                 sizeof(song->media_mid));
    if (song->media_mid[0] == '\0')
    {
        _json_string(item, "media_mid", song->media_mid,
                     sizeof(song->media_mid));
    }
    if (song->media_mid[0] == '\0')
    {
        _json_string(file, "media_mid", song->media_mid,
                     sizeof(song->media_mid));
    }
    _json_string(item, "songname", song->title, sizeof(song->title));
    if (song->title[0] == '\0')
    {
        _json_string(item, "name", song->title, sizeof(song->title));
    }
    _json_string(item, "albumname", song->album, sizeof(song->album));
    if (song->album[0] == '\0')
    {
        _json_string(album, "name", song->album, sizeof(song->album));
    }
    const cJSON *singer = cJSON_GetObjectItemCaseSensitive(item, "singer");
    if (cJSON_IsArray(singer))
    {
        const cJSON *artist = cJSON_GetArrayItem(singer, 0);
        _json_string(artist, "name", song->artist, sizeof(song->artist));
    }
    song->duration_ms = _json_u32(item, "interval") * 1000U;
    song->size_128 = _json_u32(item, "size128");
    if (song->size_128 == 0U)
    {
        song->size_128 = _json_u32(file, "size_128mp3");
    }
    song->size_320 = _json_u32(item, "size320");
    if (song->size_320 == 0U)
    {
        song->size_320 = _json_u32(file, "size_320mp3");
    }
    song->size_flac = _json_u32(item, "sizeflac");
    if (song->size_flac == 0U)
    {
        song->size_flac = _json_u32(file, "size_flac");
    }
    char album_mid[64] = {0};
    if (cJSON_IsObject(album))
    {
        _json_string(album, "mid", album_mid, sizeof(album_mid));
    }
    if (album_mid[0] == '\0')
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
        if (_json_bool(pay, "payplay") || _json_bool(pay, "pay_play"))
        {
            song->pay_flags |= 1U;
        }
        if (_json_bool(pay, "paydownload") || _json_bool(pay, "pay_down"))
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
    const cJSON *dirinfo = cJSON_GetObjectItemCaseSensitive(data, "dirinfo");
    _json_string(data, "dissname", playlist->playlist.title,
                 sizeof(playlist->playlist.title));
    if (playlist->playlist.title[0] == '\0')
    {
        _json_string(dirinfo, "title", playlist->playlist.title,
                     sizeof(playlist->playlist.title));
    }
    _json_string(data, "diss_cover", playlist->playlist.cover_url,
                 sizeof(playlist->playlist.cover_url));
    if (playlist->playlist.cover_url[0] == '\0')
    {
        _json_string(dirinfo, "picurl", playlist->playlist.cover_url,
                     sizeof(playlist->playlist.cover_url));
    }
    if (playlist->playlist.cover_url[0] == '\0')
    {
        _json_string(dirinfo, "bigpicUrl", playlist->playlist.cover_url,
                     sizeof(playlist->playlist.cover_url));
    }
    const cJSON *creator = cJSON_GetObjectItemCaseSensitive(dirinfo,
        "creator");
    _json_string(creator, "nick", playlist->playlist.creator,
                 sizeof(playlist->playlist.creator));
    playlist->playlist.track_count = _json_u32(data, "songnum");
    if (playlist->playlist.track_count == 0U)
    {
        playlist->playlist.track_count = _json_u32(dirinfo, "songnum");
    }
    if (playlist->playlist.track_count == 0U)
    {
        playlist->playlist.track_count = _json_u32(data, "songlist_size");
    }
    playlist->offset = offset;
    playlist->total = _json_u32(data, "total_song_num");
    if (playlist->total == 0U)
    {
        playlist->total = _json_u32(dirinfo, "songnum");
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
    /* crypt=1 returns a hex-encoded QRC payload, while this service parses
     * plain LRC. crypt=0 keeps the lyric field Base64-encoded LRC text. */
    cJSON_AddNumberToObject(param, "crypt", 0);
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
        /* The newer vkey response exposes a relative wifiurl instead of
         * purl. Keep purl as the primary field and use wifiurl as fallback. */
        _json_string(first, "wifiurl", purl, sizeof(purl));
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
