/** @file Minimal FreeRTOS compatibility for IMU service host tests. */
#ifndef __IMU_SERVICE_HOST_FREERTOS_H__
#define __IMU_SERVICE_HOST_FREERTOS_H__

#include <pthread.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef struct host_task *TaskHandle_t;
typedef struct host_semaphore *SemaphoreHandle_t;
typedef struct host_event_group *EventGroupHandle_t;
typedef pthread_mutex_t portMUX_TYPE;

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
#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))

#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(mux) ((void)pthread_mutex_lock((mux)))
#define taskEXIT_CRITICAL(mux)  ((void)pthread_mutex_unlock((mux)))

#endif /* __IMU_SERVICE_HOST_FREERTOS_H__ */
