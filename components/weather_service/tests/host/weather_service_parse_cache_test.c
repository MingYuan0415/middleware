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
    "{\"ip\":\"192.0.2.1\",\"asn\":{\"asn\":64500},"
    "\"location\":{\"city\":\"Shenzhen\",\"state\":\"Guangdong\","
    "\"country_code\":\"CN\",\"timezone\":\"Asia/Shanghai\","
    "\"latitude\":22.5431,\"longitude\":114.0579}}";

static const char s_envelope_prefix[] =
    "{\"schema_version\":1,\"source\":{\"id\":\"qweather\"},"
    "\"location\":{\"city\":\"Shenzhen\",\"region\":\"Guangdong\","
    "\"country\":\"CN\",\"timezone\":\"Asia/Shanghai\","
    "\"source\":\"device\",\"provider\":\"ipapi.is\","
    "\"precision\":\"city\"},\"fetched_at\":\"2026-08-05T00:00:00Z\","
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
    CHECK(location.latitude_tenths == 225);
    CHECK(location.longitude_tenths == 1141);
    CHECK(strcmp(location.city, "Shenzhen") == 0);
    CHECK(strcmp(location.region, "Guangdong") == 0);
    CHECK(strcmp(location.country, "CN") == 0);
    CHECK(strcmp(location.timezone, "Asia/Shanghai") == 0);
    CHECK(strcmp(location.provider, "ipapi.is") == 0);
    CHECK(location.acquired_at == 12345);

    const char invalid[] =
        "{\"location\":{\"latitude\":91,\"longitude\":0,"
        "\"country_code\":\"CN\",\"timezone\":\"Asia/Shanghai\"}}";
    CHECK(weather_service_parse_location((const uint8_t *)invalid,
                                         strlen(invalid), 0, &location) ==
          ESP_ERR_INVALID_RESPONSE);
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
    source.location.latitude_tenths = 225;
    source.location.longitude_tenths = 1141;
    memcpy(source.location.city, "Shenzhen", sizeof("Shenzhen"));
    memcpy(source.location.provider, "ipapi.is", sizeof("ipapi.is"));
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

int main(void)
{
    weather_service_parse_init();
    _test_location();
    _test_weather();
    _test_alert_boundaries();
    _test_cache();
    printf("weather service: %u checks, %u failures\n", s_checks, s_failures);
    return s_failures == 0U ? 0 : 1;
}
