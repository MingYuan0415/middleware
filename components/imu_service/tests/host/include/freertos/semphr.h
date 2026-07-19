#ifndef __IMU_SERVICE_HOST_SEMPHR_H__
#define __IMU_SERVICE_HOST_SEMPHR_H__

#include "freertos/FreeRTOS.h"

/** @brief Create one pthread-backed mutex. */
SemaphoreHandle_t xSemaphoreCreateMutex(void);
/** @brief Lock one fake mutex. */
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,
                          TickType_t timeout_ticks);
/** @brief Unlock one fake mutex. */
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
/** @brief Delete one fake mutex. */
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

#endif /* __IMU_SERVICE_HOST_SEMPHR_H__ */
