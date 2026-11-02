#define DBG_TAG "music_stream"
#include "mt_log.h"

#include "music_service_internal.h"

#include "audio_service.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MUSIC_STREAM_QUEUE_DEPTH 4U
#define MUSIC_STREAM_READ_BYTES 4096U
#define MUSIC_STREAM_PCM_BYTES 16384U
#define MUSIC_STREAM_PCM_MAX_BYTES (256U * 1024U)
#define MUSIC_STREAM_EVENT_STATE_CHANGED (1U << 0)

typedef struct music_stream_command
{
    char url[MUSIC_SERVICE_URL_BYTES];
    uint32_t duration_ms;
    uint32_t generation;
} music_stream_command_t;

typedef struct music_stream_runtime
{
    bool initialized;
    bool decoders_registered;
    QueueHandle_t queue;
    TaskHandle_t task;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t audio_lock;
    EventGroupHandle_t state_events;
    music_stream_status_t status;
    audio_service_config_t default_audio;
    audio_service_lease_t lease;
    bool lease_held;
    esp_http_client_handle_t http_client;
    atomic_bool stop;
    atomic_bool pause;
    atomic_bool audio_released;
    atomic_bool shutdown;
    atomic_bool exited;
    atomic_uint play_generation;
    atomic_uint cancel_generation;
    atomic_int public_state;
} music_stream_runtime_t;

static music_stream_runtime_t s_stream;

static esp_audio_simple_dec_type_t _decoder_type(const char *url)
{
    if (url == NULL)
    {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    const char *query = strchr(url, '?');
    const size_t length = query == NULL ? strlen(url) :
                          (size_t)(query - url);
    if (length >= 5U && strcasecmp(url + length - 5U, ".flac") == 0)
    {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

static void _stream_set_status(music_playback_state_t state,
                               music_service_failure_t failure)
{
    atomic_store_explicit(&s_stream.public_state, (int)state,
                          memory_order_release);
    if (s_stream.lock == NULL)
    {
        return;
    }
    if (xSemaphoreTake(s_stream.lock, portMAX_DELAY) == pdTRUE)
    {
        s_stream.status.state = state;
        s_stream.status.failure = failure;
        (void)xSemaphoreGive(s_stream.lock);
    }
}

static void _stream_set_http_client(esp_http_client_handle_t client)
{
    if (s_stream.lock == NULL ||
            xSemaphoreTake(s_stream.lock, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    s_stream.http_client = client;
    (void)xSemaphoreGive(s_stream.lock);
}

static void _stream_clear_http_client(esp_http_client_handle_t client)
{
    if (s_stream.lock == NULL ||
            xSemaphoreTake(s_stream.lock, portMAX_DELAY) != pdTRUE)
    {
        return;
    }
    if (s_stream.http_client == client)
    {
        s_stream.http_client = NULL;
    }
    (void)xSemaphoreGive(s_stream.lock);
}

static void _stream_abort_http(void)
{
    if (s_stream.lock == NULL ||
            xSemaphoreTake(s_stream.lock, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        return;
    }
    esp_http_client_handle_t client = s_stream.http_client;
    (void)xSemaphoreGive(s_stream.lock);
    if (client != NULL)
    {
        (void)esp_http_client_cancel_request(client);
    }
}

static void _stream_update_position(uint32_t position_ms)
{
    if (s_stream.lock == NULL)
    {
        return;
    }
    if (xSemaphoreTake(s_stream.lock, portMAX_DELAY) == pdTRUE)
    {
        s_stream.status.position_ms = position_ms;
        (void)xSemaphoreGive(s_stream.lock);
    }
}

static esp_err_t _stream_open_audio(uint32_t sample_rate, uint8_t bits,
                                    uint8_t channels)
{
    if (s_stream.audio_lock == NULL ||
            xSemaphoreTake(s_stream.audio_lock, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load_explicit(&s_stream.pause, memory_order_acquire))
    {
        (void)xSemaphoreGive(s_stream.audio_lock);
        return ESP_ERR_INVALID_STATE;
    }
    audio_service_config_t config =
    {
        .sample_rate_hz = sample_rate,
        .bits_per_sample = bits,
        .channels = channels,
        .mclk_multiple = bits == 24U ? 384U : 256U,
    };
    esp_err_t result = audio_service_configure(&config);
    if (result != ESP_OK)
    {
        LOG_E("audio configure failed: %s", esp_err_to_name(result));
        (void)xSemaphoreGive(s_stream.audio_lock);
        return result;
    }
    result = audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK,
                                   &s_stream.lease);
    if (result != ESP_OK)
    {
        LOG_E("audio acquire failed: %s", esp_err_to_name(result));
        (void)audio_service_configure(&s_stream.default_audio);
        (void)xSemaphoreGive(s_stream.audio_lock);
        return result;
    }
    s_stream.lease_held = true;
    (void)xSemaphoreGive(s_stream.audio_lock);
    return ESP_OK;
}

/* Called with audio_lock held. The BSP default is restored after every
 * released playback lease so the next consumer (for example Flybirds at
 * 16 kHz) starts from the product configuration rather than the stream rate. */
static esp_err_t _stream_close_audio_locked(void)
{
    esp_err_t result = ESP_OK;
    if (s_stream.lease_held)
    {
        result = audio_service_release(s_stream.lease);
        if (result != ESP_OK)
        {
            LOG_E("audio lease release failed: %s",
                  esp_err_to_name(result));
            return result;
        }
        s_stream.lease_held = false;
    }
    (void)audio_service_configure(&s_stream.default_audio);
    return ESP_OK;
}

static esp_err_t _stream_close_audio(void)
{
    if (s_stream.audio_lock == NULL ||
            xSemaphoreTake(s_stream.audio_lock, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = _stream_close_audio_locked();
    (void)xSemaphoreGive(s_stream.audio_lock);
    return result;
}

/* User pause must not keep the exclusive audio lease: another application
 * may open the codec while the music stream is paused. The stream task
 * reopens it with the saved PCM format on resume. */
static esp_err_t _stream_pause_audio(void)
{
    if (s_stream.audio_lock == NULL ||
            xSemaphoreTake(s_stream.audio_lock, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    if (s_stream.lease_held)
    {
        result = _stream_close_audio_locked();
        if (result == ESP_OK)
        {
            atomic_store_explicit(&s_stream.audio_released, true,
                                  memory_order_release);
        }
    }
    (void)xSemaphoreGive(s_stream.audio_lock);
    return result;
}

static size_t _pcm24_to_pcm16(const uint8_t *source, size_t source_bytes,
                              uint8_t *destination, size_t destination_size)
{
    if (source == NULL || destination == NULL ||
            (source_bytes % 3U) != 0U)
    {
        return 0U;
    }
    const size_t samples = source_bytes / 3U;
    const size_t needed = samples * 2U;
    if (needed > destination_size)
    {
        return 0U;
    }
    for (size_t index = 0U; index < samples; ++index)
    {
        const uint32_t raw = (uint32_t)source[index * 3U] |
                             ((uint32_t)source[index * 3U + 1U] << 8U) |
                             ((uint32_t)source[index * 3U + 2U] << 16U);
        const int32_t sign_extended = (int32_t)(raw << 8U) >> 8U;
        const int16_t converted = (int16_t)(sign_extended >> 8);
        destination[index * 2U] = (uint8_t)(converted & 0xFF);
        destination[index * 2U + 1U] = (uint8_t)((converted >> 8) & 0xFF);
    }
    return needed;
}

static void _stream_play_url(const char *url, uint32_t duration_ms,
                             uint32_t generation)
{
    bool failed = false;
    music_service_failure_t failure_reason = MUSIC_SERVICE_FAILURE_NONE;
    esp_http_client_handle_t client = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    uint8_t *input = NULL;
    uint8_t *pcm = NULL;
    size_t pcm_size = MUSIC_STREAM_PCM_BYTES;
    bool audio_ready = false;
    bool write_failure_logged = false;
    uint64_t played_bytes = 0U;
    uint32_t sample_rate = 44100U;
    uint8_t channels = 2U;
    uint8_t bits = 16U;
    uint8_t source_bits = 16U;
    bool convert_24_to_16 = false;
    uint8_t *pcm16 = NULL;

    _stream_set_status(MUSIC_PLAYBACK_BUFFERING,
                       MUSIC_SERVICE_FAILURE_NONE);
    if (s_stream.lock != NULL &&
            xSemaphoreTake(s_stream.lock, portMAX_DELAY) == pdTRUE)
    {
        s_stream.status.duration_ms = duration_ms;
        s_stream.status.generation = generation;
        (void)xSemaphoreGive(s_stream.lock);
    }
    esp_http_client_config_t config =
    {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    client = esp_http_client_init(&config);
    if (client == NULL)
    {
        _stream_set_status(MUSIC_PLAYBACK_ERROR,
                           MUSIC_SERVICE_FAILURE_INTERNAL);
        failure_reason = MUSIC_SERVICE_FAILURE_INTERNAL;
        failed = true;
        goto cleanup;
    }
    (void)esp_http_client_set_header(client, "User-Agent",
                                     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                                     "Chrome/120.0.0.0 Safari/537.36");
    (void)esp_http_client_set_header(client, "Referer", "https://y.qq.com/");
    _stream_set_http_client(client);
    if (esp_http_client_open(client, 0) != ESP_OK ||
            esp_http_client_fetch_headers(client) < 0)
    {
        _stream_set_status(MUSIC_PLAYBACK_ERROR,
                           MUSIC_SERVICE_FAILURE_NETWORK);
        failure_reason = MUSIC_SERVICE_FAILURE_NETWORK;
        failed = true;
        goto cleanup;
    }
    const int http_status = esp_http_client_get_status_code(client);
    if (http_status < 200 || http_status >= 300)
    {
        const music_service_failure_t status_failure =
            (http_status == 401 || http_status == 403) ?
            MUSIC_SERVICE_FAILURE_AUTH : MUSIC_SERVICE_FAILURE_NETWORK;
        _stream_set_status(MUSIC_PLAYBACK_ERROR, status_failure);
        failure_reason = status_failure;
        failed = true;
        goto cleanup;
    }

    esp_audio_simple_dec_cfg_t decoder_config =
    {
        .dec_type = _decoder_type(url),
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&decoder_config, &decoder) !=
            ESP_AUDIO_ERR_OK)
    {
        _stream_set_status(MUSIC_PLAYBACK_ERROR,
                           MUSIC_SERVICE_FAILURE_DECODE);
        failure_reason = MUSIC_SERVICE_FAILURE_DECODE;
        failed = true;
        goto cleanup;
    }

    input = heap_caps_malloc(MUSIC_STREAM_READ_BYTES,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pcm = heap_caps_malloc(pcm_size,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input == NULL || pcm == NULL)
    {
        _stream_set_status(MUSIC_PLAYBACK_ERROR,
                           MUSIC_SERVICE_FAILURE_INTERNAL);
        failure_reason = MUSIC_SERVICE_FAILURE_INTERNAL;
        failed = true;
        goto cleanup;
    }

    _stream_set_status(MUSIC_PLAYBACK_PLAYING, MUSIC_SERVICE_FAILURE_NONE);
    while (!atomic_load_explicit(&s_stream.stop, memory_order_acquire))
    {
        if (atomic_load_explicit(&s_stream.pause, memory_order_acquire))
        {
            _stream_set_status(MUSIC_PLAYBACK_PAUSED,
                               MUSIC_SERVICE_FAILURE_NONE);
            if (s_stream.state_events != NULL)
            {
                (void)xEventGroupWaitBits(
                    s_stream.state_events,
                    MUSIC_STREAM_EVENT_STATE_CHANGED, pdTRUE, pdFALSE,
                    portMAX_DELAY);
            }
            if (!atomic_load_explicit(&s_stream.pause,
                                      memory_order_acquire))
            {
                _stream_set_status(MUSIC_PLAYBACK_PLAYING,
                                   MUSIC_SERVICE_FAILURE_NONE);
            }
            continue;
        }
        if (atomic_load_explicit(&s_stream.audio_released,
                                 memory_order_acquire))
        {
            audio_ready = false;
        }
        const int read_length = esp_http_client_read(client, (char *)input,
            MUSIC_STREAM_READ_BYTES);
        if (read_length < 0)
        {
            LOG_E("HTTP read failed: %d", read_length);
            _stream_set_status(MUSIC_PLAYBACK_ERROR,
                               MUSIC_SERVICE_FAILURE_NETWORK);
            failure_reason = MUSIC_SERVICE_FAILURE_NETWORK;
            failed = true;
            break;
        }
        if (read_length == 0)
        {
            break;
        }
        esp_audio_simple_dec_raw_t raw =
        {
            .buffer = input,
            .len = (uint32_t)read_length,
            .eos = false,
        };
        bool fatal = false;
        uint32_t stalled_calls = 0U;
        while (raw.len > 0U &&
                !atomic_load_explicit(&s_stream.stop, memory_order_acquire))
        {
            esp_audio_simple_dec_out_t output =
            {
                .buffer = pcm,
                .len = (uint32_t)pcm_size,
            };
            raw.consumed = 0U;
            const uint32_t raw_before = raw.len;
            const esp_audio_err_t decode_result =
                esp_audio_simple_dec_process(decoder, &raw, &output);
            if (decode_result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH)
            {
                /* 4 KiB of MP3 expands to far more than the initial 16 KiB
                 * PCM buffer at 44.1 kHz stereo. Grow to the size requested
                 * by the decoder instead of silently continuing. */
                size_t needed_size = output.needed_size;
                if (needed_size <= pcm_size)
                {
                    needed_size = pcm_size * 2U;
                }
                if (needed_size > MUSIC_STREAM_PCM_MAX_BYTES)
                {
                    _stream_set_status(MUSIC_PLAYBACK_ERROR,
                                       MUSIC_SERVICE_FAILURE_DECODE);
                    failure_reason = MUSIC_SERVICE_FAILURE_DECODE;
                    failed = true;
                    fatal = true;
                    break;
                }
                uint8_t *grown = heap_caps_realloc(
                                     pcm, needed_size,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (grown == NULL)
                {
                    _stream_set_status(MUSIC_PLAYBACK_ERROR,
                                       MUSIC_SERVICE_FAILURE_DECODE);
                    failure_reason = MUSIC_SERVICE_FAILURE_DECODE;
                    failed = true;
                    fatal = true;
                    break;
                }
                pcm = grown;
                pcm_size = needed_size;
                continue;
            }
            if (decode_result != ESP_AUDIO_ERR_OK)
            {
                LOG_E("decode failed: %d raw=%u consumed=%u decoded=%u",
                      (int)decode_result, (unsigned)raw_before,
                      (unsigned)raw.consumed,
                      (unsigned)output.decoded_size);
                _stream_set_status(MUSIC_PLAYBACK_ERROR,
                                   MUSIC_SERVICE_FAILURE_DECODE);
                failure_reason = MUSIC_SERVICE_FAILURE_DECODE;
                failed = true;
                fatal = true;
                break;
            }
            if (output.decoded_size > 0U)
            {
                if (atomic_load_explicit(&s_stream.audio_released,
                                         memory_order_acquire))
                {
                    audio_ready = false;
                }
                if (!audio_ready)
                {
                    if (atomic_load_explicit(&s_stream.pause,
                                             memory_order_acquire))
                    {
                        break;
                    }
                    esp_audio_simple_dec_info_t info;
                    if (esp_audio_simple_dec_get_info(decoder, &info) ==
                            ESP_AUDIO_ERR_OK)
                    {
                        sample_rate = info.sample_rate;
                        channels = info.channel == 0U ? 2U : info.channel;
                        bits = info.bits_per_sample == 0U ? 16U :
                               info.bits_per_sample;
                    }
                    source_bits = bits;
                    esp_err_t audio_result = _stream_open_audio(
                                                 sample_rate, bits, channels);
                    if (audio_result != ESP_OK &&
                            atomic_load_explicit(&s_stream.pause,
                                                 memory_order_acquire))
                    {
                        break;
                    }
                    if (audio_result != ESP_OK && bits == 24U)
                    {
                        /* The BSP has no 24-bit clock pair for some common
                         * sample rates. Decode is still 24-bit; convert the
                         * PCM to 16-bit rather than reporting the misleading
                         * AUDIO_BUSY error. */
                        audio_result = _stream_open_audio(sample_rate, 16U,
                                                          channels);
                        if (audio_result == ESP_OK)
                        {
                            bits = 16U;
                            convert_24_to_16 = true;
                            pcm16 = heap_caps_malloc(
                                        (MUSIC_STREAM_PCM_MAX_BYTES / 3U) * 2U,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                            if (pcm16 == NULL)
                            {
                                audio_result = ESP_ERR_NO_MEM;
                            }
                        }
                    }
                    if (audio_result != ESP_OK)
                    {
                        _stream_set_status(MUSIC_PLAYBACK_ERROR,
                                           MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
                        failure_reason = MUSIC_SERVICE_FAILURE_AUDIO_BUSY;
                        failed = true;
                        fatal = true;
                        break;
                    }
                    audio_ready = true;
                    atomic_store_explicit(&s_stream.audio_released, false,
                                          memory_order_release);
                }
                size_t offset = 0U;
                size_t chunk_written = 0U;
                uint32_t write_retries = 0U;
                esp_err_t write_result = ESP_OK;
                const uint8_t *write_data = output.buffer;
                size_t write_size = output.decoded_size;
                if (convert_24_to_16)
                {
                    const size_t converted = _pcm24_to_pcm16(
                                                 output.buffer,
                                                 output.decoded_size, pcm16,
                                                 (MUSIC_STREAM_PCM_MAX_BYTES /
                                                  3U) * 2U);
                    if (converted == 0U)
                    {
                        _stream_set_status(MUSIC_PLAYBACK_ERROR,
                                           MUSIC_SERVICE_FAILURE_DECODE);
                        failure_reason = MUSIC_SERVICE_FAILURE_DECODE;
                        failed = true;
                        fatal = true;
                        break;
                    }
                    write_data = pcm16;
                    write_size = converted;
                }
                while (offset < write_size)
                {
                    size_t written = 0U;
                    write_result = audio_service_write_leased(
                                       s_stream.lease,
                                       (uint8_t *)write_data + offset,
                                       write_size - offset,
                                       &written, 1000U);
                    chunk_written += written;
                    offset += written;
                    if (write_result == ESP_OK)
                    {
                        write_retries = 0U;
                        if (written == 0U)
                        {
                            break;
                        }
                        continue;
                    }
                    if (write_result == ESP_ERR_TIMEOUT && written > 0U)
                    {
                        write_retries = 0U;
                        continue;
                    }
                    if (write_result == ESP_ERR_INVALID_STATE &&
                            atomic_load_explicit(&s_stream.pause,
                                                 memory_order_acquire))
                    {
                        break;
                    }
                    if (write_result == ESP_ERR_TIMEOUT &&
                            write_retries < 2U)
                    {
                        ++write_retries;
                        continue;
                    }
                    break;
                }
                if (convert_24_to_16)
                {
                    played_bytes += (chunk_written / 2U) * 3U;
                }
                else
                {
                    played_bytes += chunk_written;
                }
                const bool pause_rejected =
                    write_result == ESP_ERR_INVALID_STATE &&
                    atomic_load_explicit(&s_stream.pause, memory_order_acquire);
                if (write_result != ESP_OK && !pause_rejected &&
                        !write_failure_logged)
                {
                    write_failure_logged = true;
                    LOG_W("audio write failed: %s written=%u/%u",
                          esp_err_to_name(write_result),
                          (unsigned)chunk_written, (unsigned)write_size);
                }
                if (write_result != ESP_OK && write_result != ESP_ERR_TIMEOUT &&
                        !pause_rejected)
                {
                    _stream_set_status(MUSIC_PLAYBACK_ERROR,
                                       MUSIC_SERVICE_FAILURE_DECODE);
                    failure_reason = MUSIC_SERVICE_FAILURE_DECODE;
                    failed = true;
                    fatal = true;
                    break;
                }
                if (write_result == ESP_ERR_TIMEOUT && offset < write_size)
                {
                    _stream_set_status(MUSIC_PLAYBACK_ERROR,
                                       MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
                    failure_reason = MUSIC_SERVICE_FAILURE_AUDIO_BUSY;
                    failed = true;
                    fatal = true;
                    break;
                }
                const uint64_t bytes_per_second =
                    (uint64_t)sample_rate * channels * (source_bits / 8U);
                if (bytes_per_second > 0U)
                {
                    _stream_update_position((uint32_t)(played_bytes * 1000U /
                                                       bytes_per_second));
                }
            }
            if (raw.consumed == 0U)
            {
                if (++stalled_calls >= 8U)
                {
                    LOG_W("decoder made no input progress: decoded=%u",
                          (unsigned)output.decoded_size);
                    break;
                }
                continue;
            }
            stalled_calls = 0U;
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }
        if (fatal || atomic_load_explicit(&s_stream.stop,
                                          memory_order_acquire))
        {
            break;
        }
    }

cleanup:
    if (s_stream.lease_held)
    {
        if (_stream_close_audio() != ESP_OK)
        {
            failed = true;
            failure_reason = MUSIC_SERVICE_FAILURE_AUDIO_BUSY;
            _stream_set_status(MUSIC_PLAYBACK_ERROR,
                               MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
        }
    }
    if (decoder != NULL)
    {
        esp_audio_simple_dec_close(decoder);
    }
    if (input != NULL)
    {
        heap_caps_free(input);
    }
    if (pcm != NULL)
    {
        heap_caps_free(pcm);
    }
    if (pcm16 != NULL)
    {
        heap_caps_free(pcm16);
    }
    if (client != NULL)
    {
        _stream_clear_http_client(client);
        esp_http_client_close(client);
        (void)esp_http_client_cleanup(client);
    }
    const bool natural_end = !failed &&
                             !atomic_load_explicit(&s_stream.stop,
                                 memory_order_acquire) &&
                             !atomic_load_explicit(&s_stream.shutdown,
                                 memory_order_acquire);
    if (!failed)
    {
        _stream_set_status(MUSIC_PLAYBACK_IDLE, MUSIC_SERVICE_FAILURE_NONE);
    }
    if (natural_end)
    {
        music_service_stream_finished();
    }
    else if (failed && failure_reason != MUSIC_SERVICE_FAILURE_NONE &&
             !atomic_load_explicit(&s_stream.stop, memory_order_acquire) &&
             !atomic_load_explicit(&s_stream.shutdown, memory_order_acquire))
    {
        music_service_stream_failed(failure_reason);
    }
}

static void _stream_task(void *argument)
{
    (void)argument;
    for (;;)
    {
        music_stream_command_t command;
        if (xQueueReceive(s_stream.queue, &command, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if (atomic_load_explicit(&s_stream.shutdown, memory_order_acquire))
        {
            break;
        }
        if (command.generation <=
                atomic_load_explicit(&s_stream.cancel_generation,
                                     memory_order_acquire))
        {
            continue;
        }
        atomic_store_explicit(&s_stream.stop, false, memory_order_release);
        atomic_store_explicit(&s_stream.pause, false, memory_order_release);
        _stream_play_url(command.url, command.duration_ms, command.generation);
        if (atomic_load_explicit(&s_stream.shutdown, memory_order_acquire))
        {
            break;
        }
    }
    atomic_store_explicit(&s_stream.exited, true, memory_order_release);
    vTaskDelete(NULL);
}

static esp_err_t _stream_register_decoders(void)
{
    esp_audio_err_t result = esp_mp3_dec_register();
    if (result != ESP_AUDIO_ERR_OK)
    {
        LOG_E("register MP3 decoder failed: %d", (int)result);
        return ESP_ERR_NOT_SUPPORTED;
    }
    result = esp_flac_dec_register();
    if (result != ESP_AUDIO_ERR_OK)
    {
        LOG_E("register FLAC decoder failed: %d", (int)result);
        esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_stream.decoders_registered = true;
    return ESP_OK;
}

static void _stream_unregister_decoders(void)
{
    if (!s_stream.decoders_registered)
    {
        return;
    }
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_FLAC);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
    s_stream.decoders_registered = false;
}

esp_err_t music_stream_init(uint32_t priority)
{
    if (s_stream.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_stream, 0, sizeof(s_stream));
    if (_stream_register_decoders() != ESP_OK)
    {
        memset(&s_stream, 0, sizeof(s_stream));
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_stream.lock = xSemaphoreCreateMutex();
    s_stream.audio_lock = xSemaphoreCreateMutex();
    s_stream.state_events = xEventGroupCreate();
    /* PSRAM-backed queue; delete it with vQueueDeleteWithCaps(). */
    s_stream.queue = xQueueCreateWithCaps(
                         MUSIC_STREAM_QUEUE_DEPTH,
                         sizeof(music_stream_command_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_stream.lock == NULL || s_stream.audio_lock == NULL ||
            s_stream.queue == NULL || s_stream.state_events == NULL)
    {
        if (s_stream.queue != NULL)
        {
            vQueueDeleteWithCaps(s_stream.queue);
        }
        if (s_stream.state_events != NULL)
        {
            vEventGroupDelete(s_stream.state_events);
        }
        if (s_stream.audio_lock != NULL)
        {
            vSemaphoreDelete(s_stream.audio_lock);
        }
        if (s_stream.lock != NULL)
        {
            vSemaphoreDelete(s_stream.lock);
        }
        _stream_unregister_decoders();
        memset(&s_stream, 0, sizeof(s_stream));
        return ESP_ERR_NO_MEM;
    }
    LOG_I("queue item=%u B", (unsigned)sizeof(music_stream_command_t));
    if (audio_service_get_config(&s_stream.default_audio) != ESP_OK)
    {
        memset(&s_stream.default_audio, 0, sizeof(s_stream.default_audio));
    }
    if (xTaskCreatePinnedToCoreWithCaps(_stream_task, "music_stream",
                                        CONFIG_MUSIC_SERVICE_AUDIO_TASK_STACK_SIZE,
                                        NULL, priority, &s_stream.task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        vQueueDeleteWithCaps(s_stream.queue);
        vEventGroupDelete(s_stream.state_events);
        s_stream.state_events = NULL;
        vSemaphoreDelete(s_stream.audio_lock);
        s_stream.audio_lock = NULL;
        vSemaphoreDelete(s_stream.lock);
        _stream_unregister_decoders();
        memset(&s_stream, 0, sizeof(s_stream));
        return ESP_ERR_NO_MEM;
    }
    s_stream.initialized = true;
    return ESP_OK;
}

esp_err_t music_stream_deinit(void)
{
    if (!s_stream.initialized)
    {
        return ESP_OK;
    }
    atomic_store_explicit(&s_stream.shutdown, true, memory_order_release);
    atomic_store_explicit(&s_stream.stop, true, memory_order_release);
    atomic_store_explicit(&s_stream.cancel_generation,
                          atomic_load_explicit(&s_stream.play_generation,
                                  memory_order_acquire),
                          memory_order_release);
    if (s_stream.state_events != NULL)
    {
        (void)xEventGroupSetBits(s_stream.state_events,
                                 MUSIC_STREAM_EVENT_STATE_CHANGED);
    }
    _stream_abort_http();
    music_stream_command_t wake;
    memset(&wake, 0, sizeof(wake));
    (void)xQueueSend(s_stream.queue, &wake, pdMS_TO_TICKS(100U));
    for (uint32_t wait = 0U; wait < 100U; ++wait)
    {
        if (atomic_load_explicit(&s_stream.exited, memory_order_acquire))
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50U));
    }
    if (!atomic_load_explicit(&s_stream.exited, memory_order_acquire))
    {
        _stream_abort_http();
        for (uint32_t wait = 0U; wait < 100U; ++wait)
        {
            if (atomic_load_explicit(&s_stream.exited, memory_order_acquire))
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50U));
        }
    }
    esp_err_t result = ESP_OK;
    if (!atomic_load_explicit(&s_stream.exited, memory_order_acquire))
    {
        LOG_E("stream task did not stop cooperatively");
        return ESP_ERR_TIMEOUT;
    }
    if (s_stream.audio_lock != NULL)
    {
        result = _stream_close_audio();
        if (result != ESP_OK)
        {
            LOG_E("audio lease release failed during deinit: %s",
                  esp_err_to_name(result));
            return result;
        }
    }
    if (s_stream.queue != NULL)
    {
        vQueueDeleteWithCaps(s_stream.queue);
        s_stream.queue = NULL;
    }
    if (s_stream.state_events != NULL)
    {
        vEventGroupDelete(s_stream.state_events);
        s_stream.state_events = NULL;
    }
    if (s_stream.audio_lock != NULL)
    {
        vSemaphoreDelete(s_stream.audio_lock);
        s_stream.audio_lock = NULL;
    }
    if (s_stream.lock != NULL)
    {
        vSemaphoreDelete(s_stream.lock);
        s_stream.lock = NULL;
    }
    _stream_unregister_decoders();
    memset(&s_stream, 0, sizeof(s_stream));
    return result;
}

esp_err_t music_stream_play(const char *url, uint32_t duration_ms,
                            uint32_t generation)
{
    if (!s_stream.initialized || url == NULL || url[0] == '\0' ||
            generation == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    music_stream_command_t command;
    memset(&command, 0, sizeof(command));
    (void)snprintf(command.url, sizeof(command.url), "%s", url);
    command.duration_ms = duration_ms;
    command.generation = generation;
    const uint32_t previous_generation = atomic_exchange_explicit(
            &s_stream.play_generation, generation, memory_order_acq_rel);
    if (xQueueSend(s_stream.queue, &command, pdMS_TO_TICKS(100U)) != pdTRUE)
    {
        atomic_store_explicit(&s_stream.play_generation, previous_generation,
                              memory_order_release);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t music_stream_pause(bool pause)
{
    if (!s_stream.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (pause)
    {
        atomic_store_explicit(&s_stream.pause, true, memory_order_release);
        _stream_set_status(MUSIC_PLAYBACK_PAUSED, MUSIC_SERVICE_FAILURE_NONE);
        if (s_stream.state_events != NULL)
        {
            (void)xEventGroupSetBits(s_stream.state_events,
                                     MUSIC_STREAM_EVENT_STATE_CHANGED);
        }
        const esp_err_t result = _stream_pause_audio();
        if (result != ESP_OK)
        {
            LOG_W("audio pause release failed: %s",
                  esp_err_to_name(result));
        }
        return result;
    }

    atomic_store_explicit(&s_stream.pause, false, memory_order_release);
    _stream_set_status(MUSIC_PLAYBACK_BUFFERING, MUSIC_SERVICE_FAILURE_NONE);
    if (s_stream.state_events != NULL)
    {
        (void)xEventGroupSetBits(s_stream.state_events,
                                 MUSIC_STREAM_EVENT_STATE_CHANGED);
    }
    return ESP_OK;
}

esp_err_t music_stream_stop(void)
{
    if (!s_stream.initialized)
    {
        return ESP_OK;
    }
    atomic_store_explicit(&s_stream.cancel_generation,
                          atomic_load_explicit(&s_stream.play_generation,
                                  memory_order_acquire),
                          memory_order_release);
    atomic_store_explicit(&s_stream.stop, true, memory_order_release);
    if (s_stream.state_events != NULL)
    {
        (void)xEventGroupSetBits(s_stream.state_events,
                                 MUSIC_STREAM_EVENT_STATE_CHANGED);
    }
    _stream_abort_http();
    return ESP_OK;
}

uint32_t music_stream_get_stack_high_water(void)
{
    return s_stream.task != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(
               s_stream.task) : 0U;
}

bool music_stream_is_playing(void)
{
    const int state = atomic_load_explicit(&s_stream.public_state,
                                           memory_order_acquire);
    return s_stream.initialized &&
           (state == (int)MUSIC_PLAYBACK_PLAYING ||
            state == (int)MUSIC_PLAYBACK_BUFFERING);
}

esp_err_t music_stream_get_status(music_stream_status_t *status)
{
    if (status == NULL || !s_stream.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_stream.lock, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    *status = s_stream.status;
    (void)xSemaphoreGive(s_stream.lock);
    return ESP_OK;
}
