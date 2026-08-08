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
#define WEATHER_CACHE_WRITE_MIN_MS        (INT64_C(5) * 60 * 1000)
#define WEATHER_HTTP_TIMEOUT_MS           12000U
#define WEATHER_LOCATION_TIMEOUT_MS       15000U
#define WEATHER_LOCATION_RESPONSE_LIMIT   (8U * 1024U)
#define WEATHER_LOCATION_STABILIZE_MIN_MS 2400U
#define WEATHER_LOCATION_STABILIZE_RANGE_MS 1200U
#define WEATHER_STACK_WARNING_BYTES       2048U

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
    uint64_t location_state_session;
    uint64_t located_session;
    uint64_t pending_scope_session;
    weather_service_location_t session_location;
    uint64_t cache_sequence;
    int64_t next_due_ms[WEATHER_SERVICE_KIND_COUNT];
    int64_t retry_due_ms[WEATHER_SERVICE_KIND_COUNT];
    int64_t account_retry_due_ms;
    int64_t location_retry_due_ms;
    int64_t internal_retry_due_ms;
    int64_t location_stabilize_due_ms;
    uint8_t retry_attempt[WEATHER_SERVICE_KIND_COUNT];
    uint8_t location_retry_attempt;
    uint8_t internal_retry_attempt;
    int64_t last_manual_refresh_ms;
    int64_t last_cache_write_ms;
    uint32_t ipv4_address;
    uint32_t minimum_stack_remaining;
    bool initialized;
    bool network_ready;
    bool suspended;
    bool stopping;
    bool force_refresh;
    bool cache_dirty;
    bool location_auth_error;
    bool pending_scope_attempted;
    uint8_t drift_attempt;
    int64_t expired_check_seconds;
    uint64_t expired_check_generation;
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

static esp_err_t _weather_swap(weather_snapshot_node_t *node,
                               uint32_t changed_mask, uint64_t session)
{
    weather_snapshot_node_t *previous = NULL;
    bool swapped = false;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (s_weather.network_ready && s_weather.network_session == session)
    {
        previous = s_weather.current;
        node->snapshot.generation = previous == NULL ? 1U :
                                    previous->snapshot.generation + 1U;
        s_weather.current = node;
        s_weather.status.generation = node->snapshot.generation;
        s_weather.status.available_mask = node->snapshot.available_mask;
        s_weather.status.location_reused = node->snapshot.location.reused;
        s_weather.cache_dirty = true;
        swapped = true;
    }
    xSemaphoreGive(s_weather.mutex);
    if (!swapped)
    {
        _weather_node_release(node);
        return ESP_ERR_INVALID_STATE;
    }
    _weather_node_release(previous);
    _weather_publish(changed_mask);
    return ESP_OK;
}

static esp_err_t _weather_commit_snapshot(
    const weather_service_snapshot_t *snapshot, uint32_t changed_mask,
    uint64_t session)
{
    weather_snapshot_node_t *node = _weather_node_new(snapshot);
    if (node == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    return _weather_swap(node, changed_mask, session);
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
            third > 255U || fourth > 255U)
    {
        return false;
    }
    if (!(first == 10U || (first == 172U && second >= 16U &&
                           second <= 31U) ||
            (first == 192U && second == 168U)))
    {
        return false;
    }
    if (url[consumed] == '\0')
    {
        return true;
    }
    if (url[consumed] != ':')
    {
        return false;
    }
    unsigned long port = 0UL;
    const char *digits = url + consumed + 1U;
    if (digits[0] == '\0')
    {
        return false;
    }
    for (const char *position = digits; *position != '\0'; ++position)
    {
        if (*position < '0' || *position > '9')
        {
            return false;
        }
        port = port * 10UL + (unsigned long)(*position - '0');
        if (port > 65535UL)
        {
            return false;
        }
    }
    return port != 0UL;
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
    if (source == NULL || target == NULL || source->cache_directory == NULL ||
            source->task_priority == 0U ||
            source->current_refresh_seconds == 0U ||
            source->alerts_refresh_seconds == 0U ||
            source->hourly_refresh_seconds == 0U ||
            source->daily_refresh_seconds == 0U ||
            source->manual_refresh_min_seconds == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(target, 0, sizeof(*target));
    if (strlen(source->cache_directory) >= sizeof(target->cache_directory))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(target->cache_directory, source->cache_directory,
           strlen(source->cache_directory) + 1U);
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

static esp_err_t _weather_endpoint_url(char *url, size_t url_size,
                                       const char *path)
{
    size_t base_length = strlen(s_weather.config.server_base_url);
    const char *separator = base_length > 0U &&
                            s_weather.config.server_base_url[base_length - 1U] ==
                            '/' ? "" : "/";
    int count = snprintf(url, url_size, "%s%s%s",
                         s_weather.config.server_base_url, separator,
                         path[0] == '/' ? path + 1 : path);
    return count < 0 || (size_t)count >= url_size ? ESP_ERR_INVALID_SIZE :
           ESP_OK;
}

static const char *_weather_kind_name(weather_service_kind_t kind)
{
    static const char *const names[WEATHER_SERVICE_KIND_COUNT] =
    {
        "current",
        "alerts",
        "hourly",
        "daily",
    };
    return names[kind];
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

static int64_t _weather_deadline_after(int64_t now_ms, uint32_t seconds)
{
    int64_t delay_ms = (int64_t)seconds * 1000;
    return now_ms > INT64_MAX - delay_ms ? INT64_MAX : now_ms + delay_ms;
}

static uint32_t _weather_deadline_remaining(int64_t deadline_ms,
        int64_t now_ms)
{
    if (deadline_ms <= now_ms)
    {
        return 0U;
    }
    uint64_t remaining_ms = (uint64_t)(deadline_ms - now_ms);
    uint64_t seconds = (remaining_ms + 999U) / 1000U;
    return seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

static uint32_t _weather_location_stabilize_delay_ms(void)
{
    return WEATHER_LOCATION_STABILIZE_MIN_MS +
           weather_service_port_random_u32() %
           (WEATHER_LOCATION_STABILIZE_RANGE_MS + 1U);
}

static void _weather_record_stack(const char *checkpoint)
{
#ifdef ESP_PLATFORM
    uint32_t remaining = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (remaining >= s_weather.minimum_stack_remaining)
    {
        return;
    }
    s_weather.minimum_stack_remaining = remaining;
    if (remaining < WEATHER_STACK_WARNING_BYTES)
    {
        LOG_W("worker stack low after %s: %u bytes remaining",
              checkpoint, (unsigned)remaining);
    }
    else
    {
        LOG_D("worker stack minimum after %s: %u bytes remaining",
              checkpoint, (unsigned)remaining);
    }
#else
    (void)checkpoint;
#endif
}

static void _weather_schedule_failure(weather_service_kind_t kind,
                                      int64_t now_ms, uint32_t retry_after)
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
    s_weather.retry_due_ms[kind] = _weather_deadline_after(now_ms, delay);
}

static void _weather_schedule_internal_failure(int64_t now_ms)
{
    uint32_t delay = _weather_retry_delay(s_weather.internal_retry_attempt);
    if (s_weather.internal_retry_attempt < UINT8_MAX)
    {
        ++s_weather.internal_retry_attempt;
    }
    s_weather.internal_retry_due_ms = _weather_deadline_after(now_ms, delay);
    if (_weather_status_set(WEATHER_SERVICE_STATE_ERROR,
                            WEATHER_SERVICE_FAILURE_INTERNAL, delay))
    {
        _weather_publish(0U);
    }
    LOG_W("internal weather failure; retry in %u seconds", (unsigned)delay);
}

static void _weather_clear_internal_failure(void)
{
    s_weather.internal_retry_due_ms = 0;
    s_weather.internal_retry_attempt = 0U;
}

static bool _weather_should_stop_cycle(void)
{
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    bool stop = s_weather.suspended || s_weather.stopping;
    xSemaphoreGive(s_weather.mutex);
    return stop;
}

static uint32_t _weather_mark_expired(weather_service_snapshot_t *snapshot,
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
    static const uint32_t masks[WEATHER_SERVICE_KIND_COUNT] =
    {
        WEATHER_SERVICE_DATA_CURRENT,
        WEATHER_SERVICE_DATA_ALERTS,
        WEATHER_SERVICE_DATA_HOURLY,
        WEATHER_SERVICE_DATA_DAILY,
    };
    uint32_t changed_mask = 0U;
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        weather_service_dataset_meta_t *meta = metadata[kind];
        bool expired = meta->available &&
                       (now <= 0 || meta->fetched_at.epoch_seconds <= 0 ||
                        now < meta->fetched_at.epoch_seconds ||
                        now - meta->fetched_at.epoch_seconds >
                        maximum_age[kind]);
        if (meta->expired != expired)
        {
            meta->expired = expired;
            changed_mask |= masks[kind];
        }
    }
    return changed_mask;
}

static bool _weather_transport_retryable(esp_err_t error)
{
    return error != ESP_OK && error != ESP_ERR_INVALID_ARG &&
           error != ESP_ERR_INVALID_SIZE &&
           error != ESP_ERR_INVALID_RESPONSE && error != ESP_ERR_NO_MEM &&
           error != ESP_ERR_INVALID_STATE;
}

static esp_err_t _weather_http_admit(uint64_t session,
                                     uint64_t *cancel_generation)
{
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    bool admitted = s_weather.network_ready &&
                    s_weather.network_session == session &&
                    !s_weather.suspended && !s_weather.stopping;
    if (admitted)
    {
        *cancel_generation = weather_service_port_cancel_generation();
    }
    xSemaphoreGive(s_weather.mutex);
    return admitted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t _weather_locate(uint64_t session,
                                 weather_service_location_t *location,
                                 weather_service_failure_t *failure)
{
    *failure = WEATHER_SERVICE_FAILURE_LOCATION;
    char url[256];
    esp_err_t result = _weather_endpoint_url(url, sizeof(url),
                       "/api/v1/location");
    if (result != ESP_OK)
    {
        return result;
    }
    for (unsigned attempt = 0U; attempt < 2U; ++attempt)
    {
        uint64_t cancel_generation = 0U;
        result = _weather_http_admit(session, &cancel_generation);
        if (result != ESP_OK)
        {
            break;
        }
        weather_service_http_result_t response;
        LOG_D("location HTTP attempt %u/2", attempt + 1U);
        result = weather_service_port_http_get(
                     url, s_weather.config.device_token,
                     WEATHER_LOCATION_RESPONSE_LIMIT,
                     WEATHER_LOCATION_TIMEOUT_MS, cancel_generation,
                     &response);
        bool retryable = _weather_transport_retryable(result) ||
                         (result == ESP_OK && response.status_code >= 500);
        if (result == ESP_OK)
        {
            LOG_D("location HTTP response: status=%d bytes=%u",
                  response.status_code, (unsigned)response.body_size);
            if (response.status_code == 200)
            {
                result = weather_service_parse_location(
                             response.body, response.body_size,
                             weather_service_port_now_seconds(), location);
            }
            else
            {
                if (response.status_code == 401 ||
                        response.status_code == 403)
                {
                    *failure = WEATHER_SERVICE_FAILURE_AUTHENTICATION;
                }
                result = ESP_ERR_INVALID_RESPONSE;
            }
            weather_service_port_http_result_release(&response);
        }
        else
        {
            LOG_W("location HTTP attempt %u failed: %s",
                  attempt + 1U, esp_err_to_name(result));
        }
        if (retryable && attempt == 0U)
        {
            LOG_D("location HTTP immediate retry admitted");
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
           now >= location->acquired_at &&
           now - location->acquired_at <= WEATHER_LOCATION_MAX_AGE_SECONDS;
}

static bool _weather_location_same_identity(
    const weather_service_location_t *left,
    const weather_service_location_t *right)
{
    if (left->location_key[0] != '\0' && right->location_key[0] != '\0')
    {
        return strcmp(left->location_key, right->location_key) == 0;
    }
    /* The district field is a localization artifact the server may add or
       drop independently of the grid, so it must not drive the display
       fallback comparison. */
    return strcmp(left->city, right->city) == 0 &&
           strcmp(left->region, right->region) == 0 &&
           strcmp(left->country, right->country) == 0 &&
           strcmp(left->timezone, right->timezone) == 0 &&
           strcmp(left->provider, right->provider) == 0;
}

static bool _weather_location_equal(const weather_service_location_t *left,
                                    const weather_service_location_t *right)
{
    return _weather_location_same_identity(left, right) &&
           left->acquired_at == right->acquired_at &&
           left->available == right->available &&
           left->reused == right->reused;
}

static bool _weather_session_active(uint64_t session)
{
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    bool active = s_weather.network_ready &&
                  s_weather.network_session == session;
    xSemaphoreGive(s_weather.mutex);
    return active;
}

static esp_err_t _weather_stage_location(weather_snapshot_node_t **staging,
        const weather_service_location_t *location, bool fresh_session_scope)
{
    bool different = fresh_session_scope ||
                     !(*staging)->snapshot.location.available ||
                     !_weather_location_same_identity(
                         &(*staging)->snapshot.location, location);
    if (different)
    {
        weather_snapshot_node_t *replacement = _weather_node_new(NULL);
        if (replacement == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        replacement->snapshot.location = *location;
        replacement->snapshot.available_mask = WEATHER_SERVICE_DATA_LOCATION;
        _weather_node_release(*staging);
        *staging = replacement;
    }
    else
    {
        (*staging)->snapshot.location = *location;
        (*staging)->snapshot.available_mask |= WEATHER_SERVICE_DATA_LOCATION;
    }
    return ESP_OK;
}

static esp_err_t _weather_use_location_fallback(
    weather_snapshot_node_t *staging, int64_t now, uint32_t retry_after,
    weather_service_failure_t failure)
{
    weather_service_state_t state = WEATHER_SERVICE_STATE_ERROR;
    if (_weather_location_usable(&staging->snapshot.location, now))
    {
        staging->snapshot.location.reused = true;
        state = WEATHER_SERVICE_STATE_DEGRADED;
    }
    if (_weather_status_set(state, failure, retry_after))
    {
        _weather_publish(0U);
    }
    return state == WEATHER_SERVICE_STATE_DEGRADED ? ESP_OK :
           ESP_ERR_NOT_FOUND;
}

static esp_err_t _weather_prepare_location(weather_snapshot_node_t **staging,
        bool force, uint64_t *cycle_session, bool *location_scope_changed)
{
    int64_t now = weather_service_port_now_seconds();
    int64_t monotonic_now_ms = weather_service_port_now_milliseconds();
    *location_scope_changed = false;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    uint64_t session = s_weather.network_session;
    bool ready = s_weather.network_ready;
    xSemaphoreGive(s_weather.mutex);
    *cycle_session = session;
    if (!ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_weather.location_state_session != session)
    {
        s_weather.location_state_session = session;
        s_weather.located_session = 0U;
        s_weather.pending_scope_session = 0U;
        s_weather.pending_scope_attempted = false;
        memset(&s_weather.session_location, 0,
               sizeof(s_weather.session_location));
        s_weather.location_retry_due_ms = 0;
        s_weather.location_retry_attempt = 0U;
        s_weather.location_stabilize_due_ms = monotonic_now_ms +
                                              _weather_location_stabilize_delay_ms();
        s_weather.location_auth_error = false;
        s_weather.drift_attempt = 0U;
        LOG_D("network location session started");
    }
    if (s_weather.located_session == session)
    {
        return _weather_stage_location(staging,
                                       &s_weather.session_location,
                                       s_weather.pending_scope_session ==
                                       session);
    }
    if (s_weather.location_auth_error)
    {
        if (!force)
        {
            if (_weather_status_set(WEATHER_SERVICE_STATE_AUTH_ERROR,
                                    WEATHER_SERVICE_FAILURE_AUTHENTICATION,
                                    0U))
            {
                _weather_publish(0U);
            }
            return ESP_ERR_NOT_FOUND;
        }
        s_weather.location_auth_error = false;
    }
    if (!force && s_weather.location_stabilize_due_ms > monotonic_now_ms)
    {
        uint64_t remaining_ms = (uint64_t)(
                                    s_weather.location_stabilize_due_ms -
                                    monotonic_now_ms);
        uint32_t remaining_seconds = (uint32_t)((remaining_ms + 999U) /
                                                1000U);
        if (_weather_status_set(WEATHER_SERVICE_STATE_LOCATING,
                                WEATHER_SERVICE_FAILURE_NONE,
                                remaining_seconds))
        {
            _weather_publish(0U);
        }
        return ESP_ERR_NOT_FINISHED;
    }
    if (!force && s_weather.location_retry_due_ms > monotonic_now_ms)
    {
        return _weather_use_location_fallback(
                   *staging, now,
                   _weather_deadline_remaining(
                       s_weather.location_retry_due_ms, monotonic_now_ms),
                   WEATHER_SERVICE_FAILURE_LOCATION);
    }
    _weather_status_set(WEATHER_SERVICE_STATE_LOCATING,
                        WEATHER_SERVICE_FAILURE_NONE, 0U);
    _weather_publish(0U);
    LOG_D("location attempt started");
    weather_service_location_t location = {0};
    weather_service_failure_t failure = WEATHER_SERVICE_FAILURE_LOCATION;
    esp_err_t result = _weather_locate(session, &location, &failure);
    _weather_record_stack("location");
    now = weather_service_port_now_seconds();
    monotonic_now_ms = weather_service_port_now_milliseconds();
    if (!_weather_session_active(session))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (result != ESP_OK)
    {
        if (result == ESP_ERR_NO_MEM || result == ESP_ERR_INVALID_ARG)
        {
            return result;
        }
        if (result == ESP_ERR_INVALID_STATE)
        {
            return result;
        }
        if (failure == WEATHER_SERVICE_FAILURE_AUTHENTICATION)
        {
            s_weather.location_auth_error = true;
            LOG_W("location authentication rejected");
            if (_weather_status_set(WEATHER_SERVICE_STATE_AUTH_ERROR,
                                    WEATHER_SERVICE_FAILURE_AUTHENTICATION,
                                    0U))
            {
                _weather_publish(0U);
            }
            return ESP_ERR_NOT_FOUND;
        }
        uint32_t delay = _weather_retry_delay(
                             s_weather.location_retry_attempt);
        if (s_weather.location_retry_attempt < UINT8_MAX)
        {
            ++s_weather.location_retry_attempt;
        }
        s_weather.location_retry_due_ms = _weather_deadline_after(
                                              monotonic_now_ms, delay);
        LOG_W("location attempt failed: %s; retry in %u seconds",
              esp_err_to_name(result), (unsigned)delay);
        return _weather_use_location_fallback(*staging, now, delay, failure);
    }
    s_weather.session_location = location;
    s_weather.located_session = session;
    s_weather.pending_scope_session = session;
    s_weather.pending_scope_attempted = false;
    s_weather.drift_attempt = 0U;
    s_weather.location_retry_due_ms = 0;
    s_weather.location_retry_attempt = 0U;
    s_weather.location_stabilize_due_ms = 0;
    LOG_D("location attempt succeeded");
    *location_scope_changed = true;
    return _weather_stage_location(staging, &location, true);
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
    uint32_t *changed_mask, uint64_t session)
{
    weather_service_fetch_result_t fetch = {0};
    char url[256];
    esp_err_t result = _weather_endpoint_url(url, sizeof(url),
                       _weather_kind_path(kind));
    if (result != ESP_OK)
    {
        fetch.error = result;
        return fetch;
    }
    uint64_t cancel_generation = 0U;
    result = _weather_http_admit(session, &cancel_generation);
    if (result != ESP_OK)
    {
        fetch.error = result;
        return fetch;
    }
    weather_service_http_result_t response;
    result = weather_service_port_http_get(
                 url, s_weather.config.device_token,
                 _weather_kind_limit(kind), WEATHER_HTTP_TIMEOUT_MS,
                 cancel_generation,
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
        if (fetch.error == ESP_OK &&
                s_weather.located_session == session &&
                !_weather_location_same_identity(
                    &node->snapshot.location,
                    &s_weather.session_location))
        {
            s_weather.session_location = node->snapshot.location;
            ++s_weather.drift_attempt;
            if (s_weather.drift_attempt <= 1U)
            {
                /* The first consecutive drift may re-anchor on the next
                   tick to converge quickly. */
                s_weather.pending_scope_session = session;
                s_weather.pending_scope_attempted = false;
            }
            else
            {
                /* Consecutive drifts (e.g. a resolver alternating between
                   two scopes) must back off instead of polling every tick:
                   re-anchoring is throttled by the current retry deadline
                   until one anchor succeeds. */
                s_weather.pending_scope_attempted = true;
                s_weather.retry_due_ms[WEATHER_SERVICE_KIND_CURRENT] =
                    _weather_deadline_after(
                        weather_service_port_now_milliseconds(),
                        _weather_retry_delay(s_weather.drift_attempt - 1U));
            }
            fetch.scope_drifted = true;
            LOG_W("weather scope differs from located scope: kind=%u",
                  (unsigned)kind);
            fetch.error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    weather_service_port_http_result_release(&response);
    return fetch;
}

static weather_service_fetch_result_t _weather_fetch(
    weather_service_kind_t kind, weather_snapshot_node_t *node,
    uint32_t *changed_mask, uint64_t session)
{
    weather_service_fetch_result_t fetch = {0};
    for (unsigned attempt = 0U; attempt < 2U; ++attempt)
    {
        fetch = _weather_fetch_once(kind, node, changed_mask, session);
        bool retryable = _weather_transport_retryable(fetch.error) ||
                         (fetch.error == ESP_OK && fetch.status_code >= 500);
        if (!retryable || attempt != 0U || _weather_should_stop_cycle())
        {
            break;
        }
    }
    return fetch;
}

static bool _weather_kind_due(weather_service_kind_t kind, int64_t now_ms,
                              bool force)
{
    if (s_weather.account_retry_due_ms > now_ms ||
            (!force && s_weather.retry_due_ms[kind] > now_ms))
    {
        return false;
    }
    if (force)
    {
        return true;
    }
    return s_weather.next_due_ms[kind] == 0 ||
           s_weather.next_due_ms[kind] <= now_ms;
}

static bool _weather_scope_current_due(int64_t now_ms, bool fresh_scope,
                                       bool force)
{
    /* The account deadline is global and always wins, matching
       _weather_kind_due. A manual force then overrides the authentication
       freeze and the failure backoff; otherwise the authentication freeze
       wins, and only a scope that has not been anchored yet bypasses the
       retry backoff. */
    if (s_weather.account_retry_due_ms > now_ms)
    {
        return false;
    }
    if (force)
    {
        return true;
    }
    if (s_weather.next_due_ms[WEATHER_SERVICE_KIND_CURRENT] == INT64_MAX)
    {
        return false;
    }
    if (fresh_scope || !s_weather.pending_scope_attempted)
    {
        return true;
    }
    return s_weather.retry_due_ms[WEATHER_SERVICE_KIND_CURRENT] <= now_ms;
}

static void _weather_store_cache_if_due(int64_t now_ms, bool force)
{
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    bool due = s_weather.cache_dirty &&
               (force || s_weather.last_cache_write_ms == 0 ||
                now_ms - s_weather.last_cache_write_ms >=
                WEATHER_CACHE_WRITE_MIN_MS);
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
        s_weather.last_cache_write_ms = now_ms;
        s_weather.cache_dirty = false;
        xSemaphoreGive(s_weather.mutex);
    }
    else
    {
        LOG_W("cache write failed: %s", esp_err_to_name(result));
    }
    _weather_node_release(node);
}

static void _weather_force_consume(bool force)
{
    if (!force)
    {
        return;
    }
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    s_weather.force_refresh = false;
    xSemaphoreGive(s_weather.mutex);
}

static esp_err_t _weather_publish_expired_metadata(uint64_t session,
        uint32_t *changed_mask)
{
    int64_t now = weather_service_port_now_seconds();
    uint64_t generation = 0U;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (s_weather.current != NULL)
    {
        generation = s_weather.current->snapshot.generation;
    }
    xSemaphoreGive(s_weather.mutex);
    if (now == s_weather.expired_check_seconds &&
            generation == s_weather.expired_check_generation)
    {
        *changed_mask = 0U;
        return ESP_OK;
    }
    s_weather.expired_check_seconds = now;
    s_weather.expired_check_generation = generation;
    weather_snapshot_node_t *node = NULL;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (s_weather.current != NULL)
    {
        node = _weather_node_new(&s_weather.current->snapshot);
    }
    xSemaphoreGive(s_weather.mutex);
    if (node == NULL)
    {
        *changed_mask = 0U;
        return generation == 0U ? ESP_OK : ESP_ERR_NO_MEM;
    }
    uint32_t changed = _weather_mark_expired(&node->snapshot, now);
    esp_err_t result = ESP_OK;
    if (changed != 0U)
    {
        result = _weather_swap(node, changed, session);
        node = NULL;
    }
    _weather_node_release(node);
    *changed_mask = changed;
    return result;
}

static void _weather_force_restore(bool force)
{
    if (!force)
    {
        return;
    }
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (!s_weather.stopping)
    {
        s_weather.force_refresh = true;
    }
    xSemaphoreGive(s_weather.mutex);
}

static void _weather_run_cycle(bool force)
{
    int64_t wall_now = weather_service_port_now_seconds();
    int64_t monotonic_now_ms = weather_service_port_now_milliseconds();
    weather_service_state_t previous_state;
    weather_service_failure_t previous_failure;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    previous_state = s_weather.status.state;
    previous_failure = s_weather.status.failure;
    xSemaphoreGive(s_weather.mutex);
    if (!force && s_weather.internal_retry_due_ms > monotonic_now_ms)
    {
        uint32_t remaining = _weather_deadline_remaining(
                                 s_weather.internal_retry_due_ms,
                                 monotonic_now_ms);
        if (_weather_status_set(WEATHER_SERVICE_STATE_ERROR,
                                WEATHER_SERVICE_FAILURE_INTERNAL, remaining))
        {
            _weather_publish(0U);
        }
        return;
    }
    weather_snapshot_node_t *staging = _weather_clone_current();
    if (staging == NULL)
    {
        _weather_force_consume(force);
        _weather_schedule_internal_failure(monotonic_now_ms);
        return;
    }
    uint64_t cycle_session = 0U;
    bool location_scope_changed = false;
    esp_err_t location_result = _weather_prepare_location(
                                    &staging, force, &cycle_session,
                                    &location_scope_changed);
    if (location_result != ESP_OK)
    {
        _weather_node_release(staging);
        if (location_result == ESP_ERR_INVALID_STATE)
        {
            _weather_force_restore(force);
        }
        else if (location_result == ESP_ERR_NO_MEM ||
                 location_result == ESP_ERR_INVALID_ARG)
        {
            _weather_force_consume(force);
            _weather_schedule_internal_failure(monotonic_now_ms);
        }
        else if (force)
        {
            _weather_force_consume(true);
        }
        return;
    }
    if (!_weather_session_active(cycle_session))
    {
        _weather_node_release(staging);
        _weather_force_restore(force);
        return;
    }
    uint32_t metadata_changed = _weather_mark_expired(&staging->snapshot,
                                wall_now);
    bool new_location = false;
    bool location_changed = false;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (s_weather.current != NULL &&
            s_weather.current->snapshot.location.available)
    {
        new_location = location_scope_changed ||
                       s_weather.pending_scope_session == cycle_session ||
                       !_weather_location_same_identity(
                           &s_weather.current->snapshot.location,
                           &staging->snapshot.location);
        location_changed = !_weather_location_equal(
                               &s_weather.current->snapshot.location,
                               &staging->snapshot.location);
    }
    else
    {
        new_location = true;
        location_changed = true;
    }
    xSemaphoreGive(s_weather.mutex);
    if (!new_location && (location_changed || metadata_changed != 0U))
    {
        uint32_t changed_mask = metadata_changed;
        if (location_changed)
        {
            changed_mask |= WEATHER_SERVICE_DATA_LOCATION;
        }
        esp_err_t commit_result = _weather_commit_snapshot(
                                      &staging->snapshot, changed_mask,
                                      cycle_session);
        if (commit_result != ESP_OK)
        {
            _weather_node_release(staging);
            if (commit_result == ESP_ERR_NO_MEM)
            {
                _weather_force_consume(force);
                _weather_schedule_internal_failure(monotonic_now_ms);
            }
            else
            {
                _weather_force_restore(force);
            }
            return;
        }
        _weather_clear_internal_failure();
    }
    if (s_weather.located_session != cycle_session)
    {
        /* Location scope unresolved: keep the retained snapshot and do not
           fetch weather. The server attributes every request to its own
           inferred scope, so without a located anchor the echoed datasets
           could not be kept consistent with the retained location. */
        _weather_force_consume(force);
        if (s_weather.location_auth_error ||
                s_weather.next_due_ms[WEATHER_SERVICE_KIND_CURRENT] ==
                INT64_MAX)
        {
            /* The bearer-wide authentication freeze outranks the location
               degradation, so a revoked token stays visible. */
            if (_weather_status_set(WEATHER_SERVICE_STATE_AUTH_ERROR,
                                    WEATHER_SERVICE_FAILURE_AUTHENTICATION,
                                    0U))
            {
                _weather_publish(0U);
            }
        }
        _weather_node_release(staging);
        return;
    }
    bool refresh_all_for_location = new_location;
    bool any_due = false;
    if (new_location)
    {
        /* A fresh session scope must refresh current immediately. A scope
           left pending by a failed anchor fetch or by a drifted weather
           echo must respect the account deadline, the authentication
           freeze, and the failure backoff instead of polling every worker
           tick; the previous location's refresh deadline must not block it.
           The account deadline always wins; a manual force only bypasses
           the authentication freeze and the kind retry backoff. */
        any_due = _weather_scope_current_due(monotonic_now_ms,
                                             location_scope_changed, force);
    }
    else
    {
        for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
                kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
        {
            if (_weather_kind_due(kind, monotonic_now_ms, force))
            {
                any_due = true;
                break;
            }
        }
    }
    if (!any_due)
    {
        if (force && s_weather.account_retry_due_ms > monotonic_now_ms)
        {
            _weather_force_consume(true);
        }
        weather_service_state_t idle_state =
            staging->snapshot.location.reused ?
            WEATHER_SERVICE_STATE_DEGRADED : WEATHER_SERVICE_STATE_READY;
        weather_service_failure_t idle_failure =
            staging->snapshot.location.reused ?
            WEATHER_SERVICE_FAILURE_LOCATION : WEATHER_SERVICE_FAILURE_NONE;
        uint32_t idle_retry_after = 0U;
        if (staging->snapshot.location.reused &&
                s_weather.location_retry_due_ms > monotonic_now_ms)
        {
            idle_retry_after = _weather_deadline_remaining(
                                   s_weather.location_retry_due_ms,
                                   monotonic_now_ms);
        }
        if (s_weather.account_retry_due_ms > monotonic_now_ms)
        {
            idle_state = WEATHER_SERVICE_STATE_RATE_LIMITED;
            idle_failure = WEATHER_SERVICE_FAILURE_RATE_LIMITED;
            idle_retry_after = _weather_deadline_remaining(
                                   s_weather.account_retry_due_ms,
                                   monotonic_now_ms);
        }
        else if (s_weather.location_auth_error ||
                 s_weather.next_due_ms[WEATHER_SERVICE_KIND_CURRENT] ==
                 INT64_MAX || previous_state ==
                 WEATHER_SERVICE_STATE_AUTH_ERROR)
        {
            /* The authentication freeze is authoritative even when an
               intervening cycle (such as the locating state of a new
               session) overwrote the published status. */
            idle_state = WEATHER_SERVICE_STATE_AUTH_ERROR;
            idle_failure = WEATHER_SERVICE_FAILURE_AUTHENTICATION;
            idle_retry_after = 0U;
        }
        else
        {
            for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
                    kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
            {
                if (s_weather.retry_due_ms[kind] > monotonic_now_ms)
                {
                    idle_state = WEATHER_SERVICE_STATE_DEGRADED;
                    idle_failure = previous_failure;
                    break;
                }
            }
        }
        _weather_node_release(staging);
        uint32_t expired_changed = 0U;
        if (s_weather.pending_scope_session == cycle_session)
        {
            /* The idle branch is the only path a frozen or backoff-limited
               pending scope reaches; keep the retained snapshot's staleness
               metadata current there as well. */
            esp_err_t expired_result = _weather_publish_expired_metadata(
                                           cycle_session, &expired_changed);
            if (expired_result == ESP_ERR_NO_MEM)
            {
                _weather_schedule_internal_failure(monotonic_now_ms);
                return;
            }
            if (expired_result != ESP_OK)
            {
                _weather_force_restore(force);
                return;
            }
        }
        if (_weather_status_set(idle_state, idle_failure, idle_retry_after))
        {
            _weather_publish(expired_changed);
        }
        return;
    }
    _weather_force_consume(force);
    _weather_status_set(WEATHER_SERVICE_STATE_UPDATING,
                        staging->snapshot.location.reused ?
                        WEATHER_SERVICE_FAILURE_LOCATION :
                        WEATHER_SERVICE_FAILURE_NONE, 0U);
    _weather_publish(0U);
    bool any_success = false;
    bool cycle_failed = false;
    bool attempted_fetch = false;
    uint32_t successful_kinds = 0U;
    weather_service_state_t final_state =
        staging->snapshot.location.reused ? WEATHER_SERVICE_STATE_DEGRADED :
        WEATHER_SERVICE_STATE_READY;
    weather_service_failure_t final_failure =
        staging->snapshot.location.reused ? WEATHER_SERVICE_FAILURE_LOCATION :
        WEATHER_SERVICE_FAILURE_NONE;
    uint32_t final_retry_after = 0U;
    if (staging->snapshot.location.reused &&
            s_weather.location_retry_due_ms > monotonic_now_ms)
    {
        final_retry_after = _weather_deadline_remaining(
                                s_weather.location_retry_due_ms,
                                monotonic_now_ms);
    }
    for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
            kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
    {
        if (_weather_should_stop_cycle())
        {
            _weather_force_restore(force);
            break;
        }
        if (new_location && kind != WEATHER_SERVICE_KIND_CURRENT)
        {
            continue;
        }
        monotonic_now_ms = weather_service_port_now_milliseconds();
        bool due = false;
        if (refresh_all_for_location &&
                kind == WEATHER_SERVICE_KIND_CURRENT)
        {
            /* The scope-anchor current fetch bypasses the previous
               location's refresh deadline but honors account limiting,
               the authentication freeze, and the failure backoff; the
               account deadline always wins, while a manual force only
               bypasses the freeze and the retry backoff. */
            due = _weather_scope_current_due(monotonic_now_ms,
                                             location_scope_changed, force);
        }
        else if (refresh_all_for_location)
        {
            /* Non-current kinds are deferred until the anchor succeeds and
               then bypass their own due state inside the scope cycle. */
            due = true;
        }
        else
        {
            due = _weather_kind_due(kind, monotonic_now_ms, force);
        }
        if (!due)
        {
            continue;
        }
        attempted_fetch = true;
        weather_snapshot_node_t *candidate = _weather_node_new(
                &staging->snapshot);
        if (candidate == NULL)
        {
            cycle_failed = true;
            final_state = WEATHER_SERVICE_STATE_ERROR;
            final_failure = WEATHER_SERVICE_FAILURE_INTERNAL;
            _weather_schedule_internal_failure(monotonic_now_ms);
            break;
        }
        uint32_t changed_mask = 0U;
        if (refresh_all_for_location &&
                kind == WEATHER_SERVICE_KIND_CURRENT)
        {
            s_weather.pending_scope_attempted = true;
        }
        weather_service_fetch_result_t fetch = _weather_fetch(
                kind, candidate, &changed_mask, cycle_session);
        _weather_record_stack(_weather_kind_name(kind));
        if (!_weather_session_active(cycle_session))
        {
            _weather_node_release(candidate);
            _weather_node_release(staging);
            _weather_force_restore(force);
            return;
        }
        if (fetch.error == ESP_OK && fetch.status_code == 200)
        {
            bool publishable = !new_location ||
                               kind == WEATHER_SERVICE_KIND_CURRENT ||
                               (candidate->snapshot.available_mask &
                                WEATHER_SERVICE_DATA_CURRENT) != 0U;
            esp_err_t commit_result = publishable ?
                                      _weather_commit_snapshot(
                                          &candidate->snapshot,
                                          changed_mask, cycle_session) :
                                      ESP_ERR_INVALID_STATE;
            if (commit_result == ESP_OK)
            {
                _weather_node_release(staging);
                staging = candidate;
                candidate = NULL;
                new_location = false;
                if (kind == WEATHER_SERVICE_KIND_CURRENT)
                {
                    s_weather.pending_scope_session = 0U;
                    s_weather.pending_scope_attempted = false;
                    s_weather.drift_attempt = 0U;
                }
                s_weather.retry_attempt[kind] = 0U;
                s_weather.retry_due_ms[kind] = 0;
                monotonic_now_ms = weather_service_port_now_milliseconds();
                s_weather.next_due_ms[kind] = _weather_deadline_after(
                                                  monotonic_now_ms,
                                                  s_weather.config.refresh_seconds[kind]);
                _weather_clear_internal_failure();
                any_success = true;
                successful_kinds |= UINT32_C(1) << kind;
            }
            else
            {
                cycle_failed = true;
                final_state = WEATHER_SERVICE_STATE_ERROR;
                final_failure = WEATHER_SERVICE_FAILURE_INTERNAL;
                if (commit_result == ESP_ERR_NO_MEM)
                {
                    _weather_schedule_internal_failure(monotonic_now_ms);
                }
                else
                {
                    _weather_force_restore(force);
                }
            }
            _weather_node_release(candidate);
            if (commit_result != ESP_OK)
            {
                break;
            }
            continue;
        }
        _weather_node_release(candidate);
        if (fetch.error == ESP_ERR_INVALID_STATE)
        {
            _weather_node_release(staging);
            _weather_force_restore(force);
            return;
        }
        cycle_failed = true;
        int status = fetch.status_code;
        weather_service_failure_t failure;
        if (fetch.error == ESP_ERR_NO_MEM ||
                fetch.error == ESP_ERR_INVALID_ARG)
        {
            failure = WEATHER_SERVICE_FAILURE_INTERNAL;
        }
        else if (fetch.error == ESP_ERR_INVALID_SIZE ||
                 fetch.error == ESP_ERR_INVALID_RESPONSE)
        {
            failure = WEATHER_SERVICE_FAILURE_RESPONSE;
        }
        else
        {
            failure = status == 0 ? WEATHER_SERVICE_FAILURE_NETWORK :
                      _weather_http_failure(status);
        }
        if (fetch.error == ESP_OK)
        {
            LOG_W("weather dataset %u failed: HTTP %d",
                  (unsigned)kind, status);
        }
        else
        {
            LOG_W("weather dataset %u failed: %s",
                  (unsigned)kind, esp_err_to_name(fetch.error));
        }
        if (failure == WEATHER_SERVICE_FAILURE_AUTHENTICATION)
        {
            final_state = WEATHER_SERVICE_STATE_AUTH_ERROR;
            final_failure = failure;
            for (weather_service_kind_t kind = WEATHER_SERVICE_KIND_CURRENT;
                    kind < WEATHER_SERVICE_KIND_COUNT; ++kind)
            {
                /* All weather datasets share the same bearer identity, so
                   one rejected kind freezes the whole weather scope. Only
                   a manual force bypasses the freeze; a new location
                   session keeps it. */
                s_weather.next_due_ms[kind] = INT64_MAX;
            }
            break;
        }
        if (failure == WEATHER_SERVICE_FAILURE_RATE_LIMITED)
        {
            fetch.retry_after_seconds = fetch.retry_after_seconds == 0U ?
                                        60U : fetch.retry_after_seconds;
            final_state = WEATHER_SERVICE_STATE_RATE_LIMITED;
            final_failure = failure;
            final_retry_after = fetch.retry_after_seconds;
            xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
            s_weather.account_retry_due_ms = _weather_deadline_after(
                                                 monotonic_now_ms,
                                                 fetch.retry_after_seconds);
            xSemaphoreGive(s_weather.mutex);
        }
        else
        {
            final_state = WEATHER_SERVICE_STATE_DEGRADED;
            final_failure = failure;
            final_retry_after = 0U;
        }
        if (failure == WEATHER_SERVICE_FAILURE_INTERNAL)
        {
            _weather_schedule_internal_failure(monotonic_now_ms);
        }
        else if (!fetch.scope_drifted)
        {
            /* A drifted echo restarts the scope from current; it must not
               advance the retry backoff of the dataset that observed it. */
            _weather_schedule_failure(kind, monotonic_now_ms,
                                      fetch.retry_after_seconds);
        }
        if (failure == WEATHER_SERVICE_FAILURE_RATE_LIMITED ||
                fetch.scope_drifted ||
                (new_location && kind == WEATHER_SERVICE_KIND_CURRENT))
        {
            /* A drifted echo adopts a new scope: stop this cycle so no
               dataset from the old scope is committed alongside it; the
               next cycle restarts the scope from current. */
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
        _weather_store_cache_if_due(
            weather_service_port_now_milliseconds(), force);
    }
    if (final_failure == WEATHER_SERVICE_FAILURE_INTERNAL)
    {
        return;
    }
    uint32_t expired_changed = 0U;
    if (s_weather.pending_scope_session == cycle_session &&
            (successful_kinds & (UINT32_C(1) <<
                                 WEATHER_SERVICE_KIND_CURRENT)) == 0U)
    {
        /* The scope staging was cleared, so the retained public snapshot
           never sees mark_expired otherwise; keep its staleness metadata
           current without touching the location or the datasets. The mask
           is advisory: consumers must re-acquire by generation. */
        esp_err_t expired_result = _weather_publish_expired_metadata(
                                       cycle_session, &expired_changed);
        if (expired_result == ESP_ERR_NO_MEM)
        {
            _weather_schedule_internal_failure(monotonic_now_ms);
            return;
        }
        if (expired_result != ESP_OK)
        {
            _weather_force_restore(force);
            return;
        }
    }
    if (!cycle_failed && attempted_fetch)
    {
        final_state = final_failure == WEATHER_SERVICE_FAILURE_LOCATION ?
                      WEATHER_SERVICE_STATE_DEGRADED :
                      WEATHER_SERVICE_STATE_READY;
        if (successful_kinds ==
                ((UINT32_C(1) << WEATHER_SERVICE_KIND_COUNT) - 1U))
        {
            LOG_I("weather update completed");
        }
    }
    if (_weather_status_set(final_state, final_failure, final_retry_after))
    {
        _weather_publish(expired_changed);
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
    _weather_store_cache_if_due(weather_service_port_now_milliseconds(), true);
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
    s_weather.minimum_stack_remaining = UINT32_MAX;
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
            if (result == ESP_ERR_NO_MEM || result == ESP_FAIL)
            {
                LOG_W("cache load failed: %s", esp_err_to_name(result));
                goto cleanup;
            }
            LOG_D("weather cache not loaded: %s", esp_err_to_name(result));
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
    bool cancel = false;
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (ready && (!s_weather.network_ready ||
                  s_weather.ipv4_address != ipv4_address))
    {
        ++s_weather.network_session;
        notify = true;
        cancel = s_weather.network_ready;
    }
    else if (!ready && s_weather.network_ready)
    {
        notify = true;
        cancel = true;
    }
    s_weather.network_ready = ready;
    s_weather.ipv4_address = ready ? ipv4_address : 0U;
    s_weather.status.network_ready = ready;
    xSemaphoreGive(s_weather.mutex);
    if (cancel)
    {
        weather_service_port_cancel();
    }
    if (notify)
    {
        LOG_I("network state changed: %s",
              ready ? "new IPv4 session" : "unavailable");
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
    int64_t now_ms = weather_service_port_now_milliseconds();
    xSemaphoreTake(s_weather.mutex, portMAX_DELAY);
    if (!s_weather.config.configured || !s_weather.network_ready ||
            s_weather.suspended || s_weather.stopping)
    {
        xSemaphoreGive(s_weather.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_weather.account_retry_due_ms > now_ms ||
            (s_weather.last_manual_refresh_ms != 0 &&
             now_ms - s_weather.last_manual_refresh_ms <
             (int64_t)s_weather.config.manual_refresh_min_seconds * 1000))
    {
        xSemaphoreGive(s_weather.mutex);
        return ESP_ERR_TIMEOUT;
    }
    s_weather.last_manual_refresh_ms = now_ms;
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
