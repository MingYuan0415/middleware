#ifndef __NV_STORAGE_HOST_TASK_H__
#define __NV_STORAGE_HOST_TASK_H__

#include "freertos/FreeRTOS.h"

/** @brief Delay the current host thread for the requested fake ticks. */
void vTaskDelay(TickType_t ticks);

#endif /* __NV_STORAGE_HOST_TASK_H__ */
