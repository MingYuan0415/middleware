#ifndef __IMU_SERVICE_HOST_TASK_H__
#define __IMU_SERVICE_HOST_TASK_H__

#include "freertos/FreeRTOS.h"

typedef enum
{
    eNoAction = 0,
    eSetBits,
    eIncrement,
    eSetValueWithOverwrite,
    eSetValueWithoutOverwrite,
} eNotifyAction;

/** @brief Create one pthread-backed fake task. */
BaseType_t xTaskCreate(void (*entry)(void *), const char *name,
                       uint32_t stack_depth, void *context,
                       UBaseType_t priority, TaskHandle_t *out_task);
/** @brief Add notification bits to one fake task. */
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value,
                       eNotifyAction action);
/** @brief Wait for notification bits on the current fake task. */
BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                           uint32_t *value, TickType_t timeout_ticks);
/** @brief Return the current worker or external caller task handle. */
TaskHandle_t xTaskGetCurrentTaskHandle(void);
/** @brief Delay the current host thread by fake ticks. */
void vTaskDelay(TickType_t ticks);
/** @brief Model task self-deletion; the trampoline releases storage. */
void vTaskDelete(TaskHandle_t task);

#endif /* __IMU_SERVICE_HOST_TASK_H__ */
