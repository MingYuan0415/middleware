#ifndef __AUDIO_SERVICE_FAKES_H__
#define __AUDIO_SERVICE_FAKES_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp_hal.h"

typedef struct audio_service_fake_state
{
    bool expose_ops;
    bool available;
    bool started;
    bool start_enables_before_failure;
    bool muted;
    bool pa_enabled;
    bool stop_during_io;
    uint8_t volume;
    uint32_t configure_count;
    uint32_t start_count;
    uint32_t stop_count;
    uint32_t last_start_stop_count;
    uint32_t write_count;
    uint32_t read_count;
    uint32_t last_timeout_ms;
    esp_err_t configure_result;
    esp_err_t start_result;
    esp_err_t set_pa_result;
    esp_err_t stop_result;
    int state_during_io;
    bsp_audio_config_t config;
} audio_service_fake_state_t;

void audio_service_fakes_reset(void);
audio_service_fake_state_t *audio_service_fakes_state(void);
void audio_service_fakes_block_io(bool blocked);
bool audio_service_fakes_wait_for_io(uint32_t count, uint32_t timeout_ms);
void audio_service_fakes_block_stop(bool blocked);
bool audio_service_fakes_wait_for_stop(uint32_t timeout_ms);

/** @brief Scalar NVS control for the volume persistence tests. */
void audio_service_fakes_nv_reset(void);
bool audio_service_fakes_nv_present(void);
uint8_t audio_service_fakes_nv_value(void);
unsigned audio_service_fakes_nv_set_count(void);
unsigned audio_service_fakes_nv_erase_count(void);
/** @brief Force the next nv_storage_set_u8 call to fail. */
void audio_service_fakes_nv_fail_set(void);
/** @brief Force the next nv_storage_get_u8 call to fail. */
void audio_service_fakes_nv_fail_get(void);
/** @brief Force the next nv_storage_erase_key call to fail. */
void audio_service_fakes_nv_fail_erase(void);
/** @brief Store an arbitrary value directly, to seed corrupt records. */
void audio_service_fakes_nv_seed(uint8_t value);

#endif /* __AUDIO_SERVICE_FAKES_H__ */
