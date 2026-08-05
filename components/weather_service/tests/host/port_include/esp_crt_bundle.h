#ifndef __WEATHER_HOST_ESP_CRT_BUNDLE_H__
#define __WEATHER_HOST_ESP_CRT_BUNDLE_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_crt_bundle_attach(void *config);

#ifdef __cplusplus
}
#endif

#endif /* __WEATHER_HOST_ESP_CRT_BUNDLE_H__ */
