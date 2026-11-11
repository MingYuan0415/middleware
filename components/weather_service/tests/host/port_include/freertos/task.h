#ifndef __WEATHER_PORT_HOST_TASK_H__
#define __WEATHER_PORT_HOST_TASK_H__

#include "freertos/FreeRTOS.h"

#include <pthread.h>
#include <sched.h>

#define taskYIELD() sched_yield()

#define taskENTER_CRITICAL(lock) ((void)pthread_mutex_lock((lock)))
#define taskEXIT_CRITICAL(lock) ((void)pthread_mutex_unlock((lock)))

void vTaskDelay(TickType_t ticks);

#endif /* __WEATHER_PORT_HOST_TASK_H__ */
