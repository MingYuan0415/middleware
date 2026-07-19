#ifndef __IMU_SERVICE_HOST_RUNTIME_H__
#define __IMU_SERVICE_HOST_RUNTIME_H__

#include <stdbool.h>
#include <stdint.h>

/** @brief Reset fault injection when no fake resources remain active. */
bool host_freertos_reset(void);
/** @brief Fail the selected one-based mutex creation attempt, or zero for none. */
void host_freertos_fail_mutex_create_on(uint32_t attempt);
/** @brief Select whether the next event-group creation fails. */
void host_freertos_fail_event_group_create(bool fail);
/** @brief Select whether the next task creation fails. */
void host_freertos_fail_task_create(bool fail);
/** @brief Hold or release delivery of pending task notifications. */
void host_freertos_block_notifications(bool blocked);
/** @brief Return the number of task notifications issued. */
uint32_t host_freertos_notification_count(void);
/** @brief Return the number of active fake mutexes. */
uint32_t host_freertos_active_mutex_count(void);
/** @brief Return the number of active fake event groups. */
uint32_t host_freertos_active_event_group_count(void);
/** @brief Return the number of active fake tasks. */
uint32_t host_freertos_active_task_count(void);
/** @brief Wait until every fake task has returned. */
bool host_freertos_wait_for_tasks(uint32_t timeout_ms);

#endif /* __IMU_SERVICE_HOST_RUNTIME_H__ */
