#ifndef __TIME_SERVICE_HOST_RUNTIME_H__
#define __TIME_SERVICE_HOST_RUNTIME_H__

#include <stdbool.h>
#include <stdint.h>

/** @brief Return the number of task notifications issued by the fake. */
uint32_t host_freertos_notification_count(void);
/** @brief Block or release the time-service network update test hook. */
void host_freertos_block_network_update(bool blocked);
/** @brief Wait until a caller reaches the blocked network update test hook. */
bool host_freertos_wait_network_update(uint32_t timeout_ms);
/** @brief Wait until every fake task has returned. */
bool host_freertos_wait_for_tasks(uint32_t timeout_ms);

#endif /* __TIME_SERVICE_HOST_RUNTIME_H__ */
