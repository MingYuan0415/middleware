#ifndef __HOST_BLE_RUNTIME_ESP_RANDOM_H__
#define __HOST_BLE_RUNTIME_ESP_RANDOM_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deterministic xorshift32 so tests can predict random draws. */
uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);
void esp_random_fake_reset(uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_BLE_RUNTIME_ESP_RANDOM_H__ */
