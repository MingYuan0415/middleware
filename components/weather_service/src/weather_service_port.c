#define DBG_TAG "weather_http"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "weather_service_internal.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define WEATHER_CONTENT_TYPE_MAX 63U
#define WEATHER_RETRY_AFTER_MAX  31U

typedef struct weather_http_context
{
    uint8_t *body;
    size_t size;
    size_t limit;
    unsigned data_events;
    char content_type[WEATHER_CONTENT_TYPE_MAX + 1U];
    char retry_after[WEATHER_RETRY_AFTER_MAX + 1U];
    bool overflow;
    bool content_type_seen;
    bool retry_after_seen;
    bool content_type_overflow;
    bool retry_after_overflow;
} weather_http_context_t;

static atomic_uintptr_t s_active_client = ATOMIC_VAR_INIT((uintptr_t)NULL);
static atomic_uint s_cancel_readers = ATOMIC_VAR_INIT(0U);
static atomic_ullong s_cancel_generation = ATOMIC_VAR_INIT(0U);

static void _weather_port_secure_clear(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (size > 0U)
    {
        *bytes = 0U;
        ++bytes;
        --size;
    }
}

static esp_err_t _weather_port_http_event(esp_http_client_event_t *event)
{
    weather_http_context_t *context = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED)
    {
        LOG_D("HTTP connected");
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
            event->header_key != NULL && event->header_value != NULL)
    {
        char *target = NULL;
        size_t capacity = 0U;
        bool *seen = NULL;
        bool *overflow = NULL;
        if (strcasecmp(event->header_key, "Content-Type") == 0)
        {
            target = context->content_type;
            capacity = sizeof(context->content_type);
            seen = &context->content_type_seen;
            overflow = &context->content_type_overflow;
        }
        else if (strcasecmp(event->header_key, "Retry-After") == 0)
        {
            target = context->retry_after;
            capacity = sizeof(context->retry_after);
            seen = &context->retry_after_seen;
            overflow = &context->retry_after_overflow;
        }
        if (target != NULL)
        {
            size_t length = strlen(event->header_value);
            *seen = true;
            if (length >= capacity)
            {
                *overflow = true;
                target[0] = '\0';
            }
            else
            {
                memcpy(target, event->header_value, length + 1U);
            }
            LOG_D("HTTP response header captured: %s",
                  target == context->content_type ? "content-type" :
                  "retry-after");
        }
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADERS_COMPLETE)
    {
        LOG_D("HTTP headers complete: content-type=%s retry-after=%s",
              context->content_type_seen ? "seen" : "missing",
              context->retry_after_seen ? "seen" : "missing");
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
    {
        if (event->event_id == HTTP_EVENT_ON_FINISH)
        {
            LOG_D("HTTP response complete: bytes=%u data_events=%u",
                  (unsigned)context->size, context->data_events);
        }
        else if (event->event_id == HTTP_EVENT_DISCONNECTED)
        {
            LOG_D("HTTP disconnected");
        }
        return ESP_OK;
    }
    size_t length = (size_t)event->data_len;
    if (context->data_events == 0U)
    {
        LOG_D("HTTP first response data: bytes=%u", (unsigned)length);
    }
    ++context->data_events;
    if (length > context->limit - context->size)
    {
        context->overflow = true;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(context->body + context->size, event->data, length);
    context->size += length;
    return ESP_OK;
}

static esp_err_t _weather_port_set_header(esp_http_client_handle_t client,
        const char *name, const char *value)
{
    return esp_http_client_set_header(client, name, value);
}

static esp_err_t _weather_port_set_location_headers(
    esp_http_client_handle_t client,
    const weather_service_location_t *location)
{
    char latitude[16];
    char longitude[16];
    int latitude_value = location->latitude_tenths;
    int longitude_value = location->longitude_tenths;
    (void)snprintf(latitude, sizeof(latitude), "%s%d.%d",
                   latitude_value < 0 ? "-" : "",
                   abs(latitude_value) / 10, abs(latitude_value) % 10);
    (void)snprintf(longitude, sizeof(longitude), "%s%d.%d",
                   longitude_value < 0 ? "-" : "",
                   abs(longitude_value) / 10, abs(longitude_value) % 10);
    esp_err_t result = _weather_port_set_header(
                           client, "X-MT-Location-Latitude", latitude);
    if (result != ESP_OK)
    {
        return result;
    }
    result = _weather_port_set_header(
                 client, "X-MT-Location-Longitude", longitude);
    if (result != ESP_OK)
    {
        return result;
    }
    result = _weather_port_set_header(
                 client, "X-MT-Location-Provider", location->provider);
    if (result != ESP_OK)
    {
        return result;
    }
    if (location->city[0] != '\0')
    {
        result = _weather_port_set_header(
                     client, "X-MT-Location-City", location->city);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (location->region[0] != '\0')
    {
        result = _weather_port_set_header(
                     client, "X-MT-Location-Region", location->region);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (location->country[0] != '\0')
    {
        result = _weather_port_set_header(
                     client, "X-MT-Location-Country", location->country);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (location->timezone[0] != '\0')
    {
        result = _weather_port_set_header(
                     client, "X-MT-Location-Timezone", location->timezone);
    }
    return result;
}

static bool _weather_port_json_content_type(const weather_http_context_t *context)
{
    static const char expected[] = "application/json";
    if (!context->content_type_seen || context->content_type_overflow)
    {
        return false;
    }
    const char *value = context->content_type;
    while (*value == ' ' || *value == '\t')
    {
        ++value;
    }
    if (strncasecmp(value, expected, sizeof(expected) - 1U) != 0)
    {
        return false;
    }
    value += sizeof(expected) - 1U;
    while (*value == ' ' || *value == '\t')
    {
        ++value;
    }
    return *value == '\0' || *value == ';';
}

static uint32_t _weather_port_retry_after(
    const weather_http_context_t *context)
{
    if (!context->retry_after_seen || context->retry_after_overflow ||
            context->retry_after[0] == '\0')
    {
        return 0U;
    }
    uint32_t seconds = 0U;
    for (const char *value = context->retry_after; *value != '\0'; ++value)
    {
        if (*value < '0' || *value > '9')
        {
            return 0U;
        }
        if (seconds < 3600U)
        {
            uint32_t digit = (uint32_t)(*value - '0');
            seconds = seconds > 360U ? 3600U : seconds * 10U + digit;
            if (seconds > 3600U)
            {
                seconds = 3600U;
            }
        }
    }
    return seconds;
}

esp_err_t weather_service_port_http_get(
    const char *url, const char *token,
    const weather_service_location_t *location, size_t response_limit,
    uint32_t timeout_ms, uint64_t cancel_generation,
    weather_service_http_result_t *result)
{
    if (url == NULL || result == NULL || response_limit == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    if (cancel_generation != weather_service_port_cancel_generation())
    {
        return ESP_ERR_INVALID_STATE;
    }
    weather_http_context_t context =
    {
        .limit = response_limit,
    };
    context.body = heap_caps_malloc(response_limit + 1U,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (context.body == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    const esp_http_client_config_t config =
    {
        .url = url,
        .event_handler = _weather_port_http_event,
        .user_data = &context,
        .timeout_ms = (int)timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        heap_caps_free(context.body);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = ESP_OK;
    bool active = false;
    char authorization[320] = {0};
    if (token != NULL && token[0] != '\0')
    {
        int count = snprintf(authorization, sizeof(authorization),
                             "Bearer %s", token);
        if (count < 0 || (size_t)count >= sizeof(authorization))
        {
            error = ESP_ERR_INVALID_SIZE;
            goto exit;
        }
        error = _weather_port_set_header(client, "Authorization",
                                         authorization);
        if (error != ESP_OK)
        {
            error = ESP_ERR_INVALID_ARG;
            goto exit;
        }
    }
    error = _weather_port_set_header(client, "Accept", "application/json");
    if (error != ESP_OK)
    {
        error = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    error = _weather_port_set_header(client, "User-Agent",
                                     "MicroTech-weather/1");
    if (error != ESP_OK)
    {
        error = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    if (location != NULL)
    {
        error = _weather_port_set_location_headers(client, location);
        if (error != ESP_OK)
        {
            error = ESP_ERR_INVALID_ARG;
            goto exit;
        }
    }
    LOG_D("HTTP request started: class=%s limit=%u timeout_ms=%u",
          location == NULL ? "location" : "weather",
          (unsigned)response_limit, (unsigned)timeout_ms);
    atomic_store_explicit(&s_active_client, (uintptr_t)client,
                          memory_order_release);
    active = true;
    if (cancel_generation != weather_service_port_cancel_generation())
    {
        error = ESP_ERR_INVALID_STATE;
    }
    else
    {
        error = esp_http_client_perform(client);
        if (cancel_generation != weather_service_port_cancel_generation())
        {
            error = ESP_ERR_INVALID_STATE;
        }
    }
    uintptr_t expected = (uintptr_t)client;
    (void)atomic_compare_exchange_strong_explicit(
        &s_active_client, &expected, (uintptr_t)NULL,
        memory_order_acq_rel, memory_order_acquire);
    while (atomic_load_explicit(&s_cancel_readers, memory_order_acquire) != 0U)
    {
        taskYIELD();
    }
    active = false;
    if (error == ESP_OK)
    {
        result->status_code = esp_http_client_get_status_code(client);
        result->retry_after_seconds = _weather_port_retry_after(&context);
    }
    LOG_D("HTTP perform returned: error=%s status=%d bytes=%u "
          "content-type=%s retry-after=%s overflow=%s",
          esp_err_to_name(error), result->status_code, (unsigned)context.size,
          context.content_type_seen ? "seen" : "missing",
          context.retry_after_seen ? "seen" : "missing",
          context.overflow ? "yes" : "no");
    if (error == ESP_OK && context.overflow)
    {
        error = ESP_ERR_INVALID_SIZE;
    }
    if (error == ESP_OK && result->status_code == 200)
    {
        if (!_weather_port_json_content_type(&context))
        {
            error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (error == ESP_OK)
    {
        context.body[context.size] = '\0';
        result->body = context.body;
        result->body_size = context.size;
        context.body = NULL;
    }

exit:
    if (active)
    {
        uintptr_t expected = (uintptr_t)client;
        (void)atomic_compare_exchange_strong_explicit(
            &s_active_client, &expected, (uintptr_t)NULL,
            memory_order_acq_rel, memory_order_acquire);
        while (atomic_load_explicit(&s_cancel_readers,
                                    memory_order_acquire) != 0U)
        {
            taskYIELD();
        }
    }
    _weather_port_secure_clear(authorization, sizeof(authorization));
    esp_err_t cleanup_result = esp_http_client_cleanup(client);
    if (cleanup_result != ESP_OK)
    {
        LOG_W("HTTP client cleanup failed: %s",
              esp_err_to_name(cleanup_result));
        if (error == ESP_OK)
        {
            error = cleanup_result;
            heap_caps_free(result->body);
            memset(result, 0, sizeof(*result));
        }
    }
    heap_caps_free(context.body);
    return error;
}

void weather_service_port_http_result_release(
    weather_service_http_result_t *result)
{
    if (result != NULL)
    {
        heap_caps_free(result->body);
        memset(result, 0, sizeof(*result));
    }
}

uint64_t weather_service_port_cancel_generation(void)
{
    return atomic_load_explicit(&s_cancel_generation, memory_order_acquire);
}

void weather_service_port_cancel(void)
{
    atomic_fetch_add_explicit(&s_cancel_generation, 1U, memory_order_acq_rel);
    atomic_fetch_add_explicit(&s_cancel_readers, 1U, memory_order_acq_rel);
    uintptr_t value = atomic_load_explicit(&s_active_client,
                                           memory_order_acquire);
    if (value != (uintptr_t)NULL)
    {
        (void)esp_http_client_cancel_request((esp_http_client_handle_t)value);
    }
    atomic_fetch_sub_explicit(&s_cancel_readers, 1U, memory_order_release);
}

int64_t weather_service_port_now_seconds(void)
{
    return (int64_t)time(NULL);
}

int64_t weather_service_port_now_milliseconds(void)
{
    return esp_timer_get_time() / 1000;
}

uint32_t weather_service_port_random_u32(void)
{
    return esp_random();
}

void *weather_service_port_psram_calloc(size_t count, size_t size)
{
    return heap_caps_calloc(count, size,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void weather_service_port_psram_free(void *memory)
{
    heap_caps_free(memory);
}
