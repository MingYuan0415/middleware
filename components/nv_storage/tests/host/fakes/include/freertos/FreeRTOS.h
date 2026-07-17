#ifndef __NV_STORAGE_HOST_FREERTOS_H__
#define __NV_STORAGE_HOST_FREERTOS_H__

#include <pthread.h>
#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;

/** @brief Host storage backing one static FreeRTOS mutex. */
typedef struct static_semaphore
{
    pthread_mutex_t mutex;
    int initialized;
} StaticSemaphore_t;

typedef StaticSemaphore_t *SemaphoreHandle_t;

#define pdTRUE        1
#define pdFALSE       0
#define portMAX_DELAY UINT32_MAX

#endif /* __NV_STORAGE_HOST_FREERTOS_H__ */
