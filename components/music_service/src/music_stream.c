#define DBG_TAG "music_stream"
#include "mt_log.h"

#include "music_service_internal.h"

#include "audio_service.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_audio_simple_dec.h"
#include "freertos/FreeRTOS.h"
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

typedef struct music_stream_command
{
    char url[MUSIC_SERVICE_URL_BYTES];
    uint32_t duration_ms;
    uint32_t generation;
} music_stream_command_t;

typedef struct music_stream_runtime
{
    bool initialized;
    QueueHandle_t queue;
    TaskHandle_t task;
    SemaphoreHandle_t lock;
    music_stream_status_t status;
    audio_service_config_t default_audio;
    audio_service_lease_t lease;
    bool lease_held;
    bool pause_suspended;
    esp_http_client_handle_t http_client;
    atomic_bool stop;
    atomic_bool pause;
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
    if (length >= 5U && strcasecmp(url + length - 5U, ".m4a") == 0)
    {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    if (length >= 4U && strcasecmp(url + length - 4U, ".aac") == 0)
    {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
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
        return result;
    }
    result = audio_service_acquire(AUDIO_SERVICE_SESSION_PLAYBACK,
                                   &s_stream.lease);
    if (result == ESP_OK)
    {
        s_stream.lease_held = true;
    }
    return result;
}

static esp_err_t _stream_close_audio(void)
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
        s_stream.pause_suspended = false;
    }
    (void)audio_service_configure(&s_stream.default_audio);
    return ESP_OK;
}

static void _stream_play_url(const char *url, uint32_t duration_ms)
{
    bool failed = false;
    esp_http_client_handle_t client = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    uint8_t *input = NULL;
    uint8_t *pcm = NULL;
    bool audio_ready = false;
    uint64_t played_bytes = 0U;
    uint32_t sample_rate = 44100U;
    uint8_t channels = 2U;
    uint8_t bits = 16U;

    _stream_set_status(MUSIC_PLAYBACK_BUFFERING,
                       MUSIC_SERVICE_FAILURE_NONE);
    if (s_stream.lock != NULL &&
            xSemaphoreTake(s_stream.lock, portMAX_DELAY) == pdTRUE)
    {
        s_stream.status.duration_ms = duration_ms;
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
        failed = true;
        goto cleanup;
    }

    input = heap_caps_malloc(MUSIC_STREAM_READ_BYTES,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pcm = heap_caps_malloc(MUSIC_STREAM_PCM_BYTES,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input == NULL || pcm == NULL)
    {
        _stream_set_status(MUSIC_PLAYBACK_ERROR,
                           MUSIC_SERVICE_FAILURE_INTERNAL);
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
            vTaskDelay(pdMS_TO_TICKS(50U));
            continue;
        }
        const int read_length = esp_http_client_read(client, (char *)input,
            MUSIC_STREAM_READ_BYTES);
        if (read_length < 0)
        {
            _stream_set_status(MUSIC_PLAYBACK_ERROR,
                               MUSIC_SERVICE_FAILURE_NETWORK);
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
        while (raw.consumed < raw.len &&
                !atomic_load_explicit(&s_stream.stop, memory_order_acquire))
        {
            esp_audio_simple_dec_out_t output =
            {
                .buffer = pcm,
                .len = MUSIC_STREAM_PCM_BYTES,
            };
            const esp_audio_err_t decode_result =
                esp_audio_simple_dec_process(decoder, &raw, &output);
            if (decode_result != ESP_AUDIO_ERR_OK &&
                    decode_result != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH)
            {
                _stream_set_status(MUSIC_PLAYBACK_ERROR,
                                   MUSIC_SERVICE_FAILURE_DECODE);
                failed = true;
                fatal = true;
                break;
            }
            if (output.decoded_size > 0U)
            {
                if (!audio_ready)
                {
                    esp_audio_simple_dec_info_t info;
                    if (esp_audio_simple_dec_get_info(decoder, &info) ==
                            ESP_AUDIO_ERR_OK)
                    {
                        sample_rate = info.sample_rate;
                        channels = info.channel == 0U ? 2U : info.channel;
                        bits = info.bits_per_sample == 0U ? 16U :
                               info.bits_per_sample;
                    }
                    if (_stream_open_audio(sample_rate, bits, channels) !=
                            ESP_OK)
                    {
                        _stream_set_status(MUSIC_PLAYBACK_ERROR,
                                           MUSIC_SERVICE_FAILURE_AUDIO_BUSY);
                        failed = true;
                        fatal = true;
                        break;
                    }
                    audio_ready = true;
                }
                size_t written = 0U;
                const esp_err_t write_result = audio_service_write_leased(
                                                   s_stream.lease, output.buffer,
                                                   output.decoded_size, &written,
                                                   1000U);
                played_bytes += written;
                if (write_result != ESP_OK && write_result != ESP_ERR_TIMEOUT)
                {
                    if (write_result == ESP_ERR_INVALID_STATE &&
                            atomic_load_explicit(&s_stream.pause,
                                                 memory_order_acquire))
                    {
                        break;
                    }
                    _stream_set_status(MUSIC_PLAYBACK_ERROR,
                                       MUSIC_SERVICE_FAILURE_DECODE);
                    failed = true;
                    fatal = true;
                    break;
                }
                const uint64_t bytes_per_second =
                    (uint64_t)sample_rate * channels * (bits / 8U);
                if (bytes_per_second > 0U)
                {
                    _stream_update_position((uint32_t)(played_bytes * 1000U /
                                                       bytes_per_second));
                }
            }
            if (output.decoded_size == 0U && raw.consumed == 0U)
            {
                break;
            }
        }
        if (fatal || atomic_load_explicit(&s_stream.stop,
                                          memory_order_acquire))
        {
            break;
        }
    }

cleanup:
    if (audio_ready)
    {
        if (_stream_close_audio() != ESP_OK)
        {
            failed = true;
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
        _stream_play_url(command.url, command.duration_ms);
        if (atomic_load_explicit(&s_stream.shutdown, memory_order_acquire))
        {
            break;
        }
    }
    atomic_store_explicit(&s_stream.exited, true, memory_order_release);
    vTaskDelete(NULL);
}

esp_err_t music_stream_init(uint32_t priority)
{
    if (s_stream.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_stream, 0, sizeof(s_stream));
    s_stream.lock = xSemaphoreCreateMutex();
    s_stream.queue = xQueueCreate(MUSIC_STREAM_QUEUE_DEPTH,
                                  sizeof(music_stream_command_t));
    if (s_stream.lock == NULL || s_stream.queue == NULL)
    {
        if (s_stream.queue != NULL)
        {
            vQueueDelete(s_stream.queue);
        }
        if (s_stream.lock != NULL)
        {
            vSemaphoreDelete(s_stream.lock);
        }
        memset(&s_stream, 0, sizeof(s_stream));
        return ESP_ERR_NO_MEM;
    }
    if (audio_service_get_config(&s_stream.default_audio) != ESP_OK)
    {
        memset(&s_stream.default_audio, 0, sizeof(s_stream.default_audio));
    }
    if (xTaskCreatePinnedToCoreWithCaps(_stream_task, "music_stream",
                                        CONFIG_MUSIC_SERVICE_AUDIO_TASK_STACK_SIZE,
                                        NULL, priority, &s_stream.task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        vQueueDelete(s_stream.queue);
        vSemaphoreDelete(s_stream.lock);
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
    if (!atomic_load_explicit(&s_stream.exited, memory_order_acquire) &&
            s_stream.task != NULL)
    {
        LOG_E("stream task did not stop; forcing deletion");
        vTaskDelete(s_stream.task);
        s_stream.task = NULL;
    }
    if (s_stream.lease_held)
    {
        result = audio_service_release(s_stream.lease);
        if (result != ESP_OK)
        {
            LOG_E("audio lease release failed during deinit: %s",
                  esp_err_to_name(result));
            return result;
        }
        s_stream.lease_held = false;
    }
    if (s_stream.queue != NULL)
    {
        vQueueDelete(s_stream.queue);
        s_stream.queue = NULL;
    }
    if (s_stream.lock != NULL)
    {
        vSemaphoreDelete(s_stream.lock);
        s_stream.lock = NULL;
    }
    memset(&s_stream, 0, sizeof(s_stream));
    return result;
}

esp_err_t music_stream_play(const char *url, uint32_t duration_ms)
{
    if (!s_stream.initialized || url == NULL || url[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }
    music_stream_command_t command;
    memset(&command, 0, sizeof(command));
    (void)snprintf(command.url, sizeof(command.url), "%s", url);
    command.duration_ms = duration_ms;
    command.generation = atomic_fetch_add_explicit(
                             &s_stream.play_generation, 1U,
                             memory_order_acq_rel) + 1U;
    return xQueueSend(s_stream.queue, &command, pdMS_TO_TICKS(100U)) ==
           pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
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
        if (s_stream.lease_held && !s_stream.pause_suspended)
        {
            bool resume_required = false;
            const esp_err_t result = audio_service_suspend(
                                         1000U, &resume_required);
            if (result == ESP_OK)
            {
                s_stream.pause_suspended = resume_required;
            }
            else
            {
                LOG_W("audio pause suspend failed: %s",
                      esp_err_to_name(result));
            }
        }
    }
    else
    {
        atomic_store_explicit(&s_stream.pause, false, memory_order_release);
        if (s_stream.pause_suspended)
        {
            const esp_err_t result = audio_service_resume(1000U);
            if (result == ESP_OK)
            {
                s_stream.pause_suspended = false;
            }
            else
            {
                LOG_W("audio resume failed: %s", esp_err_to_name(result));
            }
        }
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
    _stream_abort_http();
    return ESP_OK;
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
