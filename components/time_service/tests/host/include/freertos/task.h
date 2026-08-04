#ifndef __TIME_SERVICE_HOST_TASK_H__
#define __TIME_SERVICE_HOST_TASK_H__

#include "freertos/FreeRTOS.h"

/** @brief Notification update action supported by the task fake. */
typedef enum
{
    eNoAction = 0,
    eSetBits,
    eIncrement,
    eSetValueWithOverwrite,
    eSetValueWithoutOverwrite,
} eNotifyAction;

/** @brief Create one pthread-backed fake task. */
BaseType_t xTaskCreatePinnedToCore(
    void (*entry)(void *), const char *name, uint32_t stack_depth,
    void *context, UBaseType_t priority, TaskHandle_t *out_task,
    BaseType_t core_id);
/** @brief Notify one fake task. */
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value,
                       eNotifyAction action);
/** @brief Wait for the current fake task notification. */
BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                           uint32_t *value, TickType_t timeout_ticks);
/** @brief Return the current fake task handle. */
TaskHandle_t xTaskGetCurrentTaskHandle(void);
/** @brief Return monotonic fake scheduler ticks. */
TickType_t xTaskGetTickCount(void);
/** @brief Delay the current host thread by fake ticks. */
void vTaskDelay(TickType_t ticks);
/** @brief Model task self-deletion; the trampoline owns storage cleanup. */
void vTaskDelete(TaskHandle_t task);

#endif /* __TIME_SERVICE_HOST_TASK_H__ */
