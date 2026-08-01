#include "audio_service.h"

#include "bsp_hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define DBG_TAG "audio_service"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

typedef esp_err_t (*audio_service_io_op_t)(void *data, size_t bytes,
        size_t *transferred, uint32_t timeout_ms);

typedef struct audio_service_context
{
    SemaphoreHandle_t lock;
    SemaphoreHandle_t drained;
    bsp_audio_ops_t ops;
    audio_service_init_config_t init_config;
    audio_service_config_t config;
    audio_service_state_t state;
    uint32_t active_io;
    bool initialized;
    bool ops_registered;
    bool pa_enabled;
    bool io_admitted;
    bool drain_waiting;
    bool stop_required;
} audio_service_context_t;

static audio_service_context_t s_audio_service =
{
    .state = AUDIO_SERVICE_STATE_UNINITIALIZED,
};

static esp_err_t _lock_service(void)
{
    if (s_audio_service.lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_audio_service.lock, portMAX_DELAY) == pdTRUE ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

static void _unlock_service(void)
{
    if (s_audio_service.lock != NULL)
    {
        (void)xSemaphoreGive(s_audio_service.lock);
    }
}

static TickType_t _timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == AUDIO_SERVICE_WAIT_FOREVER)
    {
        return portMAX_DELAY;
    }

    uint64_t ticks = ((uint64_t)timeout_ms * (uint64_t)configTICK_RATE_HZ +
                      999ULL) / 1000ULL;
    if (timeout_ms != 0U && ticks == 0U)
    {
        ticks = 1U;
    }
    if (ticks >= (uint64_t)portMAX_DELAY)
    {
        ticks = (uint64_t)portMAX_DELAY - 1U;
    }
    return (TickType_t)ticks;
}

static bsp_audio_config_t _to_bsp_config(const audio_service_config_t *config)
{
    const bsp_audio_config_t bsp_config =
    {
        .sample_rate_hz = config->sample_rate_hz,
        .bits_per_sample = config->bits_per_sample,
        .channels = config->channels,
        .mclk_multiple = config->mclk_multiple,
    };
    return bsp_config;
}

static bool _init_config_valid(const audio_service_init_config_t *config)
{
    return config != NULL && config->stream.sample_rate_hz != 0U &&
           config->stream.bits_per_sample != 0U &&
           config->stream.channels != 0U &&
           config->stream.mclk_multiple != 0U &&
           config->volume_percent <= 100U;
}

static bool _init_config_equal_locked(const audio_service_init_config_t *config)
{
    return s_audio_service.init_config.stream.sample_rate_hz ==
           config->stream.sample_rate_hz &&
           s_audio_service.init_config.stream.bits_per_sample ==
           config->stream.bits_per_sample &&
           s_audio_service.init_config.stream.channels ==
           config->stream.channels &&
           s_audio_service.init_config.stream.mclk_multiple ==
           config->stream.mclk_multiple &&
           s_audio_service.init_config.volume_percent == config->volume_percent &&
           s_audio_service.init_config.muted == config->muted &&
           s_audio_service.init_config.pa_enabled == config->pa_enabled;
}

static bool _ops_available_locked(void)
{
    return s_audio_service.ops_registered &&
           s_audio_service.ops.is_available != NULL &&
           s_audio_service.ops.is_available();
}

static esp_err_t _acquire_io_lease(bool read,
                                   audio_service_io_op_t *leased_operation)
{
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    audio_service_io_op_t operation = read ? s_audio_service.ops.read :
                                      s_audio_service.ops.write;
    if (!s_audio_service.initialized || !s_audio_service.io_admitted ||
            s_audio_service.state != AUDIO_SERVICE_STATE_RUNNING ||
            operation == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        ++s_audio_service.active_io;
        *leased_operation = operation;
    }
    _unlock_service();
    return result;
}

static void _release_io_lease(void)
{
    if (_lock_service() != ESP_OK)
    {
        return;
    }
    --s_audio_service.active_io;
    if (s_audio_service.active_io == 0U && s_audio_service.drain_waiting)
    {
        (void)xSemaphoreGive(s_audio_service.drained);
    }
    _unlock_service();
}

static esp_err_t _quiesce(uint32_t timeout_ms, bool stop_ready,
                          bool accept_unavailable,
                          bool *resume_required)
{
    audio_service_state_t initial_state = AUDIO_SERVICE_STATE_UNINITIALIZED;
    bool service_locked = false;
    bool was_running = false;

    if (resume_required != NULL)
    {
        *resume_required = false;
    }

    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    service_locked = true;
    if (!s_audio_service.initialized && !s_audio_service.stop_required)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    initial_state = s_audio_service.state;
    if (initial_state == AUDIO_SERVICE_STATE_READY && !stop_ready)
    {
        result = ESP_OK;
        goto exit;
    }
    if (initial_state != AUDIO_SERVICE_STATE_READY &&
            initial_state != AUDIO_SERVICE_STATE_RUNNING &&
            initial_state != AUDIO_SERVICE_STATE_ERROR)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    was_running = initial_state == AUDIO_SERVICE_STATE_RUNNING;
    if (resume_required != NULL)
    {
        *resume_required = was_running;
    }
    (void)xSemaphoreTake(s_audio_service.drained, 0U);
    s_audio_service.io_admitted = false;
    s_audio_service.state = AUDIO_SERVICE_STATE_SUSPENDING;
    const bool drain_required = s_audio_service.active_io != 0U;
    s_audio_service.drain_waiting = drain_required;
    _unlock_service();
    service_locked = false;

    BaseType_t drained = pdTRUE;
    if (drain_required)
    {
        drained = xSemaphoreTake(s_audio_service.drained,
                                 _timeout_to_ticks(timeout_ms));
    }

    result = _lock_service();
    if (result != ESP_OK)
    {
        goto exit;
    }
    service_locked = true;
    const bool io_quiesced = s_audio_service.active_io == 0U;
    s_audio_service.drain_waiting = false;
    if (drained != pdTRUE && !io_quiesced)
    {
        s_audio_service.state = initial_state;
        s_audio_service.io_admitted = was_running;
        result = ESP_ERR_TIMEOUT;
        goto exit;
    }

    result = s_audio_service.ops.stop();
    if (result != ESP_OK && accept_unavailable &&
            !_ops_available_locked())
    {
        result = ESP_OK;
    }
    s_audio_service.state = result == ESP_OK ? AUDIO_SERVICE_STATE_READY :
                            AUDIO_SERVICE_STATE_ERROR;

exit:
    if (service_locked)
    {
        _unlock_service();
    }
    return result;
}

esp_err_t audio_service_init(const audio_service_init_config_t *config)
{
    if (!_init_config_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio_service.lock == NULL)
    {
        s_audio_service.lock = xSemaphoreCreateMutex();
        if (s_audio_service.lock == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_audio_service.drained == NULL)
    {
        s_audio_service.drained = xSemaphoreCreateBinary();
        if (s_audio_service.drained == NULL)
        {
            vSemaphoreDelete(s_audio_service.lock);
            s_audio_service.lock = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (s_audio_service.initialized)
    {
        result = _init_config_equal_locked(config) ?
                 ESP_OK : ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (s_audio_service.stop_required)
    {
        _unlock_service();
        return ESP_ERR_INVALID_STATE;
    }
    const bsp_audio_ops_t *ops = bsp_hal_get_audio();
    if (ops == NULL)
    {
        s_audio_service.state = AUDIO_SERVICE_STATE_ERROR;
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    s_audio_service.ops = *ops;
    s_audio_service.ops_registered = true;
    if (!_ops_available_locked())
    {
        s_audio_service.state = AUDIO_SERVICE_STATE_ERROR;
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }

    s_audio_service.config = config->stream;
    const bsp_audio_config_t bsp_config = _to_bsp_config(&s_audio_service.config);
    s_audio_service.stop_required = true;
    result = s_audio_service.ops.configure(&bsp_config);
    if (result != ESP_OK)
    {
        s_audio_service.state = AUDIO_SERVICE_STATE_ERROR;
        goto exit;
    }
    result = s_audio_service.ops.set_volume(config->volume_percent);
    if (result != ESP_OK)
    {
        s_audio_service.state = AUDIO_SERVICE_STATE_ERROR;
        goto exit;
    }
    result = s_audio_service.ops.set_mute(config->muted);
    if (result != ESP_OK)
    {
        s_audio_service.state = AUDIO_SERVICE_STATE_ERROR;
        goto exit;
    }
    result = s_audio_service.ops.set_pa(config->pa_enabled);
    if (result != ESP_OK)
    {
        s_audio_service.state = AUDIO_SERVICE_STATE_ERROR;
        goto exit;
    }
    s_audio_service.initialized = true;
    s_audio_service.state = AUDIO_SERVICE_STATE_READY;
    s_audio_service.init_config = *config;
    s_audio_service.pa_enabled = config->pa_enabled;
    s_audio_service.active_io = 0U;
    s_audio_service.io_admitted = false;
    s_audio_service.drain_waiting = false;

exit:
    _unlock_service();
    return result;
}

esp_err_t audio_service_get_config(audio_service_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    *config = s_audio_service.config;

exit:
    _unlock_service();
    return result;
}

esp_err_t audio_service_deinit(void)
{
    if (s_audio_service.lock == NULL)
    {
        return ESP_OK;
    }
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    const bool cleanup_required = s_audio_service.initialized ||
                                  s_audio_service.stop_required;
    _unlock_service();

    if (cleanup_required)
    {
        result = _quiesce(AUDIO_SERVICE_WAIT_FOREVER, true, true, NULL);
        if (result != ESP_OK)
        {
            return result;
        }
    }

    result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    memset(&s_audio_service.ops, 0, sizeof(s_audio_service.ops));
    memset(&s_audio_service.init_config, 0, sizeof(s_audio_service.init_config));
    memset(&s_audio_service.config, 0, sizeof(s_audio_service.config));
    s_audio_service.initialized = false;
    s_audio_service.ops_registered = false;
    s_audio_service.pa_enabled = false;
    s_audio_service.io_admitted = false;
    s_audio_service.drain_waiting = false;
    s_audio_service.stop_required = false;
    s_audio_service.active_io = 0U;
    s_audio_service.state = AUDIO_SERVICE_STATE_UNINITIALIZED;
    _unlock_service();
    return ESP_OK;
}

bool audio_service_is_available(void)
{
    if (_lock_service() != ESP_OK)
    {
        return false;
    }
    const bool available = s_audio_service.initialized &&
                           _ops_available_locked();
    _unlock_service();
    return available;
}

audio_service_state_t audio_service_get_state(void)
{
    if (_lock_service() != ESP_OK)
    {
        return AUDIO_SERVICE_STATE_UNINITIALIZED;
    }
    const audio_service_state_t state = s_audio_service.state;
    _unlock_service();
    return state;
}

esp_err_t audio_service_configure(const audio_service_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized ||
            s_audio_service.state != AUDIO_SERVICE_STATE_READY)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    const bsp_audio_config_t bsp_config = _to_bsp_config(config);
    result = s_audio_service.ops.configure(&bsp_config);
    if (result == ESP_OK)
    {
        s_audio_service.config = *config;
        s_audio_service.state = AUDIO_SERVICE_STATE_READY;
        s_audio_service.io_admitted = false;
    }
    else if (!_ops_available_locked())
    {
        s_audio_service.state = AUDIO_SERVICE_STATE_ERROR;
    }

exit:
    _unlock_service();
    return result;
}

esp_err_t audio_service_start(void)
{
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (s_audio_service.state == AUDIO_SERVICE_STATE_RUNNING)
    {
        s_audio_service.io_admitted = true;
        result = ESP_OK;
        goto exit;
    }
    if (s_audio_service.state == AUDIO_SERVICE_STATE_SUSPENDING)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = s_audio_service.ops.start();
    s_audio_service.state = result == ESP_OK ? AUDIO_SERVICE_STATE_RUNNING :
                            AUDIO_SERVICE_STATE_ERROR;
    s_audio_service.io_admitted = result == ESP_OK;

exit:
    _unlock_service();
    return result;
}

esp_err_t audio_service_stop(void)
{
    return _quiesce(AUDIO_SERVICE_WAIT_FOREVER, false, false, NULL);
}

esp_err_t audio_service_suspend(uint32_t timeout_ms, bool *resume_required)
{
    if (resume_required == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return _quiesce(timeout_ms, false, false, resume_required);
}

esp_err_t audio_service_resume(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return audio_service_start();
}

esp_err_t audio_service_write(void *data, size_t bytes, size_t *written,
                              uint32_t timeout_ms)
{
    if (data == NULL || bytes == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (written != NULL)
    {
        *written = 0U;
    }
    audio_service_io_op_t write_op = NULL;
    esp_err_t result = _acquire_io_lease(false, &write_op);
    if (result == ESP_OK)
    {
        result = write_op(data, bytes, written, timeout_ms);
        _release_io_lease();
    }
    return result;
}

esp_err_t audio_service_read(void *data, size_t bytes, size_t *read,
                             uint32_t timeout_ms)
{
    if (data == NULL || bytes == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (read != NULL)
    {
        *read = 0U;
    }
    audio_service_io_op_t read_op = NULL;
    esp_err_t result = _acquire_io_lease(true, &read_op);
    if (result == ESP_OK)
    {
        result = read_op(data, bytes, read, timeout_ms);
        _release_io_lease();
    }
    return result;
}

esp_err_t audio_service_set_volume(uint8_t percent)
{
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = s_audio_service.ops.set_volume(percent);

exit:
    _unlock_service();
    return result;
}

esp_err_t audio_service_get_volume(uint8_t *percent)
{
    if (percent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    *percent = s_audio_service.ops.get_volume();
    result = ESP_OK;

exit:
    _unlock_service();
    return result;
}

esp_err_t audio_service_set_mute(bool muted)
{
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = s_audio_service.ops.set_mute(muted);

exit:
    _unlock_service();
    return result;
}

esp_err_t audio_service_get_mute(bool *muted)
{
    if (muted == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    *muted = s_audio_service.ops.get_mute();
    result = ESP_OK;

exit:
    _unlock_service();
    return result;
}

esp_err_t audio_service_set_pa(bool enabled)
{
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = s_audio_service.ops.set_pa(enabled);
    if (result == ESP_OK)
    {
        s_audio_service.pa_enabled = enabled;
    }

exit:
    _unlock_service();
    return result;
}

esp_err_t audio_service_get_pa(bool *enabled)
{
    if (enabled == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    *enabled = s_audio_service.pa_enabled;
    result = ESP_OK;

exit:
    _unlock_service();
    return result;
}
