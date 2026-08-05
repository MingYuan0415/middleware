#define DBG_TAG "weather_parse"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "weather_service_internal.h"

#include "cJSON.h"
#ifdef ESP_PLATFORM
    #include "esp_heap_caps.h"
#endif

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
static void *_weather_parse_allocate(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void _weather_parse_release(void *memory)
{
    heap_caps_free(memory);
}
#endif

void weather_service_parse_init(void)
{
#ifdef ESP_PLATFORM
    cJSON_Hooks hooks =
    {
        .malloc_fn = _weather_parse_allocate,
        .free_fn = _weather_parse_release,
    };
    cJSON_InitHooks(&hooks);
#else
    cJSON_InitHooks(NULL);
#endif
}

static bool _weather_parse_utf8(const char *text, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)text;
    for (size_t index = 0; index < length;)
    {
        uint8_t value = bytes[index];
        if (value < 0x20U || value == 0x7FU)
        {
            return false;
        }
        if (value < 0x80U)
        {
            ++index;
            continue;
        }
        size_t continuation = 0U;
        uint32_t codepoint = 0U;
        if ((value & 0xE0U) == 0xC0U)
        {
            continuation = 1U;
            codepoint = value & 0x1FU;
        }
        else if ((value & 0xF0U) == 0xE0U)
        {
            continuation = 2U;
            codepoint = value & 0x0FU;
        }
        else if ((value & 0xF8U) == 0xF0U)
        {
            continuation = 3U;
            codepoint = value & 0x07U;
        }
        else
        {
            return false;
        }
        if (index + continuation >= length)
        {
            return false;
        }
        for (size_t part = 1U; part <= continuation; ++part)
        {
            uint8_t next = bytes[index + part];
            if ((next & 0xC0U) != 0x80U)
            {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        if ((continuation == 1U && codepoint < 0x80U) ||
                (continuation == 2U && codepoint < 0x800U) ||
                (continuation == 3U && codepoint < 0x10000U) ||
                codepoint > 0x10FFFFU ||
                (codepoint >= 0xD800U && codepoint <= 0xDFFFU))
        {
            return false;
        }
        index += continuation + 1U;
    }
    return true;
}

static bool _weather_parse_copy_text(const cJSON *object, const char *name,
                                     char *output, size_t output_size,
                                     bool required, bool *truncated)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL && !required)
    {
        output[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return false;
    }
    size_t length = strlen(item->valuestring);
    if (!_weather_parse_utf8(item->valuestring, length) ||
            (required && length == 0U))
    {
        return false;
    }
    size_t copy_length = length;
    if (copy_length >= output_size)
    {
        copy_length = output_size - 1U;
        while (copy_length > 0U &&
                (((uint8_t)item->valuestring[copy_length] & 0xC0U) == 0x80U))
        {
            --copy_length;
        }
        if (truncated != NULL)
        {
            *truncated = true;
        }
    }
    memcpy(output, item->valuestring, copy_length);
    output[copy_length] = '\0';
    return true;
}

static bool _weather_parse_number(const cJSON *object, const char *name,
                                  double minimum, double maximum,
                                  double *output, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL && !required)
    {
        return true;
    }
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
            item->valuedouble < minimum || item->valuedouble > maximum)
    {
        return false;
    }
    *output = item->valuedouble;
    return true;
}

static int64_t _weather_parse_days_from_civil(int year, unsigned month,
        unsigned day)
{
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned adjusted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned day_of_year =
        (153U * adjusted_month + 2U) / 5U +
        day - 1U;
    const unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
                                year_of_era / 100U + day_of_year;
    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static bool _weather_parse_time_text(const char *text,
                                     weather_service_time_t *output)
{
    int year = 0;
    unsigned month = 0U;
    unsigned day = 0U;
    unsigned hour = 0U;
    unsigned minute = 0U;
    unsigned second = 0U;
    int consumed = 0;
    if (sscanf(text, "%d-%u-%uT%u:%u:%u%n", &year, &month, &day,
               &hour, &minute, &second, &consumed) != 6 ||
            year < 1970 || year > 2200 || month < 1U || month > 12U ||
            day < 1U || day > 31U || hour > 23U || minute > 59U ||
            second > 60U)
    {
        return false;
    }
    const char *zone = text + consumed;
    if (*zone == '.')
    {
        ++zone;
        while (isdigit((unsigned char) * zone))
        {
            ++zone;
        }
    }
    int offset_minutes = 0;
    if (*zone == 'Z' && zone[1] == '\0')
    {
        offset_minutes = 0;
    }
    else
    {
        int zone_hour = 0;
        int zone_minute = 0;
        char sign = '\0';
        int zone_consumed = 0;
        if (sscanf(zone, "%c%d:%d%n", &sign, &zone_hour, &zone_minute,
                   &zone_consumed) != 3 || zone[zone_consumed] != '\0' ||
                (sign != '+' && sign != '-') || zone_hour > 23 ||
                zone_minute > 59)
        {
            return false;
        }
        offset_minutes = zone_hour * 60 + zone_minute;
        if (sign == '-')
        {
            offset_minutes = -offset_minutes;
        }
    }
    int64_t local_seconds = _weather_parse_days_from_civil(year, month, day) *
                            INT64_C(86400) + (int64_t)hour * 3600 +
                            (int64_t)minute * 60 + second;
    output->epoch_seconds = local_seconds - (int64_t)offset_minutes * 60;
    output->offset_minutes = (int16_t)offset_minutes;
    return true;
}

static bool _weather_parse_time(const cJSON *object, const char *name,
                                weather_service_time_t *output, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == NULL && !required)
    {
        memset(output, 0, sizeof(*output));
        return true;
    }
    return cJSON_IsString(item) && item->valuestring != NULL &&
           _weather_parse_time_text(item->valuestring, output);
}

static uint16_t _weather_parse_tenths(double value)
{
    long rounded = lround(value * 10.0);
    if (rounded < 0L)
    {
        return 0U;
    }
    return rounded > UINT16_MAX ? UINT16_MAX : (uint16_t)rounded;
}

static int16_t _weather_parse_signed_tenths(double value)
{
    long rounded = lround(value * 10.0);
    if (rounded < INT16_MIN)
    {
        rounded = INT16_MIN;
    }
    if (rounded > INT16_MAX)
    {
        rounded = INT16_MAX;
    }
    return (int16_t)rounded;
}

static bool _weather_parse_code(const cJSON *object, const char *name,
                                uint16_t *output)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(item->valuestring, &end, 10);
    if (item->valuestring[0] == '\0' || end == NULL || *end != '\0' ||
            value > UINT16_MAX)
    {
        return false;
    }
    *output = (uint16_t)value;
    return true;
}

static bool _weather_parse_percent(const cJSON *object, const char *name,
                                   uint8_t *output)
{
    double value = 0.0;
    if (!_weather_parse_number(object, name, 0.0, 100.0, &value, true))
    {
        return false;
    }
    *output = (uint8_t)lround(value);
    return true;
}

static bool _weather_parse_meta(const cJSON *root,
                                weather_service_dataset_meta_t *meta)
{
    const cJSON *stale = cJSON_GetObjectItemCaseSensitive(root, "stale");
    if (!_weather_parse_time(root, "fetched_at", &meta->fetched_at, true) ||
            !_weather_parse_time(root, "updated_at", &meta->updated_at, true) ||
            !_weather_parse_time(root, "valid_until", &meta->valid_until,
                                 true) || !cJSON_IsBool(stale))
    {
        return false;
    }
    meta->available = true;
    meta->stale = cJSON_IsTrue(stale);
    meta->expired = false;
    return true;
}

static bool _weather_parse_public_location(const cJSON *root,
        weather_service_location_t *location)
{
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "location");
    bool truncated = false;
    char provider[WEATHER_SERVICE_PROVIDER_BYTES];
    if (!cJSON_IsObject(object) ||
            !_weather_parse_copy_text(object, "provider", provider,
                                      sizeof(provider), true, &truncated) ||
            strcmp(provider, "ipapi.is") != 0 || truncated)
    {
        return false;
    }
    return _weather_parse_copy_text(object, "city", location->city,
                                    sizeof(location->city), false, NULL) &&
           _weather_parse_copy_text(object, "region", location->region,
                                    sizeof(location->region), false, NULL) &&
           _weather_parse_copy_text(object, "country", location->country,
                                    sizeof(location->country), false, NULL) &&
           _weather_parse_copy_text(object, "timezone", location->timezone,
                                    sizeof(location->timezone), false, NULL);
}

esp_err_t weather_service_parse_location(const uint8_t *body,
        size_t body_size, int64_t acquired_at,
        weather_service_location_t *location)
{
    if (body == NULL || body_size == 0U || location == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)body, body_size);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "location");
    double latitude = 0.0;
    double longitude = 0.0;
    weather_service_location_t parsed = {0};
    bool valid = cJSON_IsObject(object) &&
                 _weather_parse_number(object, "latitude", -90.0, 90.0,
                                       &latitude, true) &&
                 _weather_parse_number(object, "longitude", -180.0, 180.0,
                                       &longitude, true) &&
                 _weather_parse_copy_text(object, "city", parsed.city,
                                          sizeof(parsed.city), false, NULL) &&
                 _weather_parse_copy_text(object, "state", parsed.region,
                                          sizeof(parsed.region), false, NULL) &&
                 _weather_parse_copy_text(object, "country_code",
                                          parsed.country,
                                          sizeof(parsed.country), true, NULL) &&
                 _weather_parse_copy_text(object, "timezone",
                                          parsed.timezone,
                                          sizeof(parsed.timezone), true, NULL);
    if (valid)
    {
        parsed.latitude_tenths = _weather_parse_signed_tenths(latitude);
        parsed.longitude_tenths = _weather_parse_signed_tenths(longitude);
        memcpy(parsed.provider, "ipapi.is", sizeof("ipapi.is"));
        parsed.acquired_at = acquired_at;
        parsed.available = true;
        *location = parsed;
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static bool _weather_parse_current(const cJSON *root, const cJSON *data,
                                   weather_service_snapshot_t *snapshot)
{
    weather_service_current_t parsed = {0};
    double value = 0.0;
    if (!_weather_parse_meta(root, &parsed.meta) ||
            !_weather_parse_time(data, "observed_at", &parsed.observed_at,
                                 true) ||
            !_weather_parse_number(data, "temperature_c", -100.0, 100.0,
                                   &value, true))
    {
        return false;
    }
    parsed.temperature_tenths_c = _weather_parse_signed_tenths(value);
    if (!_weather_parse_number(data, "feels_like_c", -100.0, 100.0,
                               &value, true))
    {
        return false;
    }
    parsed.feels_like_tenths_c = _weather_parse_signed_tenths(value);
    if (!_weather_parse_code(data, "condition_code", &parsed.condition_code) ||
            !_weather_parse_copy_text(data, "condition_text",
                                      parsed.condition_text,
                                      sizeof(parsed.condition_text), true,
                                      NULL) ||
            !_weather_parse_number(data, "wind_degrees", 0.0, 360.0,
                                   &value, true))
    {
        return false;
    }
    parsed.wind_degrees = (uint16_t)lround(value);
    if (!_weather_parse_number(data, "wind_speed_kmh", 0.0, 1000.0,
                               &value, true))
    {
        return false;
    }
    parsed.wind_speed_tenths_kmh = _weather_parse_tenths(value);
    if (!_weather_parse_copy_text(data, "wind_direction",
                                  parsed.wind_direction,
                                  sizeof(parsed.wind_direction), true, NULL) ||
            !_weather_parse_copy_text(data, "wind_scale", parsed.wind_scale,
                                      sizeof(parsed.wind_scale), true, NULL) ||
            !_weather_parse_percent(data, "humidity_percent",
                                    &parsed.humidity_percent) ||
            !_weather_parse_number(data, "precipitation_mm", 0.0, 10000.0,
                                   &value, true))
    {
        return false;
    }
    parsed.precipitation_tenths_mm = _weather_parse_tenths(value);
    if (!_weather_parse_number(data, "pressure_hpa", 100.0, 2000.0,
                               &value, true))
    {
        return false;
    }
    parsed.pressure_hpa = (uint16_t)lround(value);
    if (!_weather_parse_number(data, "visibility_km", 0.0, 1000.0,
                               &value, true))
    {
        return false;
    }
    parsed.visibility_tenths_km = _weather_parse_tenths(value);
    snapshot->current = parsed;
    snapshot->available_mask |= WEATHER_SERVICE_DATA_CURRENT;
    return true;
}

static bool _weather_parse_hour(const cJSON *object,
                                weather_service_hour_t *hour)
{
    double value = 0.0;
    if (!_weather_parse_time(object, "forecast_at", &hour->forecast_at, true) ||
            !_weather_parse_number(object, "temperature_c", -100.0, 100.0,
                                   &value, true))
    {
        return false;
    }
    hour->temperature_tenths_c = _weather_parse_signed_tenths(value);
    if (!_weather_parse_code(object, "condition_code", &hour->condition_code) ||
            !_weather_parse_copy_text(object, "condition_text",
                                      hour->condition_text,
                                      sizeof(hour->condition_text), true,
                                      NULL) ||
            !_weather_parse_number(object, "wind_speed_kmh", 0.0, 1000.0,
                                   &value, true))
    {
        return false;
    }
    hour->wind_speed_tenths_kmh = _weather_parse_tenths(value);
    if (!_weather_parse_copy_text(object, "wind_direction",
                                  hour->wind_direction,
                                  sizeof(hour->wind_direction), true, NULL) ||
            !_weather_parse_percent(object, "humidity_percent",
                                    &hour->humidity_percent) ||
            !_weather_parse_percent(object,
                                    "precipitation_chance_percent",
                                    &hour->precipitation_chance_percent) ||
            !_weather_parse_number(object, "precipitation_mm", 0.0, 10000.0,
                                   &value, true))
    {
        return false;
    }
    hour->precipitation_tenths_mm = _weather_parse_tenths(value);
    return true;
}

static bool _weather_parse_hourly(const cJSON *root, const cJSON *data,
                                  weather_service_snapshot_t *snapshot)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "hours");
    int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    if (count < 1 || count > (int)WEATHER_SERVICE_MAX_HOURS)
    {
        return false;
    }
    weather_service_hourly_t parsed = {0};
    if (!_weather_parse_meta(root, &parsed.meta))
    {
        return false;
    }
    for (int index = 0; index < count; ++index)
    {
        if (!_weather_parse_hour(cJSON_GetArrayItem(items, index),
                                 &parsed.items[index]))
        {
            return false;
        }
    }
    parsed.count = (uint8_t)count;
    snapshot->hourly = parsed;
    snapshot->available_mask |= WEATHER_SERVICE_DATA_HOURLY;
    return true;
}

static bool _weather_parse_day(const cJSON *object,
                               weather_service_day_t *day)
{
    double value = 0.0;
    if (!_weather_parse_copy_text(object, "date", day->date,
                                  sizeof(day->date), true, NULL) ||
            strlen(day->date) != 10U ||
            !_weather_parse_number(object, "temperature_min_c", -100.0,
                                   100.0, &value, true))
    {
        return false;
    }
    day->minimum_temperature_tenths_c = _weather_parse_signed_tenths(value);
    if (!_weather_parse_number(object, "temperature_max_c", -100.0, 100.0,
                               &value, true))
    {
        return false;
    }
    day->maximum_temperature_tenths_c = _weather_parse_signed_tenths(value);
    if (!_weather_parse_code(object, "condition_day_code",
                             &day->day_condition_code) ||
            !_weather_parse_code(object, "condition_night_code",
                                 &day->night_condition_code) ||
            !_weather_parse_copy_text(object, "condition_day_text",
                                      day->day_condition_text,
                                      sizeof(day->day_condition_text), true,
                                      NULL) ||
            !_weather_parse_copy_text(object, "condition_night_text",
                                      day->night_condition_text,
                                      sizeof(day->night_condition_text), true,
                                      NULL) ||
            !_weather_parse_percent(object, "humidity_percent",
                                    &day->humidity_percent) ||
            !_weather_parse_number(object, "precipitation_mm", 0.0, 10000.0,
                                   &value, true))
    {
        return false;
    }
    day->precipitation_tenths_mm = _weather_parse_tenths(value);
    if (!_weather_parse_number(object, "visibility_km", 0.0, 1000.0,
                               &value, true))
    {
        return false;
    }
    day->visibility_tenths_km = _weather_parse_tenths(value);
    if (!_weather_parse_number(object, "uv_index", 0.0, 100.0, &value, true))
    {
        return false;
    }
    day->uv_index = (uint8_t)lround(value);
    return true;
}

static bool _weather_parse_daily(const cJSON *root, const cJSON *data,
                                 weather_service_snapshot_t *snapshot)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "days");
    int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    if (count < 1 || count > (int)WEATHER_SERVICE_MAX_DAYS)
    {
        return false;
    }
    weather_service_daily_t parsed = {0};
    if (!_weather_parse_meta(root, &parsed.meta))
    {
        return false;
    }
    for (int index = 0; index < count; ++index)
    {
        if (!_weather_parse_day(cJSON_GetArrayItem(items, index),
                                &parsed.items[index]))
        {
            return false;
        }
    }
    parsed.count = (uint8_t)count;
    snapshot->daily = parsed;
    snapshot->available_mask |= WEATHER_SERVICE_DATA_DAILY;
    return true;
}

static uint64_t _weather_parse_alert_key(const char *id, const char *title)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    const char *parts[] = {id, title};
    for (size_t part = 0U; part < 2U; ++part)
    {
        for (const uint8_t *cursor = (const uint8_t *)parts[part];
                *cursor != 0U; ++cursor)
        {
            hash ^= *cursor;
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash == 0U ? 1U : hash;
}

static bool _weather_parse_alert(const cJSON *object,
                                 weather_service_alert_t *alert)
{
    char id[129];
    bool truncated = false;
    if (!_weather_parse_copy_text(object, "id", id, sizeof(id), true,
                                  &truncated) || truncated ||
            !_weather_parse_copy_text(object, "title", alert->title,
                                      sizeof(alert->title), true,
                                      &alert->content_truncated) ||
            !_weather_parse_copy_text(object, "type_name", alert->type_name,
                                      sizeof(alert->type_name), true,
                                      &alert->content_truncated) ||
            !_weather_parse_copy_text(object, "severity", alert->severity,
                                      sizeof(alert->severity), true, NULL) ||
            !_weather_parse_copy_text(object, "status", alert->status,
                                      sizeof(alert->status), true, NULL) ||
            !_weather_parse_time(object, "issued_at", &alert->issued_at,
                                 true) ||
            !_weather_parse_time(object, "starts_at", &alert->starts_at,
                                 false) ||
            !_weather_parse_time(object, "ends_at", &alert->ends_at, false) ||
            !_weather_parse_copy_text(object, "description",
                                      alert->description,
                                      sizeof(alert->description), false,
                                      &alert->content_truncated) ||
            !_weather_parse_copy_text(object, "instruction",
                                      alert->instruction,
                                      sizeof(alert->instruction), false,
                                      &alert->content_truncated))
    {
        return false;
    }
    const cJSON *source_truncated = cJSON_GetObjectItemCaseSensitive(
                                        object, "content_truncated");
    if (!cJSON_IsBool(source_truncated))
    {
        return false;
    }
    alert->content_truncated = alert->content_truncated ||
                               cJSON_IsTrue(source_truncated);
    alert->key = _weather_parse_alert_key(id, alert->title);
    return true;
}

static bool _weather_parse_alerts(const cJSON *root, const cJSON *data,
                                  weather_service_snapshot_t *snapshot)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "items");
    const cJSON *source_truncated = cJSON_GetObjectItemCaseSensitive(
                                        data, "truncated");
    if (!cJSON_IsArray(items) || !cJSON_IsBool(source_truncated))
    {
        return false;
    }
    int source_count = cJSON_GetArraySize(items);
    int count = source_count;
    if (count > (int)WEATHER_SERVICE_MAX_ALERTS)
    {
        count = (int)WEATHER_SERVICE_MAX_ALERTS;
    }
    weather_service_alerts_t parsed = {0};
    if (!_weather_parse_meta(root, &parsed.meta))
    {
        return false;
    }
    parsed.truncated = cJSON_IsTrue(source_truncated) || count < source_count;
    for (int index = 0; index < count; ++index)
    {
        if (!_weather_parse_alert(cJSON_GetArrayItem(items, index),
                                  &parsed.items[index]))
        {
            return false;
        }
    }
    parsed.count = (uint8_t)count;
    snapshot->alerts = parsed;
    snapshot->available_mask |= WEATHER_SERVICE_DATA_ALERTS;
    return true;
}

esp_err_t weather_service_parse_weather(weather_service_kind_t kind,
                                        const uint8_t *body, size_t body_size,
                                        weather_service_snapshot_t *snapshot, uint32_t *changed_mask)
{
    if (kind >= WEATHER_SERVICE_KIND_COUNT || body == NULL || body_size == 0U ||
            snapshot == NULL || changed_mask == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)body, body_size);
    const cJSON *version = cJSON_IsObject(root) ?
                           cJSON_GetObjectItemCaseSensitive(root,
                               "schema_version") : NULL;
    const cJSON *source = cJSON_IsObject(root) ?
                          cJSON_GetObjectItemCaseSensitive(root, "source") : NULL;
    const cJSON *source_id = cJSON_IsObject(source) ?
                             cJSON_GetObjectItemCaseSensitive(source, "id") : NULL;
    const cJSON *data = cJSON_IsObject(root) ?
                        cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;
    bool valid = cJSON_IsNumber(version) && version->valueint == 1 &&
                 cJSON_IsString(source_id) && source_id->valuestring != NULL &&
                 strcmp(source_id->valuestring, "qweather") == 0 &&
                 cJSON_IsObject(data) &&
                 _weather_parse_public_location(root, &snapshot->location);
    uint32_t mask = 0U;
    if (valid)
    {
        switch (kind)
        {
        case WEATHER_SERVICE_KIND_CURRENT:
            valid = _weather_parse_current(root, data, snapshot);
            mask = WEATHER_SERVICE_DATA_CURRENT;
            break;
        case WEATHER_SERVICE_KIND_ALERTS:
            valid = _weather_parse_alerts(root, data, snapshot);
            mask = WEATHER_SERVICE_DATA_ALERTS;
            break;
        case WEATHER_SERVICE_KIND_HOURLY:
            valid = _weather_parse_hourly(root, data, snapshot);
            mask = WEATHER_SERVICE_DATA_HOURLY;
            break;
        case WEATHER_SERVICE_KIND_DAILY:
            valid = _weather_parse_daily(root, data, snapshot);
            mask = WEATHER_SERVICE_DATA_DAILY;
            break;
        default:
            valid = false;
            break;
        }
    }
    if (valid)
    {
        snapshot->location.available = true;
        snapshot->available_mask |= WEATHER_SERVICE_DATA_LOCATION;
        *changed_mask = mask | WEATHER_SERVICE_DATA_LOCATION;
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
