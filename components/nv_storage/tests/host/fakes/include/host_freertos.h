#ifndef __NV_STORAGE_HOST_FREERTOS_CONTROL_H__
#define __NV_STORAGE_HOST_FREERTOS_CONTROL_H__

#include <stdbool.h>
#include <stdint.h>

/** @brief Block the next semaphore take before it acquires the semaphore. */
void host_freertos_block_next_semaphore_take(void);
/**
 * @brief Wait until the pre-acquire semaphore gate is occupied.
 * @param timeout_ms is the maximum wait in milliseconds.
 * @return true when occupied; false on timeout.
 */
bool host_freertos_wait_for_blocked_semaphore_take(uint32_t timeout_ms);
/** @brief Release the pre-acquire semaphore gate. */
void host_freertos_release_blocked_semaphore_take(void);

/** @brief Block the next successful semaphore take before it returns. */
void host_freertos_block_after_next_semaphore_take(void);
/**
 * @brief Wait until the post-acquire semaphore gate is occupied.
 * @param timeout_ms is the maximum wait in milliseconds.
 * @return true when occupied; false on timeout.
 */
bool host_freertos_wait_for_blocked_after_semaphore_take(uint32_t timeout_ms);
/** @brief Release the post-acquire semaphore gate. */
void host_freertos_release_blocked_after_semaphore_take(void);
/**
 * @brief Wait until another thread is waiting to acquire a semaphore.
 * @param timeout_ms is the maximum wait in milliseconds.
 * @return true when a waiter is present; false on timeout.
 */
bool host_freertos_wait_for_pending_semaphore_take(uint32_t timeout_ms);

#endif /* __NV_STORAGE_HOST_FREERTOS_CONTROL_H__ */
