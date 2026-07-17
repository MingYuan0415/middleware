#ifndef __TIME_SERVICE_HOST_RUNTIME_H__
#define __TIME_SERVICE_HOST_RUNTIME_H__

#include <stdbool.h>
#include <stdint.h>

/** @brief Return the number of task notifications issued by the fake. */
uint32_t host_freertos_notification_count(void);
/** @brief Wait until every fake task has returned. */
bool host_freertos_wait_for_tasks(uint32_t timeout_ms);

#endif /* __TIME_SERVICE_HOST_RUNTIME_H__ */
