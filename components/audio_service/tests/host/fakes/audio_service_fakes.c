#include "audio_service_fakes.h"

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_service.h"
#include "freertos/semphr.h"
#include "nv_storage.h"

#include "nvs.h"

typedef enum audio_service_fake_semaphore_kind
{
    AUDIO_SERVICE_FAKE_SEMAPHORE_MUTEX = 0,
    AUDIO_SERVICE_FAKE_SEMAPHORE_BINARY,
} audio_service_fake_semaphore_kind_t;

struct audio_service_fake_semaphore
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    audio_service_fake_semaphore_kind_t kind;
    bool available;
};

static audio_service_fake_state_t s_fake;
static pthread_mutex_t s_io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_io_changed = PTHREAD_COND_INITIALIZER;
static uint32_t s_io_entered;
static uint32_t s_io_active;
static bool s_io_blocked;
static bool s_stop_blocked;
static bool s_stop_entered;

static struct timespec _deadline_after_ms(uint32_t timeout_ms)
{
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    const uint64_t nanoseconds = (uint64_t)deadline.tv_nsec +
                                 (uint64_t)timeout_ms * 1000000ULL;
    deadline.tv_sec += (time_t)(nanoseconds / 1000000000ULL);
    deadline.tv_nsec = (long)(nanoseconds % 1000000000ULL);
    return deadline;
}

static void _wait_for_io_release(void)
{
    (void)pthread_mutex_lock(&s_io_lock);
    ++s_io_entered;
    ++s_io_active;
    (void)pthread_cond_broadcast(&s_io_changed);
    while (s_io_blocked)
    {
        (void)pthread_cond_wait(&s_io_changed, &s_io_lock);
    }
    --s_io_active;
    (void)pthread_mutex_unlock(&s_io_lock);
}

static SemaphoreHandle_t _create_semaphore(
    audio_service_fake_semaphore_kind_t kind)
{
    SemaphoreHandle_t semaphore = calloc(1U, sizeof(*semaphore));
    if (semaphore == NULL)
    {
        return NULL;
    }
    if (pthread_mutex_init(&semaphore->lock, NULL) != 0)
    {
        free(semaphore);
        return NULL;
    }
    if (pthread_cond_init(&semaphore->changed, NULL) != 0)
    {
        (void)pthread_mutex_destroy(&semaphore->lock);
        free(semaphore);
        return NULL;
    }
    semaphore->kind = kind;
    return semaphore;
}

static bool _is_available(void)
{
    return s_fake.available;
}

static esp_err_t _configure(const bsp_audio_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_fake.config = *config;
    ++s_fake.configure_count;
    return s_fake.configure_result;
}

static esp_err_t _start(void)
{
    ++s_fake.start_count;
    s_fake.last_start_stop_count = s_fake.stop_count;
    if (s_fake.start_result == ESP_OK || s_fake.start_enables_before_failure)
    {
        s_fake.started = true;
    }
    return s_fake.start_result;
}

static esp_err_t _stop(void)
{
    (void)pthread_mutex_lock(&s_io_lock);
    s_fake.stop_during_io |= s_io_active != 0U;
    s_stop_entered = true;
    (void)pthread_cond_broadcast(&s_io_changed);
    while (s_stop_blocked)
    {
        (void)pthread_cond_wait(&s_io_changed, &s_io_lock);
    }
    (void)pthread_mutex_unlock(&s_io_lock);
    ++s_fake.stop_count;
    if (s_fake.stop_result == ESP_OK)
    {
        s_fake.started = false;
    }
    return s_fake.stop_result;
}

static esp_err_t _write(void *data, size_t size, size_t *written,
                        uint32_t timeout_ms)
{
    if (data == NULL || size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ++s_fake.write_count;
    s_fake.last_timeout_ms = timeout_ms;
    s_fake.state_during_io = audio_service_get_state();
    _wait_for_io_release();
    if (written != NULL)
    {
        *written = size;
    }
    return ESP_OK;
}

static esp_err_t _read(void *data, size_t size, size_t *read,
                       uint32_t timeout_ms)
{
    if (data == NULL || size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(data, 0x5a, size);
    ++s_fake.read_count;
    s_fake.last_timeout_ms = timeout_ms;
    s_fake.state_during_io = audio_service_get_state();
    _wait_for_io_release();
    if (read != NULL)
    {
        *read = size;
    }
    return ESP_OK;
}

static esp_err_t _set_volume(uint8_t volume)
{
    s_fake.volume = volume;
    return ESP_OK;
}

static uint8_t _get_volume(void)
{
    return s_fake.volume;
}

static esp_err_t _set_mute(bool muted)
{
    s_fake.muted = muted;
    return ESP_OK;
}

static bool _get_mute(void)
{
    return s_fake.muted;
}

static esp_err_t _set_pa(bool enabled)
{
    if (s_fake.set_pa_result == ESP_OK)
    {
        s_fake.pa_enabled = enabled;
    }
    return s_fake.set_pa_result;
}

static const bsp_audio_ops_t s_ops =
{
    .is_available = _is_available,
    .configure = _configure,
    .start = _start,
    .stop = _stop,
    .write = _write,
    .read = _read,
    .set_volume = _set_volume,
    .get_volume = _get_volume,
    .set_mute = _set_mute,
    .get_mute = _get_mute,
    .set_pa = _set_pa,
};

void audio_service_fakes_reset(void)
{
    (void)pthread_mutex_lock(&s_io_lock);
    s_io_blocked = false;
    s_stop_blocked = false;
    s_stop_entered = false;
    s_io_entered = 0U;
    s_io_active = 0U;
    (void)pthread_cond_broadcast(&s_io_changed);
    (void)pthread_mutex_unlock(&s_io_lock);
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.available = true;
    s_fake.volume = 60U;
    s_fake.configure_result = ESP_OK;
    s_fake.start_result = ESP_OK;
    s_fake.set_pa_result = ESP_OK;
    s_fake.stop_result = ESP_OK;
    audio_service_fakes_nv_reset();
}

/* ── Scalar NVS fake for the volume persistence tests ── */

static bool s_nv_present;
static uint8_t s_nv_value;
static unsigned s_nv_set_count;
static unsigned s_nv_erase_count;
static bool s_nv_fail_set;
static bool s_nv_fail_get;
static bool s_nv_fail_erase;

void audio_service_fakes_nv_reset(void)
{
    s_nv_present = false;
    s_nv_value = 0U;
    s_nv_set_count = 0U;
    s_nv_erase_count = 0U;
    s_nv_fail_set = false;
    s_nv_fail_get = false;
    s_nv_fail_erase = false;
}

bool audio_service_fakes_nv_present(void)
{
    return s_nv_present;
}

uint8_t audio_service_fakes_nv_value(void)
{
    return s_nv_value;
}

unsigned audio_service_fakes_nv_set_count(void)
{
    return s_nv_set_count;
}

unsigned audio_service_fakes_nv_erase_count(void)
{
    return s_nv_erase_count;
}

void audio_service_fakes_nv_fail_set(void)
{
    s_nv_fail_set = true;
}

void audio_service_fakes_nv_fail_get(void)
{
    s_nv_fail_get = true;
}

void audio_service_fakes_nv_fail_erase(void)
{
    s_nv_fail_erase = true;
}

void audio_service_fakes_nv_seed(uint8_t value)
{
    s_nv_present = true;
    s_nv_value = value;
}

esp_err_t nv_storage_set_u8(const char *key, uint8_t value)
{
    assert(key != NULL);
    assert(strcmp(key, "volume") == 0);
    ++s_nv_set_count;
    if (s_nv_fail_set)
    {
        s_nv_fail_set = false;
        return ESP_FAIL;
    }
    s_nv_present = true;
    s_nv_value = value;
    return ESP_OK;
}

esp_err_t nv_storage_get_u8(const char *key, uint8_t *output)
{
    assert(key != NULL);
    assert(output != NULL);
    assert(strcmp(key, "volume") == 0);
    if (s_nv_fail_get)
    {
        s_nv_fail_get = false;
        return ESP_FAIL;
    }
    if (!s_nv_present)
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *output = s_nv_value;
    return ESP_OK;
}

esp_err_t nv_storage_erase_key(const char *key)
{
    assert(key != NULL);
    assert(strcmp(key, "volume") == 0);
    ++s_nv_erase_count;
    if (s_nv_fail_erase)
    {
        s_nv_fail_erase = false;
        return ESP_FAIL;
    }
    if (!s_nv_present)
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_nv_present = false;
    return ESP_OK;
}

audio_service_fake_state_t *audio_service_fakes_state(void)
{
    return &s_fake;
}

void audio_service_fakes_block_io(bool blocked)
{
    (void)pthread_mutex_lock(&s_io_lock);
    s_io_blocked = blocked;
    if (!blocked)
    {
        (void)pthread_cond_broadcast(&s_io_changed);
    }
    (void)pthread_mutex_unlock(&s_io_lock);
}

bool audio_service_fakes_wait_for_io(uint32_t count, uint32_t timeout_ms)
{
    const struct timespec deadline = _deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_io_lock);
    int wait_result = 0;
    while (s_io_entered < count && wait_result == 0)
    {
        wait_result = pthread_cond_timedwait(&s_io_changed, &s_io_lock,
                                             &deadline);
    }
    const bool reached = s_io_entered >= count;
    (void)pthread_mutex_unlock(&s_io_lock);
    return reached;
}

void audio_service_fakes_block_stop(bool blocked)
{
    (void)pthread_mutex_lock(&s_io_lock);
    s_stop_blocked = blocked;
    (void)pthread_cond_broadcast(&s_io_changed);
    (void)pthread_mutex_unlock(&s_io_lock);
}

bool audio_service_fakes_wait_for_stop(uint32_t timeout_ms)
{
    const struct timespec deadline = _deadline_after_ms(timeout_ms);
    (void)pthread_mutex_lock(&s_io_lock);
    int wait_result = 0;
    while (!s_stop_entered && wait_result == 0)
    {
        wait_result = pthread_cond_timedwait(&s_io_changed, &s_io_lock,
                                             &deadline);
    }
    const bool entered = s_stop_entered;
    (void)pthread_mutex_unlock(&s_io_lock);
    return entered;
}

const bsp_audio_ops_t *bsp_hal_get_audio(void)
{
    return s_fake.expose_ops ? &s_ops : NULL;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return _create_semaphore(AUDIO_SERVICE_FAKE_SEMAPHORE_MUTEX);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return _create_semaphore(AUDIO_SERVICE_FAKE_SEMAPHORE_BINARY);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    if (semaphore == NULL)
    {
        return pdFALSE;
    }
    if (semaphore->kind == AUDIO_SERVICE_FAKE_SEMAPHORE_MUTEX)
    {
        return pthread_mutex_lock(&semaphore->lock) == 0 ? pdTRUE : pdFALSE;
    }

    (void)pthread_mutex_lock(&semaphore->lock);
    int wait_result = 0;
    if (timeout == portMAX_DELAY)
    {
        while (!semaphore->available && wait_result == 0)
        {
            wait_result = pthread_cond_wait(&semaphore->changed,
                                            &semaphore->lock);
        }
    }
    else
    {
        const struct timespec deadline = _deadline_after_ms(timeout);
        while (!semaphore->available && timeout != 0U && wait_result == 0)
        {
            wait_result = pthread_cond_timedwait(&semaphore->changed,
                                                 &semaphore->lock,
                                                 &deadline);
        }
    }
    const bool taken = semaphore->available;
    if (taken)
    {
        semaphore->available = false;
    }
    (void)pthread_mutex_unlock(&semaphore->lock);
    return taken ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL)
    {
        return pdFALSE;
    }
    if (semaphore->kind == AUDIO_SERVICE_FAKE_SEMAPHORE_MUTEX)
    {
        return pthread_mutex_unlock(&semaphore->lock) == 0 ? pdTRUE : pdFALSE;
    }

    (void)pthread_mutex_lock(&semaphore->lock);
    const bool available = semaphore->available;
    semaphore->available = true;
    (void)pthread_cond_signal(&semaphore->changed);
    (void)pthread_mutex_unlock(&semaphore->lock);
    return available ? pdFALSE : pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL)
    {
        return;
    }
    (void)pthread_cond_destroy(&semaphore->changed);
    (void)pthread_mutex_destroy(&semaphore->lock);
    free(semaphore);
}
