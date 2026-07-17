#ifndef __NV_STORAGE_HOST_SEMPHR_H__
#define __NV_STORAGE_HOST_SEMPHR_H__

#include "freertos/FreeRTOS.h"

/** @brief Create one host-backed static mutex. */
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
/** @brief Acquire one host-backed mutex. */
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t timeout_ticks);
/** @brief Release one host-backed mutex. */
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
/** @brief Delete one host-backed mutex. */
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

#endif /* __NV_STORAGE_HOST_SEMPHR_H__ */
