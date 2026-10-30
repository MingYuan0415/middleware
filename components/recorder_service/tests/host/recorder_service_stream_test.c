/* Execute the real ESP_PLATFORM stream and command owner with controlled I/O. */
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "host_freertos.h"
#include "../../src/recorder_service.c"

extern void fake_sd_set_mount_path(const char *path);
extern unsigned fake_sd_leases(sd_storage_service_usage_kind_t kind);
extern bool fake_audio_io_allowed(audio_service_lease_t lease, bool read);
extern esp_err_t s_fake_audio_stop_error;
extern audio_service_lease_t fake_audio_active_lease(void);

static pthread_mutex_t s_io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_io_changed = PTHREAD_COND_INITIALIZER;
static unsigned s_entered;
static unsigned s_credits;
static bool s_write_through;
static bool s_read_failure;
static bool s_write_failure;
/* Writes that report ESP_ERR_TIMEOUT after partial progress. */
static unsigned s_write_partial_failures;
static atomic_bool s_fail_create;
static atomic_bool s_complete_before_create_return;
static atomic_bool s_join_done;
static char s_root[] = "/tmp/mt_recorder_stream_XXXXXX";
static size_t s_base_tasks;
static size_t s_base_caps;
static size_t s_base_wrong;
static size_t s_base_self;

static struct timespec _deadline(void)
{
    struct timespec value;
    assert(clock_gettime(CLOCK_REALTIME, &value) == 0);
    value.tv_sec += 2;
    return value;
}

#define WAIT_UNTIL(condition) do { \
    const int64_t deadline_us = esp_timer_get_time() + 2000000LL; \
    while (!(condition)) { \
        assert(esp_timer_get_time() < deadline_us); \
        usleep(1000U); \
    } \
} while (0)

static bool _running(void)
{
    return atomic_load_explicit(&s_recorder.task_running, memory_order_acquire);
}

BaseType_t __real_xTaskCreatePinnedToCoreWithCaps(
    void (*entry)(void *), const char *name, uint32_t depth,
    void *context, UBaseType_t priority, TaskHandle_t *task,
    BaseType_t core, UBaseType_t caps);
BaseType_t __wrap_xTaskCreatePinnedToCoreWithCaps(
    void (*entry)(void *), const char *name, uint32_t depth,
    void *context, UBaseType_t priority, TaskHandle_t *task,
    BaseType_t core, UBaseType_t caps)
{
    if (atomic_exchange(&s_fail_create, false)) return pdFAIL;
    const BaseType_t result = __real_xTaskCreatePinnedToCoreWithCaps(
                                  entry, name, depth, context, priority, task, core, caps);
    if (result == pdPASS && atomic_exchange(&s_complete_before_create_return, false))
    {
        /* Exercise completion before the owner has published the returned handle. */
        WAIT_UNTIL(!_running());
    }
    return result;
}

static esp_err_t _audio_gate(bool write)
{
    assert(pthread_mutex_lock(&s_io_lock) == 0);
    ++s_entered;
    assert(pthread_cond_broadcast(&s_io_changed) == 0);
    const struct timespec deadline = _deadline();
    while (s_credits == 0U && !(write && s_write_through))
    {
        assert(pthread_cond_timedwait(&s_io_changed, &s_io_lock, &deadline) == 0);
    }
    if (s_credits != 0U) --s_credits;
    const bool failed = write ? s_write_failure : s_read_failure;
    assert(pthread_mutex_unlock(&s_io_lock) == 0);
    return failed ? ESP_FAIL : ESP_OK;
}

esp_err_t audio_service_read_leased(audio_service_lease_t lease, void *data,
                                    size_t bytes, size_t *read_bytes,
                                    uint32_t timeout_ms)
{
    if (read_bytes != NULL) *read_bytes = 0U;
    if (!fake_audio_io_allowed(lease, true)) return ESP_ERR_INVALID_STATE;
    if (data == NULL || read_bytes == NULL) return ESP_ERR_INVALID_ARG;
    (void)timeout_ms;
    const esp_err_t result = _audio_gate(false);
    if (result == ESP_OK)
    {
        memset(data, 0x5a, bytes);
        *read_bytes = bytes;
    }
    return result;
}

esp_err_t audio_service_write_leased(audio_service_lease_t lease, void *data,
                                     size_t bytes, size_t *written,
                                     uint32_t timeout_ms)
{
    if (written != NULL) *written = 0U;
    if (!fake_audio_io_allowed(lease, false)) return ESP_ERR_INVALID_STATE;
    if (data == NULL) return ESP_ERR_INVALID_ARG;
    (void)timeout_ms;
    /* Playback receives what capture wrote, which is now filtered and gained,
     * so only the block length is a contract here. */
    (void)data;
    const esp_err_t result = _audio_gate(true);
    if (written != NULL) *written = result == ESP_OK ? bytes : 0U;
    if (s_write_partial_failures != 0U)
    {
        /* A BSP that ran out of DMA deadline still reports what it consumed;
         * the caller must treat this as progress rather than a dead stream. */
        --s_write_partial_failures;
        if (written != NULL) *written = bytes / 2U;
        return ESP_ERR_TIMEOUT;
    }
    return result;
}

static void _io_reset(bool write_through, bool read_failure, bool write_failure)
{
    assert(pthread_mutex_lock(&s_io_lock) == 0);
    s_entered = 0U;
    s_credits = 0U;
    s_write_through = write_through;
    s_read_failure = read_failure;
    s_write_failure = write_failure;
    s_write_partial_failures = 0U;
    assert(pthread_mutex_unlock(&s_io_lock) == 0);
}

static void _io_entered(unsigned count)
{
    assert(pthread_mutex_lock(&s_io_lock) == 0);
    const struct timespec deadline = _deadline();
    while (s_entered < count)
    {
        assert(pthread_cond_timedwait(&s_io_changed, &s_io_lock, &deadline) == 0);
    }
    assert(pthread_mutex_unlock(&s_io_lock) == 0);
}

static void _io_release(void)
{
    assert(pthread_mutex_lock(&s_io_lock) == 0);
    ++s_credits;
    assert(pthread_cond_broadcast(&s_io_changed) == 0);
    assert(pthread_mutex_unlock(&s_io_lock) == 0);
}

static recorder_service_snapshot_t _snapshot(void)
{
    recorder_service_snapshot_t snapshot;
    assert(recorder_service_get_snapshot(&snapshot) == ESP_OK);
    return snapshot;
}

static recorder_service_snapshot_t _command(void)
{
    WAIT_UNTIL(!_snapshot().operation_pending);
    return _snapshot();
}

static void _reaped(size_t owner_count)
{
    WAIT_UNTIL(host_caps_task_count() == s_base_caps);
    assert(host_caps_task_owner_delete_count() == owner_count + 1U);
    assert(host_dynamic_task_count() == s_base_tasks + 1U);
    assert(host_caps_task_wrong_delete_count() == s_base_wrong);
    assert(host_caps_task_self_delete_count() == s_base_self);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    _recorder_lock();
    assert(s_recorder.task == NULL && s_recorder.file == NULL);
    _recorder_unlock();
}

static unsigned _open_files(void)
{
    DIR *directory = opendir("/proc/self/fd");
    assert(directory != NULL);
    unsigned count = 0U;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (entry->d_name[0] != '.') ++count;
    }
    assert(closedir(directory) == 0);
    return count;
}

static void _start_record(void)
{
    _io_reset(false, false, false);
    assert(recorder_service_start() == ESP_OK);
    assert(_command().last_error == ESP_OK);
    _io_entered(1U);
    assert(_snapshot().state == RECORDER_SERVICE_RECORDING);
    assert(host_caps_task_count() == s_base_caps + 1U);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 1U);
    sd_storage_service_usage_t denied = {0};
    assert(sd_storage_service_usage_acquire(SD_STORAGE_SERVICE_USAGE_MAINTENANCE,
                                            &denied) == ESP_ERR_INVALID_STATE);
}

static bool _stop_requested(void)
{
    _recorder_lock();
    const bool stop = s_recorder.stop_requested;
    _recorder_unlock();
    return stop;
}

static void *_stop_thread(void *argument)
{
    (void)argument;
    assert(recorder_service_stop() == ESP_OK);
    assert(_command().last_error == ESP_OK);
    atomic_store(&s_join_done, true);
    return NULL;
}

static void *_deinit_thread(void *argument)
{
    (void)argument;
    assert(recorder_service_deinit() == ESP_OK);
    atomic_store(&s_join_done, true);
    return NULL;
}

static void _stop_blocked(bool deinit)
{
    pthread_t thread;
    atomic_store(&s_join_done, false);
    assert(pthread_create(&thread, NULL, deinit ? _deinit_thread : _stop_thread, NULL) == 0);
    WAIT_UNTIL(_stop_requested());
    _io_release();
    WAIT_UNTIL(atomic_load(&s_join_done));
    assert(pthread_join(thread, NULL) == 0);
}

static recorder_service_target_t _wav(void)
{
    const recorder_service_snapshot_t snapshot = _snapshot();
    FILE *file = fopen(snapshot.name, "rb");
    assert(file != NULL);
    recorder_wav_header_t header;
    assert(fread(&header, 1U, sizeof(header), file) == sizeof(header));
    assert(memcmp(header.riff, "RIFF", 4U) == 0);
    assert(memcmp(header.wave, "WAVE", 4U) == 0);
    assert(header.channels == 2U && header.sample_rate == 16000U);
    /* Every write is one RECORDER_PCM_BYTES block and the fake never returns a
     * short read, so a take of N gated blocks stores exactly N blocks. */
    assert(header.bits_per_sample == 16U);
    assert(header.data_size % RECORDER_PCM_BYTES == 0U);
    assert(header.data_size > 0U);
    assert(header.file_size == header.data_size + 36U);
    assert(fseek(file, 0, SEEK_END) == 0);
    assert(ftell(file) == (long)(sizeof(header) + header.data_size));
    assert(fclose(file) == 0);
    recorder_service_target_t target =
    {
        .generation = snapshot.storage_generation,
        .revision = snapshot.content_revision,
    };
    snprintf(target.name, sizeof(target.name), "%s", snapshot.name);
    return target;
}

static recorder_service_target_t _record_stop(void)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _start_record();
    _stop_blocked(false);
    assert(_snapshot().state == RECORDER_SERVICE_IDLE);
    _reaped(owners);
    return _wav();
}

static void _play_eof(const recorder_service_target_t *target, bool early_completion)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _io_reset(true, false, false);
    atomic_store(&s_complete_before_create_return, early_completion);
    assert(recorder_service_play(target) == ESP_OK);
    assert(_command().last_error == ESP_OK);
    WAIT_UNTIL(!_running());
    _reaped(owners); /* No stop or other user command drives this reap. */
    assert(_snapshot().state == RECORDER_SERVICE_IDLE);
}

static void _quick_restart(const recorder_service_target_t *target, bool next_play)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _io_reset(false, false, false);
    assert(recorder_service_play(target) == ESP_OK);
    assert(_command().last_error == ESP_OK);
    _io_entered(1U);
    _io_release();
    WAIT_UNTIL(!_running());
    /* Do not wait for owner reap before submitting the replacement. */
    _io_reset(false, false, false);
    assert((next_play ? recorder_service_play(target) : recorder_service_start()) == ESP_OK);
    assert(_command().last_error == ESP_OK);
    _io_entered(1U);
    assert(host_caps_task_owner_delete_count() == owners + 1U);
    assert(host_caps_task_count() == s_base_caps + 1U);
    _stop_blocked(false);
    _reaped(owners + 1U);
}

static void _is_busy_covers_active_session(const recorder_service_target_t *target)
{
    /* Standby admission reads this one predicate. A false negative here lets
     * the PM worker request light sleep while the recorder still owns the SD
     * card: the SPI clock stops mid-transfer and the read times out (0x107),
     * which the fault path then reports as a dead medium. */
    assert(!recorder_service_is_busy());

    _io_reset(false, false, false);
    assert(recorder_service_play(target) == ESP_OK);
    assert(_command().last_error == ESP_OK);
    _io_entered(1U);
    /* Playing: the session lease and the open file both outlive this poll. */
    assert(_snapshot().state == RECORDER_SERVICE_PLAYING);
    assert(recorder_service_is_busy());

    /* Completion publishes IDLE before the owner has cleared its stream, so
     * the snapshot alone must not decide: the file is still open here. */
    const size_t owners = host_caps_task_owner_delete_count();
    _io_release();
    WAIT_UNTIL(!_running());
    _recorder_lock();
    const bool stream_open = s_recorder.file != NULL;
    _recorder_unlock();
    if (stream_open)
    {
        assert(recorder_service_is_busy());
    }
    _reaped(owners);
    assert(!recorder_service_is_busy());

}

static void _creation_failures(const recorder_service_target_t *target)
{
    const unsigned descriptors = _open_files();
    const size_t owners = host_caps_task_owner_delete_count();
    const bool saved = atomic_exchange(&s_fail_create, true);
    assert(recorder_service_play(target) == ESP_OK);
    assert(_command().last_error == ESP_ERR_NO_MEM);
    assert(_snapshot().state == RECORDER_SERVICE_ERROR);
    assert(! _running());
    assert(_open_files() == descriptors);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    atomic_store(&s_fail_create, true);
    assert(recorder_service_start() == ESP_OK);
    assert(_command().last_error == ESP_ERR_NO_MEM);
    assert(_snapshot().state == RECORDER_SERVICE_ERROR);
    assert(!_running());
    assert(host_caps_task_count() == s_base_caps);
    assert(host_caps_task_owner_delete_count() == owners);
    assert(_open_files() == descriptors);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    _recorder_lock();
    assert(s_recorder.task == NULL && s_recorder.file == NULL);
    _recorder_unlock();
    atomic_store(&s_fail_create, saved);
}

static void _io_error(bool playback, const recorder_service_target_t *target)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _io_reset(false, !playback, playback);
    assert((playback ? recorder_service_play(target) : recorder_service_start()) == ESP_OK);
    assert(_command().last_error == ESP_OK);
    _io_entered(1U);
    _io_release();
    WAIT_UNTIL(!_running());
    _reaped(owners);
    assert(_snapshot().state == RECORDER_SERVICE_ERROR);
    assert(_snapshot().last_error == ESP_ERR_INVALID_RESPONSE);
}

static void _maximum_duration(void)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _start_record();
    const int64_t expired_start = esp_timer_get_time() - 11000000LL;
    _recorder_lock();
    s_recorder.started_us = expired_start;
    _recorder_unlock();
    _io_release();
    WAIT_UNTIL(!_running());
    _reaped(owners);
    assert(_snapshot().state == RECORDER_SERVICE_IDLE);
    (void)_wav();
}


/* A failed take must not leave its working file: .part names are invisible to
 * the library and can never be deleted from the UI. */
static size_t _part_files(void)
{
    char path[sizeof(s_root) + 16U];
    assert(snprintf(path, sizeof(path), "%s/Recordings", s_root) > 0);
    DIR *directory = opendir(path);
    assert(directory != NULL);
    size_t parts = 0U;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        const size_t length = strlen(entry->d_name);
        if (length > 5U && strcmp(entry->d_name + length - 5U, ".part") == 0)
        {
            ++parts;
        }
    }
    assert(closedir(directory) == 0);
    return parts;
}

static void _failed_take_leaves_no_part(void)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _io_error(false, NULL);
    assert(_part_files() == 0U);
    assert(_snapshot().state == RECORDER_SERVICE_ERROR);
    (void)owners;
}

/* Pausing must not queue capture for later: the file would grow with audio the
 * user never recorded while the reported duration stayed paused. The drain is
 * observable as I/O continuing while the state is PAUSED. */
static void _pause_discards_capture(void)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _start_record();
    assert(recorder_service_pause() == ESP_OK);
    for (unsigned attempt = 0U; attempt < 5000U; ++attempt)
    {
        if (_snapshot().state == RECORDER_SERVICE_PAUSED)
        {
            break;
        }
        usleep(1000U);
    }
    assert(_snapshot().state == RECORDER_SERVICE_PAUSED);
    const uint64_t paused_bytes = _snapshot().bytes;
    assert(pthread_mutex_lock(&s_io_lock) == 0);
    const unsigned entered_before = s_entered;
    assert(pthread_mutex_unlock(&s_io_lock) == 0);
    /* Grant several reads; the paused loop must consume them by discarding. */
    for (unsigned i = 0U; i < 4U; ++i)
    {
        _io_release();
        usleep(5000U);
    }
    assert(pthread_mutex_lock(&s_io_lock) == 0);
    const unsigned entered_after = s_entered;
    assert(pthread_mutex_unlock(&s_io_lock) == 0);
    assert(entered_after > entered_before);
    assert(_snapshot().bytes == paused_bytes);
    assert(_snapshot().state == RECORDER_SERVICE_PAUSED);
    assert(recorder_service_resume() == ESP_OK);
    for (unsigned attempt = 0U; attempt < 5000U; ++attempt)
    {
        if (_snapshot().state == RECORDER_SERVICE_RECORDING)
        {
            break;
        }
        usleep(1000U);
    }
    assert(_snapshot().state == RECORDER_SERVICE_RECORDING);
    _io_release();
    _stop_blocked(false);
    assert(_snapshot().state == RECORDER_SERVICE_IDLE);
    _reaped(owners);
}
/* The capture path is mono in the left I2S slot, so a take must reach both
 * channels at the configured gain; a silently quiet take was the original
 * complaint and the service had no way to observe it. */
static void _capture_gain_and_duplication(void)
{
    const size_t owners = host_caps_task_owner_delete_count();
    const recorder_service_config_t saved = s_recorder.config;
    _recorder_lock();
    s_recorder.config.input_gain_db = 6U;
    _recorder_unlock();
    /* Stop the take before reading it: the worker owns the file while it runs. */
    const recorder_service_target_t target = _record_stop();
    (void)target;

    const recorder_service_snapshot_t snapshot = _snapshot();
    FILE *file = fopen(snapshot.name, "rb");
    assert(file != NULL);
    recorder_wav_header_t header;
    assert(fread(&header, 1U, sizeof(header), file) == sizeof(header));
    assert(header.data_size > 0U);
    /* The mono source lands only in the left slot, so every stored frame must
     * carry the same value twice. The filters are exercised separately; here
     * the contract is the channel copy. */
    int16_t frame[2] = {0, 0};
    size_t inspected = 0U;
    while (inspected < header.data_size &&
            fread(frame, sizeof(frame), 1U, file) == 1U)
    {
        assert(frame[0] == frame[1]);
        inspected += sizeof(frame);
    }
    assert(inspected == header.data_size);
    assert(fclose(file) == 0);
    _recorder_lock();
    s_recorder.config = saved;
    _recorder_unlock();
    (void)owners;
}

/* A timeout is partial progress, not a dead stream. Ending the take on the
 * first short transfer threw away a playable recording; the contract says the
 * caller must consume what the BSP reported and keep going. */
static void _partial_write_keeps_playing(const recorder_service_target_t *target)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _io_reset(false, false, false);
    assert(recorder_service_play(target) == ESP_OK);
    assert(_command().last_error == ESP_OK);
    _io_entered(1U);
    /* Several timed-out writes in a row must not end playback. */
    s_write_partial_failures = 3U;
    for (unsigned i = 0U; i < 3U; ++i)
    {
        _io_release();
        usleep(2000U);
    }
    s_write_partial_failures = 0U;
    assert(_snapshot().state == RECORDER_SERVICE_PLAYING);
    assert(_snapshot().last_error == ESP_OK);
    _io_release();
    s_write_through = true;
    WAIT_UNTIL(!_running());
    _reaped(owners);
    assert(_snapshot().state == RECORDER_SERVICE_IDLE);
}

/* The capture filters are the software half of the buzz mitigation: a DC
 * offset and sub-sonic rumble must be removed, the 3.4 kHz corner must attenuate
 * the band where the measured interference lives, and speech must pass. */
static void _capture_filters(void)
{
    const recorder_service_config_t saved = s_recorder.config;
    const double pi = 3.14159265358979323846;
    const double rate = (double)RECORDER_SAMPLE_RATE_HZ;

    /* Gain is applied after the filters; set it to 0 dB so the response is
     * measured alone. */
    _recorder_lock();
    s_recorder.config.input_gain_db = 0U;
    _recorder_biquad_design(&s_recorder_high_pass, true);
    _recorder_biquad_design(&s_recorder_low_pass, false);
    _recorder_biquad_reset(&s_recorder_high_pass);
    _recorder_biquad_reset(&s_recorder_low_pass);
    _recorder_unlock();

    const size_t frames = 4096U;
    static int16_t buffer[2U * 4096U];
    struct
    {
        double hz;
        double max_gain;
        double min_gain;
    } cases[] =
    {
        /* 0 Hz is pure DC: the high-pass must remove essentially all of it. */
        {0.0, 0.02, 0.0},
        /* Speech band: essentially untouched. */
        {1000.0, 1.10, 0.90},
        /* 6 kHz is about 0.83 octaves above the corner, so a second-order
         * Butterworth gives roughly -19.5 dB (0.106x). */
        {6000.0, 0.13, 0.08},
        /* 7 kHz is where the measured interference peaks: -32 dB expected. */
        {7000.0, 0.04, 0.0},
    };

    for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); ++c)
    {
        for (size_t i = 0U; i < frames; ++i)
        {
            const double sample = 8000.0 * sin(2.0 * pi * cases[c].hz *
                                               (double)i / rate);
            buffer[2U * i] = (int16_t)sample;
            buffer[2U * i + 1U] = 0;
        }
        _recorder_lock();
        _recorder_apply_capture_shaping((uint8_t *)buffer,
                                        frames * 2U * sizeof(int16_t));
        _recorder_unlock();
        /* Skip the filter settling interval and measure the steady state. */
        double sum = 0.0;
        size_t counted = 0U;
        for (size_t i = 1024U; i < frames; ++i)
        {
            const double value = (double)buffer[2U * i];
            sum += value * value;
            ++counted;
        }
        const double rms = sqrt(sum / (double)counted);
        const double expected = 8000.0 / sqrt(2.0);
        const double gain = cases[c].hz == 0.0 ? rms / 8000.0 : rms / expected;
        assert(gain <= cases[c].max_gain && gain >= cases[c].min_gain);
        /* Both slots always agree. */
        for (size_t i = 0U; i < frames; i += 97U)
        {
            assert(buffer[2U * i] == buffer[2U * i + 1U]);
        }
    }
    _recorder_lock();
    s_recorder.config = saved;
    _recorder_unlock();
    puts("capture high-pass, low-pass and channel copy verified");
}

/* The expander works from the warm-up noise profile: hiss must fall to the
 * configured floor while speech keeps unity gain, otherwise a quiet passage
 * would pump. The signal is a 1 kHz tone so the capture filters pass it. */
static void _capture_noise_gate(void)
{
    const recorder_service_config_t saved = s_recorder.config;
    const double pi = 3.14159265358979323846;
    const size_t frames = 2048U;
    static int16_t buffer[2U * 2048U];

    _recorder_lock();
    s_recorder.config.input_gain_db = 0U;
    _recorder_biquad_design(&s_recorder_high_pass, true);
    _recorder_biquad_design(&s_recorder_low_pass, false);
    _recorder_biquad_reset(&s_recorder_high_pass);
    _recorder_biquad_reset(&s_recorder_low_pass);
    /* 200 counts of RMS floor: the gate opens at 600 and closes at 400, i.e.
     * 40000 and 160000 in the power domain it actually compares. */
    s_recorder.noise_power = 200.0F * 200.0F;
    s_recorder.gate_open = false;
    s_recorder.gate_hold = 0U;
    s_recorder.gate_gain = 1.0F;
    _recorder_unlock();

    /* 2048 frames is a whole number of 1 kHz cycles, so restarting the phase
     * every block keeps the tone continuous and does not fake a transient. */
    for (unsigned block = 0U; block < 8U; ++block)
    {
        for (size_t i = 0U; i < frames; ++i)
        {
            const double sample = 300.0 * sin(2.0 * pi * 1000.0 *
                                              (double)i / 16000.0);
            buffer[2U * i] = (int16_t)sample;
            buffer[2U * i + 1U] = 0;
        }
        _recorder_lock();
        _recorder_apply_capture_shaping((uint8_t *)buffer,
                                        frames * 2U * sizeof(int16_t));
        _recorder_unlock();
    }
    double sum = 0.0;
    for (size_t i = 0U; i < frames; ++i)
    {
        sum += (double)buffer[2U * i] * (double)buffer[2U * i];
    }
    const double quiet = sqrt(sum / (double)frames);
    /* -16 dB is above the -24 dB floor, which leaves room for the 5 ms gain
     * smoothing but still proves the expander closed. */
    assert(quiet < 0.15 * 300.0 / sqrt(2.0));

    for (unsigned block = 0U; block < 4U; ++block)
    {
        for (size_t i = 0U; i < frames; ++i)
        {
            const double sample = 8000.0 * sin(2.0 * pi * 1000.0 *
                                               (double)i / 16000.0);
            buffer[2U * i] = (int16_t)sample;
            buffer[2U * i + 1U] = 0;
        }
        _recorder_lock();
        _recorder_apply_capture_shaping((uint8_t *)buffer,
                                        frames * 2U * sizeof(int16_t));
        _recorder_unlock();
    }
    sum = 0.0;
    for (size_t i = 0U; i < frames; ++i)
    {
        sum += (double)buffer[2U * i] * (double)buffer[2U * i];
        assert(buffer[2U * i] == buffer[2U * i + 1U]);
    }
    const double loud = sqrt(sum / (double)frames);
    assert(loud > 0.95 * 8000.0 / sqrt(2.0));

    _recorder_lock();
    s_recorder.noise_power = 0.0F;
    s_recorder.gate_open = false;
    s_recorder.gate_hold = 0U;
    s_recorder.gate_gain = 1.0F;
    s_recorder.config = saved;
    _recorder_unlock();
    puts("capture noise gate verified");
}

/* The startup window is discarded before the clock starts, so an enabled
 * codec transient neither reaches the file nor inflates the reported
 * duration. */
static void _warmup_discards_leading_capture(void)
{
    const size_t owners = host_caps_task_owner_delete_count();
    const recorder_service_config_t saved = s_recorder.config;
    _recorder_lock();
    s_recorder.config.warmup_ms = 50U;
    _recorder_unlock();

    _start_record();
    /* Consume the whole warmup window, then one stored block. */
    for (unsigned i = 0U; i < 8U; ++i)
    {
        _io_release();
        usleep(1000U);
    }
    assert(_snapshot().bytes <= RECORDER_PCM_BYTES);
    _stop_blocked(false);
    assert(_snapshot().state == RECORDER_SERVICE_IDLE);
    _reaped(owners);
    _recorder_lock();
    s_recorder.config = saved;
    _recorder_unlock();
}

static void _release_failure_recovery(const recorder_service_config_t *config)
{
    const size_t owners = host_caps_task_owner_delete_count();
    _start_record();
    const audio_service_lease_t retained = fake_audio_active_lease();
    pthread_mutex_lock(&s_io_lock);
    s_read_failure = true;
    s_fake_audio_stop_error = ESP_FAIL;
    pthread_mutex_unlock(&s_io_lock);
    _io_release();
    WAIT_UNTIL(!_running());
    _reaped(owners);
    /* Stream I/O failure is the primary INVALID_RESPONSE; cleanup ESP_FAIL
     * must retain ownership without replacing that published operation error. */
    assert(_snapshot().state == RECORDER_SERVICE_ERROR);
    assert(_snapshot().last_error == ESP_ERR_INVALID_RESPONSE);
    assert(fake_audio_active_lease().generation == retained.generation);
    assert(recorder_service_deinit() == ESP_FAIL);
    assert(s_recorder.command_queue == NULL && s_initialized);
    assert(fake_audio_active_lease().generation == retained.generation);
    assert(recorder_service_deinit() == ESP_FAIL);
    s_fake_audio_stop_error = ESP_OK;
    assert(recorder_service_deinit() == ESP_OK);
    WAIT_UNTIL(host_dynamic_task_count() == s_base_tasks);
    assert(fake_audio_active_lease().token == 0U);
    assert(recorder_service_init(config) == ESP_OK);
    (void)_record_stop();
}


int main(void)
{
    assert(mkdtemp(s_root) != NULL);
    fake_sd_set_mount_path(s_root);
    s_base_tasks = host_dynamic_task_count();
    s_base_caps = host_caps_task_count();
    s_base_wrong = host_caps_task_wrong_delete_count();
    s_base_self = host_caps_task_self_delete_count();
    const recorder_service_config_t config =
    {
        .directory = "Recordings", .max_duration_seconds = 10U,
        .minimum_free_bytes = 0U,
    };
    assert(recorder_service_init(&config) == ESP_OK);
    recorder_service_target_t target = _record_stop();
    const unsigned descriptors = _open_files();
    _play_eof(&target, false);
    _is_busy_covers_active_session(&target);
    _creation_failures(&target);
    target = _record_stop();
    _io_error(true, &target);
    target = _record_stop();
    _io_error(false, NULL);
    target = _record_stop();
    _failed_take_leaves_no_part();
    target = _record_stop();
    _pause_discards_capture();
    target = _record_stop();
    _capture_gain_and_duplication();
    target = _record_stop();
    _warmup_discards_leading_capture();
    target = _record_stop();
    _capture_filters();
    _capture_noise_gate();
    _partial_write_keeps_playing(&target);
    target = _record_stop();
    _maximum_duration();
    target = _record_stop();
    _play_eof(&target, true);
    _quick_restart(&target, true);
    _quick_restart(&target, false);
    /* A completed-before-create-return handle must be reaped before next start. */
    for (unsigned i = 0U; i < 20U; ++i)
    {
        target = _record_stop();
        _play_eof(&target, true);
        assert(_open_files() == descriptors);
    }
    _release_failure_recovery(&config);
    const size_t owners = host_caps_task_owner_delete_count();
    _start_record();
    _stop_blocked(true);
    WAIT_UNTIL(host_dynamic_task_count() == s_base_tasks);
    assert(host_caps_task_count() == s_base_caps);
    assert(host_caps_task_owner_delete_count() == owners + 1U);
    assert(host_caps_task_wrong_delete_count() == s_base_wrong);
    assert(host_caps_task_self_delete_count() == s_base_self);
    assert(fake_sd_leases(SD_STORAGE_SERVICE_USAGE_SESSION) == 0U);
    assert(_open_files() == descriptors);
    char command[256];
    snprintf(command, sizeof(command), "rm -rf '%s'", s_root);
    assert(system(command) == 0);
    puts("device stream WAV, EOF, error, failure and owner lifecycle regressions passed");
    return 0;
}
