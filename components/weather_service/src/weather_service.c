#define DBG_TAG "weather_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "weather_service.h"
#include "weather_service_internal.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

EVENT_BUS_DEFINE_ID(WEATHER_SERVICE_MSG);

#define WEATHER_CMD_NETWORK (UINT32_C(1) << 0)
#define WEATHER_CMD_REFRESH (UINT32_C(1) << 1)
#define WEATHER_CMD_SUSPEND (UINT32_C(1) << 2)
#define WEATHER_CMD_RESUME  (UINT32_C(1) << 3)
#define WEATHER_CMD_STOP    (UINT32_C(1) << 4)

#define WEATHER_EVENT_PAUSED  (UINT32_C(1) << 0)
#define WEATHER_EVENT_STOPPED (UINT32_C(1) << 1)

#define WEATHER_LOCATION_MAX_AGE_SECONDS (30 * 24 * 60 * 60)
#define WEATHER_CACHE_WRITE_MIN_SECONDS   (5 * 60)
#define WEATHER_HTTP_TIMEOUT_MS           12000U
#define WEATHER_LOCATION_TIMEOUT_MS       8000U
#define WEATHER_LOCATION_RESPONSE_LIMIT   (8U * 1024U)

#define WEATHER_CURRENT_MAX_STALE_SECONDS (6 * 60 * 60)
#define WEATHER_ALERTS_MAX_STALE_SECONDS  (1 * 60 * 60)
#define WEATHER_HOURLY_MAX_STALE_SECONDS  (12 * 60 * 60)
#define WEATHER_DAILY_MAX_STALE_SECONDS   (48 * 60 * 60)

typedef struct weather_snapshot_node
{
    atomic_uint references;
    weather_service_snapshot_t snapshot;
} weather_snapshot_node_t;

typedef struct weather_runtime_config
{
    char server_base_url[192];
    char device_token[256];
    char location_url[192];
    char cache_directory[128];
    uint32_t task_priority;
    uint32_t refresh_seconds[WEATHER_SERVICE_KIND_COUNT];
    uint32_t manual_refresh_min_seconds;
    bool allow_private_http;
    bool configured;
} weather_runtime_config_t;

typedef struct weather_service_context
{
    weather_runtime_config_t config;
    SemaphoreHandle_t mutex;
    EventGroupHandle_t events;
    TaskHandle_t worker;
    weather_snapshot_node_t *current;
    weather_service_status_snapshot_t status;
    uint64_t network_session;
    uint64_t attempted_location_session;
    uint64_t cache_sequence;
    int64_t next_due[WEATHER_SERVICE_KIND_COUNT];
    int64_t retry_due[WEATHER_SERVICE_KIND_COUNT];
    int64_t account_retry_due;
    uint8_t retry_attempt[WEATHER_SERVICE_KIND_COUNT];
    int64_t last_manual_refresh;
    int64_t last_cache_write;
    uint32_t ipv4_address;
    bool initialized;
    bool network_ready;
    bool suspended;
    bool stopping;
    bool force_refresh;
    bool cache_dirty;
} weather_service_context_t;

static weather_service_context_t s_weather;

static TickType_t _weather_timeout_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == WEATHER_SERVICE_WAIT_FOREVER)
    {
        return portMAX_DELAY;
    }
    uint64_t ticks = ((uint64_t)timeout_ms * configTICK_RATE_HZ + 999U) /
                     1000U;
    if (timeout_ms > 0U && ticks == 0U)
    {
        ticks = 1U;
    }
    if (ticks >= portMAX_DELAY)
    {
        ticks = portMAX_DELAY - 1U;
    }
    return (TickType_t)ticks;
}

static weather_snapshot_node_t *_weather_node_new(
    const weather_service_snapshot_t *source)
{
    weather_snapshot_node_t *node = heap_caps_calloc(
                                        1U, sizeof(*node),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (node != NULL)
    {
        atomic_init(&node->references, 1U);
        if (source != NULL)
        {
            node->snapshot = *source;
        }
    }
    return node;
}

static void _weather_node_release(weather_snapshot_node_t *node)
{
    if (node != NULL &&
            atomic_fetch_sub_explicit(&node->references, 1U,
                                      memory_order_acq_rel) == 1U)
    {
        heap_caps_free(node);
    }
}

static weather_snapshot_node_t *_weather_node_from_snapshot(
    const weather_service_snapshot_t *snapshot)
{
    return (weather_snapshot_node_t *)((uint8_t *)snapshot -
                                       offsetof(weather_snapshot_node_t,
                                               snapshot));
}

static bool _weather_status_set(weather_service_state_t state,
                                weather_service_failure_t failure,
                                uint32_t retry_after_seconds)
{
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    bool changed = s_weather.status.state != state ||
                   s_weather.status.failure != failure ||
                   s_weather.status.retry_after_seconds != retry_after_seconds;
    s_weather.status.state = state;
    s_weather.status.failure = failure;
    s_weather.status.retry_after_seconds = retry_after_seconds;
    s_weather.status.network_ready = s_weather.network_ready;
    if (s_weather.current != NULL)
    {
        s_weather.status.generation =
            s_weather.current->snapshot.generation;
        s_weather.status.available_mask =
            s_weather.current->snapshot.available_mask;
        s_weather.status.location_reused =
            s_weather.current->snapshot.location.reused;
    }
    xSemaphoreGive(s_weather.mutex);
    return changed;
}

static void _weather_publish(uint32_t changed_mask)
{
    weather_service_event_t event;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    event = (weather_service_event_t)
    {
        .generation = s_weather.status.generation,
        .changed_mask = changed_mask,
        .state = s_weather.status.state,
        .failure = s_weather.status.failure,
    };
    xSemaphoreGive(s_weather.mutex);
    esp_err_t result = event_bus_publish(
                           WEATHER_SERVICE_MSG,
                           WEATHER_SERVICE_MSG_SUB_TYPE_SNAPSHOT,
                           &event, sizeof(event),
                           EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
    if (result != ESP_OK)
    {
        LOG_W("snapshot event failed: %s", esp_err_to_name(result));
    }
}

static void _weather_swap(weather_snapshot_node_t *node,
                          uint32_t changed_mask)
{
    weather_snapshot_node_t *previous;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    previous = s_weather.current;
    node->snapshot.generation = previous == NULL ? 1U :
                                previous->snapshot.generation + 1U;
    s_weather.current = node;
    s_weather.status.generation = node->snapshot.generation;
    s_weather.status.available_mask = node->snapshot.available_mask;
    s_weather.status.location_reused = node->snapshot.location.reused;
    s_weather.cache_dirty = true;
    xSemaphoreGive(s_weather.mutex);
    _weather_node_release(previous);
    _weather_publish(changed_mask);
}

static weather_snapshot_node_t *_weather_clone_current(void)
{
    weather_snapshot_node_t *node = NULL;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (s_weather.current != NULL)
    {
        node = _weather_node_new(&s_weather.current->snapshot);
    }
    else
    {
        node = _weather_node_new(NULL);
    }
    xSemaphoreGive(s_weather.mutex);
    return node;
}

static bool _weather_private_ipv4_url(const char *url)
{
    unsigned first = 0U;
    unsigned second = 0U;
    unsigned third = 0U;
    unsigned fourth = 0U;
    int consumed = 0;
    if (sscanf(url, "http://%u.%u.%u.%u%n", &first, &second, &third,
               &fourth, &consumed) != 4 || first > 255U || second > 255U ||
            third > 255U || fourth > 255U ||
            (url[consumed] != '\0' && url[consumed] != ':' &&
             url[consumed] != '/'))
    {
        return false;
    }
    return first == 10U || (first == 172U && second >= 16U && second <= 31U) ||
           (first == 192U && second == 168U);
}

static bool _weather_url_has_valid_authority(const char *authority,
        bool allow_path)
{
    if (authority == NULL || authority[0] == '\0' ||
            strchr(authority, '@') != NULL)
    {
        return false;
    }
    const char *path = strchr(authority, '/');
    if (path != NULL && (!allow_path || path == authority))
    {
        return false;
    }
    size_t authority_length = path == NULL ? strlen(authority) :
                              (size_t)(path - authority);
    if (authority_length == 0U)
    {
        return false;
    }
    for (const char *position = authority; *position != '\0'; ++position)
    {
        unsigned char value = (unsigned char) * position;
        if (value <= 0x20U || value == 0x7FU || value == '\\')
        {
            return false;
        }
    }
    return true;
}

static bool _weather_valid_base_url(const char *url, bool allow_private_http)
{
    size_t length = strlen(url);
    if (length == 0U || length >= sizeof(s_weather.config.server_base_url) ||
            strchr(url, '?') != NULL || strchr(url, '#') != NULL)
    {
        return false;
    }
    if (strncmp(url, "https://", 8U) == 0)
    {
        const char *authority = url + 8U;
        const char *path = strchr(authority, '/');
        return _weather_url_has_valid_authority(authority, true) &&
               (path == NULL || strcmp(path, "/") == 0);
    }
    return allow_private_http && _weather_private_ipv4_url(url);
}

static esp_err_t _weather_copy_config(const weather_service_config_t *source,
                                      weather_runtime_config_t *target)
{
    if (source == NULL || target == NULL || source->location_url == NULL ||
            source->cache_directory == NULL || source->task_priority == 0U ||
            source->current_refresh_seconds == 0U ||
            source->alerts_refresh_seconds == 0U ||
            source->hourly_refresh_seconds == 0U ||
            source->daily_refresh_seconds == 0U ||
            source->manual_refresh_min_seconds == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(target, 0, sizeof(*target));
    if (strlen(source->location_url) >= sizeof(target->location_url) ||
            strlen(source->cache_directory) >= sizeof(target->cache_directory))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(target->location_url, source->location_url,
           strlen(source->location_url) + 1U);
    memcpy(target->cache_directory, source->cache_directory,
           strlen(source->cache_directory) + 1U);
    if (strncmp(target->location_url, "https://", 8U) != 0 ||
            strchr(target->location_url, '?') != NULL ||
            strchr(target->location_url, '#') != NULL ||
            !_weather_url_has_valid_authority(target->location_url + 8U,
                    true))
    {
        return ESP_ERR_INVALID_ARG;
    }
    target->task_priority = source->task_priority;
    target->refresh_seconds[WEATHER_SERVICE_KIND_CURRENT] =
        source->current_refresh_seconds;
    target->refresh_seconds[WEATHER_SERVICE_KIND_ALERTS] =
        source->alerts_refresh_seconds;
    target->refresh_seconds[WEATHER_SERVICE_KIND_HOURLY] =
        source->hourly_refresh_seconds;
    target->refresh_seconds[WEATHER_SERVICE_KIND_DAILY] =
        source->daily_refresh_seconds;
    target->manual_refresh_min_seconds = source->manual_refresh_min_seconds;
    target->allow_private_http = source->allow_private_http;
    if (source->server_base_url == NULL || source->device_token == NULL ||
            source->server_base_url[0] == '\0' ||
            source->device_token[0] == '\0')
    {
        target->configured = false;
        return ESP_OK;
    }
    if (!_weather_valid_base_url(source->server_base_url,
                                 source->allow_private_http) ||
            strlen(source->device_token) >= sizeof(target->device_token))
    {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(target->server_base_url, source->server_base_url,
           strlen(source->server_base_url) + 1U);
    memcpy(target->device_token, source->device_token,
           strlen(source->device_token) + 1U);
    target->configured = true;
    return ESP_OK;
}

static const char *_weather_kind_path(weather_service_kind_t kind)
{
    static const char *const paths[WEATHER_SERVICE_KIND_COUNT] =
    {
        "/api/v1/weather/current",
        "/api/v1/weather/alerts",
        "/api/v1/weather/hourly",
        "/api/v1/weather/daily",
    };
    return paths[kind];
}

static size_t _weather_kind_limit(weather_service_kind_t kind)
{
    static const size_t limits[WEATHER_SERVICE_KIND_COUNT] =
    {
        16U * 1024U,
        256U * 1024U,
        64U * 1024U,
        64U * 1024U,
    };
    return limits[kind] < CONFIG_WEATHER_SERVICE_MAX_RESPONSE_BYTES ?
           limits[kind] : CONFIG_WEATHER_SERVICE_MAX_RESPONSE_BYTES;
}

static uint32_t _weather_retry_delay(uint8_t attempt)
{
    static const uint32_t seconds[] = {5U, 15U, 60U, 300U};
    size_t index = attempt < sizeof(seconds) / sizeof(seconds[0]) ? attempt :
                   sizeof(seconds) / sizeof(seconds[0]) - 1U;
    uint32_t base = seconds[index];
    uint32_t spread = base / 5U;
    uint32_t random = weather_service_port_random_u32();
    return base - spread + (spread == 0U ? 0U :
                            random % (spread * 2U + 1U));
}

static void _weather_schedule_failure(weather_service_kind_t kind,
                                      int64_t now, uint32_t retry_after)
{
    uint32_t delay = retry_after;
    if (delay == 0U)
    {
        delay = _weather_retry_delay(s_weather.retry_attempt[kind]);
        if (s_weather.retry_attempt[kind] < UINT8_MAX)
        {
            ++s_weather.retry_attempt[kind];
        }
    }
    s_weather.retry_due[kind] = now + delay;
}

static bool _weather_should_stop_cycle(void)
{
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    bool stop = s_weather.suspended || s_weather.stopping;
    xSemaphoreGive(s_weather.mutex);
    return stop;
}

static void _weather_mark_expired(weather_service_snapshot_t *snapshot,
                                  int64_t now)
{
    weather_service_dataset_meta_t *metadata[WEATHER_SERVICE_KIND_COUNT] =
    {
        &snapshot->current.meta,
        &snapshot->alerts.meta,
        &snapshot->hourly.meta,
        &snapshot->daily.meta,
    };
    static const int64_t maximum_age[WEATHER_SERVICE_KIND_COUNT] =
    {
        WEATHER_CURRENT_MAX_STALE_SECONDS,
        WEATHER_ALERTS_MAX_STALE_SECONDS,
        WEATHER_HOURLY_MAX_STALE_SECONDS,
        WEATHER_DAILY_MAX_STALE_SECONDS,
    };
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        weather_service_dataset_meta_t *meta = metadata[kind];
        meta->expired = meta->available && now > 0 &&
                        meta->fetched_at.epoch_seconds > 0 &&
                        now - meta->fetched_at.epoch_seconds >
                        maximum_age[kind];
    }
}

static bool _weather_transport_retryable(esp_err_t error)
{
    return error != ESP_OK && error != ESP_ERR_INVALID_ARG &&
           error != ESP_ERR_INVALID_SIZE &&
           error != ESP_ERR_INVALID_RESPONSE && error != ESP_ERR_NO_MEM;
}

static esp_err_t _weather_locate(weather_service_location_t *location)
{
    esp_err_t result = ESP_FAIL;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt)
    {
        weather_service_http_result_t response;
        result = weather_service_port_http_get(
                     s_weather.config.location_url, NULL, NULL,
                     WEATHER_LOCATION_RESPONSE_LIMIT,
                     WEATHER_LOCATION_TIMEOUT_MS, &response);
        bool retryable = _weather_transport_retryable(result) ||
                         (result == ESP_OK && response.status_code >= 500);
        if (result == ESP_OK)
        {
            if (response.status_code == 200)
            {
                result = weather_service_parse_location(
                             response.body, response.body_size,
                             weather_service_port_now_seconds(), location);
            }
            else
            {
                result = ESP_ERR_INVALID_RESPONSE;
            }
            weather_service_port_http_result_release(&response);
        }
        if (!retryable || attempt != 0U || _weather_should_stop_cycle())
        {
            break;
        }
    }
    return result;
}

static bool _weather_location_usable(const weather_service_location_t *location,
                                     int64_t now)
{
    return location->available && location->acquired_at > 0 && now > 0 &&
           now - location->acquired_at <= WEATHER_LOCATION_MAX_AGE_SECONDS;
}

static esp_err_t _weather_prepare_location(weather_snapshot_node_t **staging)
{
    uint64_t session;
    int64_t now = weather_service_port_now_seconds();
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    session = s_weather.network_session;
    bool attempted = s_weather.attempted_location_session == session;
    xSemaphoreGive(s_weather.mutex);
    if (attempted)
    {
        return _weather_location_usable(&(*staging)->snapshot.location, now) ?
               ESP_OK : ESP_ERR_NOT_FOUND;
    }
    _weather_status_set(WEATHER_SERVICE_STATE_LOCATING,
                        WEATHER_SERVICE_FAILURE_NONE, 0U);
    _weather_publish(0U);
    weather_service_location_t location = {0};
    esp_err_t result = _weather_locate(&location);
    now = weather_service_port_now_seconds();
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    s_weather.attempted_location_session = session;
    xSemaphoreGive(s_weather.mutex);
    if (result != ESP_OK)
    {
        if ((*staging)->snapshot.location.available &&
                _weather_location_usable(&(*staging)->snapshot.location, now))
        {
            (*staging)->snapshot.location.reused = true;
            _weather_status_set(WEATHER_SERVICE_STATE_DEGRADED,
                                WEATHER_SERVICE_FAILURE_LOCATION, 0U);
            return ESP_OK;
        }
        _weather_status_set(WEATHER_SERVICE_STATE_ERROR,
                            WEATHER_SERVICE_FAILURE_LOCATION, 0U);
        _weather_publish(0U);
        return result;
    }
    bool different = !(*staging)->snapshot.location.available ||
                     (*staging)->snapshot.location.latitude_tenths !=
                     location.latitude_tenths ||
                     (*staging)->snapshot.location.longitude_tenths !=
                     location.longitude_tenths;
    if (different)
    {
        weather_snapshot_node_t *replacement = _weather_node_new(NULL);
        if (replacement == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        replacement->snapshot.location = location;
        replacement->snapshot.available_mask = WEATHER_SERVICE_DATA_LOCATION;
        _weather_node_release(*staging);
        *staging = replacement;
    }
    else
    {
        (*staging)->snapshot.location = location;
        (*staging)->snapshot.available_mask |= WEATHER_SERVICE_DATA_LOCATION;
    }
    return ESP_OK;
}

static weather_service_failure_t _weather_http_failure(int status)
{
    if (status == 401 || status == 403)
    {
        return WEATHER_SERVICE_FAILURE_AUTHENTICATION;
    }
    if (status == 429)
    {
        return WEATHER_SERVICE_FAILURE_RATE_LIMITED;
    }
    if (status >= 500)
    {
        return WEATHER_SERVICE_FAILURE_UPSTREAM;
    }
    return WEATHER_SERVICE_FAILURE_RESPONSE;
}

static weather_service_fetch_result_t _weather_fetch_once(
    weather_service_kind_t kind, weather_snapshot_node_t *node,
    uint32_t *changed_mask)
{
    weather_service_fetch_result_t fetch = {0};
    char url[256];
    size_t base_length = strlen(s_weather.config.server_base_url);
    const char *path = _weather_kind_path(kind);
    bool slash = base_length > 0U &&
                 s_weather.config.server_base_url[base_length - 1U] == '/';
    int count = snprintf(url, sizeof(url), "%s%s%s",
                         s_weather.config.server_base_url,
                         slash ? "" : "/", path[0] == '/' ? path + 1 : path);
    if (count < 0 || (size_t)count >= sizeof(url))
    {
        fetch.error = ESP_ERR_INVALID_SIZE;
        return fetch;
    }
    weather_service_http_result_t response;
    esp_err_t result = weather_service_port_http_get(
                           url, s_weather.config.device_token,
                           &node->snapshot.location,
                           _weather_kind_limit(kind), WEATHER_HTTP_TIMEOUT_MS,
                           &response);
    fetch.status_code = response.status_code;
    fetch.retry_after_seconds = response.retry_after_seconds;
    if (result != ESP_OK)
    {
        fetch.error = result;
        weather_service_port_http_result_release(&response);
        return fetch;
    }
    if (response.status_code == 200)
    {
        fetch.error = weather_service_parse_weather(
                          kind, response.body, response.body_size,
                          &node->snapshot, changed_mask);
    }
    weather_service_port_http_result_release(&response);
    return fetch;
}

static weather_service_fetch_result_t _weather_fetch(
    weather_service_kind_t kind, weather_snapshot_node_t *node,
    uint32_t *changed_mask)
{
    weather_service_fetch_result_t fetch = {0};
    for (unsigned attempt = 0U; attempt < 2U; ++attempt)
    {
        fetch = _weather_fetch_once(kind, node, changed_mask);
        bool retryable = _weather_transport_retryable(fetch.error) ||
                         (fetch.error == ESP_OK && fetch.status_code >= 500);
        if (!retryable || attempt != 0U || _weather_should_stop_cycle())
        {
            break;
        }
    }
    return fetch;
}

static bool _weather_kind_due(weather_service_kind_t kind, int64_t now,
                              bool force)
{
    if (s_weather.account_retry_due > now || s_weather.retry_due[kind] > now)
    {
        return false;
    }
    if (force)
    {
        return true;
    }
    return s_weather.next_due[kind] == 0 || s_weather.next_due[kind] <= now;
}

static void _weather_store_cache_if_due(int64_t now, bool force)
{
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    bool due = s_weather.cache_dirty &&
               (force || s_weather.last_cache_write == 0 ||
                now - s_weather.last_cache_write >=
                WEATHER_CACHE_WRITE_MIN_SECONDS);
    weather_snapshot_node_t *node = s_weather.current;
    if (due && node != NULL)
    {
        atomic_fetch_add_explicit(&node->references, 1U, memory_order_relaxed);
    }
    uint64_t sequence = s_weather.cache_sequence + 1U;
    xSemaphoreGive(s_weather.mutex);
    if (!due || node == NULL)
    {
        return;
    }
    esp_err_t result = weather_service_cache_store(
                           s_weather.config.cache_directory,
                           &node->snapshot, sequence);
    if (result == ESP_OK)
    {
        xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
        s_weather.cache_sequence = sequence;
        s_weather.last_cache_write = now;
        s_weather.cache_dirty = false;
        xSemaphoreGive(s_weather.mutex);
    }
    else
    {
        LOG_W("cache write failed: %s", esp_err_to_name(result));
    }
    _weather_node_release(node);
}

static void _weather_run_cycle(bool force)
{
    int64_t now = weather_service_port_now_seconds();
    weather_service_state_t previous_state;
    weather_service_failure_t previous_failure;
    uint32_t previous_retry_after;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    previous_state = s_weather.status.state;
    previous_failure = s_weather.status.failure;
    previous_retry_after = s_weather.status.retry_after_seconds;
    xSemaphoreGive(s_weather.mutex);
    weather_snapshot_node_t *staging = _weather_clone_current();
    if (staging == NULL)
    {
        _weather_status_set(WEATHER_SERVICE_STATE_ERROR,
                            WEATHER_SERVICE_FAILURE_INTERNAL, 0U);
        return;
    }
    if (_weather_prepare_location(&staging) != ESP_OK)
    {
        _weather_node_release(staging);
        return;
    }
    _weather_mark_expired(&staging->snapshot, now);
    bool new_location = false;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (s_weather.current == NULL ||
            s_weather.current->snapshot.location.latitude_tenths !=
            staging->snapshot.location.latitude_tenths ||
            s_weather.current->snapshot.location.longitude_tenths !=
            staging->snapshot.location.longitude_tenths)
    {
        new_location = true;
    }
    xSemaphoreGive(s_weather.mutex);
    bool any_due = false;
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        if (_weather_kind_due(kind, now, force || new_location))
        {
            any_due = true;
            break;
        }
    }
    if (!any_due)
    {
        weather_service_state_t idle_state =
            staging->snapshot.location.reused ?
            WEATHER_SERVICE_STATE_DEGRADED : WEATHER_SERVICE_STATE_READY;
        weather_service_failure_t idle_failure =
            staging->snapshot.location.reused ?
            WEATHER_SERVICE_FAILURE_LOCATION : WEATHER_SERVICE_FAILURE_NONE;
        uint32_t idle_retry_after = 0U;
        if (previous_state == WEATHER_SERVICE_STATE_AUTH_ERROR)
        {
            idle_state = previous_state;
            idle_failure = previous_failure;
            idle_retry_after = previous_retry_after;
        }
        else if (s_weather.account_retry_due > now)
        {
            idle_state = WEATHER_SERVICE_STATE_RATE_LIMITED;
            idle_failure = WEATHER_SERVICE_FAILURE_RATE_LIMITED;
            idle_retry_after = (uint32_t)(s_weather.account_retry_due - now);
        }
        else
        {
            for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
                    kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
            {
                if (s_weather.retry_due[kind] > now)
                {
                    idle_state = WEATHER_SERVICE_STATE_DEGRADED;
                    idle_failure = previous_failure;
                    break;
                }
            }
        }
        _weather_node_release(staging);
        if (_weather_status_set(idle_state, idle_failure, idle_retry_after))
        {
            _weather_publish(0U);
        }
        return;
    }
    _weather_status_set(WEATHER_SERVICE_STATE_UPDATING,
                        staging->snapshot.location.reused ?
                        WEATHER_SERVICE_FAILURE_LOCATION :
                        WEATHER_SERVICE_FAILURE_NONE, 0U);
    _weather_publish(0U);
    bool any_success = false;
    bool cycle_failed = false;
    bool attempted_fetch = false;
    weather_service_state_t final_state =
        staging->snapshot.location.reused ? WEATHER_SERVICE_STATE_DEGRADED :
        WEATHER_SERVICE_STATE_READY;
    weather_service_failure_t final_failure =
        staging->snapshot.location.reused ? WEATHER_SERVICE_FAILURE_LOCATION :
        WEATHER_SERVICE_FAILURE_NONE;
    uint32_t final_retry_after = 0U;
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        if (_weather_should_stop_cycle())
        {
            break;
        }
        if (!_weather_kind_due(kind, now, force || new_location))
        {
            continue;
        }
        attempted_fetch = true;
        uint32_t changed_mask = 0U;
        weather_service_fetch_result_t fetch = _weather_fetch(
                kind, staging,
                &changed_mask);
        if (fetch.error == ESP_OK && fetch.status_code == 200)
        {
            s_weather.retry_attempt[kind] = 0U;
            s_weather.retry_due[kind] = 0;
            s_weather.next_due[kind] = now +
                                       s_weather.config.refresh_seconds[kind];
            any_success = true;
            weather_snapshot_node_t *published = _weather_node_new(
                    &staging->snapshot);
            if (published != NULL &&
                    (!new_location || kind == WEATHER_SERVICE_KIND_CURRENT ||
                     (published->snapshot.available_mask &
                      WEATHER_SERVICE_DATA_CURRENT) != 0U))
            {
                _weather_swap(published, changed_mask);
                new_location = false;
            }
            else
            {
                _weather_node_release(published);
            }
            continue;
        }
        cycle_failed = true;
        int status = fetch.status_code;
        weather_service_failure_t failure = status == 0 ?
                                            WEATHER_SERVICE_FAILURE_NETWORK :
                                            _weather_http_failure(status);
        if (fetch.error != ESP_OK && status == 200)
        {
            failure = WEATHER_SERVICE_FAILURE_RESPONSE;
        }
        if (failure == WEATHER_SERVICE_FAILURE_AUTHENTICATION)
        {
            final_state = WEATHER_SERVICE_STATE_AUTH_ERROR;
            final_failure = failure;
            s_weather.next_due[kind] = INT64_MAX;
            break;
        }
        if (failure == WEATHER_SERVICE_FAILURE_RATE_LIMITED)
        {
            fetch.retry_after_seconds = fetch.retry_after_seconds == 0U ?
                                        60U : fetch.retry_after_seconds;
            final_state = WEATHER_SERVICE_STATE_RATE_LIMITED;
            final_failure = failure;
            final_retry_after = fetch.retry_after_seconds;
            s_weather.account_retry_due = now + fetch.retry_after_seconds;
        }
        else
        {
            final_state = WEATHER_SERVICE_STATE_DEGRADED;
            final_failure = failure;
            final_retry_after = 0U;
        }
        _weather_schedule_failure(kind, now, fetch.retry_after_seconds);
        if (failure == WEATHER_SERVICE_FAILURE_RATE_LIMITED ||
                (new_location && kind == WEATHER_SERVICE_KIND_CURRENT))
        {
            break;
        }
    }
    _weather_node_release(staging);
    if (_weather_should_stop_cycle())
    {
        return;
    }
    if (any_success)
    {
        _weather_store_cache_if_due(now, force);
    }
    if (!cycle_failed && attempted_fetch)
    {
        final_state = final_failure == WEATHER_SERVICE_FAILURE_LOCATION ?
                      WEATHER_SERVICE_STATE_DEGRADED :
                      WEATHER_SERVICE_STATE_READY;
    }
    if (_weather_status_set(final_state, final_failure, final_retry_after))
    {
        _weather_publish(0U);
    }
}

static void _weather_worker(void *context)
{
    (void)context;
    uint32_t command = 0U;
    while (true)
    {
        (void)xTaskNotifyWait(0U, UINT32_MAX, &command,
                              pdMS_TO_TICKS(1000U));
        if ((command & WEATHER_CMD_STOP) != 0U)
        {
            break;
        }
        if ((command & WEATHER_CMD_SUSPEND) != 0U)
        {
            xEventGroupSetBits(s_weather.events, WEATHER_EVENT_PAUSED);
        }
        xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
        bool suspended = s_weather.suspended;
        xSemaphoreGive(s_weather.mutex);
        if (suspended)
        {
            command = 0U;
            continue;
        }
        bool configured;
        bool network_ready;
        bool force;
        xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
        configured = s_weather.config.configured;
        network_ready = s_weather.network_ready;
        force = s_weather.force_refresh;
        s_weather.force_refresh = false;
        xSemaphoreGive(s_weather.mutex);
        if (!configured)
        {
            _weather_status_set(WEATHER_SERVICE_STATE_UNCONFIGURED,
                                WEATHER_SERVICE_FAILURE_NOT_CONFIGURED, 0U);
        }
        else if (!network_ready)
        {
            _weather_status_set(WEATHER_SERVICE_STATE_WAITING_NETWORK,
                                WEATHER_SERVICE_FAILURE_NETWORK, 0U);
        }
        else
        {
            _weather_run_cycle(force);
        }
        command = 0U;
    }
    _weather_store_cache_if_due(weather_service_port_now_seconds(), true);
    xEventGroupSetBits(s_weather.events, WEATHER_EVENT_STOPPED);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t weather_service_init(const weather_service_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_weather.initialized)
    {
        return ESP_OK;
    }
    weather_runtime_config_t copied;
    esp_err_t result = _weather_copy_config(config, &copied);
    if (result != ESP_OK)
    {
        return result;
    }
    memset(&s_weather, 0, sizeof(s_weather));
    s_weather.config = copied;
    weather_service_parse_init();
    s_weather.mutex = xSemaphoreCreateMutex();
    s_weather.events = xEventGroupCreate();
    if (s_weather.mutex == NULL || s_weather.events == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    s_weather.status = (weather_service_status_snapshot_t)
    {
        .state = copied.configured ? WEATHER_SERVICE_STATE_WAITING_NETWORK :
                 WEATHER_SERVICE_STATE_UNCONFIGURED,
                 .failure = copied.configured ? WEATHER_SERVICE_FAILURE_NETWORK :
                            WEATHER_SERVICE_FAILURE_NOT_CONFIGURED,
                            .initialized = true,
                            .configured = copied.configured,
    };
    if (copied.configured)
    {
        weather_snapshot_node_t *cached = _weather_node_new(NULL);
        if (cached == NULL)
        {
            result = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        uint64_t sequence = 0U;
        result = weather_service_cache_load(copied.cache_directory,
                                            &cached->snapshot, &sequence);
        if (result == ESP_OK)
        {
            _weather_mark_expired(&cached->snapshot,
                                  weather_service_port_now_seconds());
            cached->snapshot.location.reused = true;
            s_weather.current = cached;
            s_weather.cache_sequence = sequence;
            s_weather.status.generation = cached->snapshot.generation;
            s_weather.status.available_mask = cached->snapshot.available_mask;
            s_weather.status.location_reused = true;
        }
        else
        {
            _weather_node_release(cached);
            result = ESP_OK;
        }
    }
    s_weather.initialized = true;
    if (xTaskCreatePinnedToCoreWithCaps(
                _weather_worker, "weather_worker",
                CONFIG_WEATHER_SERVICE_TASK_STACK_SIZE, NULL,
                copied.task_priority, &s_weather.worker,
                CONFIG_MAIN_PROJECT_TASK_CORE_ID,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        s_weather.initialized = false;
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    LOG_I("worker started, configured=%s", copied.configured ? "yes" : "no");
    return ESP_OK;

cleanup:
    _weather_node_release(s_weather.current);
    if (s_weather.events != NULL)
    {
        vEventGroupDelete(s_weather.events);
    }
    if (s_weather.mutex != NULL)
    {
        vSemaphoreDelete(s_weather.mutex);
    }
    memset(&s_weather, 0, sizeof(s_weather));
    return result;
}

esp_err_t weather_service_deinit(uint32_t timeout_ms)
{
    if (!s_weather.initialized)
    {
        return ESP_OK;
    }
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    s_weather.stopping = true;
    xSemaphoreGive(s_weather.mutex);
    weather_service_port_cancel();
    xTaskNotify(s_weather.worker, WEATHER_CMD_STOP, eSetBits);
    EventBits_t bits = xEventGroupWaitBits(
                           s_weather.events, WEATHER_EVENT_STOPPED,
                           pdFALSE, pdTRUE, _weather_timeout_ticks(timeout_ms));
    if ((bits & WEATHER_EVENT_STOPPED) == 0U)
    {
        return ESP_ERR_TIMEOUT;
    }
    weather_snapshot_node_t *current = s_weather.current;
    s_weather.current = NULL;
    _weather_node_release(current);
    vEventGroupDelete(s_weather.events);
    vSemaphoreDelete(s_weather.mutex);
    memset(s_weather.config.device_token, 0,
           sizeof(s_weather.config.device_token));
    memset(&s_weather, 0, sizeof(s_weather));
    return ESP_OK;
}

esp_err_t weather_service_suspend(uint32_t timeout_ms)
{
    if (!s_weather.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    s_weather.suspended = true;
    xSemaphoreGive(s_weather.mutex);
    weather_service_port_cancel();
    xEventGroupClearBits(s_weather.events, WEATHER_EVENT_PAUSED);
    xTaskNotify(s_weather.worker, WEATHER_CMD_SUSPEND, eSetBits);
    EventBits_t bits = xEventGroupWaitBits(
                           s_weather.events, WEATHER_EVENT_PAUSED,
                           pdFALSE, pdTRUE, _weather_timeout_ticks(timeout_ms));
    if ((bits & WEATHER_EVENT_PAUSED) == 0U)
    {
        return ESP_ERR_TIMEOUT;
    }
    _weather_status_set(WEATHER_SERVICE_STATE_SUSPENDED,
                        WEATHER_SERVICE_FAILURE_NONE, 0U);
    return ESP_OK;
}

esp_err_t weather_service_resume(uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!s_weather.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    s_weather.suspended = false;
    xSemaphoreGive(s_weather.mutex);
    xEventGroupClearBits(s_weather.events, WEATHER_EVENT_PAUSED);
    xTaskNotify(s_weather.worker, WEATHER_CMD_RESUME, eSetBits);
    return ESP_OK;
}

esp_err_t weather_service_set_network_ready(bool ready,
        uint32_t ipv4_address)
{
    if (!s_weather.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    bool notify = false;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (ready && !s_weather.network_ready)
    {
        ++s_weather.network_session;
        notify = true;
    }
    else if (!ready && s_weather.network_ready)
    {
        notify = true;
    }
    s_weather.network_ready = ready;
    s_weather.ipv4_address = ready ? ipv4_address : 0U;
    s_weather.status.network_ready = ready;
    xSemaphoreGive(s_weather.mutex);
    if (notify)
    {
        xTaskNotify(s_weather.worker, WEATHER_CMD_NETWORK, eSetBits);
    }
    return ESP_OK;
}

esp_err_t weather_service_request_refresh(void)
{
    if (!s_weather.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t now = weather_service_port_now_seconds();
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (s_weather.last_manual_refresh != 0 &&
            now - s_weather.last_manual_refresh <
            s_weather.config.manual_refresh_min_seconds)
    {
        xSemaphoreGive(s_weather.mutex);
        return ESP_ERR_TIMEOUT;
    }
    s_weather.last_manual_refresh = now;
    s_weather.force_refresh = true;
    xSemaphoreGive(s_weather.mutex);
    xTaskNotify(s_weather.worker, WEATHER_CMD_REFRESH, eSetBits);
    return ESP_OK;
}

esp_err_t weather_service_get_status(
    weather_service_status_snapshot_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_weather.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    *status = s_weather.status;
    xSemaphoreGive(s_weather.mutex);
    return ESP_OK;
}

esp_err_t weather_service_snapshot_acquire(
    const weather_service_snapshot_t **snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_weather.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    weather_snapshot_node_t *node = s_weather.current;
    if (node != NULL)
    {
        atomic_fetch_add_explicit(&node->references, 1U, memory_order_relaxed);
        *snapshot = &node->snapshot;
    }
    xSemaphoreGive(s_weather.mutex);
    return node == NULL ? ESP_ERR_NOT_FOUND : ESP_OK;
}

void weather_service_snapshot_release(
    const weather_service_snapshot_t *snapshot)
{
    if (snapshot != NULL)
    {
        _weather_node_release(_weather_node_from_snapshot(snapshot));
    }
}

bool weather_service_is_available(void)
{
    return s_weather.initialized;
}
