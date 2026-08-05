#ifndef __WEATHER_HOST_ESP_HEAP_CAPS_H__
#define __WEATHER_HOST_ESP_HEAP_CAPS_H__

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT   (UINT32_C(1) << 2)
#define MALLOC_CAP_SPIRAM (UINT32_C(1) << 10)

void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities);
void *heap_caps_malloc(size_t size, unsigned capabilities);
void heap_caps_free(void *memory);

#endif /* __WEATHER_HOST_ESP_HEAP_CAPS_H__ */
