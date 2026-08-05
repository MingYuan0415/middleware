#define DBG_TAG "weather_http"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "weather_service_internal.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct weather_http_context
{
    uint8_t *body;
    size_t size;
    size_t limit;
    bool overflow;
} weather_http_context_t;

static atomic_uintptr_t s_active_client = ATOMIC_VAR_INIT((uintptr_t)NULL);
static atomic_uint s_cancel_readers = ATOMIC_VAR_INIT(0U);

static esp_err_t _weather_port_http_event(esp_http_client_event_t *event)
{
    weather_http_context_t *context = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
    {
        return ESP_OK;
    }
    size_t length = (size_t)event->data_len;
    if (length > context->limit - context->size)
    {
        context->overflow = true;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(context->body + context->size, event->data, length);
    context->size += length;
    return ESP_OK;
}

static void _weather_port_set_location_headers(
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
    (void)esp_http_client_set_header(client, "X-MT-Location-Latitude",
                                     latitude);
    (void)esp_http_client_set_header(client, "X-MT-Location-Longitude",
                                     longitude);
    (void)esp_http_client_set_header(client, "X-MT-Location-Provider",
                                     location->provider);
    if (location->city[0] != '\0')
    {
        (void)esp_http_client_set_header(client, "X-MT-Location-City",
                                         location->city);
    }
    if (location->region[0] != '\0')
    {
        (void)esp_http_client_set_header(client, "X-MT-Location-Region",
                                         location->region);
    }
    if (location->country[0] != '\0')
    {
        (void)esp_http_client_set_header(client, "X-MT-Location-Country",
                                         location->country);
    }
    if (location->timezone[0] != '\0')
    {
        (void)esp_http_client_set_header(client, "X-MT-Location-Timezone",
                                         location->timezone);
    }
}

static uint32_t _weather_port_retry_after(esp_http_client_handle_t client)
{
    char *header = NULL;
    if (esp_http_client_get_header(client, "Retry-After", &header) != ESP_OK ||
            header == NULL)
    {
        return 0U;
    }
    char *end = NULL;
    unsigned long seconds = strtoul(header, &end, 10);
    if (header[0] == '\0' || end == NULL || *end != '\0')
    {
        return 0U;
    }
    return seconds > 3600UL ? 3600U : (uint32_t)seconds;
}

esp_err_t weather_service_port_http_get(
    const char *url, const char *token,
    const weather_service_location_t *location, size_t response_limit,
    uint32_t timeout_ms, weather_service_http_result_t *result)
{
    if (url == NULL || result == NULL || response_limit == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
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
    char authorization[320] = {0};
    if (token != NULL && token[0] != '\0')
    {
        int count = snprintf(authorization, sizeof(authorization),
                             "Bearer %s", token);
        if (count < 0 || (size_t)count >= sizeof(authorization))
        {
            esp_http_client_cleanup(client);
            heap_caps_free(context.body);
            return ESP_ERR_INVALID_SIZE;
        }
        (void)esp_http_client_set_header(client, "Authorization",
                                         authorization);
    }
    (void)esp_http_client_set_header(client, "Accept", "application/json");
    (void)esp_http_client_set_header(client, "User-Agent",
                                     "MicroTech-weather/1");
    if (location != NULL)
    {
        _weather_port_set_location_headers(client, location);
    }
    atomic_store_explicit(&s_active_client, (uintptr_t)client,
                          memory_order_release);
    esp_err_t error = esp_http_client_perform(client);
    uintptr_t expected = (uintptr_t)client;
    (void)atomic_compare_exchange_strong_explicit(
        &s_active_client, &expected, (uintptr_t)NULL,
        memory_order_acq_rel, memory_order_acquire);
    while (atomic_load_explicit(&s_cancel_readers, memory_order_acquire) != 0U)
    {
        taskYIELD();
    }
    if (error == ESP_OK)
    {
        result->status_code = esp_http_client_get_status_code(client);
        result->retry_after_seconds = _weather_port_retry_after(client);
    }
    if (error == ESP_OK && context.overflow)
    {
        error = ESP_ERR_INVALID_SIZE;
    }
    if (error == ESP_OK && result->status_code == 200)
    {
        char *content_type = NULL;
        if (esp_http_client_get_header(client, "Content-Type", &content_type) !=
                ESP_OK || content_type == NULL ||
                strncmp(content_type, "application/json", 16U) != 0)
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
    memset(authorization, 0, sizeof(authorization));
    (void)esp_http_client_cleanup(client);
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

void weather_service_port_cancel(void)
{
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

uint32_t weather_service_port_random_u32(void)
{
    return esp_random();
}
