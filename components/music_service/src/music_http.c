#define DBG_TAG "music_http"
#include "mt_log.h"

#include "music_service_internal.h"
#include "music_http.h"
#include "music_login_protocol.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"

#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <time.h>

#define MUSIC_HTTP_TIMEOUT_MS 10000
#define MUSIC_HTTP_MAX_REDIRECTS 8
#define MUSIC_COOKIE_MAX 24

typedef struct music_cookie
{
    char name[32];
    char value[512];
    char domain[96];
    char path[96];
    uint32_t expires_at;
    uint32_t last_used;
    bool secure;
} music_cookie_t;

typedef struct music_http_context
{
    music_http_result_t *result;
    size_t limit;
    bool overflow;
    bool capture_cookies;
    char host[128];
} music_http_context_t;

static MUSIC_SERVICE_EXT_BSS music_cookie_t s_cookies[MUSIC_COOKIE_MAX];
static size_t s_cookie_count;
static SemaphoreHandle_t s_http_lock;
static SemaphoreHandle_t s_cookie_lock;

esp_err_t music_http_init(void)
{
    if (s_http_lock != NULL || s_cookie_lock != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_http_lock = xSemaphoreCreateMutex();
    s_cookie_lock = xSemaphoreCreateMutex();
    if (s_http_lock == NULL || s_cookie_lock == NULL)
    {
        if (s_http_lock != NULL)
        {
            vSemaphoreDelete(s_http_lock);
            s_http_lock = NULL;
        }
        if (s_cookie_lock != NULL)
        {
            vSemaphoreDelete(s_cookie_lock);
            s_cookie_lock = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void music_http_deinit(void)
{
    if (s_http_lock != NULL)
    {
        vSemaphoreDelete(s_http_lock);
        s_http_lock = NULL;
    }
    if (s_cookie_lock != NULL)
    {
        vSemaphoreDelete(s_cookie_lock);
        s_cookie_lock = NULL;
    }
}

static bool _http_lock_take(void)
{
    return s_http_lock != NULL &&
           xSemaphoreTake(s_http_lock, portMAX_DELAY) == pdTRUE;
}

static void _http_lock_give(void)
{
    if (s_http_lock != NULL)
    {
        (void)xSemaphoreGive(s_http_lock);
    }
}

static bool _cookie_lock_take(void)
{
    return s_cookie_lock != NULL &&
           xSemaphoreTake(s_cookie_lock, portMAX_DELAY) == pdTRUE;
}

static void _cookie_lock_give(void)
{
    if (s_cookie_lock != NULL)
    {
        (void)xSemaphoreGive(s_cookie_lock);
    }
}

static void *_psram_alloc(size_t size)
{
    return heap_caps_calloc(1U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void _psram_free(void *pointer)
{
    heap_caps_free(pointer);
}

static uint32_t s_cookie_tick;

static void _url_host_path(const char *url, char *host, size_t host_size,
                           char *path, size_t path_size)
{
    host[0] = '\0';
    path[0] = '/';
    if (url == NULL)
    {
        return;
    }
    const char *scheme = strstr(url, "://");
    const char *start = scheme == NULL ? url : scheme + 3;
    const char *end = start;
    while (*end != '\0' && *end != '/' && *end != ':' && *end != '?')
    {
        ++end;
    }
    size_t length = (size_t)(end - start);
    if (length >= host_size)
    {
        length = host_size - 1U;
    }
    memcpy(host, start, length);
    host[length] = '\0';
    if (*end == '/')
    {
        (void)snprintf(path, path_size, "%s", end);
    }
}

static bool _cookie_domain_matches(const music_cookie_t *cookie,
                                   const char *host)
{
    if (cookie->domain[0] == '\0')
    {
        return true;
    }
    const size_t host_len = strlen(host);
    const size_t domain_len = strlen(cookie->domain);
    if (host_len == domain_len)
    {
        return strcasecmp(host, cookie->domain) == 0;
    }
    return host_len > domain_len &&
           strcasecmp(host + host_len - domain_len, cookie->domain) == 0 &&
           host[host_len - domain_len - 1U] == '.';
}

static void _cookie_clear(void)
{
    memset(s_cookies, 0, sizeof(s_cookies));
    s_cookie_count = 0U;
}

static void _cookie_set(const char *name, const char *value,
                        const char *domain, const char *path,
                        uint32_t expires_at, bool secure)
{
    if (name == NULL || value == NULL || name[0] == '\0')
    {
        return;
    }
    for (size_t index = 0U; index < s_cookie_count; ++index)
    {
        if (strcmp(s_cookies[index].name, name) == 0 &&
                strcmp(s_cookies[index].domain, domain) == 0 &&
                strcmp(s_cookies[index].path, path) == 0)
        {
            (void)snprintf(s_cookies[index].value,
                           sizeof(s_cookies[index].value), "%s", value);
            s_cookies[index].expires_at = expires_at;
            s_cookies[index].secure = secure;
            s_cookies[index].last_used = ++s_cookie_tick;
            return;
        }
    }
    size_t slot = s_cookie_count;
    if (s_cookie_count == MUSIC_COOKIE_MAX)
    {
        slot = 0U;
        for (size_t index = 1U; index < s_cookie_count; ++index)
        {
            if (s_cookies[index].last_used < s_cookies[slot].last_used)
            {
                slot = index;
            }
        }
    }
    else
    {
        ++s_cookie_count;
    }
    memset(&s_cookies[slot], 0, sizeof(s_cookies[slot]));
    (void)snprintf(s_cookies[slot].name, sizeof(s_cookies[slot].name),
                   "%s", name);
    (void)snprintf(s_cookies[slot].value, sizeof(s_cookies[slot].value),
                   "%s", value);
    (void)snprintf(s_cookies[slot].domain, sizeof(s_cookies[slot].domain),
                   "%s", domain == NULL ? "" : domain);
    (void)snprintf(s_cookies[slot].path, sizeof(s_cookies[slot].path),
                   "%s", path == NULL || path[0] == '\0' ? "/" : path);
    s_cookies[slot].expires_at = expires_at;
    s_cookies[slot].secure = secure;
    s_cookies[slot].last_used = ++s_cookie_tick;
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
        if (strcmp(s_cookies[index].name, name) == 0 &&
                (s_cookies[index].expires_at == 0U ||
                 (uint32_t)time(NULL) < s_cookies[index].expires_at))
        {
            (void)snprintf(value, value_size, "%s", s_cookies[index].value);
            s_cookies[index].last_used = ++s_cookie_tick;
            return true;
        }
    }
    return false;
}

static void _cookie_from_header(const char *header, const char *host)
{
    char name[32];
    char value[512];
    if (!music_login_cookie_parse(header, name, sizeof(name),
                                  value, sizeof(value)))
    {
        return;
    }
    char domain[96] = {0};
    char path[96] = "/";
    uint32_t expires_at = 0U;
    bool secure = false;
    const char *domain_key = strstr(header, "Domain=");
    if (domain_key != NULL)
    {
        const char *domain_start = domain_key + 7;
        if (*domain_start == '.')
        {
            ++domain_start;
        }
        (void)snprintf(domain, sizeof(domain), "%s", domain_start);
        char *end = strchr(domain, ';');
        if (end != NULL)
        {
            *end = '\0';
        }
    }
    const char *path_key = strstr(header, "Path=");
    if (path_key != NULL)
    {
        (void)snprintf(path, sizeof(path), "%s", path_key + 5);
        char *end = strchr(path, ';');
        if (end != NULL)
        {
            *end = '\0';
        }
    }
    const char *max_age = strstr(header, "Max-Age=");
    if (max_age != NULL)
    {
        expires_at = (uint32_t)time(NULL) +
                     (uint32_t)strtoul(max_age + 8, NULL, 10);
    }
    if (strstr(header, "Secure") != NULL)
    {
        secure = true;
    }
    _cookie_set(name, value, domain[0] == '\0' ? host : domain,
                path, expires_at, secure);
}

static void _cookie_header(char *output, size_t output_size,
                           const char *host, const char *request_path)
{
    output[0] = '\0';
    size_t used = 0U;
    for (size_t index = 0U; index < s_cookie_count; ++index)
    {
        if (!_cookie_domain_matches(&s_cookies[index], host) ||
                (s_cookies[index].path[0] != '\0' &&
                 strncmp(request_path, s_cookies[index].path,
                         strlen(s_cookies[index].path)) != 0) ||
                (s_cookies[index].expires_at != 0U &&
                 (uint32_t)time(NULL) >= s_cookies[index].expires_at))
        {
            continue;
        }
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
                if (_cookie_lock_take())
                {
                    _cookie_from_header(event->header_value, context->host);
                    _cookie_lock_give();
                }
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
            return ESP_FAIL;
        }
        memcpy(context->result->body + context->result->length,
               event->data, (size_t)event->data_len);
        context->result->length += (size_t)event->data_len;
        context->result->body[context->result->length] = '\0';
    }
    return ESP_OK;
}

void music_http_result_free(music_http_result_t *result);

static esp_err_t _http_once(esp_http_client_method_t method, const char *url,
                            const char *body, const char *content_type,
                            const char *referer, const char *cookie_header,
                            bool with_cookie,
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
    char request_host[128];
    char request_path[128];
    _url_host_path(url, request_host, sizeof(request_host),
                   request_path, sizeof(request_path));
    (void)snprintf(context.host, sizeof(context.host), "%s", request_host);
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
    esp_err_t setup_error = esp_http_client_set_header(
                                client, "User-Agent",
                                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                "AppleWebKit/537.36 (KHTML, like Gecko) "
                                "Chrome/120.0.0.0 Safari/537.36");
    if (setup_error == ESP_OK)
    {
        setup_error = esp_http_client_set_header(
                          client, "Referer",
                          referer == NULL ? "https://y.qq.com/" : referer);
    }
    if (setup_error == ESP_OK)
    {
        setup_error = esp_http_client_set_header(client, "Accept", "*/*");
    }
    if (setup_error == ESP_OK &&
            cookie_header != NULL && cookie_header[0] != '\0')
    {
        setup_error = esp_http_client_set_header(client, "Cookie",
            cookie_header);
    }
    else if (setup_error == ESP_OK && with_cookie)
    {
        char *cookie = _psram_alloc(4096U);
        if (cookie == NULL)
        {
            setup_error = ESP_ERR_NO_MEM;
        }
        else if (!_cookie_lock_take())
        {
            setup_error = ESP_ERR_INVALID_STATE;
        }
        else
        {
            _cookie_header(cookie, 4096U, request_host, request_path);
            _cookie_lock_give();
            if (cookie[0] != '\0')
            {
                setup_error = esp_http_client_set_header(client, "Cookie",
                    cookie);
            }
        }
        _psram_free(cookie);
    }
    if (setup_error == ESP_OK && body != NULL)
    {
        setup_error = esp_http_client_set_header(
                          client, "Content-Type",
                          content_type == NULL ?
                          "application/json" : content_type);
        if (setup_error == ESP_OK)
        {
            setup_error = esp_http_client_set_post_field(
                              client, body, (int)strlen(body));
        }
    }
    if (setup_error != ESP_OK)
    {
        (void)esp_http_client_cleanup(client);
        _psram_free(result->body);
        result->body = NULL;
        return setup_error;
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
                               const char *referer, const char *cookie_header,
                               bool with_cookie,
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
                                     referer, cookie_header, with_cookie,
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
                music_http_result_free(result);
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
    music_http_result_free(result);
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t _http_get_referer(const char *url, bool with_cookie,
                                   const char *referer,
                                   music_http_result_t *result,
                                   size_t max_body)
{
    return _http_request(HTTP_METHOD_GET, url, NULL, NULL, referer, NULL,
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
    return _http_request(HTTP_METHOD_GET, url, NULL, NULL, referer, NULL,
                         with_cookie, result, 0U);
}

static esp_err_t _http_post(const char *url, const char *body,
                            music_http_result_t *result, size_t max_body)
{
    return _http_request(HTTP_METHOD_POST, url, body, "application/json",
                         NULL, NULL, true, result, max_body);
}

static esp_err_t _http_post_form_no_redirect(const char *url,
        const char *body, const char *referer,
        music_http_result_t *result, size_t max_body)
{
    return _http_once(HTTP_METHOD_POST, url, body,
                      "application/x-www-form-urlencoded", referer, NULL,
                      true, result, max_body);
}

void music_http_result_free(music_http_result_t *result)
{
    if (result != NULL && result->body != NULL)
    {
        _psram_free(result->body);
        result->body = NULL;
        result->length = 0U;
    }
}

bool music_http_cookie_get(const char *name, char *value, size_t value_size)
{
    if (!_cookie_lock_take())
    {
        return false;
    }
    const bool found = _cookie_get(name, value, value_size);
    _cookie_lock_give();
    return found;
}

void music_http_cookie_clear(void)
{
    /* Drain the active transfer before clearing. The HTTP lock is the request
     * fence; hold it until the cookie mutation completes. */
    if (_http_lock_take())
    {
        if (_cookie_lock_take())
        {
            _cookie_clear();
            _cookie_lock_give();
        }
        _http_lock_give();
    }
}

esp_err_t music_http_get_referer(const char *url, bool with_cookie,
                                 const char *referer,
                                 music_http_result_t *result, size_t max_body)
{
    if (!_http_lock_take())
    {
        if (result != NULL)
        {
            memset(result, 0, sizeof(*result));
        }
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = _http_get_referer(url, with_cookie, referer,
        result, max_body);
    _http_lock_give();
    return error;
}

esp_err_t music_http_get(const char *url, bool with_cookie,
                         music_http_result_t *result, size_t max_body)
{
    if (!_http_lock_take())
    {
        if (result != NULL)
        {
            memset(result, 0, sizeof(*result));
        }
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = _http_get(url, with_cookie, result, max_body);
    _http_lock_give();
    return error;
}

esp_err_t music_http_get_with_cookie_header(const char *url,
        const char *cookie_header,
        music_http_result_t *result, size_t max_body)
{
    if (!_http_lock_take())
    {
        if (result != NULL)
        {
            memset(result, 0, sizeof(*result));
        }
        return ESP_ERR_INVALID_STATE;
    }
    /* Keep the caller-supplied credential header pinned to the first host. */
    const esp_err_t error = _http_once(HTTP_METHOD_GET, url, NULL, NULL,
                                       NULL, cookie_header, false, result,
                                       max_body);
    _http_lock_give();
    return error;
}

esp_err_t music_http_get_headers(const char *url, bool with_cookie,
                                 const char *referer,
                                 music_http_result_t *result)
{
    if (!_http_lock_take())
    {
        if (result != NULL)
        {
            memset(result, 0, sizeof(*result));
        }
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = _http_get_headers(url, with_cookie, referer,
        result);
    _http_lock_give();
    return error;
}

esp_err_t music_http_post(const char *url, const char *body,
                          music_http_result_t *result, size_t max_body)
{
    if (!_http_lock_take())
    {
        if (result != NULL)
        {
            memset(result, 0, sizeof(*result));
        }
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = _http_post(url, body, result, max_body);
    _http_lock_give();
    return error;
}

esp_err_t music_http_post_form_no_redirect(const char *url,
        const char *body, const char *referer,
        music_http_result_t *result, size_t max_body)
{
    if (!_http_lock_take())
    {
        if (result != NULL)
        {
            memset(result, 0, sizeof(*result));
        }
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = _http_post_form_no_redirect(url, body, referer,
        result, max_body);
    _http_lock_give();
    return error;
}
