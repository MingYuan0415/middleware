/** @file Task API declarations for the connectivity host fixture. */
#ifndef __CONNECTIVITY_HOST_FREERTOS_TASK_H__
#define __CONNECTIVITY_HOST_FREERTOS_TASK_H__

#include "freertos/FreeRTOS.h"

/** @brief Create a pthread-backed static task. */
TaskHandle_t xTaskCreateStaticPinnedToCore(
    void (*entry)(void *), const char *name, uint32_t stack_depth,
    void *context, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *task_storage, BaseType_t core_id);
/** @brief Return the modeled task core affinity. */
BaseType_t xTaskGetCoreID(TaskHandle_t task);
/** @brief Return the current host task handle. */
TaskHandle_t xTaskGetCurrentTaskHandle(void);
/** @brief Return a modeled minimum-free stack value. */
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
/** @brief Return the host tick count. */
TickType_t xTaskGetTickCount(void);
/** @brief Increment a host task's notification count. */
BaseType_t xTaskNotifyGive(TaskHandle_t task);
/** @brief Wait for and consume the current task's notification count. */
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit,
                          TickType_t timeout_ticks);
/** @brief Delay the current host task. */
void vTaskDelay(TickType_t ticks);
/** @brief Update a host task's modeled priority. */
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority);
/** @brief Suspend the current host task until another thread deletes it. */
void vTaskSuspend(TaskHandle_t task);
/** @brief Return the modeled task state. */
eTaskState eTaskGetState(TaskHandle_t task);
/** @brief Delete a host task. */
void vTaskDelete(TaskHandle_t task);

/** @brief Dynamic-task boundary used by the host stop worker. */
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
                                   uint32_t depth, void *arg, UBaseType_t priority, TaskHandle_t *task, BaseType_t core);

#endif /* __CONNECTIVITY_HOST_FREERTOS_TASK_H__ */
