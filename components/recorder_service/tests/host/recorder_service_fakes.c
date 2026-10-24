#include "audio_service.h"
#include "recorder_service.h"
#include "sd_storage_service.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_ui;
static bool s_ui_registered;
static char s_mount[128] = "/tmp";
static uint32_t s_generation = 1U;
static uint32_t s_revision = 1U;
static uint32_t s_token;
static esp_err_t s_space_error;
static uint64_t s_free = UINT64_MAX;
static bool s_block;
static bool s_scanning;
static bool s_expect_finalize;
static bool s_noted;
static bool s_expect_discard;
static sd_storage_service_usage_kind_t s_block_usage_kind;
static bool s_block_usage;
static bool s_usage_blocked;
static struct
{
    sd_storage_service_usage_t usage;
    sd_storage_service_usage_kind_t kind;
} s_leases[16];

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000LL + now.tv_nsec / 1000LL;
}

void fake_sd_set_ui_thread(void)
{
    s_ui = pthread_self();
    s_ui_registered = true;
}

static unsigned _count(sd_storage_service_usage_kind_t kind)
{
    unsigned count = 0U;
    for (size_t i = 0U; i < 16U; ++i)
    {
        count += s_leases[i].usage.token != 0U && s_leases[i].kind == kind;
    }
    return count;
}

unsigned fake_sd_leases(sd_storage_service_usage_kind_t kind)
{
    pthread_mutex_lock(&s_mutex);
    const unsigned count = _count(kind);
    pthread_mutex_unlock(&s_mutex);
    return count;
}

static int _slot(const sd_storage_service_usage_t *usage)
{
    for (int i = 0; i < 16; ++i)
    {
        if (usage != NULL && usage->token != 0U &&
                usage->token == s_leases[i].usage.token &&
                usage->generation == s_leases[i].usage.generation)
        {
            return i;
        }
    }
    return -1;
}

void fake_sd_set_mount_path(const char *path)
{
    pthread_mutex_lock(&s_mutex);
    snprintf(s_mount, sizeof(s_mount), "%s", path != NULL ? path : "");
    ++s_generation;
    pthread_mutex_unlock(&s_mutex);
}

void fake_sd_mutate(bool remount)
{
    pthread_mutex_lock(&s_mutex);
    ++s_revision;
    s_generation += remount;
    pthread_mutex_unlock(&s_mutex);
}

void fake_sd_space(esp_err_t error, uint64_t bytes)
{
    pthread_mutex_lock(&s_mutex);
    s_space_error = error;
    s_free = bytes;
    pthread_mutex_unlock(&s_mutex);
}

void fake_sd_block_scan(bool block)
{
    pthread_mutex_lock(&s_mutex);
    s_block = block;
    if (block)
    {
        s_scanning = false;
    }
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_mutex);
}

void fake_sd_wait_scan(void)
{
    pthread_mutex_lock(&s_mutex);
    while (!s_scanning)
    {
        pthread_cond_wait(&s_cond, &s_mutex);
    }
    pthread_mutex_unlock(&s_mutex);
}

void fake_sd_expect_discard(void)
{
    pthread_mutex_lock(&s_mutex);
    s_expect_discard = true;
    pthread_mutex_unlock(&s_mutex);
}

void fake_sd_expect_finalize(bool expect)
{
    pthread_mutex_lock(&s_mutex);
    s_expect_finalize = expect;
    s_noted = false;
    pthread_mutex_unlock(&s_mutex);
}

void fake_sd_block_usage(sd_storage_service_usage_kind_t kind, bool block)
{
    pthread_mutex_lock(&s_mutex);
    s_block_usage_kind = kind;
    s_block_usage = block;
    s_usage_blocked = false;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_mutex);
}

void fake_sd_wait_usage(void)
{
    pthread_mutex_lock(&s_mutex);
    while (!s_usage_blocked)
    {
        pthread_cond_wait(&s_cond, &s_mutex);
    }
    pthread_mutex_unlock(&s_mutex);
}
const char *sd_storage_service_get_mount_path(void)
{
    assert(!s_ui_registered || !pthread_equal(s_ui, pthread_self()));
    pthread_mutex_lock(&s_mutex);
    assert(_count(SD_STORAGE_SERVICE_USAGE_ACCESS) +
           _count(SD_STORAGE_SERVICE_USAGE_SESSION) +
           _count(SD_STORAGE_SERVICE_USAGE_MAINTENANCE) > 0U);
    /* Copying is safe for the simulated fault/remount during the later space
     * barrier, after recorder path resolution has finished. */
    const char *path = s_mount[0] != '\0' ? s_mount : NULL;
    pthread_mutex_unlock(&s_mutex);
    return path;
}

esp_err_t sd_storage_service_get_snapshot(sd_storage_service_snapshot_t *snapshot)
{
    pthread_mutex_lock(&s_mutex);
    *snapshot = (sd_storage_service_snapshot_t)
    {
        .mounted = s_mount[0] != '\0',
        .generation = s_generation,
        .revision = s_revision,
    };
    pthread_mutex_unlock(&s_mutex);
    return ESP_OK;
}

uint32_t sd_storage_service_content_revision(void)
{
    pthread_mutex_lock(&s_mutex);
    const uint32_t revision = s_revision;
    pthread_mutex_unlock(&s_mutex);
    return revision;
}

esp_err_t sd_storage_service_usage_acquire(sd_storage_service_usage_kind_t kind,
        sd_storage_service_usage_t *usage)
{
    pthread_mutex_lock(&s_mutex);
    if (s_block_usage && kind == s_block_usage_kind)
    {
        s_usage_blocked = true;
        pthread_cond_broadcast(&s_cond);
        while (s_block_usage)
        {
            pthread_cond_wait(&s_cond, &s_mutex);
        }
    }
    const bool busy = _count(SD_STORAGE_SERVICE_USAGE_MAINTENANCE) > 0U ||
                      (kind == SD_STORAGE_SERVICE_USAGE_MAINTENANCE &&
                       (_count(SD_STORAGE_SERVICE_USAGE_ACCESS) > 0U ||
                        _count(SD_STORAGE_SERVICE_USAGE_SESSION) > 0U));
    if (s_mount[0] == '\0' || busy)
    {
        pthread_mutex_unlock(&s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0U; i < 16U; ++i)
    {
        if (s_leases[i].usage.token == 0U)
        {
            *usage = (sd_storage_service_usage_t)
            {
                .token = ++s_token, .generation = s_generation
            };
            s_leases[i].usage = *usage;
            s_leases[i].kind = kind;
            pthread_mutex_unlock(&s_mutex);
            return ESP_OK;
        }
    }
    pthread_mutex_unlock(&s_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t sd_storage_service_get_space(const sd_storage_service_usage_t *usage,
                                       uint64_t *total, uint64_t *free_bytes)
{
    assert(!s_ui_registered || !pthread_equal(s_ui, pthread_self()));
    pthread_mutex_lock(&s_mutex);
    assert(_slot(usage) >= 0);
    s_scanning = true;
    pthread_cond_broadcast(&s_cond);
    while (s_block)
    {
        pthread_cond_wait(&s_cond, &s_mutex);
    }
    *total = 1024ULL * 1024ULL * 1024ULL;
    *free_bytes = s_free;
    const esp_err_t result = s_space_error;
    pthread_mutex_unlock(&s_mutex);
    return result;
}

esp_err_t sd_storage_service_note_content_change(const sd_storage_service_usage_t *usage)
{
    pthread_mutex_lock(&s_mutex);
    assert(_slot(usage) >= 0);
    if (s_expect_finalize)
    {
        pthread_mutex_unlock(&s_mutex);
        recorder_service_snapshot_t snapshot;
        assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.state != RECORDER_SERVICE_IDLE);
        FILE *file = fopen(snapshot.name, "rb");
        char riff[4];
        assert(file != NULL && fread(riff, 1U, 4U, file) == 4U);
        assert(memcmp(riff, "RIFF", 4U) == 0 && fclose(file) == 0);
        pthread_mutex_lock(&s_mutex);
        s_noted = true;
    }
    ++s_revision;
    pthread_mutex_unlock(&s_mutex);
    return ESP_OK;
}

esp_err_t sd_storage_service_usage_release(const sd_storage_service_usage_t *usage)
{
    pthread_mutex_lock(&s_mutex);
    const int slot = _slot(usage);
    assert(slot >= 0);
    if (s_expect_finalize && s_leases[slot].kind == SD_STORAGE_SERVICE_USAGE_SESSION)
    {
        recorder_service_snapshot_t snapshot;
        /* Drop fake mutex: the getter also consults the storage snapshot. */
        pthread_mutex_unlock(&s_mutex);
        assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
        assert(snapshot.state != RECORDER_SERVICE_IDLE && s_noted);
        assert(recorder_service_start() == ESP_ERR_INVALID_STATE);
        pthread_mutex_lock(&s_mutex);
    }
    if (s_expect_discard && s_leases[slot].kind == SD_STORAGE_SERVICE_USAGE_ACCESS)
    {
        s_expect_discard = false;
        pthread_mutex_unlock(&s_mutex);
        recorder_service_file_t files[RECORDER_SERVICE_MAX_FILES];
        size_t count = 99U;
        assert(recorder_service_list(files, RECORDER_SERVICE_MAX_FILES, &count) == ESP_OK);
        assert(count == 0U);
        pthread_mutex_lock(&s_mutex);
    }
    memset(&s_leases[slot], 0, sizeof(s_leases[slot]));
    pthread_mutex_unlock(&s_mutex);
    return ESP_OK;
}

esp_err_t sd_storage_service_usage_report_fault(const sd_storage_service_usage_t *usage,
        esp_err_t error)
{
    (void)error;
    pthread_mutex_lock(&s_mutex);
    assert(_slot(usage) >= 0);
    ++s_generation;
    s_mount[0] = '\0';
    pthread_mutex_unlock(&s_mutex);
    return ESP_OK;
}

esp_err_t audio_service_get_config(audio_service_config_t *config)
{
    *config = (audio_service_config_t)
    {
        .sample_rate_hz = 16000U, .bits_per_sample = 16U, .channels = 2U
    };
    return ESP_OK;
}

unsigned s_fake_audio_starts;

esp_err_t audio_service_start(void)
{
    ++s_fake_audio_starts;
    return ESP_OK;
}

esp_err_t s_fake_audio_stop_error;

esp_err_t audio_service_stop(void)
{
    return s_fake_audio_stop_error;
}
