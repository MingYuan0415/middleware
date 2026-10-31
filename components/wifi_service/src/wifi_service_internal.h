#ifndef __WIFI_SERVICE_INTERNAL_H__
#define __WIFI_SERVICE_INTERNAL_H__

#include "wifi_service.h"
#include "wifi_service_port.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef WIFI_SERVICE_RETRY_DELAY_1_MS
    #define WIFI_SERVICE_RETRY_DELAY_1_MS 1000U
#endif
#ifndef WIFI_SERVICE_RETRY_DELAY_2_MS
    #define WIFI_SERVICE_RETRY_DELAY_2_MS 2000U
#endif
#ifndef WIFI_SERVICE_RETRY_DELAY_3_MS
    #define WIFI_SERVICE_RETRY_DELAY_3_MS 4000U
#endif
#ifndef WIFI_SERVICE_ASSOCIATION_TIMEOUT_MS
    #define WIFI_SERVICE_ASSOCIATION_TIMEOUT_MS 15000U
#endif
#ifndef WIFI_SERVICE_DHCP_TIMEOUT_MS
    #define WIFI_SERVICE_DHCP_TIMEOUT_MS 15000U
#endif

/** Largest association or DHCP deadline accepted through an option block. */
#define WIFI_SERVICE_MAX_PHASE_TIMEOUT_MS 60000U

#define WIFI_SERVICE_CREDENTIAL_SLOTS 2U
#define WIFI_SERVICE_RETRY_LIMIT      3U
#define WIFI_SERVICE_WORKER_POLL_MS   20U
#ifndef WIFI_SERVICE_SCAN_TIMEOUT_MS
    #define WIFI_SERVICE_SCAN_TIMEOUT_MS 15000U
#endif

/** @brief Creation state of the process-lifetime static control plane. */
typedef enum
{
    WIFI_CORE_EMPTY = 0,
    WIFI_CORE_CREATING,
    WIFI_CORE_READY,
    WIFI_CORE_BROKEN,
} wifi_core_state_t;

/** @brief Worker-owned runtime lifecycle state. */
typedef enum
{
    WIFI_RUNTIME_OFFLINE = 0,
    WIFI_RUNTIME_INITIALIZING,
    WIFI_RUNTIME_READY,
    WIFI_RUNTIME_SUSPEND_PENDING,
    WIFI_RUNTIME_SUSPENDED,
    WIFI_RUNTIME_RESUME_PENDING,
    WIFI_RUNTIME_DEINITIALIZING,
    WIFI_RUNTIME_CLEANUP_PENDING,
} wifi_runtime_state_t;

/** @brief Serialized control-plane request type. */
typedef enum
{
    WIFI_CONTROL_NONE = 0,
    WIFI_CONTROL_INIT,
    WIFI_CONTROL_SUSPEND,
    WIFI_CONTROL_RESUME,
    WIFI_CONTROL_DEINIT,
} wifi_control_type_t;

/** @brief Queue item type consumed by the worker. */
typedef enum
{
    WIFI_ITEM_SCAN = 0,
    WIFI_ITEM_CONNECT,
    WIFI_ITEM_DISCONNECT,
    WIFI_ITEM_PORT_EVENT,
    WIFI_ITEM_CONTROL,
} wifi_item_type_t;

/** @brief Active public operation tracked by the worker. */
typedef enum
{
    WIFI_OPERATION_NONE = 0,
    WIFI_OPERATION_SCAN,
    WIFI_OPERATION_CONNECT,
    WIFI_OPERATION_DISCONNECT,
} wifi_operation_kind_t;

/** @brief Result of atomically claiming a terminal operation transition. */
typedef enum
{
    WIFI_TERMINAL_CLAIMED = 0,
    WIFI_TERMINAL_CANCELED,
    WIFI_TERMINAL_STALE,
} wifi_terminal_claim_t;

/** @brief Fixed-size command or driver event queued to the worker. */
typedef struct wifi_queue_item
{
    wifi_item_type_t type;
    wifi_service_session_id_t session_id;
    wifi_service_operation_id_t operation_id;
    uint64_t admission_generation;
    uint64_t credential_generation;
    uint8_t credential_slot;
    wifi_service_connect_options_t options;
    wifi_service_port_event_t event;
} wifi_queue_item_t;

/** @brief One protected deep-copy slot for admitted credentials. */
typedef struct wifi_credential_slot
{
    bool in_use;
    uint64_t generation;
    wifi_service_port_credentials_t value;
} wifi_credential_slot_t;

/** @brief State owned exclusively by the process-lifetime worker task. */
typedef struct wifi_worker_context
{
    wifi_service_status_snapshot_t status;
    wifi_service_scan_snapshot_t scan;
    bool status_publish_pending;
    bool scan_publish_pending;
    bool publish_retry_scheduled;
    TickType_t publish_retry_deadline;
    wifi_service_port_credentials_t credentials;
    bool has_credentials;
    wifi_service_connect_options_t attempt_options;
    bool radio_ready;
    bool suspended;
    bool scan_active;
    bool scan_id_known;
    uint8_t next_scan_id;
    TickType_t scan_deadline;
    bool scan_deadline_pending;
    wifi_service_state_t pre_scan_state;
    bool retry_pending;
    TickType_t retry_deadline;
    bool phase_deadline_pending;
    TickType_t phase_deadline;
    wifi_operation_kind_t operation_kind;
    wifi_service_session_id_t operation_session;
    wifi_service_operation_id_t operation_id;
    uint64_t port_epoch;
} wifi_worker_context_t;

/** @brief Absolute task-tick deadline used by blocking control calls. */
typedef struct wifi_deadline
{
    bool forever;
    TickType_t start;
    TickType_t total;
} wifi_deadline_t;

/**
 * @brief Process-lifetime shared control plane and its static RTOS storage.
 *
 * @note Fields following runtime_state are protected by state_mutex after core
 *       creation. Atomic fields coordinate event and control completion paths.
 */
typedef struct wifi_service_shared
{
    atomic_int core_state;
    atomic_bool event_overflow;
    atomic_uint_fast64_t generation;

    StaticQueue_t queue_control;
    union wifi_service_queue_bytes
    {
        max_align_t alignment;
        uint8_t bytes[CONFIG_WIFI_SERVICE_QUEUE_DEPTH * sizeof(wifi_queue_item_t)];
    } queue_bytes;
    QueueHandle_t queue;

    StaticSemaphore_t state_mutex_control;
    StaticSemaphore_t control_mutex_control;
    StaticSemaphore_t control_done_control;
    SemaphoreHandle_t state_mutex;
    SemaphoreHandle_t control_mutex;
    SemaphoreHandle_t control_done;

    StaticTask_t worker_control;
    StackType_t worker_stack[CONFIG_WIFI_SERVICE_TASK_STACK];
    TaskHandle_t worker;
    bool primitives_created;
    wifi_service_config_t config;

    wifi_runtime_state_t runtime_state;
    bool accept_commands;
    wifi_service_session_id_t current_session;
    wifi_service_operation_id_t current_operation;
    uint64_t admission_generation;
    wifi_service_session_id_t cancel_session;
    wifi_service_operation_id_t cancel_operation;
    wifi_service_session_id_t claimed_session;
    wifi_service_operation_id_t claimed_operation;
    wifi_service_status_snapshot_t status_cache;
    wifi_service_scan_snapshot_t scan_cache;
    wifi_credential_slot_t credential_slots[WIFI_SERVICE_CREDENTIAL_SLOTS];
    /* One scan is admitted at a time; keeping its targets here instead of in
     * every queue item removes six target records per queued command. */
    bool scan_targets_valid;
    uint8_t scan_target_count;
    wifi_service_scan_target_t scan_targets[WIFI_SERVICE_MAX_SCAN_TARGETS];

    atomic_bool control_inflight;
    atomic_int control_type;
    atomic_uint_fast64_t control_generation;
    atomic_int control_request;
    atomic_uint_fast64_t control_completed_generation;
    atomic_int control_result;

#ifdef WIFI_SERVICE_TESTING
    atomic_bool worker_credentials_zero;
#endif
} wifi_service_shared_t;

/** @brief Shared private state defined by the public control-plane module. */
extern wifi_service_shared_t g_wifi_service;

/**
 * @brief Return a nonzero process-wide generation value.
 * @return Next nonzero generation.
 */
uint64_t wifi_service_internal_next_generation(void);

/** @brief Clear credential slots while the caller holds state_mutex. */
void wifi_service_internal_clear_slots_locked(void);

/**
 * @brief Return the admitted scan filter list while a scan owns it.
 *
 * The worker is the only consumer while the scan operation is current. The
 * pointer remains valid until the scan ends or the service is suspended.
 */
const wifi_service_scan_target_t *wifi_service_internal_scan_targets(
    uint8_t *target_count);

/** @brief Clear any scan targets that have not been consumed by the worker. */
void wifi_service_internal_clear_scan_targets(void);

/** @brief Abort a scan that missed its deadline and publish the failure. */
void wifi_service_worker_check_scan_deadline(wifi_worker_context_t *context);

/**
 * @brief Compare task ticks using wrap-safe signed subtraction.
 * @param now is the current task tick.
 * @param deadline is the absolute deadline tick.
 * @return true when now reached deadline; false otherwise.
 */
bool wifi_service_worker_tick_reached(TickType_t now, TickType_t deadline);

/**
 * @brief Clear pending event-bus publication state owned by the worker.
 * @param context is the worker-owned runtime state.
 */
void wifi_service_worker_clear_pending_publications(
    wifi_worker_context_t *context);

/**
 * @brief Retry worker publications after event-bus backpressure.
 * @param context is the worker-owned runtime state.
 */
void wifi_service_worker_retry_publications(wifi_worker_context_t *context);

/**
 * @brief Cache and publish the worker status snapshot.
 * @param context is the worker-owned runtime state.
 */
void wifi_service_worker_publish_status(wifi_worker_context_t *context);

/**
 * @brief Cache and publish the worker scan snapshot.
 * @param context is the worker-owned runtime state.
 */
void wifi_service_worker_publish_scan(wifi_worker_context_t *context);

/**
 * @brief Publish a service availability transition.
 * @param available is the new radio availability state.
 */
void wifi_service_worker_publish_availability(bool available);

/**
 * @brief Securely clear the worker-owned credential copy.
 * @param context is the worker-owned runtime state.
 */
void wifi_service_worker_wipe_secret(wifi_worker_context_t *context);

/**
 * @brief Return the completed-retry budget of the current attempt.
 * @param context is the worker-owned runtime state.
 * @return Zero when the attempt must not retry, otherwise the retry limit.
 */
uint8_t wifi_service_worker_retry_limit(const wifi_worker_context_t *context);

/**
 * @brief Return the association deadline ticks of the current attempt.
 * @param context is the worker-owned runtime state.
 * @return Absolute-relative tick count resolved from the attempt budget.
 */
TickType_t wifi_service_worker_association_ticks(
    const wifi_worker_context_t *context);

/**
 * @brief Return the DHCP deadline ticks of the current attempt.
 * @param context is the worker-owned runtime state.
 * @return Absolute-relative tick count resolved from the attempt budget.
 */
TickType_t wifi_service_worker_dhcp_ticks(const wifi_worker_context_t *context);

/**
 * @brief Commit a protected runtime state and command-admission policy.
 * @param state is the runtime state to publish.
 * @param accept_commands controls public command admission.
 */
void wifi_service_worker_set_runtime(wifi_runtime_state_t state,
                                     bool accept_commands);

/**
 * @brief Move a failed worker transaction into cleanup-pending state.
 * @param context is the worker-owned runtime state.
 * @param error is the transaction failure to publish.
 */
void wifi_service_worker_enter_cleanup_pending(
    wifi_worker_context_t *context, esp_err_t error);

/** @brief Release shared and worker operation ownership without a terminal. */
void wifi_service_worker_release_operation(wifi_worker_context_t *context);

/** @brief Latch a CONNECT/DISCONNECT terminal, then release its ownership. */
void wifi_service_worker_complete_operation(wifi_worker_context_t *context,
        esp_err_t result, wifi_service_failure_t failure, bool canceled);

typedef enum
{
    WIFI_RADIO_TARGET_STOPPED = 0,
    WIFI_RADIO_TARGET_STARTED,
} wifi_radio_target_t;

typedef struct wifi_service_radio_transition
{
    esp_err_t result;
    wifi_service_port_state_t state;
    bool credentials_cleared;
} wifi_service_radio_transition_t;

/** @brief Stop, verify STOPPED, clear credentials, and optionally start. */
wifi_service_radio_transition_t wifi_service_worker_transition_radio(
    wifi_worker_context_t *context, wifi_radio_target_t target);

/** @brief Publish a failed transition according to its final physical state. */
void wifi_service_worker_fail_transition(wifi_worker_context_t *context,
        const wifi_service_radio_transition_t *transition);

/**
 * @brief Submit the worker's credential copy to the Wi-Fi port.
 * @param context is the worker-owned runtime state.
 * @return ESP_OK when association starts, otherwise an ESP-IDF error.
 */
esp_err_t wifi_service_worker_connect_driver(wifi_worker_context_t *context);

/**
 * @brief Schedule or terminate a connection retry sequence.
 * @param context is the worker-owned runtime state.
 * @param reason is the Wi-Fi disconnect reason.
 * @param error is the driver operation result.
 */
void wifi_service_worker_schedule_retry(wifi_worker_context_t *context,
                                        uint16_t reason, esp_err_t error,
                                        wifi_service_failure_t failure);

/**
 * @brief Terminate an expired association or DHCP phase.
 * @param context is the worker-owned runtime state.
 */
void wifi_service_worker_check_phase_deadline(
    wifi_worker_context_t *context);

/**
 * @brief Cancel an operation invalidated by session or cancel state.
 * @param context is the worker-owned runtime state.
 */
void wifi_service_worker_cancel_stale_operation(
    wifi_worker_context_t *context);

/**
 * @brief Process one admitted command or Wi-Fi port event.
 * @param context is the worker-owned runtime state.
 * @param item is the immutable queued work item.
 */
void wifi_service_worker_process_item(wifi_worker_context_t *context,
                                      const wifi_queue_item_t *item);

/**
 * @brief Reconcile state after a port event could not enter the queue.
 * @param context is the worker-owned runtime state.
 */
void wifi_service_worker_reconcile_overflow(wifi_worker_context_t *context);

/**
 * @brief Process-lifetime worker entry point.
 * @param argument is unused.
 */
void wifi_service_worker_run(void *argument);

#endif /* __WIFI_SERVICE_INTERNAL_H__ */
