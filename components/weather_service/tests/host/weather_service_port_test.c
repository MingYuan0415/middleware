#include "weather_service_internal.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/task.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct weather_host_http_response
{
    const char *content_type_key;
    const char *content_type;
    const char *retry_after_key;
    const char *retry_after;
    const uint8_t *body;
    size_t body_size;
    int status_code;
    esp_err_t perform_error;
    unsigned eagain_count;
} weather_host_http_response_t;

struct weather_host_http_client
{
    esp_http_client_config_t config;
    int status_code;
    unsigned perform_calls;
    bool cancel_requested;
};

typedef struct weather_port_thread_result
{
    esp_err_t error;
    weather_service_http_result_t response;
} weather_port_thread_result_t;

static const uint8_t s_json_body[] = "{\"ok\":true}";
static pthread_mutex_t s_http_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_http_ready = PTHREAD_COND_INITIALIZER;
static weather_host_http_response_t s_response;
static bool s_perform_entered;
static unsigned s_cleanup_count;
static unsigned s_psram_allocations;
static unsigned s_header_count;
static unsigned s_header_failure;
static unsigned s_perform_count;
static esp_http_client_config_t s_last_client_config;
static bool s_header_block;
static bool s_header_entered;
static bool s_perform_block;
static bool s_perform_returned;
static bool s_cleanup_before_cancel_release;
static bool s_fail_body_allocation;
static esp_err_t s_cancel_result;
static bool s_cancel_block;
static bool s_cancel_entered;
static pthread_mutex_t s_cancel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cancel_ready = PTHREAD_COND_INITIALIZER;

static int64_t s_timer_us;

int64_t esp_timer_get_time(void)
{
    return s_timer_us;
}

void vTaskDelay(TickType_t ticks)
{
    s_timer_us += (int64_t)ticks * INT64_C(1000);
}

static void _host_response_reset(void)
{
    pthread_mutex_lock(&s_http_lock);
    s_response = (weather_host_http_response_t)
    {
        .content_type_key = "Content-Type",
        .content_type = "application/json",
        .body = s_json_body,
        .body_size = sizeof(s_json_body) - 1U,
        .status_code = 200,
    };
    s_perform_entered = false;
    s_cleanup_count = 0U;
    s_psram_allocations = 0U;
    s_header_count = 0U;
    s_header_failure = 0U;
    s_perform_count = 0U;
    memset(&s_last_client_config, 0, sizeof(s_last_client_config));
    s_header_block = false;
    s_header_entered = false;
    s_perform_block = false;
    s_perform_returned = false;
    s_cleanup_before_cancel_release = false;
    s_fail_body_allocation = false;
    s_cancel_result = ESP_OK;
    s_cancel_block = false;
    s_cancel_entered = false;
    s_timer_us = INT64_C(1234567000);
    pthread_mutex_unlock(&s_http_lock);
}

static esp_err_t _host_emit_header(esp_http_client_handle_t client,
                                   const char *key, const char *value)
{
    if (key == NULL || value == NULL)
    {
        return ESP_OK;
    }
    esp_http_client_event_t event =
    {
        .event_id = HTTP_EVENT_ON_HEADER,
        .client = client,
        .user_data = client->config.user_data,
        .header_key = (char *)key,
        .header_value = (char *)value,
    };
    return client->config.event_handler(&event);
}

static esp_err_t _host_emit_body(esp_http_client_handle_t client,
                                 const uint8_t *body, size_t body_size)
{
    if (body_size == 0U)
    {
        return ESP_OK;
    }
    esp_http_client_event_t event =
    {
        .event_id = HTTP_EVENT_ON_DATA,
        .client = client,
        .data = (void *)body,
        .data_len = (int)body_size,
        .user_data = client->config.user_data,
    };
    return client->config.event_handler(&event);
}

static esp_err_t _host_get_with_timeout(
    size_t response_limit, uint32_t timeout_ms,
    weather_service_http_result_t *result)
{
    return weather_service_port_http_get(
               "https://weather.example.com/current", "test-token",
               response_limit, timeout_ms,
               weather_service_port_cancel_generation(), result);
}

static esp_err_t _host_get_http(size_t response_limit,
                                weather_service_http_result_t *result)
{
    return weather_service_port_http_get(
               "http://192.168.1.2:8080/current", "test-token",
               response_limit, 1000U,
               weather_service_port_cancel_generation(), result);
}

static esp_err_t _host_get(size_t response_limit,
                           weather_service_http_result_t *result)
{
    return _host_get_with_timeout(response_limit, 1000U, result);
}

static void _test_content_type(void)
{
    static const char *const accepted[] =
    {
        "application/json",
        "Application/JSON",
        " application/json ; charset=utf-8",
        "application/json;charset=utf-8",
    };
    for (size_t index = 0U;
            index < sizeof(accepted) / sizeof(accepted[0]); ++index)
    {
        _host_response_reset();
        s_response.content_type_key = "cOnTeNt-TyPe";
        s_response.content_type = accepted[index];
        weather_service_http_result_t result;
        assert(_host_get(128U, &result) == ESP_OK);
        assert(result.status_code == 200);
        assert(result.body_size == sizeof(s_json_body) - 1U);
        assert(memcmp(result.body, s_json_body, result.body_size) == 0);
        weather_service_port_http_result_release(&result);
        assert(s_cleanup_count == 1U);
        assert(s_psram_allocations == 1U);
    }

    _host_response_reset();
    s_response.content_type = NULL;
    weather_service_http_result_t result;
    assert(_host_get(128U, &result) == ESP_ERR_INVALID_RESPONSE);
    assert(result.body == NULL);

    _host_response_reset();
    s_response.content_type = "text/plain";
    assert(_host_get(128U, &result) == ESP_ERR_INVALID_RESPONSE);
    assert(result.body == NULL);
}

static void _test_retry_after(void)
{
    _host_response_reset();
    s_response.status_code = 429;
    s_response.retry_after_key = "rEtRy-AfTeR";
    s_response.retry_after = "75";
    weather_service_http_result_t result;
    assert(_host_get(128U, &result) == ESP_OK);
    assert(result.status_code == 429);
    assert(result.retry_after_seconds == 75U);
    weather_service_port_http_result_release(&result);

    _host_response_reset();
    s_response.status_code = 429;
    s_response.retry_after_key = "Retry-After";
    s_response.retry_after = "75s";
    assert(_host_get(128U, &result) == ESP_OK);
    assert(result.retry_after_seconds == 0U);
    weather_service_port_http_result_release(&result);

    _host_response_reset();
    s_response.status_code = 429;
    s_response.retry_after_key = "Retry-After";
    s_response.retry_after = "9999999999999999999999999999999";
    assert(_host_get(128U, &result) == ESP_OK);
    assert(result.retry_after_seconds == 3600U);
    weather_service_port_http_result_release(&result);
}

static void _test_response_overflow(void)
{
    _host_response_reset();
    weather_service_http_result_t result;
    assert(_host_get(4U, &result) == ESP_ERR_INVALID_SIZE);
    assert(result.body == NULL);
    assert(s_cleanup_count == 1U);
}

static void *_host_blocked_request(void *context)
{
    weather_port_thread_result_t *thread_result = context;
    thread_result->error = _host_get(128U, &thread_result->response);
    return NULL;
}

static void _test_cancel_cleanup(void)
{
    _host_response_reset();
    s_response.eagain_count = UINT32_MAX;
    s_perform_block = true;
    weather_port_thread_result_t thread_result = {0};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, _host_blocked_request,
                          &thread_result) == 0);
    pthread_mutex_lock(&s_http_lock);
    while (!s_perform_entered)
    {
        pthread_cond_wait(&s_http_ready, &s_http_lock);
    }
    pthread_mutex_unlock(&s_http_lock);
    weather_service_port_cancel();
    assert(pthread_join(thread, NULL) == 0);
    assert(thread_result.error == ESP_ERR_INVALID_STATE);
    assert(thread_result.response.body == NULL);
    assert(s_cleanup_count == 1U);

    _host_response_reset();
    weather_service_http_result_t retry_result;
    assert(_host_get(128U, &retry_result) == ESP_OK);
    weather_service_port_http_result_release(&retry_result);
}

static void *_host_cancel_request(void *context)
{
    (void)context;
    weather_service_port_cancel();
    return NULL;
}

static void _test_cancel_cleanup_handshake(void)
{
    _host_response_reset();
    pthread_mutex_lock(&s_http_lock);
    s_perform_block = true;
    pthread_mutex_unlock(&s_http_lock);
    pthread_mutex_lock(&s_cancel_lock);
    s_cancel_block = true;
    pthread_mutex_unlock(&s_cancel_lock);

    weather_port_thread_result_t request_result = {0};
    pthread_t request_thread;
    assert(pthread_create(&request_thread, NULL, _host_blocked_request,
                          &request_result) == 0);
    pthread_mutex_lock(&s_http_lock);
    while (!s_perform_entered)
    {
        pthread_cond_wait(&s_http_ready, &s_http_lock);
    }
    pthread_mutex_unlock(&s_http_lock);

    pthread_t cancel_thread;
    assert(pthread_create(&cancel_thread, NULL, _host_cancel_request,
                          NULL) == 0);
    pthread_mutex_lock(&s_cancel_lock);
    while (!s_cancel_entered)
    {
        pthread_cond_wait(&s_cancel_ready, &s_cancel_lock);
    }
    pthread_mutex_unlock(&s_cancel_lock);

    /* Let perform return and enter cleanup while cancellation still owns its
     * handle reference. Cleanup must not free the client in this window. */
    pthread_mutex_lock(&s_http_lock);
    s_perform_block = false;
    pthread_cond_broadcast(&s_http_ready);
    while (!s_perform_returned)
    {
        pthread_cond_wait(&s_http_ready, &s_http_lock);
    }
    pthread_mutex_unlock(&s_http_lock);
    pthread_mutex_lock(&s_cancel_lock);
    assert(!s_cleanup_before_cancel_release);
    s_cancel_block = false;
    pthread_cond_broadcast(&s_cancel_ready);
    pthread_mutex_unlock(&s_cancel_lock);

    assert(pthread_join(cancel_thread, NULL) == 0);
    assert(pthread_join(request_thread, NULL) == 0);
    assert(request_result.error == ESP_ERR_INVALID_STATE);
    assert(request_result.response.body == NULL);
    assert(!s_cleanup_before_cancel_release);
    assert(s_cleanup_count == 1U);
}

static void _test_cancel_invalid_state(void)
{
    _host_response_reset();
    pthread_mutex_lock(&s_http_lock);
    s_perform_block = true;
    pthread_mutex_unlock(&s_http_lock);
    pthread_mutex_lock(&s_cancel_lock);
    s_cancel_result = ESP_ERR_INVALID_STATE;
    pthread_mutex_unlock(&s_cancel_lock);

    weather_port_thread_result_t request_result = {0};
    pthread_t request_thread;
    assert(pthread_create(&request_thread, NULL, _host_blocked_request,
                          &request_result) == 0);
    pthread_mutex_lock(&s_http_lock);
    while (!s_perform_entered)
    {
        pthread_cond_wait(&s_http_ready, &s_http_lock);
    }
    pthread_mutex_unlock(&s_http_lock);
    weather_service_port_cancel();

    pthread_mutex_lock(&s_http_lock);
    s_perform_block = false;
    pthread_cond_broadcast(&s_http_ready);
    pthread_mutex_unlock(&s_http_lock);
    assert(pthread_join(request_thread, NULL) == 0);
    assert(request_result.error == ESP_ERR_INVALID_STATE);
    assert(s_cleanup_count == 1U);
}

static void _test_allocation_failure_result_initialized(void)
{
    _host_response_reset();
    s_fail_body_allocation = true;
    weather_service_http_result_t result =
    {
        .body = (uint8_t *)(uintptr_t)UINTPTR_MAX,
        .body_size = SIZE_MAX,
        .status_code = -1,
        .retry_after_seconds = UINT32_MAX,
    };
    assert(_host_get(128U, &result) == ESP_ERR_NO_MEM);
    assert(result.body == NULL);
    assert(result.body_size == 0U);
    assert(result.status_code == 0);
    assert(result.retry_after_seconds == 0U);
}

static void _test_header_failures(void)
{
    for (unsigned header = 1U; header <= 3U; ++header)
    {
        _host_response_reset();
        s_header_failure = header;
        weather_service_http_result_t result;
        assert(_host_get(128U, &result) == ESP_ERR_INVALID_ARG);
        assert(result.body == NULL);
        assert(s_header_count == header);
        assert(s_perform_count == 0U);
        assert(s_cleanup_count == 1U);
    }

    _host_response_reset();
    weather_service_http_result_t result;
    assert(_host_get(128U, &result) == ESP_OK);
    assert(s_header_count == 3U);
    weather_service_port_http_result_release(&result);
}

static void _test_stale_cancel_generation(void)
{
    _host_response_reset();
    uint64_t generation = weather_service_port_cancel_generation();
    weather_service_port_cancel();
    weather_service_http_result_t result;
    assert(weather_service_port_http_get(
               "https://weather.example.com/current", "test-token",
               128U, 1000U, generation, &result) == ESP_ERR_INVALID_STATE);
    assert(s_psram_allocations == 0U);
    assert(s_cleanup_count == 0U);
    assert(s_perform_count == 0U);
}

static void _test_cancel_before_active_publication(void)
{
    _host_response_reset();
    pthread_mutex_lock(&s_http_lock);
    s_header_block = true;
    pthread_mutex_unlock(&s_http_lock);
    weather_port_thread_result_t thread_result = {0};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, _host_blocked_request,
                          &thread_result) == 0);
    pthread_mutex_lock(&s_http_lock);
    while (!s_header_entered)
    {
        pthread_cond_wait(&s_http_ready, &s_http_lock);
    }
    pthread_mutex_unlock(&s_http_lock);
    weather_service_port_cancel();
    pthread_mutex_lock(&s_http_lock);
    s_header_block = false;
    pthread_cond_broadcast(&s_http_ready);
    pthread_mutex_unlock(&s_http_lock);
    assert(pthread_join(thread, NULL) == 0);
    assert(thread_result.error == ESP_ERR_INVALID_STATE);
    assert(thread_result.response.body == NULL);
    assert(s_perform_count == 0U);
    assert(s_cleanup_count == 1U);
}

static void _test_timeout_while_eagain(void)
{
    _host_response_reset();
    s_response.eagain_count = UINT32_MAX;
    weather_service_http_result_t result;
    assert(_host_get_with_timeout(128U, 25U, &result) == ESP_ERR_TIMEOUT);
    assert(result.body == NULL);
    assert(s_cleanup_count == 1U);
}

static void _test_transport_mode_by_scheme(void)
{
    _host_response_reset();
    weather_service_http_result_t result;
    assert(_host_get_with_timeout(128U, 1000U, &result) == ESP_OK);
    assert(s_perform_count == 1U);
    assert(s_last_client_config.is_async);
    weather_service_port_http_result_release(&result);

    _host_response_reset();
    assert(_host_get_http(128U, &result) == ESP_OK);
    assert(s_perform_count == 1U);
    assert(!s_last_client_config.is_async);
    weather_service_port_http_result_release(&result);
}

esp_http_client_handle_t esp_http_client_init(
    const esp_http_client_config_t *config)
{
    struct weather_host_http_client *client = calloc(1U, sizeof(*client));
    if (client != NULL)
    {
        client->config = *config;
        pthread_mutex_lock(&s_http_lock);
        s_last_client_config = *config;
        pthread_mutex_unlock(&s_http_lock);
        client->status_code = s_response.status_code;
    }
    return client;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                     const char *key, const char *value)
{
    if (client == NULL || key == NULL || value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_http_lock);
    if (s_header_block)
    {
        s_header_entered = true;
        pthread_cond_broadcast(&s_http_ready);
        while (s_header_block)
        {
            pthread_cond_wait(&s_http_ready, &s_http_lock);
        }
    }
    pthread_mutex_unlock(&s_http_lock);
    ++s_header_count;
    return s_header_failure == s_header_count ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    ++s_perform_count;
    ++client->perform_calls;
    pthread_mutex_lock(&s_http_lock);
    s_perform_entered = true;
    pthread_cond_broadcast(&s_http_ready);
    while (s_perform_block && !client->cancel_requested)
    {
        pthread_cond_wait(&s_http_ready, &s_http_lock);
    }
    if (client->cancel_requested)
    {
        pthread_mutex_unlock(&s_http_lock);
        return ESP_FAIL;
    }
    if (s_response.eagain_count > 0U)
    {
        --s_response.eagain_count;
        pthread_mutex_unlock(&s_http_lock);
        return ESP_ERR_HTTP_EAGAIN;
    }
    weather_host_http_response_t response = s_response;
    s_perform_returned = true;
    pthread_cond_broadcast(&s_http_ready);
    pthread_mutex_unlock(&s_http_lock);
    if (response.perform_error != ESP_OK)
    {
        return response.perform_error;
    }
    esp_err_t result = _host_emit_header(client, response.content_type_key,
                                         response.content_type);
    if (result == ESP_OK)
    {
        result = _host_emit_header(client, response.retry_after_key,
                                   response.retry_after);
    }
    if (result == ESP_OK)
    {
        result = _host_emit_body(client, response.body, response.body_size);
    }
    return result;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    return client->status_code;
}

esp_err_t esp_http_client_cancel_request(esp_http_client_handle_t client)
{
    if (client == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_cancel_lock);
    s_cancel_entered = true;
    pthread_cond_broadcast(&s_cancel_ready);
    while (s_cancel_block)
    {
        pthread_cond_wait(&s_cancel_ready, &s_cancel_lock);
    }
    const esp_err_t result = s_cancel_result;
    pthread_mutex_unlock(&s_cancel_lock);
    pthread_mutex_lock(&s_http_lock);
    if (result == ESP_OK)
    {
        client->cancel_requested = true;
    }
    pthread_cond_broadcast(&s_http_ready);
    pthread_mutex_unlock(&s_http_lock);
    return result;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    pthread_mutex_lock(&s_cancel_lock);
    if (s_cancel_block)
    {
        s_cleanup_before_cancel_release = true;
    }
    ++s_cleanup_count;
    pthread_mutex_unlock(&s_cancel_lock);
    free(client);
    return ESP_OK;
}

esp_err_t esp_crt_bundle_attach(void *config)
{
    (void)config;
    return ESP_OK;
}

void *heap_caps_malloc(size_t size, unsigned capabilities)
{
    if ((capabilities & MALLOC_CAP_SPIRAM) != 0U)
    {
        ++s_psram_allocations;
        if (s_fail_body_allocation)
        {
            return NULL;
        }
    }
    return malloc(size);
}

void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities)
{
    if ((capabilities & MALLOC_CAP_SPIRAM) != 0U)
    {
        ++s_psram_allocations;
    }
    return calloc(count, size);
}

void heap_caps_free(void *memory)
{
    free(memory);
}

uint32_t esp_random(void)
{
    return 0U;
}

int main(void)
{
    _test_content_type();
    _test_retry_after();
    _test_response_overflow();
    _test_header_failures();
    _test_allocation_failure_result_initialized();
    _test_stale_cancel_generation();
    _test_cancel_before_active_publication();
    _test_cancel_cleanup();
    _test_cancel_cleanup_handshake();
    _test_cancel_invalid_state();
    _test_timeout_while_eagain();
    _test_transport_mode_by_scheme();
    puts("weather port host tests passed");
    return 0;
}
