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

/** @brief Return defaults selected by Kconfig. */
audio_service_config_t audio_service_get_default_config(void);

/**
 * @brief Initialize the service around the board audio device.
 *
 * This function does not claim or release the BSP device. The board must have
 * been initialized before this call and remains the owner of its resources.
 */
esp_err_t audio_service_init(void);

/**
 * @brief Stop streaming and reset service-owned state.
 *
 * The caller must serialize this function with other lifecycle control APIs.
 * In-flight read/write calls are allowed to finish before resources are
 * reset. Synchronization objects are retained for a later init so rejected
 * I/O calls can safely race this transition.
 */
esp_err_t audio_service_deinit(void);

/** @brief Return true when the board audio path is available to this service. */
bool audio_service_is_available(void);

/** @brief Return the current service lifecycle state. */
audio_service_state_t audio_service_get_state(void);

/**
 * @brief Configure PCM format while stopped and in the READY state.
 *
 * An ERROR state must first be recovered through stop, resume, or deinit. The
 * caller must serialize this function with other lifecycle control APIs.
 */
esp_err_t audio_service_configure(const audio_service_config_t *config);

/**
 * @brief Start full-duplex DMA.
 *
 * The caller must serialize this function with other lifecycle control APIs.
 */
esp_err_t audio_service_start(void);

/**
 * @brief Stop full-duplex DMA after all in-flight I/O finishes.
 *
 * The caller must serialize this function with other lifecycle control APIs.
 */
esp_err_t audio_service_stop(void);

/**
 * @brief Quiesce streaming for system sleep within a bounded wait.
 *
 * New read/write calls are rejected before existing calls are drained. A
 * READY service is accepted without starting it on resume. An ERROR service
 * still retries the BSP stop operation. The caller must serialize this
 * function with other lifecycle control APIs.
 *
 * @param timeout_ms maximum drain wait, or AUDIO_SERVICE_WAIT_FOREVER.
 * @param resume_required receives true only when the service was RUNNING.
 * @return ESP_OK when quiesced, ESP_ERR_TIMEOUT when I/O did not drain, or a
 *         BSP stop error. Failed transitions may be retried or resumed.
 */
esp_err_t audio_service_suspend(uint32_t timeout_ms, bool *resume_required);

/**
 * @brief Restore streaming after a suspend that requires resume.
 *
 * This operation also recovers a failed RUNNING suspend from ERROR when the
 * BSP can start successfully. The caller must serialize this function with
 * other lifecycle control APIs.
 *
 * @param timeout_ms reserved as the lifecycle deadline; BSP start is
 *                   synchronous and currently has no independent wait.
 * @return ESP_OK when streaming and I/O admission are restored.
 */
esp_err_t audio_service_resume(uint32_t timeout_ms);

/** @brief Write interleaved PCM bytes to the speaker. */
esp_err_t audio_service_write(void *data, size_t bytes, size_t *written,
                              uint32_t timeout_ms);

/** @brief Read interleaved PCM bytes from the microphone. */
esp_err_t audio_service_read(void *data, size_t bytes, size_t *read,
                             uint32_t timeout_ms);

/** @brief Set the speaker volume in the range 0..100 percent. */
esp_err_t audio_service_set_volume(uint8_t percent);

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
