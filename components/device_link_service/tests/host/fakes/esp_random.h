#ifndef __HOST_ESP_RANDOM_H__
#define __HOST_ESP_RANDOM_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Deterministic host fake for esp_fill_random().
 *
 * The fake feeds a monotonically increasing byte counter, so successive
 * binding windows always produce different discriminator and POP values.
 */
void esp_fill_random(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_ESP_RANDOM_H__ */
