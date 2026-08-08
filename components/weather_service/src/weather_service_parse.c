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
#include <stdatomic.h>
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

static atomic_flag s_weather_parse_initialized = ATOMIC_FLAG_INIT;
static atomic_bool s_weather_parse_ready = ATOMIC_VAR_INIT(false);

void weather_service_parse_init(void)
{
    if (atomic_flag_test_and_set_explicit(&s_weather_parse_initialized,
                                          memory_order_acq_rel))
    {
        while (!atomic_load_explicit(&s_weather_parse_ready,
                                     memory_order_acquire))
        {
            atomic_signal_fence(memory_order_acquire);
        }
        return;
    }
    /* cJSON hooks are process-global; Weather Service owns their one-time setup. */
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
    atomic_store_explicit(&s_weather_parse_ready, true, memory_order_release);
}

static bool _weather_parse_leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static bool _weather_parse_valid_date(int year, unsigned month, unsigned day)
{
    static const uint8_t days_per_month[] =
    {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U,
    };
    if (year < 1970 || year > 2200 || month < 1U || month > 12U)
    {
        return false;
    }
    unsigned maximum = days_per_month[month - 1U];
    if (month == 2U && _weather_parse_leap_year(year))
    {
        maximum = 29U;
    }
    return day >= 1U && day <= maximum;
}

static bool _weather_parse_digits(const char *text, size_t count)
{
    for (size_t index = 0U; index < count; ++index)
    {
        if (!isdigit((unsigned char)text[index]))
        {
            return false;
        }
    }
    return true;
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
    if (strlen(text) < 20U || !_weather_parse_digits(text, 4U) ||
            text[4] != '-' || !_weather_parse_digits(text + 5, 2U) ||
            text[7] != '-' || !_weather_parse_digits(text + 8, 2U) ||
            text[10] != 'T' || !_weather_parse_digits(text + 11, 2U) ||
            text[13] != ':' || !_weather_parse_digits(text + 14, 2U) ||
            text[16] != ':' || !_weather_parse_digits(text + 17, 2U) ||
            sscanf(text, "%d-%u-%uT%u:%u:%u%n", &year, &month, &day,
                   &hour, &minute, &second, &consumed) != 6 ||
            consumed != 19 || !_weather_parse_valid_date(year, month, day) ||
            hour > 23U || minute > 59U || second > 59U)
    {
        return false;
    }
    const char *zone = text + consumed;
    if (*zone == '.')
    {
        ++zone;
        if (!isdigit((unsigned char) * zone))
        {
            return false;
        }
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
        if (strlen(zone) != 6U ||
                (zone[0] != '+' && zone[0] != '-') ||
                !_weather_parse_digits(zone + 1, 2U) || zone[3] != ':' ||
                !_weather_parse_digits(zone + 4, 2U) ||
                sscanf(zone, "%c%d:%d%n", &sign, &zone_hour, &zone_minute,
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

static bool _weather_parse_date_text(const char *text)
{
    if (text == NULL || strlen(text) != 10U ||
            !_weather_parse_digits(text, 4U) || text[4] != '-' ||
            !_weather_parse_digits(text + 5, 2U) || text[7] != '-' ||
            !_weather_parse_digits(text + 8, 2U))
    {
        return false;
    }
    int year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
               (text[2] - '0') * 10 + text[3] - '0';
    unsigned month = (unsigned)(text[5] - '0') * 10U +
                     (unsigned)(text[6] - '0');
    unsigned day = (unsigned)(text[8] - '0') * 10U +
                   (unsigned)(text[9] - '0');
    return _weather_parse_valid_date(year, month, day);
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

static bool _weather_parse_provider(const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        return false;
    }
    size_t length = 0U;
    for (const char *cursor = text; *cursor != '\0'; ++cursor)
    {
        char character = *cursor;
        bool alnum = (character >= 'a' && character <= 'z') ||
                     (character >= '0' && character <= '9');
        bool symbol = character == '.' || character == '_' ||
                      character == '-';
        if (!alnum && !symbol)
        {
            return false;
        }
        if (length == 0U && !alnum)
        {
            return false;
        }
        ++length;
        if (length > 32U)
        {
            return false;
        }
    }
    return true;
}

static bool _weather_parse_location_key(const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        return true;
    }
    if (strlen(text) != 16U)
    {
        return false;
    }
    for (size_t index = 0U; index < 16U; ++index)
    {
        char character = text[index];
        if (!((character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f')))
        {
            return false;
        }
    }
    return true;
}

static bool _weather_parse_location_object(const cJSON *object,
        weather_service_location_t *location)
{
    bool truncated = false;
    char provider[WEATHER_SERVICE_PROVIDER_BYTES];
    char location_key[17];
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(object, "source");
    const cJSON *precision = cJSON_GetObjectItemCaseSensitive(object,
                             "precision");
    if (!cJSON_IsObject(object) || !cJSON_IsString(source) ||
            source->valuestring == NULL ||
            (strcmp(source->valuestring, "ip") != 0 &&
             strcmp(source->valuestring, "device") != 0) ||
            !cJSON_IsString(precision) || precision->valuestring == NULL ||
            (strcmp(precision->valuestring, "coarse") != 0 &&
             strcmp(precision->valuestring, "city") != 0) ||
            !_weather_parse_copy_text(object, "provider", provider,
                                      sizeof(provider), true, &truncated) ||
            truncated || !_weather_parse_provider(provider) ||
            !_weather_parse_copy_text(object, "location_key", location_key,
                                      sizeof(location_key), false,
                                      &truncated) || truncated ||
            !_weather_parse_location_key(location_key))
    {
        return false;
    }
    memcpy(location->provider, provider, sizeof(location->provider));
    memcpy(location->location_key, location_key,
           sizeof(location->location_key));
    return _weather_parse_copy_text(object, "city", location->city,
                                    sizeof(location->city), false, NULL) &&
           _weather_parse_copy_text(object, "district", location->district,
                                    sizeof(location->district), false, NULL) &&
           _weather_parse_copy_text(object, "region", location->region,
                                    sizeof(location->region), false, NULL) &&
           _weather_parse_copy_text(object, "country", location->country,
                                    sizeof(location->country), false, NULL) &&
           _weather_parse_copy_text(object, "timezone", location->timezone,
                                    sizeof(location->timezone), false, NULL);
}

static bool _weather_parse_public_location(const cJSON *root,
        weather_service_location_t *location)
{
    const cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "location");
    return _weather_parse_location_object(object, location);
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
    const cJSON *version = cJSON_IsObject(root) ?
                           cJSON_GetObjectItemCaseSensitive(root,
                               "schema_version") : NULL;
    const cJSON *object = cJSON_IsObject(root) ?
                          cJSON_GetObjectItemCaseSensitive(root, "location") :
                          NULL;
    weather_service_location_t parsed = {0};
    bool valid = cJSON_IsNumber(version) && version->valuedouble == 1.0 &&
                 _weather_parse_location_object(object, &parsed);
    if (valid)
    {
        parsed.acquired_at = acquired_at;
        parsed.available = true;
        *location = parsed;
        LOG_D("location JSON accepted");
    }
    else
    {
        LOG_W("location JSON rejected: bytes=%u",
              (unsigned)body_size);
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
            !_weather_parse_date_text(day->date) ||
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

static esp_err_t _weather_parse_alerts(const cJSON *root, const cJSON *data,
                                       weather_service_snapshot_t *snapshot)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "items");
    const cJSON *source_truncated = cJSON_GetObjectItemCaseSensitive(
                                        data, "truncated");
    if (!cJSON_IsArray(items) || !cJSON_IsBool(source_truncated))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    int source_count = cJSON_GetArraySize(items);
    int count = source_count;
    if (count > (int)WEATHER_SERVICE_MAX_ALERTS)
    {
        count = (int)WEATHER_SERVICE_MAX_ALERTS;
    }
    weather_service_alerts_t *parsed = weather_service_port_psram_calloc(
                                           1U, sizeof(*parsed));
    if (parsed == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    if (!_weather_parse_meta(root, &parsed->meta))
    {
        goto cleanup;
    }
    parsed->truncated = cJSON_IsTrue(source_truncated) || count < source_count;
    for (int index = 0; index < count; ++index)
    {
        if (!_weather_parse_alert(cJSON_GetArrayItem(items, index),
                                  &parsed->items[index]))
        {
            goto cleanup;
        }
    }
    parsed->count = (uint8_t)count;
    snapshot->alerts = *parsed;
    snapshot->available_mask |= WEATHER_SERVICE_DATA_ALERTS;
    result = ESP_OK;

cleanup:
    weather_service_port_psram_free(parsed);
    return result;
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
    bool valid = cJSON_IsNumber(version) && version->valuedouble == 1.0 &&
                 cJSON_IsString(source_id) && source_id->valuestring != NULL &&
                 strcmp(source_id->valuestring, "qweather") == 0 &&
                 cJSON_IsObject(data) &&
                 _weather_parse_public_location(root, &snapshot->location);
    uint32_t mask = 0U;
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    if (valid)
    {
        switch (kind)
        {
        case WEATHER_SERVICE_KIND_CURRENT:
            valid = _weather_parse_current(root, data, snapshot);
            mask = WEATHER_SERVICE_DATA_CURRENT;
            break;
        case WEATHER_SERVICE_KIND_ALERTS:
            result = _weather_parse_alerts(root, data, snapshot);
            valid = result == ESP_OK;
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
        result = ESP_OK;
    }
    cJSON_Delete(root);
    return result;
}
