#ifndef __SYSTEM_PM_H__
#define __SYSTEM_PM_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of EXT1 GPIO wake sources. */
#define SYSTEM_PM_MAX_WAKE_SOURCES 4U

/** @brief Sentinel used when no wake source can be identified. */
#define SYSTEM_PM_WAKE_SOURCE_NONE (-1)

/**
 * @brief Active level shared by all configured EXT1 wake sources.
 */
typedef enum
{
    SYSTEM_PM_WAKE_LEVEL_LOW = 0, /**< Wake when any configured GPIO is low. */
    SYSTEM_PM_WAKE_LEVEL_HIGH,    /**< Wake when any configured GPIO is high. */
} system_pm_wake_level_t;

/**
 * @brief One RTC GPIO that may wake the system from light sleep.
 */
typedef struct system_pm_wake_source
{
    int gpio_num;                         /**< RTC-capable GPIO number. */
    system_pm_wake_level_t active_level;  /**< Required active level. */
} system_pm_wake_source_t;

/**
 * @brief Classified reason for the most recent standby completion.
 */
typedef enum
{
    SYSTEM_PM_WAKE_REASON_UNKNOWN = 0, /**< No wake cause was reported. */
    SYSTEM_PM_WAKE_REASON_GPIO,        /**< EXT1 GPIO caused the wake. */
    SYSTEM_PM_WAKE_REASON_TIMER,       /**< Timer caused the wake. */
    SYSTEM_PM_WAKE_REASON_OTHER,       /**< Another source caused the wake. */
    SYSTEM_PM_WAKE_REASON_SLEEP_ERROR, /**< Standby transaction failed. */
} system_pm_wake_reason_t;

/**
 * @brief Result and wake-source details for one standby transaction.
 */
typedef struct system_pm_wake_event
{
    system_pm_wake_reason_t reason; /**< Classified completion reason. */
    int source_index;               /**< Matching config index or NONE. */
    int gpio_num;                   /**< Matching GPIO or NONE. */
    uint32_t wakeup_causes;         /**< Raw ESP-IDF wake-cause mask. */
    uint64_t gpio_wakeup_mask;      /**< Raw EXT1 GPIO wake mask. */
    esp_err_t prepare_result;       /**< Peripheral quiesce result. */
    esp_err_t sleep_result;         /**< Light-sleep entry result. */
    esp_err_t complete_result;      /**< Peripheral recovery result. */
} system_pm_wake_event_t;

/**
 * @brief Receive a standby completion from the system PM worker.
 *
 * @param event is valid only for the callback invocation.
 * @param context is the configured callback context.
 *
 * @warning The callback must only enqueue or notify and return immediately.
 */
typedef void (*system_pm_wake_callback_t)(const system_pm_wake_event_t *event,
        void *context);

/**
 * @brief Quiesce or restore non-retained peripherals around light sleep.
 *
 * @param timeout_ms is the operation timeout in milliseconds.
 * @param context is the configured sleep-hook context.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 *
 * @warning Hooks run in the system PM worker and may block.
 */
typedef esp_err_t (*system_pm_sleep_hook_t)(uint32_t timeout_ms, void *context);

/**
 * @brief Validate a standby generation immediately before sleep commit.
 *
 * @param generation identifies the standby request being committed.
 * @param context is the configured commit-hook context.
 *
 * @return true to admit sleep; false to cancel before commit.
 */
typedef bool (*system_pm_commit_guard_t)(uint32_t generation, void *context);

/**
 * @brief Observe a committed generation immediately before light sleep.
 *
 * @param generation identifies the committed standby request.
 * @param context is the configured commit-hook context.
 */
typedef void (*system_pm_commit_callback_t)(uint32_t generation, void *context);

/**
 * @brief Complete system light-sleep configuration.
 *
 * @note All wake sources must be unique RTC GPIOs with the same active level.
 */
typedef struct system_pm_config
{
    system_pm_wake_source_t wake_sources[SYSTEM_PM_MAX_WAKE_SOURCES]; /**< GPIOs. */
    size_t wake_source_count;                    /**< Number of valid GPIOs. */
    system_pm_sleep_hook_t prepare_sleep;        /**< Required quiesce hook. */
    system_pm_sleep_hook_t complete_sleep;       /**< Required restore hook. */
    void *sleep_hook_context;                    /**< Shared hook context. */
    uint32_t prepare_timeout_ms;                 /**< Hook and cancel timeout. */
    system_pm_wake_callback_t wake_callback;     /**< Optional wake callback. */
    void *wake_callback_context;                 /**< Wake callback context. */
    system_pm_commit_guard_t commit_guard;       /**< Optional commit guard. */
    system_pm_commit_callback_t commit_callback; /**< Optional commit notice. */
    void *commit_context;                        /**< Commit-hook context. */
    uint32_t task_priority;                      /**< Standby worker priority. */
} system_pm_config_t;

/**
 * @brief Copy configuration and create the sleep worker and CPU-frequency lock.
 *
 * @param config is the complete light-sleep configuration.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 *
 * @warning Call from task context. Lifecycle calls must be serialized.
 */
esp_err_t system_pm_init(const system_pm_config_t *config);

/**
 * @brief Join the worker and release owned PM and RTOS resources.
 *
 * @note Failed PM-lock cleanup is retained for a later retry. An admitted wake
 *       callback is joined with the worker; callbacks not admitted are skipped.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_STATE during committed sleep or
 *         an incompatible lifecycle state; otherwise an ESP-IDF error.
 *
 * @warning This is a blocking task-context operation. The caller must prevent
 *          concurrent public API calls before deinitialization.
 */
esp_err_t system_pm_deinit(void);

/**
 * @brief Queue one asynchronous standby transaction.
 *
 * @return ESP_OK when queued; ESP_ERR_INVALID_STATE when unavailable;
 *         ESP_FAIL when command serialization fails.
 *
 * @warning Call from task context, not an ISR.
 */
esp_err_t system_pm_request_standby(void);

/**
 * @brief Cancel queued preparation or retry pending peripheral recovery.
 *
 * @return The complete_sleep result; ESP_ERR_INVALID_STATE after sleep commit;
 *         ESP_FAIL when command serialization fails.
 *
 * @warning This task-context operation may block for preparation rollback.
 */
esp_err_t system_pm_cancel_standby(void);

/**
 * @brief Acquire a reference to the maximum CPU-frequency PM lock.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t system_pm_acquire_cpu_max_freq(void);

/**
 * @brief Release one reference to the maximum CPU-frequency PM lock.
 *
 * @return ESP_OK on success, otherwise an ESP-IDF error.
 */
esp_err_t system_pm_release_cpu_max_freq(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __SYSTEM_PM_H__ */
