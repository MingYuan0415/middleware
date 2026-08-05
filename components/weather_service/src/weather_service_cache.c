#define DBG_TAG "weather_cache"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "weather_service_internal.h"

#ifdef ESP_PLATFORM
    #include "esp_heap_caps.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEATHER_CACHE_MAGIC        UINT32_C(0x3157544D)
#define WEATHER_CACHE_VERSION      1U
#define WEATHER_CACHE_HEADER_BYTES 24U

typedef struct weather_cache_writer
{
    uint8_t *data;
    size_t capacity;
    size_t position;
    bool valid;
} weather_cache_writer_t;

typedef struct weather_cache_reader
{
    const uint8_t *data;
    size_t size;
    size_t position;
    bool valid;
} weather_cache_reader_t;

static void *_weather_cache_allocate(size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}

static void _weather_cache_release(void *memory)
{
#ifdef ESP_PLATFORM
    heap_caps_free(memory);
#else
    free(memory);
#endif
}

static void _weather_cache_put_u8(weather_cache_writer_t *writer,
                                  uint8_t value)
{
    if (!writer->valid || writer->position >= writer->capacity)
    {
        writer->valid = false;
        return;
    }
    writer->data[writer->position++] = value;
}

static void _weather_cache_put_u16(weather_cache_writer_t *writer,
                                   uint16_t value)
{
    _weather_cache_put_u8(writer, (uint8_t)value);
    _weather_cache_put_u8(writer, (uint8_t)(value >> 8U));
}

static void _weather_cache_put_u32(weather_cache_writer_t *writer,
                                   uint32_t value)
{
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
    {
        _weather_cache_put_u8(writer, (uint8_t)(value >> shift));
    }
}

static void _weather_cache_put_u64(weather_cache_writer_t *writer,
                                   uint64_t value)
{
    for (unsigned shift = 0U; shift < 64U; shift += 8U)
    {
        _weather_cache_put_u8(writer, (uint8_t)(value >> shift));
    }
}

static void _weather_cache_put_string(weather_cache_writer_t *writer,
                                      const char *text, size_t maximum)
{
    size_t length = strnlen(text, maximum);
    if (length >= maximum || length > UINT16_MAX)
    {
        writer->valid = false;
        return;
    }
    _weather_cache_put_u16(writer, (uint16_t)length);
    for (size_t index = 0U; index < length; ++index)
    {
        _weather_cache_put_u8(writer, (uint8_t)text[index]);
    }
}

static uint8_t _weather_cache_get_u8(weather_cache_reader_t *reader)
{
    if (!reader->valid || reader->position >= reader->size)
    {
        reader->valid = false;
        return 0U;
    }
    return reader->data[reader->position++];
}

static uint16_t _weather_cache_get_u16(weather_cache_reader_t *reader)
{
    uint16_t value = _weather_cache_get_u8(reader);
    value |= (uint16_t)_weather_cache_get_u8(reader) << 8U;
    return value;
}

static uint32_t _weather_cache_get_u32(weather_cache_reader_t *reader)
{
    uint32_t value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
    {
        value |= (uint32_t)_weather_cache_get_u8(reader) << shift;
    }
    return value;
}

static uint64_t _weather_cache_get_u64(weather_cache_reader_t *reader)
{
    uint64_t value = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U)
    {
        value |= (uint64_t)_weather_cache_get_u8(reader) << shift;
    }
    return value;
}

static void _weather_cache_get_string(weather_cache_reader_t *reader,
                                      char *text, size_t maximum)
{
    uint16_t length = _weather_cache_get_u16(reader);
    if (!reader->valid || length >= maximum ||
            reader->position + length > reader->size)
    {
        reader->valid = false;
        return;
    }
    memcpy(text, reader->data + reader->position, length);
    text[length] = '\0';
    reader->position += length;
}

static void _weather_cache_put_time(weather_cache_writer_t *writer,
                                    const weather_service_time_t *value)
{
    _weather_cache_put_u64(writer, (uint64_t)value->epoch_seconds);
    _weather_cache_put_u16(writer, (uint16_t)value->offset_minutes);
}

static void _weather_cache_get_time(weather_cache_reader_t *reader,
                                    weather_service_time_t *value)
{
    value->epoch_seconds = (int64_t)_weather_cache_get_u64(reader);
    value->offset_minutes = (int16_t)_weather_cache_get_u16(reader);
}

static void _weather_cache_put_meta(weather_cache_writer_t *writer,
                                    const weather_service_dataset_meta_t *meta)
{
    _weather_cache_put_time(writer, &meta->fetched_at);
    _weather_cache_put_time(writer, &meta->updated_at);
    _weather_cache_put_time(writer, &meta->valid_until);
    _weather_cache_put_u8(writer, meta->available ? 1U : 0U);
    _weather_cache_put_u8(writer, meta->stale ? 1U : 0U);
    _weather_cache_put_u8(writer, meta->expired ? 1U : 0U);
}

static void _weather_cache_get_meta(weather_cache_reader_t *reader,
                                    weather_service_dataset_meta_t *meta)
{
    _weather_cache_get_time(reader, &meta->fetched_at);
    _weather_cache_get_time(reader, &meta->updated_at);
    _weather_cache_get_time(reader, &meta->valid_until);
    meta->available = _weather_cache_get_u8(reader) != 0U;
    meta->stale = _weather_cache_get_u8(reader) != 0U;
    meta->expired = _weather_cache_get_u8(reader) != 0U;
}

static void _weather_cache_encode_location(weather_cache_writer_t *writer,
        const weather_service_location_t *location)
{
    _weather_cache_put_string(writer, location->city, sizeof(location->city));
    _weather_cache_put_string(writer, location->region,
                              sizeof(location->region));
    _weather_cache_put_string(writer, location->country,
                              sizeof(location->country));
    _weather_cache_put_string(writer, location->timezone,
                              sizeof(location->timezone));
    _weather_cache_put_string(writer, location->provider,
                              sizeof(location->provider));
    _weather_cache_put_u16(writer, (uint16_t)location->latitude_tenths);
    _weather_cache_put_u16(writer, (uint16_t)location->longitude_tenths);
    _weather_cache_put_u64(writer, (uint64_t)location->acquired_at);
    _weather_cache_put_u8(writer, location->available ? 1U : 0U);
    _weather_cache_put_u8(writer, location->reused ? 1U : 0U);
}

static void _weather_cache_decode_location(weather_cache_reader_t *reader,
        weather_service_location_t *location)
{
    _weather_cache_get_string(reader, location->city, sizeof(location->city));
    _weather_cache_get_string(reader, location->region,
                              sizeof(location->region));
    _weather_cache_get_string(reader, location->country,
                              sizeof(location->country));
    _weather_cache_get_string(reader, location->timezone,
                              sizeof(location->timezone));
    _weather_cache_get_string(reader, location->provider,
                              sizeof(location->provider));
    location->latitude_tenths = (int16_t)_weather_cache_get_u16(reader);
    location->longitude_tenths = (int16_t)_weather_cache_get_u16(reader);
    location->acquired_at = (int64_t)_weather_cache_get_u64(reader);
    location->available = _weather_cache_get_u8(reader) != 0U;
    location->reused = _weather_cache_get_u8(reader) != 0U;
}

static void _weather_cache_encode_current(weather_cache_writer_t *writer,
        const weather_service_current_t *current)
{
    _weather_cache_put_meta(writer, &current->meta);
    _weather_cache_put_time(writer, &current->observed_at);
    _weather_cache_put_u16(writer, (uint16_t)current->temperature_tenths_c);
    _weather_cache_put_u16(writer, (uint16_t)current->feels_like_tenths_c);
    _weather_cache_put_u16(writer, current->condition_code);
    _weather_cache_put_string(writer, current->condition_text,
                              sizeof(current->condition_text));
    _weather_cache_put_u16(writer, current->wind_degrees);
    _weather_cache_put_u16(writer, current->wind_speed_tenths_kmh);
    _weather_cache_put_string(writer, current->wind_direction,
                              sizeof(current->wind_direction));
    _weather_cache_put_string(writer, current->wind_scale,
                              sizeof(current->wind_scale));
    _weather_cache_put_u8(writer, current->humidity_percent);
    _weather_cache_put_u16(writer, current->precipitation_tenths_mm);
    _weather_cache_put_u16(writer, current->pressure_hpa);
    _weather_cache_put_u16(writer, current->visibility_tenths_km);
}

static void _weather_cache_decode_current(weather_cache_reader_t *reader,
        weather_service_current_t *current)
{
    _weather_cache_get_meta(reader, &current->meta);
    _weather_cache_get_time(reader, &current->observed_at);
    current->temperature_tenths_c = (int16_t)_weather_cache_get_u16(reader);
    current->feels_like_tenths_c = (int16_t)_weather_cache_get_u16(reader);
    current->condition_code = _weather_cache_get_u16(reader);
    _weather_cache_get_string(reader, current->condition_text,
                              sizeof(current->condition_text));
    current->wind_degrees = _weather_cache_get_u16(reader);
    current->wind_speed_tenths_kmh = _weather_cache_get_u16(reader);
    _weather_cache_get_string(reader, current->wind_direction,
                              sizeof(current->wind_direction));
    _weather_cache_get_string(reader, current->wind_scale,
                              sizeof(current->wind_scale));
    current->humidity_percent = _weather_cache_get_u8(reader);
    current->precipitation_tenths_mm = _weather_cache_get_u16(reader);
    current->pressure_hpa = _weather_cache_get_u16(reader);
    current->visibility_tenths_km = _weather_cache_get_u16(reader);
}

static void _weather_cache_encode_hour(weather_cache_writer_t *writer,
                                       const weather_service_hour_t *hour)
{
    _weather_cache_put_time(writer, &hour->forecast_at);
    _weather_cache_put_u16(writer, (uint16_t)hour->temperature_tenths_c);
    _weather_cache_put_u16(writer, hour->condition_code);
    _weather_cache_put_string(writer, hour->condition_text,
                              sizeof(hour->condition_text));
    _weather_cache_put_u16(writer, hour->wind_speed_tenths_kmh);
    _weather_cache_put_string(writer, hour->wind_direction,
                              sizeof(hour->wind_direction));
    _weather_cache_put_u8(writer, hour->humidity_percent);
    _weather_cache_put_u8(writer, hour->precipitation_chance_percent);
    _weather_cache_put_u16(writer, hour->precipitation_tenths_mm);
}

static void _weather_cache_decode_hour(weather_cache_reader_t *reader,
                                       weather_service_hour_t *hour)
{
    _weather_cache_get_time(reader, &hour->forecast_at);
    hour->temperature_tenths_c = (int16_t)_weather_cache_get_u16(reader);
    hour->condition_code = _weather_cache_get_u16(reader);
    _weather_cache_get_string(reader, hour->condition_text,
                              sizeof(hour->condition_text));
    hour->wind_speed_tenths_kmh = _weather_cache_get_u16(reader);
    _weather_cache_get_string(reader, hour->wind_direction,
                              sizeof(hour->wind_direction));
    hour->humidity_percent = _weather_cache_get_u8(reader);
    hour->precipitation_chance_percent = _weather_cache_get_u8(reader);
    hour->precipitation_tenths_mm = _weather_cache_get_u16(reader);
}

static void _weather_cache_encode_day(weather_cache_writer_t *writer,
                                      const weather_service_day_t *day)
{
    _weather_cache_put_string(writer, day->date, sizeof(day->date));
    _weather_cache_put_u16(writer,
                           (uint16_t)day->minimum_temperature_tenths_c);
    _weather_cache_put_u16(writer,
                           (uint16_t)day->maximum_temperature_tenths_c);
    _weather_cache_put_u16(writer, day->day_condition_code);
    _weather_cache_put_u16(writer, day->night_condition_code);
    _weather_cache_put_string(writer, day->day_condition_text,
                              sizeof(day->day_condition_text));
    _weather_cache_put_string(writer, day->night_condition_text,
                              sizeof(day->night_condition_text));
    _weather_cache_put_u8(writer, day->humidity_percent);
    _weather_cache_put_u16(writer, day->precipitation_tenths_mm);
    _weather_cache_put_u16(writer, day->visibility_tenths_km);
    _weather_cache_put_u8(writer, day->uv_index);
}

static void _weather_cache_decode_day(weather_cache_reader_t *reader,
                                      weather_service_day_t *day)
{
    _weather_cache_get_string(reader, day->date, sizeof(day->date));
    day->minimum_temperature_tenths_c =
        (int16_t)_weather_cache_get_u16(reader);
    day->maximum_temperature_tenths_c =
        (int16_t)_weather_cache_get_u16(reader);
    day->day_condition_code = _weather_cache_get_u16(reader);
    day->night_condition_code = _weather_cache_get_u16(reader);
    _weather_cache_get_string(reader, day->day_condition_text,
                              sizeof(day->day_condition_text));
    _weather_cache_get_string(reader, day->night_condition_text,
                              sizeof(day->night_condition_text));
    day->humidity_percent = _weather_cache_get_u8(reader);
    day->precipitation_tenths_mm = _weather_cache_get_u16(reader);
    day->visibility_tenths_km = _weather_cache_get_u16(reader);
    day->uv_index = _weather_cache_get_u8(reader);
}

static void _weather_cache_encode_alert(weather_cache_writer_t *writer,
                                        const weather_service_alert_t *alert)
{
    _weather_cache_put_u64(writer, alert->key);
    _weather_cache_put_time(writer, &alert->issued_at);
    _weather_cache_put_time(writer, &alert->starts_at);
    _weather_cache_put_time(writer, &alert->ends_at);
    _weather_cache_put_string(writer, alert->title, sizeof(alert->title));
    _weather_cache_put_string(writer, alert->type_name,
                              sizeof(alert->type_name));
    _weather_cache_put_string(writer, alert->severity,
                              sizeof(alert->severity));
    _weather_cache_put_string(writer, alert->status, sizeof(alert->status));
    _weather_cache_put_string(writer, alert->description,
                              sizeof(alert->description));
    _weather_cache_put_string(writer, alert->instruction,
                              sizeof(alert->instruction));
    _weather_cache_put_u8(writer, alert->content_truncated ? 1U : 0U);
}

static void _weather_cache_decode_alert(weather_cache_reader_t *reader,
                                        weather_service_alert_t *alert)
{
    alert->key = _weather_cache_get_u64(reader);
    _weather_cache_get_time(reader, &alert->issued_at);
    _weather_cache_get_time(reader, &alert->starts_at);
    _weather_cache_get_time(reader, &alert->ends_at);
    _weather_cache_get_string(reader, alert->title, sizeof(alert->title));
    _weather_cache_get_string(reader, alert->type_name,
                              sizeof(alert->type_name));
    _weather_cache_get_string(reader, alert->severity,
                              sizeof(alert->severity));
    _weather_cache_get_string(reader, alert->status, sizeof(alert->status));
    _weather_cache_get_string(reader, alert->description,
                              sizeof(alert->description));
    _weather_cache_get_string(reader, alert->instruction,
                              sizeof(alert->instruction));
    alert->content_truncated = _weather_cache_get_u8(reader) != 0U;
}

static bool _weather_cache_encode(const weather_service_snapshot_t *snapshot,
                                  uint8_t *data, size_t capacity,
                                  size_t *encoded_size)
{
    if (snapshot->hourly.count > WEATHER_SERVICE_MAX_HOURS ||
            snapshot->daily.count > WEATHER_SERVICE_MAX_DAYS ||
            snapshot->alerts.count > WEATHER_SERVICE_MAX_ALERTS)
    {
        return false;
    }
    weather_cache_writer_t writer =
    {
        .data = data,
        .capacity = capacity,
        .valid = true,
    };
    _weather_cache_put_u64(&writer, snapshot->generation);
    _weather_cache_put_u32(&writer, snapshot->available_mask);
    _weather_cache_encode_location(&writer, &snapshot->location);
    _weather_cache_encode_current(&writer, &snapshot->current);
    _weather_cache_put_meta(&writer, &snapshot->hourly.meta);
    _weather_cache_put_u8(&writer, snapshot->hourly.count);
    for (uint8_t index = 0U; index < snapshot->hourly.count; ++index)
    {
        _weather_cache_encode_hour(&writer, &snapshot->hourly.items[index]);
    }
    _weather_cache_put_meta(&writer, &snapshot->daily.meta);
    _weather_cache_put_u8(&writer, snapshot->daily.count);
    for (uint8_t index = 0U; index < snapshot->daily.count; ++index)
    {
        _weather_cache_encode_day(&writer, &snapshot->daily.items[index]);
    }
    _weather_cache_put_meta(&writer, &snapshot->alerts.meta);
    _weather_cache_put_u8(&writer, snapshot->alerts.count);
    _weather_cache_put_u8(&writer, snapshot->alerts.truncated ? 1U : 0U);
    for (uint8_t index = 0U; index < snapshot->alerts.count; ++index)
    {
        _weather_cache_encode_alert(&writer, &snapshot->alerts.items[index]);
    }
    *encoded_size = writer.position;
    return writer.valid;
}

static bool _weather_cache_decode(const uint8_t *data, size_t size,
                                  weather_service_snapshot_t *snapshot)
{
    weather_cache_reader_t reader =
    {
        .data = data,
        .size = size,
        .valid = true,
    };
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->generation = _weather_cache_get_u64(&reader);
    snapshot->available_mask = _weather_cache_get_u32(&reader);
    _weather_cache_decode_location(&reader, &snapshot->location);
    _weather_cache_decode_current(&reader, &snapshot->current);
    _weather_cache_get_meta(&reader, &snapshot->hourly.meta);
    snapshot->hourly.count = _weather_cache_get_u8(&reader);
    if (snapshot->hourly.count > WEATHER_SERVICE_MAX_HOURS)
    {
        reader.valid = false;
    }
    for (uint8_t index = 0U;
            reader.valid && index < snapshot->hourly.count; ++index)
    {
        _weather_cache_decode_hour(&reader, &snapshot->hourly.items[index]);
    }
    _weather_cache_get_meta(&reader, &snapshot->daily.meta);
    snapshot->daily.count = _weather_cache_get_u8(&reader);
    if (snapshot->daily.count > WEATHER_SERVICE_MAX_DAYS)
    {
        reader.valid = false;
    }
    for (uint8_t index = 0U;
            reader.valid && index < snapshot->daily.count; ++index)
    {
        _weather_cache_decode_day(&reader, &snapshot->daily.items[index]);
    }
    _weather_cache_get_meta(&reader, &snapshot->alerts.meta);
    snapshot->alerts.count = _weather_cache_get_u8(&reader);
    snapshot->alerts.truncated = _weather_cache_get_u8(&reader) != 0U;
    if (snapshot->alerts.count > WEATHER_SERVICE_MAX_ALERTS)
    {
        reader.valid = false;
    }
    for (uint8_t index = 0U;
            reader.valid && index < snapshot->alerts.count; ++index)
    {
        _weather_cache_decode_alert(&reader, &snapshot->alerts.items[index]);
    }
    return reader.valid && reader.position == reader.size;
}

static uint32_t _weather_cache_crc32(const uint8_t *data, size_t size)
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

static void _weather_cache_header_put(uint8_t *header, uint64_t sequence,
                                      uint32_t size, uint32_t crc)
{
    weather_cache_writer_t writer =
    {
        .data = header,
        .capacity = WEATHER_CACHE_HEADER_BYTES,
        .valid = true,
    };
    _weather_cache_put_u32(&writer, WEATHER_CACHE_MAGIC);
    _weather_cache_put_u16(&writer, WEATHER_CACHE_VERSION);
    _weather_cache_put_u16(&writer, 0U);
    _weather_cache_put_u64(&writer, sequence);
    _weather_cache_put_u32(&writer, size);
    _weather_cache_put_u32(&writer, crc);
}

static bool _weather_cache_header_get(const uint8_t *header,
                                      uint64_t *sequence, uint32_t *size,
                                      uint32_t *crc)
{
    weather_cache_reader_t reader =
    {
        .data = header,
        .size = WEATHER_CACHE_HEADER_BYTES,
        .valid = true,
    };
    uint32_t magic = _weather_cache_get_u32(&reader);
    uint16_t version = _weather_cache_get_u16(&reader);
    uint16_t reserved = _weather_cache_get_u16(&reader);
    *sequence = _weather_cache_get_u64(&reader);
    *size = _weather_cache_get_u32(&reader);
    *crc = _weather_cache_get_u32(&reader);
    return reader.valid && magic == WEATHER_CACHE_MAGIC &&
           version == WEATHER_CACHE_VERSION && reserved == 0U && *size > 0U &&
           *size <= sizeof(weather_service_snapshot_t) * 2U;
}

static bool _weather_cache_full_read(int descriptor, uint8_t *data,
                                     size_t size)
{
    size_t position = 0U;
    while (position < size)
    {
        ssize_t count = read(descriptor, data + position, size - position);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return false;
        }
        position += (size_t)count;
    }
    return true;
}

static bool _weather_cache_full_write(int descriptor, const uint8_t *data,
                                      size_t size)
{
    size_t position = 0U;
    while (position < size)
    {
        ssize_t count = write(descriptor, data + position, size - position);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            return false;
        }
        position += (size_t)count;
    }
    return true;
}

static esp_err_t _weather_cache_read_slot(const char *path,
        weather_service_snapshot_t *snapshot, uint64_t *sequence)
{
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0)
    {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    uint8_t header[WEATHER_CACHE_HEADER_BYTES];
    uint32_t payload_size = 0U;
    uint32_t expected_crc = 0U;
    esp_err_t result = ESP_ERR_INVALID_CRC;
    uint8_t *payload = NULL;
    if (!_weather_cache_full_read(descriptor, header, sizeof(header)) ||
            !_weather_cache_header_get(header, sequence, &payload_size,
                                       &expected_crc))
    {
        goto exit;
    }
    payload = _weather_cache_allocate(payload_size);
    if (payload == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    uint8_t trailing = 0U;
    if (!_weather_cache_full_read(descriptor, payload, payload_size) ||
            read(descriptor, &trailing, sizeof(trailing)) != 0 ||
            _weather_cache_crc32(payload, payload_size) != expected_crc ||
            !_weather_cache_decode(payload, payload_size, snapshot))
    {
        goto exit;
    }
    result = ESP_OK;

exit:
    _weather_cache_release(payload);
    (void)close(descriptor);
    return result;
}

esp_err_t weather_service_cache_load(const char *directory,
                                     weather_service_snapshot_t *snapshot, uint64_t *sequence)
{
    if (directory == NULL || snapshot == NULL || sequence == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char path_a[256];
    char path_b[256];
    int path_a_size = snprintf(path_a, sizeof(path_a), "%s/weather_a.bin",
                               directory);
    int path_b_size = snprintf(path_b, sizeof(path_b), "%s/weather_b.bin",
                               directory);
    if (path_a_size < 0 || (size_t)path_a_size >= sizeof(path_a) ||
            path_b_size < 0 || (size_t)path_b_size >= sizeof(path_b))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    weather_service_snapshot_t *first = _weather_cache_allocate(sizeof(*first));
    weather_service_snapshot_t *second = _weather_cache_allocate(sizeof(*second));
    if (first == NULL || second == NULL)
    {
        _weather_cache_release(first);
        _weather_cache_release(second);
        return ESP_ERR_NO_MEM;
    }
    uint64_t first_sequence = 0U;
    uint64_t second_sequence = 0U;
    esp_err_t first_result = _weather_cache_read_slot(path_a, first,
                             &first_sequence);
    esp_err_t second_result = _weather_cache_read_slot(path_b, second,
                              &second_sequence);
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (first_result == ESP_OK || second_result == ESP_OK)
    {
        if (second_result == ESP_OK &&
                (first_result != ESP_OK || second_sequence > first_sequence))
        {
            *snapshot = *second;
            *sequence = second_sequence;
        }
        else
        {
            *snapshot = *first;
            *sequence = first_sequence;
        }
        result = ESP_OK;
    }
    _weather_cache_release(second);
    _weather_cache_release(first);
    return result;
}

esp_err_t weather_service_cache_store(const char *directory,
                                      const weather_service_snapshot_t *snapshot, uint64_t sequence)
{
    if (directory == NULL || snapshot == NULL || sequence == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    char target[256];
    char temporary[256] = {0};
    const char slot = (sequence & 1U) != 0U ? 'a' : 'b';
    int target_size = snprintf(target, sizeof(target), "%s/weather_%c.bin",
                               directory, slot);
    int temporary_size = snprintf(temporary, sizeof(temporary),
                                  "%s/weather_%c.tmp", directory, slot);
    if (target_size < 0 || (size_t)target_size >= sizeof(target) ||
            temporary_size < 0 ||
            (size_t)temporary_size >= sizeof(temporary))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t capacity = sizeof(*snapshot) * 2U;
    uint8_t *payload = _weather_cache_allocate(capacity);
    if (payload == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    size_t payload_size = 0U;
    esp_err_t result = ESP_FAIL;
    int descriptor = -1;
    if (!_weather_cache_encode(snapshot, payload, capacity, &payload_size))
    {
        result = ESP_ERR_INVALID_SIZE;
        goto exit;
    }
    uint8_t header[WEATHER_CACHE_HEADER_BYTES];
    _weather_cache_header_put(header, sequence, (uint32_t)payload_size,
                              _weather_cache_crc32(payload, payload_size));
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0)
    {
        goto exit;
    }
    if (!_weather_cache_full_write(descriptor, header, sizeof(header)) ||
            !_weather_cache_full_write(descriptor, payload, payload_size) ||
            fsync(descriptor) != 0)
    {
        goto exit;
    }
    if (close(descriptor) != 0)
    {
        descriptor = -1;
        goto exit;
    }
    descriptor = -1;
    if (rename(temporary, target) != 0)
    {
        goto exit;
    }
    result = ESP_OK;
    descriptor = open(directory, O_RDONLY);
    if (descriptor >= 0 && fsync(descriptor) != 0)
    {
        LOG_W("cache committed without directory sync");
    }

exit:
    if (descriptor >= 0)
    {
        (void)close(descriptor);
    }
    if (result != ESP_OK && temporary[0] != '\0')
    {
        (void)unlink(temporary);
    }
    _weather_cache_release(payload);
    return result;
}
