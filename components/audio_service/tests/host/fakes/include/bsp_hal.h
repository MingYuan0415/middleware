#ifndef __BSP_HAL_H__
#define __BSP_HAL_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct bsp_audio_config
{
    uint32_t sample_rate_hz;
    uint8_t bits_per_sample;
    uint8_t channels;
    uint16_t mclk_multiple;
} bsp_audio_config_t;

typedef struct bsp_audio_ops
{
    bool (*is_available)(void);
    esp_err_t (*configure)(const bsp_audio_config_t *config);
    esp_err_t (*start)(void);
    esp_err_t (*stop)(void);
    esp_err_t (*write)(void *data, size_t size, size_t *written,
                       uint32_t timeout_ms);
    esp_err_t (*read)(void *data, size_t size, size_t *read,
                      uint32_t timeout_ms);
    esp_err_t (*set_volume)(uint8_t volume);
    uint8_t (*get_volume)(void);
    esp_err_t (*set_mute)(bool muted);
    bool (*get_mute)(void);
    esp_err_t (*set_pa)(bool enabled);
} bsp_audio_ops_t;

const bsp_audio_ops_t *bsp_hal_get_audio(void);

#endif /* __BSP_HAL_H__ */
