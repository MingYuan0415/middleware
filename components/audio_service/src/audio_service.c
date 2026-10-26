#include "audio_service.h"

#include "bsp_hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nv_storage.h"

#include "nvs.h"

#include <string.h>

#define DBG_TAG "audio_service"
#include "mt_log.h"

/* Speaker level persisted across boots. Owned here like every other module
 * that stores its own setting, so the key never leaks to the UI bridge. */
#define AUDIO_SERVICE_VOLUME_KEY "volume"

typedef esp_err_t (*audio_service_io_op_t)(void *data, size_t bytes,
        size_t *transferred, uint32_t timeout_ms);

typedef enum audio_quiesce_mode
{
    AUDIO_QUIESCE_RELEASE,
    AUDIO_QUIESCE_SUSPEND,
    AUDIO_QUIESCE_DEINIT,
} audio_quiesce_mode_t;

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
    audio_service_lease_t lease;
    audio_service_session_kind_t session_kind;
    uint32_t lease_generation;
    bool leased;
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

static bool _lease_valid_locked(const audio_service_lease_t *lease)
{
    return lease != NULL && s_audio_service.leased &&
           lease->token == s_audio_service.lease.token &&
           lease->generation == s_audio_service.lease.generation;
}

static esp_err_t _acquire_io_lease(audio_service_lease_t lease, bool read,
                                   audio_service_io_op_t *leased_operation)
{
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    audio_service_io_op_t operation = read ? s_audio_service.ops.read :
                                      s_audio_service.ops.write;
    if (!s_audio_service.initialized || !_lease_valid_locked(&lease) ||
            !s_audio_service.io_admitted ||
            s_audio_service.state != AUDIO_SERVICE_STATE_RUNNING ||
            (read && s_audio_service.session_kind == AUDIO_SERVICE_SESSION_PLAYBACK) ||
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

static esp_err_t _quiesce(audio_quiesce_mode_t mode,
                          const audio_service_lease_t *lease,
                          uint32_t timeout_ms, bool *resume_required)
{
    if (resume_required != NULL)
    {
        *resume_required = false;
    }
    if (mode == AUDIO_QUIESCE_DEINIT && s_audio_service.lock == NULL)
    {
        return ESP_OK;
    }
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    bool service_locked = true;
    const audio_service_state_t initial_state = s_audio_service.state;
    const bool initial_admission = s_audio_service.io_admitted;
    if (initial_state == AUDIO_SERVICE_STATE_SUSPENDING ||
            (mode == AUDIO_QUIESCE_RELEASE && !_lease_valid_locked(lease)) ||
            (mode == AUDIO_QUIESCE_DEINIT && s_audio_service.leased))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!s_audio_service.initialized && !s_audio_service.stop_required)
    {
        if (mode == AUDIO_QUIESCE_DEINIT)
        {
            goto commit;
        }
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (initial_state == AUDIO_SERVICE_STATE_READY && mode != AUDIO_QUIESCE_DEINIT)
    {
        goto commit;
    }
    if (initial_state != AUDIO_SERVICE_STATE_READY &&
            initial_state != AUDIO_SERVICE_STATE_RUNNING &&
            initial_state != AUDIO_SERVICE_STATE_ERROR)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (resume_required != NULL)
    {
        *resume_required = initial_state == AUDIO_SERVICE_STATE_RUNNING;
    }
    s_audio_service.io_admitted = false;
    s_audio_service.state = AUDIO_SERVICE_STATE_SUSPENDING;
    if (s_audio_service.active_io != 0U)
    {
        (void)xSemaphoreTake(s_audio_service.drained, 0U);
        s_audio_service.drain_waiting = true;
        _unlock_service();
        service_locked = false;
        const BaseType_t drained = xSemaphoreTake(s_audio_service.drained,
            _timeout_to_ticks(timeout_ms));
        result = _lock_service();
        if (result != ESP_OK)
        {
            goto exit;
        }
        service_locked = true;
        s_audio_service.drain_waiting = false;
        if (drained != pdTRUE && s_audio_service.active_io != 0U)
        {
            s_audio_service.state = initial_state;
            s_audio_service.io_admitted = initial_admission;
            result = ESP_ERR_TIMEOUT;
            goto exit;
        }
    }
    result = s_audio_service.ops.stop();
    if (result != ESP_OK && mode == AUDIO_QUIESCE_DEINIT && !_ops_available_locked())
    {
        result = ESP_OK;
    }
    s_audio_service.state = result == ESP_OK ? AUDIO_SERVICE_STATE_READY :
                            AUDIO_SERVICE_STATE_ERROR;
    if (result != ESP_OK)
    {
        goto exit;
    }

commit:
    if (mode != AUDIO_QUIESCE_SUSPEND)
    {
        s_audio_service.leased = false;
        memset(&s_audio_service.lease, 0, sizeof(s_audio_service.lease));
        s_audio_service.session_kind = AUDIO_SERVICE_SESSION_RECORD;
    }
    if (mode == AUDIO_QUIESCE_DEINIT)
    {
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
    }

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
    s_audio_service.leased = false;
    memset(&s_audio_service.lease, 0, sizeof(s_audio_service.lease));
    s_audio_service.session_kind = AUDIO_SERVICE_SESSION_RECORD;

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
    return _quiesce(AUDIO_QUIESCE_DEINIT, NULL,
                    AUDIO_SERVICE_WAIT_FOREVER, NULL);
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
    if (!s_audio_service.initialized || s_audio_service.leased ||
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

esp_err_t audio_service_acquire(audio_service_session_kind_t kind,
                                audio_service_lease_t *lease)
{
    if (lease == NULL ||
            (unsigned)kind > (unsigned)AUDIO_SERVICE_SESSION_MAINTENANCE)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *lease = (audio_service_lease_t)
    {
        0U, 0U
    };
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized || s_audio_service.leased ||
            (s_audio_service.state != AUDIO_SERVICE_STATE_READY &&
             s_audio_service.state != AUDIO_SERVICE_STATE_ERROR) ||
            s_audio_service.ops.start == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (s_audio_service.state == AUDIO_SERVICE_STATE_ERROR)
    {
        result = s_audio_service.ops.stop();
        if (result != ESP_OK)
        {
            goto exit;
        }
        s_audio_service.state = AUDIO_SERVICE_STATE_READY;
    }
    result = s_audio_service.ops.start();
    if (result == ESP_OK)
    {
        uint32_t generation = ++s_audio_service.lease_generation;
        if (generation == 0U)
        {
            generation = ++s_audio_service.lease_generation;
        }
        s_audio_service.lease.token = generation;
        s_audio_service.lease.generation = generation;
        s_audio_service.session_kind = kind;
        s_audio_service.leased = true;
        s_audio_service.state = AUDIO_SERVICE_STATE_RUNNING;
        s_audio_service.io_admitted = true;
        *lease = s_audio_service.lease;
    }
    else
    {
        const esp_err_t cleanup = s_audio_service.ops.stop();
        s_audio_service.state = cleanup == ESP_OK ? AUDIO_SERVICE_STATE_READY :
                                AUDIO_SERVICE_STATE_ERROR;
        s_audio_service.io_admitted = false;
    }

exit:
    _unlock_service();
    return result;
}

esp_err_t audio_service_release(audio_service_lease_t lease)
{
    return _quiesce(AUDIO_QUIESCE_RELEASE, &lease,
                    AUDIO_SERVICE_WAIT_FOREVER, NULL);
}

esp_err_t audio_service_suspend(uint32_t timeout_ms, bool *resume_required)
{
    if (resume_required == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return _quiesce(AUDIO_QUIESCE_SUSPEND, NULL, timeout_ms, resume_required);
}

esp_err_t audio_service_resume(uint32_t timeout_ms)
{
    (void)timeout_ms;
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.initialized ||
            s_audio_service.state == AUDIO_SERVICE_STATE_SUSPENDING)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (s_audio_service.state == AUDIO_SERVICE_STATE_READY && !s_audio_service.leased)
    {
        goto exit;
    }
    if (!s_audio_service.leased)
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
    if (s_audio_service.state != AUDIO_SERVICE_STATE_READY &&
            s_audio_service.state != AUDIO_SERVICE_STATE_ERROR)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (s_audio_service.ops.start == NULL)
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

esp_err_t audio_service_write_leased(audio_service_lease_t lease,
                                     void *data, size_t bytes,
                                     size_t *written, uint32_t timeout_ms)
{
    if (written != NULL)
    {
        *written = 0U;
    }
    if (data == NULL || bytes == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    audio_service_io_op_t operation = NULL;
    esp_err_t result = _acquire_io_lease(lease, false, &operation);
    if (result == ESP_OK)
    {
        result = operation(data, bytes, written, timeout_ms);
        _release_io_lease();
    }
    return result;
}

esp_err_t audio_service_read_leased(audio_service_lease_t lease,
                                    void *data, size_t bytes,
                                    size_t *read, uint32_t timeout_ms)
{
    if (read != NULL)
    {
        *read = 0U;
    }
    if (data == NULL || bytes == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    audio_service_io_op_t operation = NULL;
    esp_err_t result = _acquire_io_lease(lease, true, &operation);
    if (result == ESP_OK)
    {
        result = operation(data, bytes, read, timeout_ms);
        _release_io_lease();
    }
    return result;
}

bool audio_service_is_leased(void)
{
    if (_lock_service() != ESP_OK)
    {
        return false;
    }
    const bool leased = s_audio_service.leased;
    _unlock_service();
    return leased;
}

esp_err_t audio_service_get_lease(audio_service_lease_t *lease)
{
    if (lease == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = _lock_service();
    if (result != ESP_OK)
    {
        return result;
    }
    if (!s_audio_service.leased)
    {
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }
    *lease = s_audio_service.lease;
    result = ESP_OK;

exit:
    _unlock_service();
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

/* Storage never runs under the service lock: nv_storage commits can block on
 * flash, and holding the audio lock across them would stall every stream and
 * quiesce path. The level is already audible, so a failed write is reported
 * without rolling the speaker back. */
esp_err_t audio_service_set_volume_persisted(uint8_t percent)
{
    if (percent > 100U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t applied = audio_service_set_volume(percent);
    if (applied != ESP_OK)
    {
        return applied;
    }
    const esp_err_t stored = nv_storage_set_u8(AUDIO_SERVICE_VOLUME_KEY,
        percent);
    if (stored != ESP_OK)
    {
        LOG_W("volume store failed: %s", esp_err_to_name(stored));
    }
    return stored;
}

esp_err_t audio_service_restore_volume(void)
{
    uint8_t percent = 0U;
    const esp_err_t stored = nv_storage_get_u8(AUDIO_SERVICE_VOLUME_KEY,
        &percent);
    if (stored == ESP_ERR_NVS_NOT_FOUND)
    {
        /* Never written: the configured default applied by init() stands. */
        return ESP_OK;
    }
    if (stored != ESP_OK)
    {
        return stored;
    }
    if (percent > 100U)
    {
        /* Refuse a corrupt record instead of muting the speaker with it. */
        LOG_W("stored volume %u out of range, keeping the default",
              (unsigned)percent);
        return ESP_ERR_INVALID_STATE;
    }
    return audio_service_set_volume(percent);
}

esp_err_t audio_service_reset_persisted_state(void)
{
    const esp_err_t result = nv_storage_erase_key(AUDIO_SERVICE_VOLUME_KEY);
    /* An absent key is already the desired post-reset state. */
    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
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
