/** @file Minimal FreeRTOS type compatibility for time-service tests. */
#ifndef __TIME_SERVICE_HOST_FREERTOS_H__
#define __TIME_SERVICE_HOST_FREERTOS_H__

#include <stdint.h>

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
/** @brief Opaque host task handle. */
typedef struct host_task *TaskHandle_t;
/** @brief Opaque host semaphore handle. */
typedef struct host_semaphore *SemaphoreHandle_t;
/** @brief Opaque host event-group handle. */
typedef struct host_event_group *EventGroupHandle_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define configTICK_RATE_HZ 1000U
#define configMAX_PRIORITIES 25U
#define portMAX_DELAY      UINT32_MAX
#define BIT0               (UINT32_C(1) << 0)
#define BIT1               (UINT32_C(1) << 1)
#define BIT2               (UINT32_C(1) << 2)
#define BIT3               (UINT32_C(1) << 3)
#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))

#endif /* __TIME_SERVICE_HOST_FREERTOS_H__ */
