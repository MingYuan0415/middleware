/**
 * @brief Generic bounded background chore worker.
 *
 * The chore service owns one background task that executes short, bounded
 * jobs submitted by any task. It is intended for work that is inconvenient
 * to run on the GUI worker or in the submitting context: periodic
 * housekeeping, snapshot formatting, small filesystem or storage queries.
 * Jobs must not call LVGL, must not block on long I/O, and should poll the
 * cancellation token and return promptly.
 */

#ifndef __CHORE_SERVICE_H__
#define __CHORE_SERVICE_H__

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHORE_SERVICE_WAIT_FOREVER UINT32_MAX

/** @brief Product-owned chore worker policy. */
typedef struct chore_service_config
{
    uint32_t task_priority;       /**< Worker priority, 0 < p < configMAX_PRIORITIES. */
    uint32_t warning_duration_ms; /**< Warn when one job run reaches or exceeds this. */
} chore_service_config_t;

/** @brief Cooperative cancellation handle passed to a running job. */
typedef struct chore_service_cancel_token
{
    const atomic_bool *requested; /**< Polled flag, valid only during run(). */
} chore_service_cancel_token_t;

/** @brief One queued or periodic background job. */
typedef struct chore_service_job
{
    /**
     * @brief Execute one job pass.
     *
     * Runs on the worker task. Must be bounded and must poll the cancel
     * token, returning promptly when cancellation is requested.
     */
    void (*run)(const chore_service_cancel_token_t *cancel, void *arg);
    /**
     * @brief Release the job argument exactly once.
     *
     * Called on the worker task after completion, cancellation, or service
     * shutdown. May be NULL when no release is needed. The submitter must
     * not free the argument itself.
     */
    void (*release)(void *arg);
    void *arg;         /**< Job argument, owned by the service once submitted. */
    uint32_t delay_ms; /**< Delay before the first run; zero runs as soon as possible. */
    uint32_t period_ms; /**< Repeat period; zero means one-shot. */
} chore_service_job_t;

/** @brief Submit result identifying a pending job. */
typedef struct chore_service_handle
{
    uint32_t slot;       /**< Internal pool slot index. */
    uint32_t generation; /**< Pool generation, invalidates stale handles. */
} chore_service_handle_t;

/** @brief Small status copy safe for synchronous callers. */
typedef struct chore_service_status
{
    bool initialized;      /**< Service is initialized. */
    bool suspended;        /**< Worker is paused or a pause is pending. */
    bool stopping;         /**< Shutdown is in progress. */
    uint8_t queued_count;  /**< Pending or running job count. */
    uint8_t running_count; /**< Jobs currently executing. */
    uint32_t stack_high_water; /**< Worker minimum free stack bytes. */
    uint64_t completed_count;  /**< Jobs completed since init. */
    uint64_t cancelled_count;  /**< Jobs cancelled since init. */
} chore_service_status_t;

/**
 * @brief Initialize the singleton chore worker.
 *
 * Repeated initialization with the same configuration is idempotent; a
 * different active configuration returns ESP_ERR_INVALID_STATE. Init and
 * deinit must be serialized by the caller; once initialized, the job APIs
 * (submit/cancel/suspend/resume/get_status) may be called from any task,
 * including concurrently with deinit, which drains in-flight calls before
 * releasing its resources.
 *
 * @param config worker priority and duration-warning policy.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG on invalid policy;
 *         ESP_ERR_INVALID_STATE when a different configuration is active.
 */
esp_err_t chore_service_init(const chore_service_config_t *config);

/**
 * @brief Stop the worker, cancel all jobs, and release every argument.
 *
 * Cancelled job arguments are released before this call returns successfully.
 * A timed-out deinit leaves the shutdown in progress; the release callbacks
 * still run when the worker finishes, and the call may be retried.
 *
 * @param timeout_ms quiescence deadline, or CHORE_SERVICE_WAIT_FOREVER.
 *
 * @return ESP_OK on success; ESP_ERR_TIMEOUT when the worker is still busy.
 */
esp_err_t chore_service_deinit(uint32_t timeout_ms);

/**
 * @brief Submit one job to the worker.
 *
 * The job argument ownership transfers to the service only when this call
 * succeeds; release() then runs exactly once.
 *
 * @param job job description; run must be non-NULL.
 * @param handle receives the pending-job handle when ESP_OK.
 *
 * @return ESP_OK; ESP_ERR_NO_MEM when the fixed pool is full;
 *         ESP_ERR_INVALID_STATE when the service is not running.
 */
esp_err_t chore_service_submit(const chore_service_job_t *job,
                               chore_service_handle_t *handle);

/**
 * @brief Request cooperative cancellation and wait for quiescence.
 *
 * A running job observes the request through its cancel token and returns;
 * a queued job is never started. The release callback runs before the
 * wait succeeds. Calling cancel on a handle that is no longer pending
 * returns ESP_OK; cancelling a still-pending job while the service is
 * shutting down returns ESP_ERR_INVALID_STATE. Handles stay valid for the
 * slot generation; after 2^32 slot releases the generation wraps and a
 * very old handle could alias a new job in the same slot.
 *
 * @param handle handle returned by chore_service_submit().
 * @param timeout_ms quiescence deadline, or CHORE_SERVICE_WAIT_FOREVER.
 *
 * @return ESP_OK when the job is quiescent; ESP_ERR_TIMEOUT when the job
 *         is still running past the deadline; the request remains pending;
 *         ESP_ERR_INVALID_STATE during shutdown.
 */
esp_err_t chore_service_cancel(chore_service_handle_t *handle,
                               uint32_t timeout_ms);

/**
 * @brief Quiesce the worker before light sleep.
 *
 * The currently running job completes; queued jobs stay queued and resume
 * later without a catch-up burst. Returns ESP_OK when the service is not
 * initialized, and ESP_ERR_INVALID_STATE when the worker shuts down while
 * the pause is pending (for example during a concurrent deinit).
 *
 * @param timeout_ms pause deadline, or CHORE_SERVICE_WAIT_FOREVER.
 *
 * @return ESP_OK when paused; ESP_ERR_TIMEOUT when the pause was not
 *         acknowledged in time (the pause request is rolled back);
 *         ESP_ERR_INVALID_STATE during shutdown.
 */
esp_err_t chore_service_suspend(uint32_t timeout_ms);

/**
 * @brief Resume job execution after light sleep.
 *
 * Returns ESP_OK when the service is not initialized and
 * ESP_ERR_INVALID_STATE when the worker shuts down while the resume is
 * pending.
 *
 * @param timeout_ms resume deadline, or CHORE_SERVICE_WAIT_FOREVER.
 *
 * @return ESP_OK when running again; ESP_ERR_TIMEOUT when the resume was
 *         not acknowledged in time; ESP_ERR_INVALID_STATE during shutdown.
 */
esp_err_t chore_service_resume(uint32_t timeout_ms);

/**
 * @brief Copy the current small service status.
 *
 * A stopping snapshot is still returned while shutdown is in progress.
 *
 * @param status receives the current status when ESP_OK.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG when status is NULL;
 *         ESP_ERR_INVALID_STATE when the service is not initialized.
 */
esp_err_t chore_service_get_status(chore_service_status_t *status);

/**
 * @brief Report whether the chore service is initialized and admits calls.
 *
 * Returns false while shutdown is in progress.
 */
bool chore_service_is_available(void);

/**
 * @brief Poll whether cancellation was requested.
 *
 * Jobs should call this inside their run loop and return promptly when it
 * reports true.
 */
bool chore_service_cancel_pending(
    const chore_service_cancel_token_t *cancel);

#ifdef __cplusplus
}
#endif

#endif /* __CHORE_SERVICE_H__ */
