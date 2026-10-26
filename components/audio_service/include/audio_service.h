#ifndef __AUDIO_SERVICE_H__
#define __AUDIO_SERVICE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief PCM format configured for full-duplex playback and capture. */
typedef struct audio_service_config
{
    uint32_t sample_rate_hz; /**< Sample rate in hertz. */
    uint8_t bits_per_sample; /**< PCM word width. */
    uint8_t channels;        /**< Interleaved channel count. */
    uint16_t mclk_multiple;  /**< MCLK to LRCK ratio. */
} audio_service_config_t;

/** @brief Complete startup policy supplied by the product integration. */
typedef struct audio_service_init_config
{
    audio_service_config_t stream; /**< Initial full-duplex PCM format. */
    uint8_t volume_percent;        /**< Initial speaker volume, 0 through 100. */
    bool muted;                    /**< Initial speaker mute state. */
    bool pa_enabled;               /**< Requested amplifier state while streaming. */
} audio_service_init_config_t;

/** @brief Audio service lifecycle state. */
typedef enum
{
    AUDIO_SERVICE_STATE_UNINITIALIZED = 0,
    AUDIO_SERVICE_STATE_READY,
    AUDIO_SERVICE_STATE_RUNNING,
    AUDIO_SERVICE_STATE_SUSPENDING,
    AUDIO_SERVICE_STATE_ERROR,
} audio_service_state_t;

/** @brief Sentinel accepted by suspend/resume to wait without a deadline. */
#define AUDIO_SERVICE_WAIT_FOREVER UINT32_MAX

/**
 * @brief Initialize the service around the board audio device.
 *
 * This function does not claim or release the BSP device. The board must have
 * been initialized before this call and remains the owner of its resources.
 * The startup owner must serialize the first initialization, including creation
 * of synchronization objects. After initialization, internal reservations
 * isolate lifecycle transitions; conflicting controls return INVALID_STATE.
 *
 * @param config is validated and copied by the service.
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t audio_service_init(const audio_service_init_config_t *config);

/**
 * @brief Copy the active PCM stream configuration.
 *
 * @param config receives the active configuration.
 * @return ESP_OK when initialized; ESP_ERR_INVALID_ARG for NULL;
 *         ESP_ERR_INVALID_STATE before initialization.
 */
esp_err_t audio_service_get_config(audio_service_config_t *config);

/**
 * @brief Quiesce the stream and reset service-owned state.
 *
 * An active lease is rejected with ESP_ERR_INVALID_STATE; the owner must
 * release it first. Stop and reset share one internal lifecycle reservation.
 * Conflicting controls return ESP_ERR_INVALID_STATE. In-flight I/O drains
 * before reset. Synchronization objects and the lease generation are retained
 * across deinit/init; partial initialization cleanup can be retried here.
 *
 * @return ESP_OK when reset (also before init), otherwise a cleanup error.
 */
esp_err_t audio_service_deinit(void);

/** @brief Return true when the board audio path is available to this service. */
bool audio_service_is_available(void);

/** @brief Return the current service lifecycle state. */
audio_service_state_t audio_service_get_state(void);

/**
 * @brief Configure PCM format while READY and without a lease.
 *
 * READY may also represent a suspended owner; that case is rejected.
 * ERROR and a conflicting lifecycle reservation return INVALID_STATE.
 *
 * @param config is validated and copied on success.
 * @return ESP_OK, an argument/state error, or the BSP configuration error.
 */
esp_err_t audio_service_configure(const audio_service_config_t *config);

/** @brief Intended use of one exclusive audio lease. */
typedef enum
{
    AUDIO_SERVICE_SESSION_RECORD = 0,  /**< Read and write, e.g. WAV capture. */
    AUDIO_SERVICE_SESSION_PLAYBACK,    /**< Write only. */
    AUDIO_SERVICE_SESSION_MAINTENANCE, /**< Read and write, benchmark path. */
} audio_service_session_kind_t;

/** @brief Exclusive stream lease returned by audio_service_acquire(). */
typedef struct audio_service_lease
{
    uint32_t token;      /**< Nonzero owner token. */
    uint32_t generation; /**< Invalidates handles after release/reacquire. */
} audio_service_lease_t;

/**
 * @brief Claim the stream when idle and start full-duplex DMA.
 *
 * A lease is exclusive; busy or reserved transitions return INVALID_STATE
 * without preempting the owner. An initialized, ownerless READY or ERROR
 * service may acquire. ERROR first retries stop. A failed start is compensated
 * by stop: successful cleanup returns to READY, failed cleanup retains ERROR.
 * No owner or generation is allocated on failure.
 *
 * @param kind selects the read/write permissions of the lease.
 * @param lease receives the owner handle on success and zero on failure.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for invalid kind/NULL,
 *         ESP_ERR_INVALID_STATE when uninitialized, busy, or reserved, or
 *         the BSP cleanup/start error (the original start error is preserved).
 */
esp_err_t audio_service_acquire(audio_service_session_kind_t kind,
                                audio_service_lease_t *lease);

/**
 * @brief Release one lease and stop DMA after draining in-flight I/O.
 *
 * Only the current owner can release. A stale handle returns
 * ESP_ERR_INVALID_STATE without touching the stream. A failed stop keeps the
 * lease in ERROR for retry; a reservation conflict also retains ownership.
 * Releasing a READY suspended owner clears its lease without another stop.
 *
 * @param lease is the handle returned by audio_service_acquire().
 * @return ESP_OK, ESP_ERR_INVALID_STATE for a stale handle, or the BSP error.
 */
esp_err_t audio_service_release(audio_service_lease_t lease);

/**
 * @brief Quiesce streaming for system sleep within a bounded wait.
 *
 * New read/write calls are rejected before existing calls are drained. The
 * lease is retained; READY can mean idle or a suspended owner. READY suspend
 * is a no-op with resume_required=false. ERROR retries BSP stop. System PM
 * integration restores streaming with resume; it does not acquire a new lease.
 * Drain timeout restores the previous state/admission and retains ownership;
 * stop failure closes admission and retains ERROR and any owner for retry.
 *
 * @param timeout_ms maximum drain wait, or AUDIO_SERVICE_WAIT_FOREVER.
 * @param resume_required receives true only when the service was RUNNING.
 * @return ESP_OK when quiesced, ESP_ERR_TIMEOUT when I/O did not drain,
 *         ESP_ERR_INVALID_STATE for a conflicting reservation, or a BSP error.
 */
esp_err_t audio_service_suspend(uint32_t timeout_ms, bool *resume_required);

/**
 * @brief Restore streaming after a suspend that retained a lease.
 *
 * READY without an owner succeeds without starting DMA. RUNNING with an owner
 * is already restored. READY or ERROR with an owner retries start using the
 * same lease. Uninitialized, reserved, or ownerless ERROR states are rejected.
 *
 * @param timeout_ms reserved as the lifecycle deadline; BSP start is
 *                   synchronous and currently has no independent wait.
 * @return ESP_OK when restored or idle READY; ESP_ERR_INVALID_STATE for an
 *         invalid state/reservation, or the BSP start error.
 */
esp_err_t audio_service_resume(uint32_t timeout_ms);

/**
 * @brief Write interleaved PCM bytes to the speaker under one lease.
 *
 * Lease identity, RUNNING state, admission, and the in-flight reference are
 * checked/committed under one lock. Rejected calls set non-NULL written to
 * zero. BSP calls may report partial progress even on timeout or another error;
 * callers must account for that progress before retrying.
 *
 * @return ESP_ERR_INVALID_STATE when the handle is stale; otherwise the BSP
 *         result.
 */
esp_err_t audio_service_write_leased(audio_service_lease_t lease,
                                     void *data, size_t bytes,
                                     size_t *written, uint32_t timeout_ms);

/**
 * @brief Read interleaved PCM bytes from the microphone under one lease.
 *
 * Reading requires a RECORD or MAINTENANCE lease.
 * Admission and the in-flight reference use the same atomic lease check as
 * write. Rejected calls set non-NULL read to zero; an admitted BSP call may
 * report partial progress even when its result is not ESP_OK.
 *
 * @return ESP_ERR_INVALID_STATE when the handle is stale or a PLAYBACK lease
 *         attempts a read; otherwise the BSP result.
 */
esp_err_t audio_service_read_leased(audio_service_lease_t lease,
                                    void *data, size_t bytes,
                                    size_t *read, uint32_t timeout_ms);

/** @brief Report whether one exclusive lease is currently held. */
bool audio_service_is_leased(void);

/**
 * @brief Copy the active lease for diagnostics.
 * @return ESP_OK, or ESP_ERR_NOT_FOUND when no lease is held.
 */
esp_err_t audio_service_get_lease(audio_service_lease_t *lease);

/**
 * @brief Set the speaker volume in the range 0..100 percent.
 *
 * The level is applied only; nothing is written to storage. Use this for
 * transient changes such as a drag preview or a benchmark override.
 */
esp_err_t audio_service_set_volume(uint8_t percent);

/**
 * @brief Apply and persist the speaker volume.
 *
 * Intended for a settled user choice: call it once when the knob is released,
 * never while it is moving, because each call commits to flash. A failed store
 * is reported without reverting the level, which is already audible.
 *
 * @return ESP_OK when applied and stored; otherwise the audio or storage error.
 */
esp_err_t audio_service_set_volume_persisted(uint8_t percent);

/**
 * @brief Restore the persisted speaker volume over the configured default.
 *
 * Call after audio_service_init() succeeded. A key that was never written keeps
 * the init_config default and reports ESP_OK; a corrupt record is refused so
 * the default stands.
 *
 * @return ESP_OK when restored or absent; ESP_ERR_INVALID_STATE when the stored
 *         level is out of range and the default stands; otherwise the storage
 *         or audio error.
 */
esp_err_t audio_service_restore_volume(void);

/**
 * @brief Delete the persisted speaker volume during a factory reset.
 *
 * @return ESP_OK when erased or already absent; otherwise the storage error.
 */
esp_err_t audio_service_reset_persisted_state(void);

/** @brief Copy the current speaker volume. */
esp_err_t audio_service_get_volume(uint8_t *percent);

/** @brief Mute or unmute the speaker. */
esp_err_t audio_service_set_mute(bool muted);

/** @brief Copy the speaker mute state. */
esp_err_t audio_service_get_mute(bool *muted);

/** @brief Enable or disable the NS4150B power amplifier. */
esp_err_t audio_service_set_pa(bool enabled);

/** @brief Copy the requested power-amplifier state. */
esp_err_t audio_service_get_pa(bool *enabled);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_SERVICE_H__ */
