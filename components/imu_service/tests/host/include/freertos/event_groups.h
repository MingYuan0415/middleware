#ifndef __IMU_SERVICE_HOST_EVENT_GROUPS_H__
#define __IMU_SERVICE_HOST_EVENT_GROUPS_H__

#include "freertos/FreeRTOS.h"

/** @brief Create one pthread-backed event group. */
EventGroupHandle_t xEventGroupCreate(void);
/** @brief Set bits in one fake event group. */
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
/** @brief Clear bits in one fake event group. */
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
/** @brief Wait for selected fake event-group bits. */
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group,
                                EventBits_t bits_to_wait_for,
                                BaseType_t clear_on_exit,
                                BaseType_t wait_for_all,
                                TickType_t timeout_ticks);
/** @brief Delete one fake event group. */
void vEventGroupDelete(EventGroupHandle_t group);

#endif /* __IMU_SERVICE_HOST_EVENT_GROUPS_H__ */
