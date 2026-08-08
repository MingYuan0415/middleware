#include "weather_service_internal.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned s_checks;
static unsigned s_failures;
static unsigned s_alert_allocations;
static bool s_fail_alert_allocation;

#define CHECK(condition) _check((condition), #condition, __LINE__)

void *weather_service_port_psram_calloc(size_t count, size_t size)
{
    ++s_alert_allocations;
    if (s_fail_alert_allocation)
    {
        return NULL;
    }
    return calloc(count, size);
}

void weather_service_port_psram_free(void *memory)
{
    free(memory);
}

static void _check(bool condition, const char *expression, int line)
{
    ++s_checks;
    if (!condition)
    {
        ++s_failures;
        fprintf(stderr, "line %d: CHECK(%s) failed\n", line, expression);
    }
}

static const char s_location_json[] =
    "{\"schema_version\":1,\"location\":{\"city\":\"Shenzhen\","
    "\"district\":\"Nanshan\",\"region\":\"Guangdong\",\"country\":\"CN\","
    "\"timezone\":\"Asia/Shanghai\",\"source\":\"ip\","
    "\"provider\":\"maxmind\",\"precision\":\"coarse\","
    "\"location_key\":\"9f4a2b3c8d1e5f06\"},"
    "\"accuracy_radius_km\":50}";

static const char s_envelope_prefix[] =
    "{\"schema_version\":1,\"source\":{\"id\":\"qweather\"},"
    "\"location\":{\"city\":\"Shenzhen\",\"district\":\"Nanshan\","
    "\"region\":\"Guangdong\","
    "\"country\":\"CN\",\"timezone\":\"Asia/Shanghai\","
    "\"source\":\"ip\",\"provider\":\"maxmind\","
    "\"precision\":\"coarse\",\"location_key\":\"9f4a2b3c8d1e5f06\"},"
    "\"fetched_at\":\"2026-08-05T00:00:00Z\","
    "\"updated_at\":\"2026-08-05T07:55:00+08:00\","
    "\"valid_until\":\"2026-08-05T08:20:00+08:00\",\"stale\":false,"
    "\"data\":";

static esp_err_t _parse(weather_service_kind_t kind, const char *data,
                        weather_service_snapshot_t *snapshot,
                        uint32_t *changed_mask)
{
    size_t length = strlen(s_envelope_prefix) + strlen(data) + 2U;
    char *json = malloc(length);
    if (json == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    int count = snprintf(json, length, "%s%s}", s_envelope_prefix, data);
    esp_err_t result = count < 0 || (size_t)count >= length ?
                       ESP_ERR_INVALID_SIZE :
                       weather_service_parse_weather(kind,
                           (const uint8_t *)json, (size_t)count,
                           snapshot, changed_mask);
    free(json);
    return result;
}

static char *_build_alerts(unsigned count, int invalid_index)
{
    static const char item[] =
        "%s{\"id\":\"alert-%u\",\"title\":\"Warning %u\","
        "\"type_name\":\"Weather\",\"severity\":\"moderate\","
        "\"status\":\"active\","
        "\"issued_at\":\"2026-08-05T07:00:00+08:00\","
        "\"description\":\"Details\",\"instruction\":\"Advice\","
        "\"content_truncated\":false}";
    size_t capacity = 256U + (size_t)count * (sizeof(item) + 32U);
    char *json = calloc(1U, capacity);
    if (json == NULL)
    {
        return NULL;
    }
    size_t used = (size_t)snprintf(json, capacity,
                                   "{\"truncated\":false,\"items\":[");
    for (unsigned index = 0U; index < count; ++index)
    {
        int written;
        if ((int)index == invalid_index)
        {
            written = snprintf(json + used, capacity - used,
                               "%s{\"title\":1}", index == 0U ? "" : ",");
        }
        else
        {
            written = snprintf(json + used, capacity - used, item,
                               index == 0U ? "" : ",", index, index);
        }
        if (written < 0 || (size_t)written >= capacity - used)
        {
            free(json);
            return NULL;
        }
        used += (size_t)written;
    }
    if (snprintf(json + used, capacity - used, "]}") != 2)
    {
        free(json);
        return NULL;
    }
    return json;
}

static void _test_alert_boundaries(void)
{
    weather_service_snapshot_t snapshot = {0};
    uint32_t changed = 0U;
    s_alert_allocations = 0U;
    CHECK(_parse(WEATHER_SERVICE_KIND_ALERTS,
                 "{\"truncated\":false,\"items\":[]}",
                 &snapshot, &changed) == ESP_OK);
    CHECK(snapshot.alerts.count == 0U);
    CHECK(s_alert_allocations == 1U);

    char *alerts = _build_alerts(WEATHER_SERVICE_MAX_ALERTS, -1);
    CHECK(alerts != NULL);
    if (alerts != NULL)
    {
        CHECK(_parse(WEATHER_SERVICE_KIND_ALERTS, alerts, &snapshot,
                     &changed) == ESP_OK);
        CHECK(snapshot.alerts.count == WEATHER_SERVICE_MAX_ALERTS);
        CHECK(!snapshot.alerts.truncated);
        free(alerts);
    }

    alerts = _build_alerts(WEATHER_SERVICE_MAX_ALERTS + 1U, -1);
    CHECK(alerts != NULL);
    if (alerts != NULL)
    {
        CHECK(_parse(WEATHER_SERVICE_KIND_ALERTS, alerts, &snapshot,
                     &changed) == ESP_OK);
        CHECK(snapshot.alerts.count == WEATHER_SERVICE_MAX_ALERTS);
        CHECK(snapshot.alerts.truncated);
        free(alerts);
    }

    weather_service_alerts_t previous = snapshot.alerts;
    changed = UINT32_C(0xA5A5);
    alerts = _build_alerts(3U, 1);
    CHECK(alerts != NULL);
    if (alerts != NULL)
    {
        CHECK(_parse(WEATHER_SERVICE_KIND_ALERTS, alerts, &snapshot,
                     &changed) == ESP_ERR_INVALID_RESPONSE);
        CHECK(memcmp(&snapshot.alerts, &previous, sizeof(previous)) == 0);
        CHECK(changed == UINT32_C(0xA5A5));
        free(alerts);
    }

    s_fail_alert_allocation = true;
    CHECK(_parse(WEATHER_SERVICE_KIND_ALERTS,
                 "{\"truncated\":false,\"items\":[]}",
                 &snapshot, &changed) == ESP_ERR_NO_MEM);
    s_fail_alert_allocation = false;
    CHECK(memcmp(&snapshot.alerts, &previous, sizeof(previous)) == 0);
    CHECK(changed == UINT32_C(0xA5A5));

    char description[WEATHER_SERVICE_ALERT_TEXT_BYTES + 80U];
    memset(description, 'x', sizeof(description) - 1U);
    description[sizeof(description) - 1U] = '\0';
    size_t capacity = strlen(description) + 1024U;
    alerts = malloc(capacity);
    CHECK(alerts != NULL);
    if (alerts != NULL)
    {
        int written = snprintf(alerts, capacity,
                               "{\"truncated\":false,\"items\":[{"
                               "\"id\":\"long\",\"title\":\"Warning\","
                               "\"type_name\":\"Weather\","
                               "\"severity\":\"moderate\","
                               "\"status\":\"active\","
                               "\"issued_at\":\"2026-08-05T07:00:00+08:00\","
                               "\"description\":\"%s\","
                               "\"instruction\":\"Advice\","
                               "\"content_truncated\":false}]}", description);
        CHECK(written > 0 && (size_t)written < capacity);
        if (written > 0 && (size_t)written < capacity)
        {
            CHECK(_parse(WEATHER_SERVICE_KIND_ALERTS, alerts, &snapshot,
                         &changed) == ESP_OK);
            CHECK(snapshot.alerts.items[0].content_truncated);
            CHECK(strlen(snapshot.alerts.items[0].description) ==
                  WEATHER_SERVICE_ALERT_TEXT_BYTES - 1U);
        }
        free(alerts);
    }
}

static void _test_location(void)
{
    weather_service_location_t location = {0};
    CHECK(weather_service_parse_location((const uint8_t *)s_location_json,
                                         strlen(s_location_json), 12345,
                                         &location) == ESP_OK);
    CHECK(location.available);
    CHECK(strcmp(location.city, "Shenzhen") == 0);
    CHECK(strcmp(location.district, "Nanshan") == 0);
    CHECK(strcmp(location.region, "Guangdong") == 0);
    CHECK(strcmp(location.country, "CN") == 0);
    CHECK(strcmp(location.timezone, "Asia/Shanghai") == 0);
    CHECK(strcmp(location.provider, "maxmind") == 0);
    CHECK(strcmp(location.location_key, "9f4a2b3c8d1e5f06") == 0);
    CHECK(location.acquired_at == 12345);

    const char no_key[] =
        "{\"schema_version\":1,\"location\":{\"source\":\"ip\","
        "\"provider\":\"maxmind\",\"precision\":\"coarse\"}}";
    CHECK(weather_service_parse_location((const uint8_t *)no_key,
                                         strlen(no_key), 0, &location) ==
          ESP_OK);
    CHECK(location.location_key[0] == '\0');
    CHECK(location.district[0] == '\0');

    char long_district[WEATHER_SERVICE_DISTRICT_BYTES + 32U];
    memset(long_district, 'x', sizeof(long_district) - 1U);
    long_district[sizeof(long_district) - 1U] = '\0';
    char district_json[320];
    int district_count = snprintf(
                             district_json, sizeof(district_json),
                             "{\"schema_version\":1,\"location\":{"
                             "\"source\":\"ip\",\"provider\":\"maxmind\","
                             "\"precision\":\"coarse\","
                             "\"district\":\"%s\"}}", long_district);
    CHECK(district_count > 0 && (size_t)district_count <
          sizeof(district_json));
    CHECK(weather_service_parse_location(
              (const uint8_t *)district_json, (size_t)district_count, 0,
              &location) == ESP_OK);
    CHECK(strlen(location.district) ==
          WEATHER_SERVICE_DISTRICT_BYTES - 1U);

    static const char *const invalid[] =
    {
        "{\"schema_version\":2,\"location\":{\"source\":\"ip\","
        "\"provider\":\"maxmind\",\"precision\":\"coarse\"}}",
        "{\"schema_version\":1.5,\"location\":{\"source\":\"ip\","
        "\"provider\":\"maxmind\",\"precision\":\"coarse\"}}",
        "{\"schema_version\":1,\"location\":{\"source\":\"ip\","
        "\"provider\":\"maxmind\",\"precision\":\"coarse\","
        "\"district\":\"\\u0085Nanshan\"}}",
        "{\"schema_version\":1,\"location\":{\"source\":\"ip\","
        "\"provider\":\"maxmind\",\"precision\":\"coarse\","
        "\"district\":\"Nans\\u009Fhan\"}}",
        "{\"schema_version\":1,\"location\":{\"source\":\"gps\","
        "\"provider\":\"maxmind\",\"precision\":\"coarse\"}}",
        "{\"schema_version\":1,\"location\":{\"source\":\"ip\","
        "\"provider\":\"maxmind\",\"precision\":\"exact\"}}",
        "{\"schema_version\":1,\"location\":{\"source\":\"ip\","
        "\"provider\":\"MaxMind\",\"precision\":\"coarse\"}}",
        "{\"schema_version\":1,\"location\":{\"source\":\"ip\","
        "\"precision\":\"coarse\"}}",
        "{\"schema_version\":1,\"location\":{\"source\":\"ip\","
        "\"provider\":\"maxmind\",\"precision\":\"coarse\","
        "\"location_key\":\"9f4a2b3c8d1e5f0\"}}",
        "{\"schema_version\":1,\"location\":{\"source\":\"ip\","
        "\"provider\":\"maxmind\",\"precision\":\"coarse\","
        "\"location_key\":\"9F4A2B3C8D1E5F06\"}}",
        "{\"schema_version\":1,\"location\":{\"source\":\"ip\","
        "\"provider\":\"maxmind\",\"precision\":\"coarse\","
        "\"location_key\":\"9f4a2b3c8d1e5f0g\"}}",
        "{\"schema_version\":1}",
        "{}",
    };
    for (size_t index = 0U;
            index < sizeof(invalid) / sizeof(invalid[0]); ++index)
    {
        CHECK(weather_service_parse_location(
                  (const uint8_t *)invalid[index], strlen(invalid[index]),
                  0, &location) == ESP_ERR_INVALID_RESPONSE);
    }

    static const size_t provider_lengths[] = {15U, 16U, 32U, 33U};
    for (size_t index = 0U;
            index < sizeof(provider_lengths) / sizeof(provider_lengths[0]);
            ++index)
    {
        size_t length = provider_lengths[index];
        char provider[40];
        memset(provider, 'a', length);
        provider[length] = '\0';
        char json[192];
        int count = snprintf(json, sizeof(json),
                             "{\"schema_version\":1,\"location\":{"
                             "\"source\":\"ip\",\"provider\":\"%s\","
                             "\"precision\":\"coarse\"}}", provider);
        CHECK(count > 0 && (size_t)count < sizeof(json));
        esp_err_t expected = length <= 32U ? ESP_OK :
                             ESP_ERR_INVALID_RESPONSE;
        CHECK(weather_service_parse_location((const uint8_t *)json,
                                             (size_t)count, 0, &location) ==
              expected);
        if (expected == ESP_OK)
        {
            CHECK(strlen(location.provider) == length);
        }
    }
}

static void _test_weather(void)
{
    weather_service_snapshot_t snapshot = {0};
    uint32_t changed = 0U;
    const char current[] =
        "{\"observed_at\":\"2026-08-05T07:50:00+08:00\","
        "\"temperature_c\":31.2,\"feels_like_c\":35.6,"
        "\"condition_code\":\"101\",\"condition_text\":\"Cloudy\","
        "\"wind_degrees\":135,\"wind_speed_kmh\":12.3,"
        "\"wind_direction\":\"SE\",\"wind_scale\":\"3\","
        "\"humidity_percent\":72,\"precipitation_mm\":0.2,"
        "\"pressure_hpa\":1004,\"visibility_km\":18.5}";
    CHECK(_parse(WEATHER_SERVICE_KIND_CURRENT, current, &snapshot,
                 &changed) == ESP_OK);
    CHECK(changed == (WEATHER_SERVICE_DATA_CURRENT |
                      WEATHER_SERVICE_DATA_LOCATION));
    CHECK(snapshot.current.temperature_tenths_c == 312);
    CHECK(snapshot.current.feels_like_tenths_c == 356);
    CHECK(snapshot.current.condition_code == 101U);
    CHECK(snapshot.current.humidity_percent == 72U);
    CHECK(snapshot.current.observed_at.epoch_seconds == 1785887400);

    const char leap_day[] =
        "{\"observed_at\":\"2024-02-29T23:59:59Z\","
        "\"temperature_c\":31.2,\"feels_like_c\":35.6,"
        "\"condition_code\":\"101\",\"condition_text\":\"Cloudy\","
        "\"wind_degrees\":135,\"wind_speed_kmh\":12.3,"
        "\"wind_direction\":\"SE\",\"wind_scale\":\"3\","
        "\"humidity_percent\":72,\"precipitation_mm\":0.2,"
        "\"pressure_hpa\":1004,\"visibility_km\":18.5}";
    CHECK(_parse(WEATHER_SERVICE_KIND_CURRENT, leap_day, &snapshot,
                 &changed) == ESP_OK);

    const char impossible_time[] =
        "{\"observed_at\":\"2026-02-29T00:00:00Z\","
        "\"temperature_c\":31.2,\"feels_like_c\":35.6,"
        "\"condition_code\":\"101\",\"condition_text\":\"Cloudy\","
        "\"wind_degrees\":135,\"wind_speed_kmh\":12.3,"
        "\"wind_direction\":\"SE\",\"wind_scale\":\"3\","
        "\"humidity_percent\":72,\"precipitation_mm\":0.2,"
        "\"pressure_hpa\":1004,\"visibility_km\":18.5}";
    CHECK(_parse(WEATHER_SERVICE_KIND_CURRENT, impossible_time, &snapshot,
                 &changed) == ESP_ERR_INVALID_RESPONSE);

    const char hourly[] =
        "{\"hours\":[{\"forecast_at\":\"2026-08-05T09:00:00+08:00\","
        "\"temperature_c\":32,\"condition_code\":\"100\","
        "\"condition_text\":\"Sunny\",\"wind_speed_kmh\":10,"
        "\"wind_direction\":\"S\",\"humidity_percent\":65,"
        "\"precipitation_chance_percent\":10,\"precipitation_mm\":0}]}";
    CHECK(_parse(WEATHER_SERVICE_KIND_HOURLY, hourly, &snapshot,
                 &changed) == ESP_OK);
    CHECK(snapshot.hourly.count == 1U);
    CHECK(snapshot.hourly.items[0].temperature_tenths_c == 320);

    const char daily[] =
        "{\"days\":[{\"date\":\"2026-08-05\","
        "\"temperature_min_c\":27,\"temperature_max_c\":34,"
        "\"condition_day_code\":\"100\",\"condition_night_code\":\"150\","
        "\"condition_day_text\":\"Sunny\",\"condition_night_text\":\"Clear\","
        "\"humidity_percent\":70,\"precipitation_mm\":0,"
        "\"visibility_km\":20,\"uv_index\":8}]}";
    CHECK(_parse(WEATHER_SERVICE_KIND_DAILY, daily, &snapshot,
                 &changed) == ESP_OK);
    CHECK(snapshot.daily.count == 1U);
    CHECK(snapshot.daily.items[0].maximum_temperature_tenths_c == 340);

    const char impossible_daily[] =
        "{\"days\":[{\"date\":\"2026-02-31\","
        "\"temperature_min_c\":27,\"temperature_max_c\":34,"
        "\"condition_day_code\":\"100\","
        "\"condition_night_code\":\"150\","
        "\"condition_day_text\":\"Sunny\","
        "\"condition_night_text\":\"Clear\","
        "\"humidity_percent\":70,\"precipitation_mm\":0,"
        "\"visibility_km\":20,\"uv_index\":8}]}";
    CHECK(_parse(WEATHER_SERVICE_KIND_DAILY, impossible_daily, &snapshot,
                 &changed) == ESP_ERR_INVALID_RESPONSE);

    const char alerts[] =
        "{\"truncated\":false,\"items\":[{\"id\":\"alert-1\","
        "\"title\":\"Heat warning\",\"type_name\":\"High temperature\","
        "\"severity\":\"severe\",\"status\":\"active\","
        "\"issued_at\":\"2026-08-05T07:00:00+08:00\","
        "\"starts_at\":\"2026-08-05T08:00:00+08:00\","
        "\"ends_at\":\"2026-08-05T18:00:00+08:00\","
        "\"description\":\"Hot weather expected\","
        "\"instruction\":\"Stay hydrated\",\"content_truncated\":false}]}";
    CHECK(_parse(WEATHER_SERVICE_KIND_ALERTS, alerts, &snapshot,
                 &changed) == ESP_OK);
    CHECK(snapshot.alerts.count == 1U);
    CHECK(snapshot.alerts.items[0].key != 0U);
    CHECK(strcmp(snapshot.alerts.items[0].title, "Heat warning") == 0);

    const char wrong_source[] =
        "{\"schema_version\":1,\"source\":{\"id\":\"other\"},"
        "\"location\":{},\"data\":{}}";
    CHECK(weather_service_parse_weather(WEATHER_SERVICE_KIND_CURRENT,
                                        (const uint8_t *)wrong_source,
                                        strlen(wrong_source), &snapshot,
                                        &changed) == ESP_ERR_INVALID_RESPONSE);

    const char fractional_version[] =
        "{\"schema_version\":1.5,\"source\":{\"id\":\"qweather\"},"
        "\"location\":{\"source\":\"ip\",\"provider\":\"maxmind\","
        "\"precision\":\"coarse\"},"
        "\"fetched_at\":\"2026-08-05T00:00:00Z\","
        "\"updated_at\":\"2026-08-05T07:55:00+08:00\","
        "\"valid_until\":\"2026-08-05T08:20:00+08:00\",\"stale\":false,"
        "\"data\":{}}";
    CHECK(weather_service_parse_weather(WEATHER_SERVICE_KIND_CURRENT,
                                        (const uint8_t *)fractional_version,
                                        strlen(fractional_version), &snapshot,
                                        &changed) == ESP_ERR_INVALID_RESPONSE);

    const char c1_district[] =
        "{\"schema_version\":1,\"source\":{\"id\":\"qweather\"},"
        "\"location\":{\"source\":\"ip\",\"provider\":\"maxmind\","
        "\"precision\":\"coarse\",\"district\":\"Nans\\u0085han\"},"
        "\"fetched_at\":\"2026-08-05T00:00:00Z\","
        "\"updated_at\":\"2026-08-05T07:55:00+08:00\","
        "\"valid_until\":\"2026-08-05T08:20:00+08:00\",\"stale\":false,"
        "\"data\":{}}";
    CHECK(weather_service_parse_weather(WEATHER_SERVICE_KIND_CURRENT,
                                        (const uint8_t *)c1_district,
                                        strlen(c1_district), &snapshot,
                                        &changed) == ESP_ERR_INVALID_RESPONSE);

    const char wrong_provider[] =
        "{\"schema_version\":1,\"source\":{\"id\":\"qweather\"},"
        "\"location\":{\"source\":\"ip\",\"provider\":\"Max Mind\","
        "\"precision\":\"coarse\"},"
        "\"fetched_at\":\"2026-08-05T00:00:00Z\","
        "\"updated_at\":\"2026-08-05T07:55:00+08:00\","
        "\"valid_until\":\"2026-08-05T08:20:00+08:00\",\"stale\":false,"
        "\"data\":{}}";
    CHECK(weather_service_parse_weather(WEATHER_SERVICE_KIND_CURRENT,
                                        (const uint8_t *)wrong_provider,
                                        strlen(wrong_provider), &snapshot,
                                        &changed) == ESP_ERR_INVALID_RESPONSE);

    const char missing_source[] =
        "{\"schema_version\":1,\"source\":{\"id\":\"qweather\"},"
        "\"location\":{\"provider\":\"maxmind\"},"
        "\"fetched_at\":\"2026-08-05T00:00:00Z\","
        "\"updated_at\":\"2026-08-05T07:55:00+08:00\","
        "\"valid_until\":\"2026-08-05T08:20:00+08:00\",\"stale\":false,"
        "\"data\":{}}";
    CHECK(weather_service_parse_weather(WEATHER_SERVICE_KIND_CURRENT,
                                        (const uint8_t *)missing_source,
                                        strlen(missing_source), &snapshot,
                                        &changed) == ESP_ERR_INVALID_RESPONSE);
}

static void _test_cache(void)
{
    char directory[] = "/tmp/mt-weather-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    weather_service_snapshot_t source = {0};
    source.generation = 7U;
    source.available_mask = WEATHER_SERVICE_DATA_LOCATION |
                            WEATHER_SERVICE_DATA_CURRENT;
    source.location.available = true;
    memcpy(source.location.city, "Shenzhen", sizeof("Shenzhen"));
    memcpy(source.location.district, "Nanshan", sizeof("Nanshan"));
    memcpy(source.location.provider, "maxmind", sizeof("maxmind"));
    memcpy(source.location.location_key, "9f4a2b3c8d1e5f06",
           sizeof("9f4a2b3c8d1e5f06"));
    source.current.meta.available = true;
    source.current.temperature_tenths_c = 312;
    memcpy(source.current.condition_text, "Cloudy", sizeof("Cloudy"));

    CHECK(weather_service_cache_store(directory, &source, 1U) == ESP_OK);
    source.generation = 8U;
    source.current.temperature_tenths_c = 320;
    CHECK(weather_service_cache_store(directory, &source, 2U) == ESP_OK);

    weather_service_snapshot_t loaded = {0};
    uint64_t sequence = 0U;
    CHECK(weather_service_cache_load(directory, &loaded, &sequence) == ESP_OK);
    CHECK(sequence == 2U);
    CHECK(loaded.generation == 8U);
    CHECK(loaded.current.temperature_tenths_c == 320);
    CHECK(strcmp(loaded.location.city, "Shenzhen") == 0);
    CHECK(loaded.location.location_key[0] == '\0');
    CHECK(loaded.location.district[0] == '\0');

    char path[128];
    int count = snprintf(path, sizeof(path), "%s/weather_b.bin", directory);
    CHECK(count > 0 && (size_t)count < sizeof(path));
    int descriptor = open(path, O_WRONLY | O_TRUNC);
    CHECK(descriptor >= 0);
    if (descriptor >= 0)
    {
        CHECK(write(descriptor, "bad", 3U) == 3);
        CHECK(close(descriptor) == 0);
    }
    memset(&loaded, 0, sizeof(loaded));
    sequence = 0U;
    CHECK(weather_service_cache_load(directory, &loaded, &sequence) == ESP_OK);
    CHECK(sequence == 1U);
    CHECK(loaded.generation == 7U);

    (void)snprintf(path, sizeof(path), "%s/weather_a.bin", directory);
    descriptor = open(path, O_WRONLY | O_APPEND);
    CHECK(descriptor >= 0);
    if (descriptor >= 0)
    {
        CHECK(write(descriptor, "x", 1U) == 1);
        CHECK(close(descriptor) == 0);
    }
    memset(&loaded, 0, sizeof(loaded));
    sequence = 0U;
    CHECK(weather_service_cache_load(directory, &loaded, &sequence) ==
          ESP_ERR_NOT_FOUND);

    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/weather_b.bin", directory);
    (void)unlink(path);
    CHECK(rmdir(directory) == 0);

    char long_directory[300];
    memset(long_directory, 'a', sizeof(long_directory));
    long_directory[sizeof(long_directory) - 1U] = '\0';
    CHECK(weather_service_cache_load(long_directory, &loaded, &sequence) ==
          ESP_ERR_INVALID_SIZE);
    CHECK(weather_service_cache_store(long_directory, &source, 3U) ==
          ESP_ERR_INVALID_SIZE);

    source.hourly.count = WEATHER_SERVICE_MAX_HOURS + 1U;
    CHECK(weather_service_cache_store("/tmp", &source, 3U) ==
          ESP_ERR_INVALID_SIZE);
}

static uint32_t _crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0U; index < size; ++index)
    {
        crc ^= data[index];
        for (unsigned bit = 0U; bit < 8U; ++bit)
        {
            uint32_t mask = (uint32_t) - (int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static void _test_cache_legacy_coordinate_slots(void)
{
    char directory[] = "/tmp/mt-weather-legacy-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    weather_service_snapshot_t source = {0};
    source.generation = 7U;
    source.available_mask = WEATHER_SERVICE_DATA_LOCATION |
                            WEATHER_SERVICE_DATA_CURRENT;
    source.location.available = true;
    memcpy(source.location.city, "Shenzhen", sizeof("Shenzhen"));
    memcpy(source.location.region, "Guangdong", sizeof("Guangdong"));
    memcpy(source.location.country, "CN", sizeof("CN"));
    memcpy(source.location.timezone, "Asia/Shanghai",
           sizeof("Asia/Shanghai"));
    memcpy(source.location.provider, "maxmind", sizeof("maxmind"));
    source.location.acquired_at = 1000;
    source.current.meta.available = true;
    source.current.temperature_tenths_c = 312;
    memcpy(source.current.condition_text, "Cloudy", sizeof("Cloudy"));
    CHECK(weather_service_cache_store(directory, &source, 1U) == ESP_OK);

    char path[128];
    int count = snprintf(path, sizeof(path), "%s/weather_a.bin", directory);
    CHECK(count > 0 && (size_t)count < sizeof(path));
    int descriptor = open(path, O_RDONLY);
    CHECK(descriptor >= 0);
    if (descriptor < 0)
    {
        return;
    }
    struct stat info;
    CHECK(fstat(descriptor, &info) == 0);
    size_t file_size = (size_t)info.st_size;
    uint8_t *file = malloc(file_size);
    CHECK(file != NULL);
    if (file != NULL)
    {
        CHECK(read(descriptor, file, file_size) == (ssize_t)file_size);
    }
    (void)close(descriptor);
    if (file == NULL)
    {
        return;
    }
    const size_t header_bytes = 24U;
    size_t latitude_offset = header_bytes + 8U + 4U;
    latitude_offset += 2U + strlen("Shenzhen");
    latitude_offset += 2U + strlen("Guangdong");
    latitude_offset += 2U + strlen("CN");
    latitude_offset += 2U + strlen("Asia/Shanghai");
    latitude_offset += 2U + strlen("maxmind");
    file[latitude_offset] = (uint8_t)225U;
    file[latitude_offset + 1U] = (uint8_t)(225U >> 8U);
    file[latitude_offset + 2U] = (uint8_t)1141U;
    file[latitude_offset + 3U] = (uint8_t)(1141U >> 8U);
    size_t payload_size = file_size - header_bytes;
    uint32_t crc = _crc32(file + header_bytes, payload_size);
    file[20U] = (uint8_t)crc;
    file[21U] = (uint8_t)(crc >> 8U);
    file[22U] = (uint8_t)(crc >> 16U);
    file[23U] = (uint8_t)(crc >> 24U);
    descriptor = open(path, O_WRONLY | O_TRUNC);
    CHECK(descriptor >= 0 && write(descriptor, file, file_size) ==
          (ssize_t)file_size);
    (void)close(descriptor);
    free(file);

    weather_service_snapshot_t loaded = {0};
    uint64_t sequence = 0U;
    CHECK(weather_service_cache_load(directory, &loaded, &sequence) == ESP_OK);
    CHECK(sequence == 1U);
    CHECK(strcmp(loaded.location.city, "Shenzhen") == 0);
    CHECK(strcmp(loaded.location.provider, "maxmind") == 0);
    CHECK(loaded.location.acquired_at == 1000);
    CHECK(loaded.location.available);
    CHECK(loaded.location.location_key[0] == '\0');
    CHECK(loaded.current.temperature_tenths_c == 312);
    CHECK((loaded.available_mask & WEATHER_SERVICE_DATA_CURRENT) != 0U);

    CHECK(weather_service_cache_store(directory, &loaded, 2U) == ESP_OK);
    memset(&loaded, 0, sizeof(loaded));
    sequence = 0U;
    CHECK(weather_service_cache_load(directory, &loaded, &sequence) == ESP_OK);
    CHECK(sequence == 2U);
    CHECK(strcmp(loaded.location.provider, "maxmind") == 0);
    CHECK(loaded.location.acquired_at == 1000);
    CHECK(loaded.current.temperature_tenths_c == 312);

    (void)snprintf(path, sizeof(path), "%s/weather_a.bin", directory);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/weather_b.bin", directory);
    (void)unlink(path);
    CHECK(rmdir(directory) == 0);
}

int main(void)
{
    weather_service_parse_init();
    _test_location();
    _test_weather();
    _test_alert_boundaries();
    _test_cache();
    _test_cache_legacy_coordinate_slots();
    printf("weather service: %u checks, %u failures\n", s_checks, s_failures);
    return s_failures == 0U ? 0 : 1;
}
