/** @file Sleep/PM/GPIO stubs and fault injection for system_pm host tests. */
#ifndef __SYSTEM_PM_HOST_FAKES_H__
#define __SYSTEM_PM_HOST_FAKES_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Reset every stub and fault control to its default. */
void system_pm_fakes_reset(void);

/** @brief Make esp_pm_lock_create() fail once with @p result. */
void system_pm_fakes_fail_lock_create(esp_err_t result);
/** @brief Make esp_pm_lock_acquire() fail once with @p result. */
void system_pm_fakes_fail_lock_acquire(esp_err_t result);
/** @brief Make esp_pm_lock_release() fail once with @p result. */
void system_pm_fakes_fail_lock_release(esp_err_t result);
/** @brief Make esp_pm_lock_delete() fail once with @p result. */
void system_pm_fakes_fail_lock_delete(esp_err_t result);
/** @brief Make esp_sleep_enable_ext1_wakeup_io() fail with @p result. */
void system_pm_fakes_set_enable_ext1_result(esp_err_t result);
/** @brief Make esp_light_sleep_start() return @p result. */
void system_pm_fakes_set_light_sleep_result(esp_err_t result);
/** @brief Select the wake-cause mask and EXT1 status of the next wake. */
void system_pm_fakes_set_wakeup(uint32_t causes, uint64_t ext1_status);

/** @brief Count live PM locks. */
unsigned system_pm_fakes_live_locks(void);
/** @brief Count PM lock acquire calls. */
unsigned system_pm_fakes_acquire_count(void);
/** @brief Count PM lock release calls. */
unsigned system_pm_fakes_release_count(void);
/** @brief Count PM lock delete calls. */
unsigned system_pm_fakes_delete_count(void);
/** @brief Count light-sleep entries. */
unsigned system_pm_fakes_sleep_count(void);
/** @brief Read the mask of the most recent EXT1 enable. */
uint64_t system_pm_fakes_last_ext1_mask(void);
/** @brief Read the mode of the most recent EXT1 enable. */
int system_pm_fakes_last_ext1_mode(void);
/** @brief Count EXT1 enable calls. */
unsigned system_pm_fakes_ext1_enable_count(void);
/** @brief Count EXT1 disable calls. */
unsigned system_pm_fakes_ext1_disable_count(void);
/** @brief Count GPIO wake GPIOs the RTC layer accepts. */
unsigned system_pm_fakes_rtc_gpio_count(void);
/** @brief Report every GPIO the RTC layer accepts. */
void system_pm_fakes_set_rtc_gpios(const int *gpios, size_t count);

#endif /* __SYSTEM_PM_HOST_FAKES_H__ */
