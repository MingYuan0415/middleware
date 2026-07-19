#ifndef __FREERTOS_H__
#define __FREERTOS_H__

#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define configTICK_RATE_HZ 1000

#endif /* __FREERTOS_H__ */
