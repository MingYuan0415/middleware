/** @file Scalar NVS surface used by the audio_service host tests. */
#ifndef __AUDIO_SERVICE_HOST_NV_STORAGE_H__
#define __AUDIO_SERVICE_HOST_NV_STORAGE_H__

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nv_storage_set_u8(const char *key, uint8_t value);
esp_err_t nv_storage_get_u8(const char *key, uint8_t *output);
esp_err_t nv_storage_erase_key(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_SERVICE_HOST_NV_STORAGE_H__ */
