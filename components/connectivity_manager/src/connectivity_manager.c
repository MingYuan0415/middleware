#define DBG_TAG "connectivity"
#include "mt_log.h"

#include "connectivity_manager.h"
#include "connectivity_manager_digest.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#if CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0 || \
    CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1
    #include "freertos/idf_additions.h"
#endif
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nv_storage.h"
#include "wifi_service.h"

#ifndef CONNECTIVITY_MANAGER_ATTEMPT_RETRY_DELAY_MS
    #define CONNECTIVITY_MANAGER_ATTEMPT_RETRY_DELAY_MS 1000U
#endif
#ifndef CONNECTIVITY_MANAGER_SCAN_RETRY_DELAY_MS
    #define CONNECTIVITY_MANAGER_SCAN_RETRY_DELAY_MS 1000U
#endif
#ifndef CONNECTIVITY_MANAGER_CYCLE_MAX_MS
    #define CONNECTIVITY_MANAGER_CYCLE_MAX_MS 360000U
#endif

#define CONNECTIVITY_MANAGER_SCAN_ATTEMPTS  2U
#define CONNECTIVITY_MANAGER_FALLBACK_POLL_MS 1000U
#define CONNECTIVITY_MANAGER_RADIO_WAIT_MS  2000U
#define CONNECTIVITY_MANAGER_RADIO_STOP_WAIT_MS 5000U
#define CONNECTIVITY_MANAGER_RADIO_INIT_WAIT_MS 5000U
#define CONNECTIVITY_MANAGER_PROFILE_KEY    "wifi_profile"
#define CONNECTIVITY_MANAGER_PROFILE_MAGIC  UINT32_C(0x57465031)
#define CONNECTIVITY_MANAGER_PROFILE_VERSION UINT16_C(5)
#define CONNECTIVITY_MANAGER_PROFILE_V4_VERSION UINT16_C(4)
#define CONNECTIVITY_MANAGER_PROFILE_V3_VERSION UINT16_C(3)
#define CONNECTIVITY_MANAGER_PROFILE_V2_VERSION UINT16_C(2)
#define CONNECTIVITY_MANAGER_PROFILE_V1_VERSION UINT16_C(1)
#define CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES 32U
/* The on-flash record always reserves five entry slots and one active index, so
 * a record written by a build with more profiles stays structurally readable. */
#define CONNECTIVITY_MANAGER_PERSISTED_PROFILES 5U
/* Retry and phase budgets of one policy attempt: the manager owns candidate
 * ordering, so a single attempt must not consume the executor's retries. */
#define CONNECTIVITY_MANAGER_ATTEMPT_BUDGET_RETRY_LIMIT 0U
#define CONNECTIVITY_MANAGER_ATTEMPT_BUDGET_ASSOCIATION_TIMEOUT_MS 8000U
#define CONNECTIVITY_MANAGER_ATTEMPT_BUDGET_DHCP_TIMEOUT_MS 10000U
#define CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE UINT8_MAX
/** Reported when no fallback sweep is running. */
#define CONNECTIVITY_MANAGER_NO_ATTEMPT UINT8_MAX
#define CONNECTIVITY_MANAGER_TERMINAL_OUTBOX_CAPACITY \
    (CONFIG_CONNECTIVITY_MANAGER_QUEUE_DEPTH + 2U)
#define CONNECTIVITY_MANAGER_COMMAND_POOL_CAPACITY \
    (CONFIG_CONNECTIVITY_MANAGER_QUEUE_DEPTH + 1U)

EVENT_BUS_DEFINE_ID(CONNECTIVITY_MANAGER_MSG);

_Static_assert(sizeof(connectivity_manager_status_snapshot_t)
               <= EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
               "connectivity status exceeds the UI payload capacity");
_Static_assert(sizeof(connectivity_manager_scan_snapshot_t)
               <= EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
               "connectivity scan exceeds the UI payload capacity");
_Static_assert(sizeof(connectivity_manager_profile_snapshot_t)
               <= EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
               "connectivity profile exceeds the UI payload capacity");

typedef enum
{
    MANAGER_COMMAND_INIT = 0,
    MANAGER_COMMAND_SCAN,
    MANAGER_COMMAND_CONNECT,
    MANAGER_COMMAND_SYNC_PROFILE,
    MANAGER_COMMAND_SAVE_PROFILE,
    MANAGER_COMMAND_DISCONNECT,
    MANAGER_COMMAND_RECONNECT,
    MANAGER_COMMAND_FORGET,
    MANAGER_COMMAND_CONNECT_NETWORK,
    MANAGER_COMMAND_FORGET_NETWORK,
    MANAGER_COMMAND_AUTO,
    MANAGER_COMMAND_CANCEL,
    MANAGER_COMMAND_SUSPEND,
    MANAGER_COMMAND_RESUME,
    MANAGER_COMMAND_DEINIT,
} manager_command_type_t;

typedef enum
{
    MANAGER_OPERATION_NONE = 0,
    MANAGER_OPERATION_SCAN,
    MANAGER_OPERATION_CONNECT,
    MANAGER_OPERATION_DISCONNECT,
} manager_operation_kind_t;

typedef enum
{
    MANAGER_DEFER_NONE = 0,
    MANAGER_DEFER_QUEUED,
    MANAGER_DEFER_REJECTED,
} manager_defer_result_t;

typedef enum
{
    MANAGER_LIFECYCLE_OFFLINE = 0,
    MANAGER_LIFECYCLE_STORAGE_RESETTING,
    MANAGER_LIFECYCLE_INITIALIZING,
    MANAGER_LIFECYCLE_RUNNING,
    MANAGER_LIFECYCLE_STOPPING,
} manager_lifecycle_t;

typedef struct manager_deadline
{
    TickType_t started_at;
    TickType_t duration;
    bool wait_forever;
} manager_deadline_t;

typedef struct manager_profile_entry
{
    uint8_t security;
    uint8_t ssid_length;
    uint8_t password_length;
    uint8_t reserved;
    uint8_t ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES];
    uint8_t password[CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES + 1U];
    uint8_t trailing_reserved[2];
} manager_profile_entry_t;

typedef struct manager_profile
{
    uint32_t magic;
    uint16_t version;
    uint8_t count;
    uint8_t active_index;
    uint8_t wifi_enabled;
    uint8_t reserved[3];
    uint64_t profile_revision;
    uint64_t last_client_sync_id;
    uint8_t last_sync_identity[CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES];
    uint8_t sync_window_valid;
    uint8_t sync_reserved[7];
    manager_profile_entry_t
    entries[CONNECTIVITY_MANAGER_PERSISTED_PROFILES];
} manager_profile_t;

/** @brief Persisted multi-profile record retained for v4 migration. */
typedef struct manager_profile_v4
{
    uint32_t magic;
    uint16_t version;
    uint8_t count;
    uint8_t active_index;
    uint8_t auto_connect;
    uint8_t reserved[3];
    uint64_t profile_revision;
    uint64_t last_client_sync_id;
    uint8_t last_sync_identity[CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES];
    uint8_t sync_window_valid;
    uint8_t sync_reserved[7];
    manager_profile_entry_t
    entries[CONNECTIVITY_MANAGER_PERSISTED_PROFILES];
} manager_profile_v4_t;

/** @brief Persisted single-profile record retained for v3 migration. */
typedef struct manager_profile_v3
{
    uint32_t magic;
    uint16_t version;
    uint8_t security;
    uint8_t auto_connect;
    uint8_t ssid_length;
    uint8_t password_length;
    uint8_t reserved[2];
    uint8_t ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES];
    uint8_t password[CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES + 1U];
    uint8_t trailing_reserved[3];
    uint64_t profile_revision;
    uint64_t last_client_sync_id;
    uint8_t last_sync_identity[CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES];
    uint8_t sync_window_valid;
    uint8_t sync_reserved[7];
} manager_profile_v3_t;

typedef struct manager_profile_v2
{
    uint32_t magic;
    uint16_t version;
    uint8_t security;
    uint8_t auto_connect;
    uint8_t ssid_length;
    uint8_t password_length;
    uint8_t reserved[2];
    uint8_t ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES];
    uint8_t password[CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES + 1U];
    uint8_t trailing_reserved[3];
    uint64_t profile_revision;
    uint64_t last_client_sync_id;
} manager_profile_v2_t;

typedef struct manager_profile_v1
{
    uint32_t magic;
    uint16_t version;
    uint8_t security;
    uint8_t auto_connect;
    uint8_t ssid_length;
    uint8_t password_length;
    uint8_t reserved[2];
    uint8_t ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES];
    uint8_t password[CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES + 1U];
    uint8_t trailing_reserved[3];
} manager_profile_v1_t;

typedef struct manager_command
{
    manager_command_type_t type;
    connectivity_manager_operation_id_t operation_id;
    uint64_t control_generation;
    uint32_t timeout_ms;
    bool enabled;
    uint64_t client_sync_id;
    manager_profile_entry_t credentials;
} manager_command_t;

typedef enum
{
    MANAGER_TERMINAL_STATUS = 0,
    MANAGER_TERMINAL_SCAN,
    MANAGER_TERMINAL_PROFILES,
} manager_terminal_type_t;

typedef struct manager_terminal_event
{
    manager_terminal_type_t type;
    union
    {
        connectivity_manager_status_snapshot_t status;
        connectivity_manager_scan_snapshot_t scan;
        connectivity_manager_profile_snapshot_t profiles;
    } payload;
} manager_terminal_event_t;

typedef struct manager_worker
{
    wifi_service_session_id_t session_id;
    wifi_service_operation_id_t service_operation_id;
    manager_operation_kind_t operation_kind;
    manager_command_type_t active_command;
    connectivity_manager_operation_id_t operation_id;
    bool active_cancel_requested;
    uint64_t service_status_generation;
    uint64_t service_scan_generation;
    /* Saved networks; index 0..profile_count-1 are valid entries. */
    manager_profile_entry_t profiles[CONNECTIVITY_MANAGER_MAX_PROFILES];
    uint8_t profile_count;
    uint8_t active_index;
    /* Full persisted width: the order survives a rerun with fewer active
     * profiles, and only entries below profile_count are consulted. */
    uint8_t attempt_order[CONNECTIVITY_MANAGER_PERSISTED_PROFILES];
    /* One transient candidate that is not saved yet. */
    manager_profile_entry_t target;
    bool target_valid;
    bool target_candidate;
    bool target_persisted;
    bool target_reconnectable;
    /* Deferred single-entry removal applied at the disconnect terminal. */
    uint8_t pending_removal;
    bool pending_removal_valid;
    bool pending_removal_drops_link;
    uint64_t profile_revision;
    uint8_t wifi_enabled;
    uint64_t last_client_sync_id;
    uint8_t last_sync_identity[CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES];
    uint8_t sync_window_valid;
    /* Round-robin attempt over the saved networks. */
    uint8_t attempt_active;
    uint8_t attempt_position;
    uint8_t attempt_ordinal;
    bool active_sync;
    connectivity_manager_client_sync_id_t active_client_sync_id;
    uint8_t active_sync_identity[CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES];
    bool radio_initialized;
    bool suspended;
    bool cycle_active;
    bool cycle_scan_pending;
    bool resume_cycle_after_scan;
    uint8_t scan_matched_mask;
    uint8_t cycle_attempt_count;
    uint8_t cycle_start_index;
    uint8_t cycle_scan_attempts;
    connectivity_manager_operation_id_t cycle_operation_id;
    TickType_t cycle_retry_deadline;
    TickType_t cycle_scan_retry_deadline;
    bool cycle_scan_retry_pending;
    bool policy_dirty;
    bool retry_pending;
    bool connect_resume_pending;
    bool pending_command_valid;
    manager_command_t pending_command;
    TickType_t retry_deadline;
    uint8_t retry_count;
    UBaseType_t reported_stack_minimum_free;
    bool stack_minimum_reported;
    connectivity_manager_status_snapshot_t status;
    connectivity_manager_scan_snapshot_t scan;
    connectivity_manager_profile_snapshot_t profiles_snapshot;
} manager_worker_t;

typedef struct manager_shared
{
    atomic_uint_fast64_t generation;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t control_mutex;
    SemaphoreHandle_t control_done;
    QueueHandle_t queue;
    StaticSemaphore_t mutex_control;
    StaticSemaphore_t control_mutex_control;
    StaticSemaphore_t control_done_control;
    StaticQueue_t queue_control;
    union manager_queue_bytes
    {
        uint8_t bytes[CONFIG_CONNECTIVITY_MANAGER_QUEUE_DEPTH *
                      sizeof(manager_command_t *)];
        uintptr_t alignment;
    } queue_storage;
    manager_command_t
    command_pool[CONNECTIVITY_MANAGER_COMMAND_POOL_CAPACITY];
    bool command_pool_used[CONNECTIVITY_MANAGER_COMMAND_POOL_CAPACITY];
    TaskHandle_t worker;
    StaticTask_t worker_control;
    /* The policy state stays in the singleton: as a task-stack local it
     * consumed half the worker stack, and the scan-snapshot publish chain
     * (event bus plus Device Link callbacks) runs on this same task. */
    manager_worker_t worker_state;
    StackType_t worker_stack[CONFIG_CONNECTIVITY_MANAGER_TASK_STACK];
    connectivity_manager_config_t config;
    bool control_inflight;
    manager_command_type_t control_type;
    uint64_t control_generation;
    atomic_uint_fast64_t control_completed_generation;
    atomic_int control_result;
    atomic_bool worker_stopping;
    connectivity_manager_status_snapshot_t status_cache;
    connectivity_manager_scan_snapshot_t scan_cache;
    connectivity_manager_profile_snapshot_t profile_cache;
    event_bus_sub_handle_t wifi_status_subscription;
    event_bus_sub_handle_t wifi_scan_subscription;
    manager_terminal_event_t
    terminal_outbox[CONNECTIVITY_MANAGER_TERMINAL_OUTBOX_CAPACITY];
    size_t terminal_outbox_head;
    size_t terminal_outbox_count;
} manager_shared_t;

static manager_shared_t s_manager;
static atomic_int s_manager_lifecycle = MANAGER_LIFECYCLE_OFFLINE;
static atomic_uint s_manager_api_users = 0U;
static atomic_bool s_manager_deinit_active = false;

static esp_err_t _manager_profile_store(const manager_worker_t *worker);
static void _manager_apply_pending_removal(manager_worker_t *worker,
        connectivity_manager_operation_id_t operation_id);
static void _manager_touch_profile(manager_worker_t *worker, uint8_t index);
static esp_err_t _manager_subscribe_wifi_events(void);
static void _manager_unsubscribe_wifi_events(void);

_Static_assert(CONNECTIVITY_MANAGER_SSID_MAX_BYTES ==
               WIFI_SERVICE_SSID_MAX_BYTES, "SSID limits must match");
_Static_assert(CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES ==
               WIFI_SERVICE_PASSWORD_MAX_BYTES, "password limits must match");
_Static_assert(CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS ==
               WIFI_SERVICE_MAX_SCAN_RECORDS, "scan limits must match");
_Static_assert(sizeof(manager_profile_t) == 592U,
               "Wi-Fi profile v5 record size changed");
_Static_assert(sizeof(manager_profile_v4_t) == 592U,
               "Wi-Fi profile v4 record size changed");
_Static_assert(sizeof(manager_profile_v3_t) == 168U,
               "Wi-Fi profile v3 record size changed");
_Static_assert(sizeof(manager_profile_v2_t) == 128U,
               "Wi-Fi profile v2 record size changed");
_Static_assert(sizeof(manager_profile_v1_t) == 112U,
               "Wi-Fi profile v1 record size changed");
_Static_assert(CONNECTIVITY_MANAGER_MAX_PROFILES ==
               CONNECTIVITY_MANAGER_PERSISTED_PROFILES,
               "the in-RAM list and the persisted record share one width");
_Static_assert(sizeof(connectivity_manager_status_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE, "status snapshot too large");
_Static_assert(sizeof(connectivity_manager_scan_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE, "scan snapshot too large");
_Static_assert(sizeof(connectivity_manager_profile_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE, "profile snapshot too large");

static void _manager_secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size > 0U)
    {
        *bytes = 0U;
        ++bytes;
        --size;
    }
}

static manager_command_t *_manager_command_pool_acquire(
    const manager_command_t *command)
{
    manager_command_t *slot = NULL;
    xSemaphoreTake(s_manager.mutex, portMAX_DELAY);
    for (size_t index = 0U;
            index < CONNECTIVITY_MANAGER_COMMAND_POOL_CAPACITY; ++index)
    {
        if (!s_manager.command_pool_used[index])
        {
            s_manager.command_pool_used[index] = true;
            s_manager.command_pool[index] = *command;
            slot = &s_manager.command_pool[index];
            break;
        }
    }
    xSemaphoreGive(s_manager.mutex);
    return slot;
}

static void _manager_command_pool_release(manager_command_t *command)
{
    xSemaphoreTake(s_manager.mutex, portMAX_DELAY);
    for (size_t index = 0U;
            index < CONNECTIVITY_MANAGER_COMMAND_POOL_CAPACITY; ++index)
    {
        if (command == &s_manager.command_pool[index])
        {
            _manager_secure_zero(&s_manager.command_pool[index],
                                 sizeof(s_manager.command_pool[index]));
            s_manager.command_pool_used[index] = false;
            break;
        }
    }
    xSemaphoreGive(s_manager.mutex);
}

static void _manager_clear_candidate(manager_worker_t *worker)
{
    if (!worker->target_candidate)
    {
        return;
    }
    _manager_secure_zero(&worker->target, sizeof(worker->target));
    worker->target_valid = false;
    worker->target_candidate = false;
    worker->target_persisted = false;
    worker->target_reconnectable = false;
}

static void _manager_clear_target(manager_worker_t *worker)
{
    _manager_secure_zero(&worker->target, sizeof(worker->target));
    worker->target_valid = false;
    worker->target_candidate = false;
    worker->target_persisted = false;
    worker->target_reconnectable = false;
}

static void _manager_clear_ephemeral_link(manager_worker_t *worker)
{
    if (worker->target_valid && !worker->target_candidate &&
            !worker->target_persisted && !worker->target_reconnectable)
    {
        _manager_clear_target(worker);
    }
}

/** @brief Return the selected saved entry, or NULL when no profile is saved. */
static const manager_profile_entry_t *_manager_active_profile(
    const manager_worker_t *worker)
{
    return worker->profile_count == 0U ||
           worker->active_index >= worker->profile_count ?
           NULL : &worker->profiles[worker->active_index];
}

/** @brief Return the entry the next CONNECT must use, or NULL when absent. */
static const manager_profile_entry_t *_manager_target_profile(
    const manager_worker_t *worker)
{
    return worker->target_valid ? &worker->target :
           _manager_active_profile(worker);
}

/** @brief Find a saved entry by exact SSID bytes. */
static uint8_t _manager_find_profile(const manager_worker_t *worker,
                                     const uint8_t *ssid, size_t ssid_length)
{
    for (uint8_t index = 0U; index < worker->profile_count; ++index)
    {
        const manager_profile_entry_t *entry = &worker->profiles[index];

        if (entry->ssid_length == ssid_length &&
                memcmp(entry->ssid, ssid, ssid_length) == 0)
        {
            return index;
        }
    }
    return CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE;
}

/**
 * @brief Report whether a scan record matches one saved entry.
 *
 * The scan snapshot keeps SSID and security together, so a network whose
 * advertised security differs from the saved entry is not the saved profile.
 */
static bool _manager_scan_record_saved(
    const manager_worker_t *worker, const char *ssid,
    connectivity_manager_security_t security)
{
    const size_t ssid_length = strlen(ssid);
    const uint8_t index = _manager_find_profile(
                              worker, (const uint8_t *)ssid, ssid_length);

    return index != CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE &&
           (connectivity_manager_security_t)
           worker->profiles[index].security == security;
}

/**
 * @brief Move one saved entry to the front of the attempt order.
 *
 * The order decides which saved network the manager tries next; it lives in RAM
 * so a connection does not write flash.
 */
static void _manager_touch_profile(manager_worker_t *worker, uint8_t index)
{
    uint8_t order[CONNECTIVITY_MANAGER_PERSISTED_PROFILES];
    size_t count = 0U;

    if (index >= worker->profile_count)
    {
        return;
    }
    order[count++] = index;
    for (size_t step = 0U; step < CONNECTIVITY_MANAGER_PERSISTED_PROFILES;
            ++step)
    {
        const uint8_t candidate = worker->attempt_order[step];

        if (candidate != index && candidate < worker->profile_count &&
                count < worker->profile_count)
        {
            order[count++] = candidate;
        }
    }
    memset(worker->attempt_order, 0, sizeof(worker->attempt_order));
    memcpy(worker->attempt_order, order, count);
}

/** @brief Keep only entries below `count` in the attempt order. */
static void _manager_rebuild_order(manager_worker_t *worker)
{
    uint8_t order[CONNECTIVITY_MANAGER_PERSISTED_PROFILES];
    size_t count = 0U;

    for (size_t step = 0U; step < CONNECTIVITY_MANAGER_PERSISTED_PROFILES;
            ++step)
    {
        const uint8_t candidate = worker->attempt_order[step];

        if (candidate < worker->profile_count && count < worker->profile_count)
        {
            order[count++] = candidate;
        }
    }
    for (uint8_t index = 0U; index < worker->profile_count; ++index)
    {
        bool listed = false;

        for (size_t step = 0U; step < count; ++step)
        {
            if (order[step] == index)
            {
                listed = true;
                break;
            }
        }
        if (!listed && count < worker->profile_count)
        {
            order[count++] = index;
        }
    }
    memset(worker->attempt_order, 0, sizeof(worker->attempt_order));
    memcpy(worker->attempt_order, order, count);
}

/** @brief Make one saved entry the target of the next CONNECT. */
static void _manager_set_saved_target_index(manager_worker_t *worker,
        uint8_t index)
{
    _manager_clear_target(worker);
    if (index >= worker->profile_count)
    {
        return;
    }
    worker->target = worker->profiles[index];
    worker->target_valid = true;
    worker->target_persisted = true;
    worker->target_reconnectable = true;
}

static void _manager_set_saved_target(manager_worker_t *worker)
{
    _manager_set_saved_target_index(worker, worker->active_index);
}

static void _manager_set_ephemeral_link(
    manager_worker_t *worker, const manager_profile_entry_t *source)
{
    _manager_clear_target(worker);
    if (source == NULL)
    {
        return;
    }
    worker->target.security = source->security;
    worker->target.ssid_length = source->ssid_length;
    memcpy(worker->target.ssid, source->ssid, source->ssid_length);
    worker->target_valid = true;
}

static bool _manager_bytes_are_zero(const uint8_t *data, size_t size)
{
    for (size_t index = 0U; index < size; ++index)
    {
        if (data[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

static uint64_t _manager_next_generation(void)
{
    uint64_t generation = atomic_fetch_add_explicit(
                              &s_manager.generation, 1U,
                              memory_order_relaxed) + 1U;
    if (generation == 0U)
    {
        generation = atomic_fetch_add_explicit(
                         &s_manager.generation, 1U,
                         memory_order_relaxed) + 1U;
    }
    return generation;
}

static manager_deadline_t _manager_deadline(uint32_t timeout_ms)
{
    manager_deadline_t deadline =
    {
        .started_at = xTaskGetTickCount(),
        .wait_forever = timeout_ms == CONNECTIVITY_MANAGER_WAIT_FOREVER,
    };
    if (!deadline.wait_forever)
    {
        deadline.duration = pdMS_TO_TICKS(timeout_ms);
        if (timeout_ms > 0U && deadline.duration == 0U)
        {
            deadline.duration = 1U;
        }
    }
    return deadline;
}

static TickType_t _manager_deadline_remaining(
    const manager_deadline_t *deadline)
{
    if (deadline->wait_forever)
    {
        return portMAX_DELAY;
    }
    const TickType_t elapsed = xTaskGetTickCount() - deadline->started_at;
    return elapsed >= deadline->duration ? 0U : deadline->duration - elapsed;
}

static uint32_t _manager_deadline_remaining_ms(
    const manager_deadline_t *deadline)
{
    if (deadline->wait_forever)
    {
        return CONNECTIVITY_MANAGER_WAIT_FOREVER;
    }
    const TickType_t remaining = _manager_deadline_remaining(deadline);
    if (remaining == 0U)
    {
        return 0U;
    }
    uint64_t remaining_ms = ((uint64_t)remaining * 1000U +
                             configTICK_RATE_HZ - 1U) /
                            configTICK_RATE_HZ;
    if (remaining_ms >= CONNECTIVITY_MANAGER_WAIT_FOREVER)
    {
        remaining_ms = CONNECTIVITY_MANAGER_WAIT_FOREVER - 1U;
    }
    return (uint32_t)remaining_ms;
}

static esp_err_t _manager_wait_worker_suspended(
    const manager_deadline_t *deadline)
{
    while (s_manager.worker != NULL &&
            eTaskGetState(s_manager.worker) != eSuspended)
    {
        if (_manager_deadline_remaining(deadline) == 0U)
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1U);
    }
    return ESP_OK;
}

static bool _manager_tick_reached(TickType_t now, TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static bool _manager_api_acquire(void)
{
    if (atomic_load_explicit(&s_manager_lifecycle,
                             memory_order_acquire) !=
            MANAGER_LIFECYCLE_RUNNING)
    {
        return false;
    }
    atomic_fetch_add_explicit(&s_manager_api_users, 1U,
                              memory_order_acq_rel);
    if (atomic_load_explicit(&s_manager_lifecycle,
                             memory_order_acquire) ==
            MANAGER_LIFECYCLE_RUNNING)
    {
        return true;
    }
    atomic_fetch_sub_explicit(&s_manager_api_users, 1U,
                              memory_order_acq_rel);
    return false;
}

static void _manager_api_release(void)
{
    atomic_fetch_sub_explicit(&s_manager_api_users, 1U,
                              memory_order_release);
}

static void _manager_copy_ssid(char destination[], const uint8_t source[],
                               size_t length)
{
    memset(destination, 0, CONNECTIVITY_MANAGER_SSID_MAX_BYTES + 1U);
    if (length > 0U && length <= CONNECTIVITY_MANAGER_SSID_MAX_BYTES)
    {
        memcpy(destination, source, length);
    }
}

static bool _manager_credentials_policy_valid(
    const char *ssid, size_t ssid_length,
    const char *password, size_t password_length,
    connectivity_manager_security_t security)
{
    wifi_service_security_t wifi_security;

    switch (security)
    {
    case CONNECTIVITY_MANAGER_SECURITY_OPEN:
        wifi_security = WIFI_SERVICE_SECURITY_OPEN;
        break;
    case CONNECTIVITY_MANAGER_SECURITY_PERSONAL:
        wifi_security = WIFI_SERVICE_SECURITY_PERSONAL;
        break;
    default:
        return false;
    }
    const wifi_service_credentials_t credentials =
    {
        .ssid = ssid,
        .ssid_length = ssid_length,
        .password = password,
        .password_length = password_length,
        .security = wifi_security,
    };

    return wifi_service_credentials_valid(&credentials);
}

static bool _manager_entry_payload_valid(
    uint8_t security, uint8_t ssid_length, uint8_t password_length,
    uint8_t reserved,
    const uint8_t ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES],
    const uint8_t password[CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES + 1U],
    const uint8_t *trailing_reserved, size_t trailing_length)
{
    if (ssid_length > CONNECTIVITY_MANAGER_SSID_MAX_BYTES ||
            password_length > CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES ||
            security > CONNECTIVITY_MANAGER_SECURITY_PERSONAL ||
            reserved != 0U ||
            !_manager_bytes_are_zero(trailing_reserved, trailing_length) ||
            !_manager_credentials_policy_valid(
                (const char *)ssid, ssid_length,
                (const char *)password, password_length,
                (connectivity_manager_security_t)security))
    {
        return false;
    }
    return _manager_bytes_are_zero(ssid + ssid_length,
                                   CONNECTIVITY_MANAGER_SSID_MAX_BYTES -
                                   ssid_length) &&
           _manager_bytes_are_zero(
               password + password_length,
               CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES + 1U -
               password_length);
}

/** @brief Validate one saved-network entry, tolerating cleared tail slots. */
static bool _manager_entry_valid(const manager_profile_entry_t *entry)
{
    if (_manager_bytes_are_zero((const uint8_t *)entry, sizeof(*entry)))
    {
        return false;
    }
    return _manager_entry_payload_valid(
               entry->security, entry->ssid_length, entry->password_length,
               entry->reserved, entry->ssid, entry->password,
               entry->trailing_reserved, sizeof(entry->trailing_reserved));
}

static bool _manager_sync_fields_valid(uint64_t last_client_sync_id,
                                       const uint8_t identity[],
                                       uint8_t sync_window_valid)
{
    const bool digest_zero = _manager_bytes_are_zero(
                                 identity, CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES);

    return sync_window_valid != 0U ?
           last_client_sync_id != 0U && !digest_zero :
           last_client_sync_id == 0U && digest_zero;
}

static bool _manager_profile_valid(const manager_profile_t *profile)
{
    if (profile->magic != CONNECTIVITY_MANAGER_PROFILE_MAGIC ||
            profile->version != CONNECTIVITY_MANAGER_PROFILE_VERSION ||
            profile->profile_revision == 0U ||
            profile->wifi_enabled > 1U ||
            profile->sync_window_valid > 1U ||
            profile->count > CONNECTIVITY_MANAGER_PERSISTED_PROFILES ||
            !_manager_bytes_are_zero(profile->reserved,
                                     sizeof(profile->reserved)) ||
            !_manager_bytes_are_zero(profile->sync_reserved,
                                     sizeof(profile->sync_reserved)) ||
            !_manager_sync_fields_valid(profile->last_client_sync_id,
                                        profile->last_sync_identity,
                                        profile->sync_window_valid))
    {
        return false;
    }
    if (profile->count == 0U)
    {
        return profile->active_index == CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE;
    }
    if (profile->active_index >= profile->count)
    {
        return false;
    }
    for (size_t index = 0U; index < CONNECTIVITY_MANAGER_PERSISTED_PROFILES;
            ++index)
    {
        const manager_profile_entry_t *entry = &profile->entries[index];
        const bool occupied = index < profile->count;

        if (occupied != _manager_entry_valid(entry))
        {
            return false;
        }
    }
    return true;
}

static bool _manager_profile_v2_valid(const manager_profile_v2_t *profile)
{
    return profile->magic == CONNECTIVITY_MANAGER_PROFILE_MAGIC &&
           profile->version == CONNECTIVITY_MANAGER_PROFILE_V2_VERSION &&
           profile->profile_revision != 0U &&
           profile->auto_connect <= 1U &&
           _manager_bytes_are_zero(profile->reserved, 2U) &&
           _manager_bytes_are_zero(profile->trailing_reserved,
                                   sizeof(profile->trailing_reserved)) &&
           _manager_entry_payload_valid(
               profile->security, profile->ssid_length,
               profile->password_length, 0U, profile->ssid, profile->password,
               profile->trailing_reserved, 0U);
}

static bool _manager_profile_v1_valid(const manager_profile_v1_t *profile)
{
    return profile->magic == CONNECTIVITY_MANAGER_PROFILE_MAGIC &&
           profile->version == CONNECTIVITY_MANAGER_PROFILE_V1_VERSION &&
           profile->auto_connect <= 1U &&
           _manager_bytes_are_zero(profile->reserved, 2U) &&
           _manager_bytes_are_zero(profile->trailing_reserved,
                                   sizeof(profile->trailing_reserved)) &&
           _manager_entry_payload_valid(
               profile->security, profile->ssid_length,
               profile->password_length, 0U, profile->ssid, profile->password,
               profile->trailing_reserved, 0U);
}

static bool _manager_profile_v3_valid(const manager_profile_v3_t *profile)
{
    return profile->magic == CONNECTIVITY_MANAGER_PROFILE_MAGIC &&
           profile->version == CONNECTIVITY_MANAGER_PROFILE_V3_VERSION &&
           profile->profile_revision != 0U &&
           profile->auto_connect <= 1U &&
           profile->sync_window_valid <= 1U &&
           _manager_bytes_are_zero(profile->reserved, 2U) &&
           _manager_bytes_are_zero(profile->trailing_reserved,
                                   sizeof(profile->trailing_reserved)) &&
           _manager_bytes_are_zero(profile->sync_reserved,
                                   sizeof(profile->sync_reserved)) &&
           _manager_entry_payload_valid(
               profile->security, profile->ssid_length,
               profile->password_length, 0U, profile->ssid, profile->password,
               profile->trailing_reserved, 0U) &&
           _manager_sync_fields_valid(profile->last_client_sync_id,
                                      profile->last_sync_identity,
                                      profile->sync_window_valid);
}

static bool _manager_profile_v4_valid(const manager_profile_v4_t *profile)
{
    if (profile->magic != CONNECTIVITY_MANAGER_PROFILE_MAGIC ||
            profile->version != CONNECTIVITY_MANAGER_PROFILE_V4_VERSION ||
            profile->profile_revision == 0U ||
            profile->auto_connect > 1U ||
            profile->sync_window_valid > 1U ||
            profile->count > CONNECTIVITY_MANAGER_PERSISTED_PROFILES ||
            !_manager_bytes_are_zero(profile->reserved,
                                     sizeof(profile->reserved)) ||
            !_manager_bytes_are_zero(profile->sync_reserved,
                                     sizeof(profile->sync_reserved)) ||
            !_manager_sync_fields_valid(profile->last_client_sync_id,
                                        profile->last_sync_identity,
                                        profile->sync_window_valid))
    {
        return false;
    }
    if (profile->count == 0U)
    {
        return profile->active_index == CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE;
    }
    if (profile->active_index >= profile->count)
    {
        return false;
    }
    for (size_t index = 0U; index < CONNECTIVITY_MANAGER_PERSISTED_PROFILES;
            ++index)
    {
        const bool occupied = index < profile->count;

        if (occupied != _manager_entry_valid(&profile->entries[index]))
        {
            return false;
        }
    }
    return true;
}

static void _manager_entry_from_payload(
    manager_profile_entry_t *entry, uint8_t security, uint8_t ssid_length,
    uint8_t password_length,
    const uint8_t ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES],
    const uint8_t password[CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES + 1U])
{
    memset(entry, 0, sizeof(*entry));
    entry->security = security;
    entry->ssid_length = ssid_length;
    entry->password_length = password_length;
    memcpy(entry->ssid, ssid, sizeof(entry->ssid));
    memcpy(entry->password, password, sizeof(entry->password));
}

static void _manager_worker_clear_profiles(manager_worker_t *worker)
{
    _manager_secure_zero(worker->profiles, sizeof(worker->profiles));
    worker->profile_count = 0U;
    worker->active_index = CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE;
    worker->profile_revision = 0U;
    worker->wifi_enabled = 0U;
    worker->last_client_sync_id = 0U;
    worker->sync_window_valid = 0U;
    _manager_secure_zero(worker->last_sync_identity,
                         sizeof(worker->last_sync_identity));
    worker->attempt_active = 0U;
    worker->attempt_position = 0U;
    for (size_t index = 0U; index < CONNECTIVITY_MANAGER_PERSISTED_PROFILES;
            ++index)
    {
        worker->attempt_order[index] = (uint8_t)index;
    }
}

static esp_err_t _manager_profile_load(manager_worker_t *worker)
{
    uint8_t raw[sizeof(manager_profile_t)];
    manager_profile_t profile;
    memset(raw, 0, sizeof(raw));
    memset(&profile, 0, sizeof(profile));
    size_t size = sizeof(raw);
    esp_err_t result = nv_storage_get_blob(CONNECTIVITY_MANAGER_PROFILE_KEY,
                                           raw, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        _manager_secure_zero(raw, sizeof(raw));
        return ESP_OK;
    }
    if (result == ESP_ERR_INVALID_SIZE
#ifdef ESP_ERR_NVS_INVALID_LENGTH
            || result == ESP_ERR_NVS_INVALID_LENGTH
#endif
       )
    {
        _manager_secure_zero(raw, sizeof(raw));
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (result != ESP_OK)
    {
        _manager_secure_zero(raw, sizeof(raw));
        return result;
    }
    bool migration_required = false;

    if (size == sizeof(manager_profile_v1_t))
    {
        manager_profile_v1_t legacy;

        memcpy(&legacy, raw, sizeof(legacy));
        if (!_manager_profile_v1_valid(&legacy))
        {
            _manager_secure_zero(&legacy, sizeof(legacy));
            _manager_secure_zero(raw, sizeof(raw));
            return ESP_ERR_INVALID_RESPONSE;
        }
        profile.magic = CONNECTIVITY_MANAGER_PROFILE_MAGIC;
        profile.version = CONNECTIVITY_MANAGER_PROFILE_VERSION;
        profile.count = 1U;
        profile.active_index = 0U;
        profile.wifi_enabled = legacy.auto_connect;
        profile.profile_revision = CONNECTIVITY_MANAGER_PROFILE_REVISION_INITIAL;
        _manager_entry_from_payload(&profile.entries[0], legacy.security,
                                    legacy.ssid_length, legacy.password_length,
                                    legacy.ssid, legacy.password);
        _manager_secure_zero(&legacy, sizeof(legacy));
        migration_required = true;
    }
    else if (size == sizeof(manager_profile_v2_t))
    {
        manager_profile_v2_t legacy;

        memcpy(&legacy, raw, sizeof(legacy));
        if (!_manager_profile_v2_valid(&legacy))
        {
            _manager_secure_zero(&legacy, sizeof(legacy));
            _manager_secure_zero(&profile, sizeof(profile));
            _manager_secure_zero(raw, sizeof(raw));
            return ESP_ERR_INVALID_RESPONSE;
        }
        profile.magic = CONNECTIVITY_MANAGER_PROFILE_MAGIC;
        profile.version = CONNECTIVITY_MANAGER_PROFILE_VERSION;
        profile.count = 1U;
        profile.active_index = 0U;
        profile.wifi_enabled = legacy.auto_connect;
        profile.profile_revision = legacy.profile_revision;
        _manager_entry_from_payload(&profile.entries[0], legacy.security,
                                    legacy.ssid_length, legacy.password_length,
                                    legacy.ssid, legacy.password);
        _manager_secure_zero(&legacy, sizeof(legacy));
        migration_required = true;
    }
    else if (size == sizeof(manager_profile_v3_t))
    {
        manager_profile_v3_t legacy;

        memcpy(&legacy, raw, sizeof(legacy));
        if (!_manager_profile_v3_valid(&legacy))
        {
            _manager_secure_zero(&legacy, sizeof(legacy));
            _manager_secure_zero(&profile, sizeof(profile));
            _manager_secure_zero(raw, sizeof(raw));
            return ESP_ERR_INVALID_RESPONSE;
        }
        profile.magic = CONNECTIVITY_MANAGER_PROFILE_MAGIC;
        profile.version = CONNECTIVITY_MANAGER_PROFILE_VERSION;
        profile.count = 1U;
        profile.active_index = 0U;
        profile.wifi_enabled = legacy.auto_connect;
        profile.profile_revision = legacy.profile_revision;
        profile.last_client_sync_id = legacy.last_client_sync_id;
        profile.sync_window_valid = legacy.sync_window_valid;
        memcpy(profile.last_sync_identity, legacy.last_sync_identity,
               sizeof(profile.last_sync_identity));
        _manager_entry_from_payload(&profile.entries[0], legacy.security,
                                    legacy.ssid_length, legacy.password_length,
                                    legacy.ssid, legacy.password);
        _manager_secure_zero(&legacy, sizeof(legacy));
        migration_required = true;
    }
    else if (size == sizeof(profile))
    {
        memcpy(&profile, raw, sizeof(profile));
        if (profile.version == CONNECTIVITY_MANAGER_PROFILE_V4_VERSION)
        {
            manager_profile_v4_t legacy;

            memcpy(&legacy, raw, sizeof(legacy));
            if (!_manager_profile_v4_valid(&legacy))
            {
                _manager_secure_zero(&legacy, sizeof(legacy));
                _manager_secure_zero(&profile, sizeof(profile));
                _manager_secure_zero(raw, sizeof(raw));
                return ESP_ERR_INVALID_RESPONSE;
            }
            memset(&profile, 0, sizeof(profile));
            profile.magic = CONNECTIVITY_MANAGER_PROFILE_MAGIC;
            profile.version = CONNECTIVITY_MANAGER_PROFILE_VERSION;
            profile.count = legacy.count;
            profile.active_index = legacy.active_index;
            profile.wifi_enabled = legacy.auto_connect;
            profile.profile_revision = legacy.profile_revision;
            profile.last_client_sync_id = legacy.last_client_sync_id;
            profile.sync_window_valid = legacy.sync_window_valid;
            memcpy(profile.last_sync_identity, legacy.last_sync_identity,
                   sizeof(profile.last_sync_identity));
            memcpy(profile.entries, legacy.entries, sizeof(profile.entries));
            _manager_secure_zero(&legacy, sizeof(legacy));
            migration_required = true;
        }
    }
    else
    {
        _manager_secure_zero(raw, sizeof(raw));
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!_manager_profile_valid(&profile))
    {
        _manager_secure_zero(&profile, sizeof(profile));
        _manager_secure_zero(raw, sizeof(raw));
        return ESP_ERR_INVALID_RESPONSE;
    }
    _manager_worker_clear_profiles(worker);
    /* The in-RAM list and the persisted record are both exactly
     * CONNECTIVITY_MANAGER_MAX_PROFILES wide, so a valid record loads whole. */
    worker->profile_count = profile.count;
    worker->active_index = profile.count == 0U ?
                           CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE :
                           profile.active_index;
    worker->wifi_enabled = profile.wifi_enabled;
    worker->profile_revision = profile.profile_revision;
    worker->last_client_sync_id = profile.last_client_sync_id;
    worker->sync_window_valid = profile.sync_window_valid;
    memcpy(worker->last_sync_identity, profile.last_sync_identity,
           sizeof(worker->last_sync_identity));
    for (size_t index = 0U; index < worker->profile_count; ++index)
    {
        worker->profiles[index] = profile.entries[index];
    }
    _manager_secure_zero(&profile, sizeof(profile));
    _manager_secure_zero(raw, sizeof(raw));
    if (worker->profile_count == 0U && worker->wifi_enabled != 0U)
    {
        worker->wifi_enabled = 0U;
    }
    worker->target_reconnectable = worker->profile_count > 0U;
    if (migration_required)
    {
        result = _manager_profile_store(worker);
        if (result != ESP_OK)
        {
            /* The upgraded record never reached flash, so the RAM list must not
             * claim to be loaded; the original bytes stay intact. */
            _manager_worker_clear_profiles(worker);
            worker->target_reconnectable = false;
            return result;
        }
    }
    return ESP_OK;
}

static esp_err_t _manager_profile_store(const manager_worker_t *worker)
{
    manager_profile_t record;

    /* The selected index must address a retained entry; a wider record clamped
     * on load once violated this and produced an unreadable record. */
    if (worker == NULL || worker->profile_count > CONNECTIVITY_MANAGER_MAX_PROFILES ||
            worker->profile_revision == 0U ||
            (worker->profile_count > 0U &&
             worker->active_index >= worker->profile_count))
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&record, 0, sizeof(record));
    record.magic = CONNECTIVITY_MANAGER_PROFILE_MAGIC;
    record.version = CONNECTIVITY_MANAGER_PROFILE_VERSION;
    record.count = worker->profile_count;
    record.active_index = worker->profile_count == 0U ?
                          CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE :
                          worker->active_index;
    record.wifi_enabled = worker->wifi_enabled;
    record.profile_revision = worker->profile_revision;
    record.last_client_sync_id = worker->last_client_sync_id;
    record.sync_window_valid = worker->sync_window_valid;
    memcpy(record.last_sync_identity, worker->last_sync_identity,
           sizeof(record.last_sync_identity));
    for (size_t index = 0U; index < worker->profile_count; ++index)
    {
        record.entries[index] = worker->profiles[index];
    }
    const esp_err_t result = nv_storage_set_blob(
                                 CONNECTIVITY_MANAGER_PROFILE_KEY, &record,
                                 sizeof(record));

    _manager_secure_zero(&record, sizeof(record));
    return result;
}

static esp_err_t _manager_profile_sync_identity(
    const manager_profile_entry_t *profile, bool auto_connect,
    uint8_t digest[CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES])
{
    static const uint8_t domain[] =
        "device-link.wifi.v1.set-credentials";
    uint8_t canonical[sizeof(domain) + 5U +
                      CONNECTIVITY_MANAGER_SSID_MAX_BYTES +
                      CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES];
    size_t offset = 0U;

    if (profile == NULL || digest == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t wire_security = 0U;

    switch ((connectivity_manager_security_t)profile->security)
    {
    case CONNECTIVITY_MANAGER_SECURITY_OPEN:
        wire_security = 1U;
        break;
    case CONNECTIVITY_MANAGER_SECURITY_PERSONAL:
        wire_security = 2U;
        break;
    case CONNECTIVITY_MANAGER_SECURITY_UNSUPPORTED:
    default:
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&canonical[offset], domain, sizeof(domain));
    offset += sizeof(domain);
    canonical[offset++] = 1U;
    canonical[offset++] = wire_security;
    canonical[offset++] = auto_connect ? 1U : 0U;
    canonical[offset++] = profile->ssid_length;
    memcpy(&canonical[offset], profile->ssid, profile->ssid_length);
    offset += profile->ssid_length;
    canonical[offset++] = profile->password_length;
    memcpy(&canonical[offset], profile->password, profile->password_length);
    offset += profile->password_length;
    const esp_err_t result = connectivity_manager_digest_sha256(
                                 canonical, offset, digest);

    _manager_secure_zero(canonical, sizeof(canonical));
    return result;
}

static void _manager_cache_status_snapshot(
    manager_worker_t *worker,
    const connectivity_manager_status_snapshot_t *snapshot)
{
    worker->status.generation = snapshot->generation;
    xSemaphoreTake(s_manager.mutex, portMAX_DELAY);
    s_manager.status_cache = *snapshot;
    xSemaphoreGive(s_manager.mutex);
}

static void _manager_cache_scan_snapshot(
    manager_worker_t *worker,
    const connectivity_manager_scan_snapshot_t *snapshot)
{
    worker->scan.generation = snapshot->generation;
    xSemaphoreTake(s_manager.mutex, portMAX_DELAY);
    s_manager.scan_cache = *snapshot;
    xSemaphoreGive(s_manager.mutex);
}

/** @brief Rebuild the password-free saved-network snapshot. */
static void _manager_build_profile_snapshot(manager_worker_t *worker)
{
    connectivity_manager_profile_snapshot_t *snapshot =
        &worker->profiles_snapshot;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->count = worker->profile_count;
    snapshot->active_index = worker->profile_count == 0U ?
                             CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE :
                             worker->active_index;
    for (uint8_t index = 0U; index < worker->profile_count; ++index)
    {
        const manager_profile_entry_t *entry = &worker->profiles[index];
        connectivity_manager_profile_info_t *info = &snapshot->profiles[index];

        _manager_copy_ssid(info->ssid, entry->ssid, entry->ssid_length);
        info->ssid_length = entry->ssid_length;
        info->security = (connectivity_manager_security_t)entry->security;
        info->active = index == snapshot->active_index ? 1U : 0U;
    }
    for (size_t index = 0U; index < CONNECTIVITY_MANAGER_MAX_PROFILES; ++index)
    {
        const uint8_t ordered = worker->attempt_order[index];

        snapshot->order[index] = ordered < worker->profile_count ?
                                 ordered : CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE;
    }
}

static void _manager_publish_profiles(manager_worker_t *worker)
{
    _manager_build_profile_snapshot(worker);
    worker->profiles_snapshot.generation = _manager_next_generation();
    connectivity_manager_profile_snapshot_t published =
        worker->profiles_snapshot;
    xSemaphoreTake(s_manager.mutex, portMAX_DELAY);
    s_manager.profile_cache = published;
    xSemaphoreGive(s_manager.mutex);
    const esp_err_t result = event_bus_publish(
                                 CONNECTIVITY_MANAGER_MSG,
                                 CONNECTIVITY_MANAGER_MSG_SUB_TYPE_PROFILE_SNAPSHOT,
                                 &published, sizeof(published),
                                 EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
    if (result != ESP_OK)
    {
        LOG_W("profile publish failed: %d", (int)result);
    }
}

static esp_err_t _manager_dispatch_terminal(
    const manager_terminal_event_t *event)
{
    switch (event->type)
    {
    case MANAGER_TERMINAL_STATUS:
        return event_bus_publish(
                   CONNECTIVITY_MANAGER_MSG,
                   CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                   &event->payload.status, sizeof(event->payload.status),
                   EVENT_BUS_PUBLISH_FLAG_ATOMIC);
    case MANAGER_TERMINAL_SCAN:
        return event_bus_publish(
                   CONNECTIVITY_MANAGER_MSG,
                   CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                   &event->payload.scan, sizeof(event->payload.scan),
                   EVENT_BUS_PUBLISH_FLAG_ATOMIC);
    case MANAGER_TERMINAL_PROFILES:
    default:
        return event_bus_publish(
                   CONNECTIVITY_MANAGER_MSG,
                   CONNECTIVITY_MANAGER_MSG_SUB_TYPE_PROFILE_SNAPSHOT,
                   &event->payload.profiles, sizeof(event->payload.profiles),
                   EVENT_BUS_PUBLISH_FLAG_ATOMIC);
    }
}

static bool _manager_flush_terminal_outbox(void)
{
    while (s_manager.terminal_outbox_count > 0U)
    {
        manager_terminal_event_t *event =
            &s_manager.terminal_outbox[s_manager.terminal_outbox_head];
        const esp_err_t result = _manager_dispatch_terminal(event);
        if (result != ESP_OK)
        {
            return false;
        }
        _manager_secure_zero(event, sizeof(*event));
        s_manager.terminal_outbox_head =
            (s_manager.terminal_outbox_head + 1U) %
            CONNECTIVITY_MANAGER_TERMINAL_OUTBOX_CAPACITY;
        --s_manager.terminal_outbox_count;
    }
    return true;
}

static void _manager_queue_terminal(const manager_terminal_event_t *event)
{
    while (s_manager.terminal_outbox_count >=
            CONNECTIVITY_MANAGER_TERMINAL_OUTBOX_CAPACITY)
    {
        if (!_manager_flush_terminal_outbox())
        {
            vTaskDelay(1U);
        }
    }
    const size_t tail = (s_manager.terminal_outbox_head +
                         s_manager.terminal_outbox_count) %
                        CONNECTIVITY_MANAGER_TERMINAL_OUTBOX_CAPACITY;
    s_manager.terminal_outbox[tail] = *event;
    ++s_manager.terminal_outbox_count;
}

static void _manager_report_stack_minimum_free(manager_worker_t *worker)
{
    const UBaseType_t minimum_free = uxTaskGetStackHighWaterMark(NULL);
    if (!worker->stack_minimum_reported ||
            minimum_free < worker->reported_stack_minimum_free)
    {
        worker->reported_stack_minimum_free = minimum_free;
        worker->stack_minimum_reported = true;
        LOG_I("stack minimum free=%u bytes", (unsigned)minimum_free);
    }
}

static void _manager_publish_status(
    manager_worker_t *worker,
    const connectivity_manager_status_snapshot_t *snapshot, uint32_t flags)
{
    connectivity_manager_status_snapshot_t published = *snapshot;
    published.generation = _manager_next_generation();
    _manager_cache_status_snapshot(worker, &published);
    const esp_err_t result = event_bus_publish(
                                 CONNECTIVITY_MANAGER_MSG,
                                 CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                                 &published, sizeof(published), flags);
    if (result != ESP_OK)
    {
        LOG_W("status publish failed: %d", (int)result);
    }
}

static void _manager_cache_status(manager_worker_t *worker)
{
    if (worker->operation_id == 0U && worker->status.operation_complete)
    {
        /* Keep the last terminal in the polling cache, but do not publish it
         * again as a second terminal during an unrelated status refresh. */
        connectivity_manager_status_snapshot_t terminal = worker->status;
        connectivity_manager_status_snapshot_t published = worker->status;
        published.operation_id = 0U;
        published.operation_complete = false;
        published.operation_canceled = false;
        published.operation_conflict = false;
        _manager_publish_status(worker, &published,
                                EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
        terminal.generation = _manager_next_generation();
        _manager_cache_status_snapshot(worker, &terminal);
        return;
    }
    if (worker->operation_id != 0U)
    {
        worker->status.operation_id = worker->operation_id;
        worker->status.operation_complete = false;
        worker->status.operation_canceled = false;
        worker->status.operation_conflict = false;
    }
    else
    {
        worker->status.operation_id = 0U;
        worker->status.operation_complete = false;
        worker->status.operation_canceled = false;
        worker->status.operation_conflict = false;
    }
    _manager_publish_status(worker, &worker->status,
                            EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

static void _manager_publish_status_terminal(
    manager_worker_t *worker,
    connectivity_manager_operation_id_t operation_id, esp_err_t result,
    connectivity_manager_failure_t failure)
{
    if (operation_id == 0U)
    {
        return;
    }
    connectivity_manager_status_snapshot_t terminal = worker->status;
    terminal.operation_id = operation_id;
    terminal.operation_complete = true;
    terminal.operation_canceled = result == ESP_ERR_NOT_FINISHED;
    terminal.operation_conflict =
        failure == CONNECTIVITY_MANAGER_FAILURE_CONFLICT;
    terminal.last_error = result;
    terminal.failure = terminal.operation_conflict ?
                       CONNECTIVITY_MANAGER_FAILURE_NONE : failure;
    terminal.generation = _manager_next_generation();
    _manager_cache_status_snapshot(worker, &terminal);
    manager_terminal_event_t event =
    {
        .type = MANAGER_TERMINAL_STATUS,
        .payload.status = terminal,
    };
    esp_err_t publish_result = ESP_ERR_INVALID_STATE;
    if (s_manager.terminal_outbox_count == 0U)
    {
        publish_result = _manager_dispatch_terminal(&event);
    }
    if (publish_result != ESP_OK)
    {
        LOG_W("terminal status deferred: %d", (int)publish_result);
        _manager_queue_terminal(&event);
    }
    /* Keep the worker-side status in sync with the cached terminal. Without
     * this, the next non-terminal cache update clears operation_complete and
     * polling callers never observe the terminal. */
    worker->status.operation_id = operation_id;
    worker->status.operation_complete = true;
    worker->status.operation_canceled = terminal.operation_canceled;
    worker->status.operation_conflict = terminal.operation_conflict;
    worker->status.last_error = result;
    worker->status.failure = terminal.failure;
    _manager_report_stack_minimum_free(worker);
}

static void _manager_publish_scan(
    manager_worker_t *worker,
    const connectivity_manager_scan_snapshot_t *snapshot, uint32_t flags)
{
    connectivity_manager_scan_snapshot_t published = *snapshot;
    if (published.running)
    {
        published.operation_canceled = false;
    }
    published.generation = _manager_next_generation();
    _manager_cache_scan_snapshot(worker, &published);
    const esp_err_t result = event_bus_publish(
                                 CONNECTIVITY_MANAGER_MSG,
                                 CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                                 &published, sizeof(published), flags);
    if (result != ESP_OK)
    {
        LOG_W("scan publish failed: %d", (int)result);
    }
}

static void _manager_cache_scan(manager_worker_t *worker)
{
    if (!worker->scan.running && worker->scan.operation_id != 0U)
    {
        connectivity_manager_scan_snapshot_t terminal = worker->scan;
        terminal.operation_canceled =
            terminal.last_error == ESP_ERR_NOT_FINISHED;
        terminal.generation = _manager_next_generation();
        _manager_cache_scan_snapshot(worker, &terminal);
        manager_terminal_event_t event =
        {
            .type = MANAGER_TERMINAL_SCAN,
            .payload.scan = terminal,
        };
        esp_err_t result = ESP_ERR_INVALID_STATE;
        if (s_manager.terminal_outbox_count == 0U)
        {
            result = _manager_dispatch_terminal(&event);
        }
        if (result != ESP_OK)
        {
            LOG_W("terminal scan deferred: %d", (int)result);
            _manager_queue_terminal(&event);
        }
        _manager_report_stack_minimum_free(worker);
        return;
    }
    _manager_publish_scan(worker, &worker->scan,
                          worker->scan.running ?
                          EVENT_BUS_PUBLISH_FLAG_UI_LATEST : 0U);
    if (!worker->stack_minimum_reported)
    {
        _manager_report_stack_minimum_free(worker);
    }
}

static void _manager_publish_scan_terminal(
    manager_worker_t *worker,
    connectivity_manager_operation_id_t operation_id, esp_err_t result)
{
    connectivity_manager_scan_snapshot_t terminal;
    memset(&terminal, 0, sizeof(terminal));
    terminal.operation_id = operation_id;
    terminal.last_error = result;
    terminal.operation_canceled = result == ESP_ERR_NOT_FINISHED;
    terminal.generation = _manager_next_generation();
    _manager_cache_scan_snapshot(worker, &terminal);
    manager_terminal_event_t event =
    {
        .type = MANAGER_TERMINAL_SCAN,
        .payload.scan = terminal,
    };
    esp_err_t publish_result = ESP_ERR_INVALID_STATE;
    if (s_manager.terminal_outbox_count == 0U)
    {
        publish_result = _manager_dispatch_terminal(&event);
    }
    if (publish_result != ESP_OK)
    {
        LOG_W("terminal scan deferred: %d", (int)publish_result);
        _manager_queue_terminal(&event);
    }
    _manager_report_stack_minimum_free(worker);
}

static connectivity_manager_failure_t _manager_map_failure(
    wifi_service_failure_t failure)
{
    switch (failure)
    {
    case WIFI_SERVICE_FAILURE_AUTHENTICATION:
        return CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION;
    case WIFI_SERVICE_FAILURE_AP_NOT_FOUND:
        return CONNECTIVITY_MANAGER_FAILURE_AP_NOT_FOUND;
    case WIFI_SERVICE_FAILURE_ASSOCIATION_TIMEOUT:
        return CONNECTIVITY_MANAGER_FAILURE_ASSOCIATION_TIMEOUT;
    case WIFI_SERVICE_FAILURE_DHCP_TIMEOUT:
        return CONNECTIVITY_MANAGER_FAILURE_DHCP_TIMEOUT;
    case WIFI_SERVICE_FAILURE_LINK_LOST:
        return CONNECTIVITY_MANAGER_FAILURE_LINK_LOST;
    case WIFI_SERVICE_FAILURE_DRIVER:
        return CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
    case WIFI_SERVICE_FAILURE_NONE:
    default:
        return CONNECTIVITY_MANAGER_FAILURE_NONE;
    }
}

static connectivity_manager_state_t _manager_map_state(
    wifi_service_state_t state)
{
    switch (state)
    {
    case WIFI_SERVICE_STATE_IDLE:
        return CONNECTIVITY_MANAGER_STATE_IDLE;
    case WIFI_SERVICE_STATE_SCANNING:
        return CONNECTIVITY_MANAGER_STATE_SCANNING;
    case WIFI_SERVICE_STATE_CONNECTING:
        return CONNECTIVITY_MANAGER_STATE_CONNECTING;
    case WIFI_SERVICE_STATE_WAITING_IP:
        return CONNECTIVITY_MANAGER_STATE_WAITING_IP;
    case WIFI_SERVICE_STATE_IP_READY:
        return CONNECTIVITY_MANAGER_STATE_IP_READY;
    case WIFI_SERVICE_STATE_RETRY_WAIT:
        return CONNECTIVITY_MANAGER_STATE_RETRY_WAIT;
    case WIFI_SERVICE_STATE_SUSPENDED:
        return CONNECTIVITY_MANAGER_STATE_SUSPENDED;
    case WIFI_SERVICE_STATE_OFFLINE:
    default:
        return CONNECTIVITY_MANAGER_STATE_OFFLINE;
    }
}

static connectivity_manager_security_t _manager_map_security(
    wifi_service_security_t security)
{
    switch (security)
    {
    case WIFI_SERVICE_SECURITY_OPEN:
        return CONNECTIVITY_MANAGER_SECURITY_OPEN;
    case WIFI_SERVICE_SECURITY_PERSONAL:
        return CONNECTIVITY_MANAGER_SECURITY_PERSONAL;
    case WIFI_SERVICE_SECURITY_UNSUPPORTED:
    default:
        return CONNECTIVITY_MANAGER_SECURITY_UNSUPPORTED;
    }
}

static void _manager_set_target_status(manager_worker_t *worker)
{
    const manager_profile_entry_t *active = _manager_active_profile(worker);

    worker->status.saved_profile = worker->profile_count > 0U;
    worker->status.profile_count = worker->profile_count;
    worker->status.wifi_enabled = worker->profile_count > 0U &&
                                  worker->wifi_enabled != 0U;
    worker->status.profile_persisted = worker->target_valid &&
                                       worker->target_persisted;
    worker->status.profile_revision = worker->profile_count > 0U ?
                                      worker->profile_revision :
                                      CONNECTIVITY_MANAGER_PROFILE_REVISION_INITIAL;
    worker->status.applied_client_sync_id = worker->profile_count > 0U &&
                                            worker->sync_window_valid != 0U ?
                                            worker->last_client_sync_id : 0U;
    const manager_profile_entry_t *target = _manager_target_profile(worker);
    /* A sweep moves `target` across saved entries, but the reported SSID stays
     * on the selected profile: that is what Device Link's `profile_ssid` means,
     * and letting the cursor drive it would make a failed sweep look like the
     * user reselected whatever was tried last. The cursor is reported
     * separately through attempt_index/attempt_total. A connect with fresh,
     * unsaved credentials is not a sweep, so it still names its own SSID. */
    const bool sweeping = worker->attempt_active != 0U && worker->target_valid &&
                          !worker->target_candidate;
    const manager_profile_entry_t *named = sweeping ? active : target;

    if (named != NULL)
    {
        _manager_copy_ssid(worker->status.ssid, named->ssid,
                           named->ssid_length);
    }
    else if (active != NULL)
    {
        _manager_copy_ssid(worker->status.ssid, active->ssid,
                           active->ssid_length);
    }
    else
    {
        worker->status.ssid[0] = '\0';
    }
    if (sweeping && worker->attempt_position > 0U)
    {
        uint8_t matched_count = 0U;
        for (uint8_t index = 0U; index < worker->profile_count; ++index)
        {
            if ((worker->scan_matched_mask & (uint8_t)(1U << index)) != 0U)
            {
                ++matched_count;
            }
        }
        worker->status.attempt_index =
            worker->attempt_order[worker->attempt_position - 1U];
        worker->status.attempt_ordinal = worker->attempt_ordinal;
        worker->status.attempt_total = matched_count;
    }
    else
    {
        worker->status.attempt_index = CONNECTIVITY_MANAGER_NO_ATTEMPT;
        worker->status.attempt_ordinal = 0U;
        worker->status.attempt_total = 0U;
    }
}

static void _manager_publish_command_terminal(
    manager_worker_t *worker, const manager_command_t *command,
    esp_err_t result, connectivity_manager_failure_t failure)
{
    if (command->type == MANAGER_COMMAND_SCAN)
    {
        _manager_publish_scan_terminal(worker, command->operation_id, result);
        return;
    }
    _manager_publish_status_terminal(worker, command->operation_id, result,
                                     failure);
}

static void _manager_clear_active_operation(manager_worker_t *worker)
{
    worker->operation_kind = MANAGER_OPERATION_NONE;
    worker->active_command = MANAGER_COMMAND_INIT;
    worker->operation_id = 0U;
    worker->service_operation_id = 0U;
    worker->active_cancel_requested = false;
    worker->connect_resume_pending = false;
    worker->active_sync = false;
    worker->active_client_sync_id = 0U;
    _manager_secure_zero(worker->active_sync_identity,
                         sizeof(worker->active_sync_identity));
}

static void _manager_complete_status_operation(
    manager_worker_t *worker, esp_err_t result,
    connectivity_manager_failure_t failure)
{
    const connectivity_manager_operation_id_t operation_id =
        worker->operation_id;
    worker->status.last_error = result;
    worker->status.failure = failure;
    _manager_publish_status_terminal(worker, operation_id, result,
                                     failure);
    _manager_clear_active_operation(worker);
    if (operation_id == 0U)
    {
        _manager_cache_status(worker);
    }
}

static unsigned _manager_command_priority(manager_command_type_t type)
{
    switch (type)
    {
    case MANAGER_COMMAND_FORGET:
    case MANAGER_COMMAND_FORGET_NETWORK:
        return 4U;
    case MANAGER_COMMAND_DISCONNECT:
        return 3U;
    case MANAGER_COMMAND_CONNECT:
    case MANAGER_COMMAND_CONNECT_NETWORK:
    case MANAGER_COMMAND_SYNC_PROFILE:
    case MANAGER_COMMAND_SAVE_PROFILE:
    case MANAGER_COMMAND_RECONNECT:
        return 2U;
    case MANAGER_COMMAND_SCAN:
        return 1U;
    default:
        return 0U;
    }
}

static void _manager_run_pending(manager_worker_t *worker);
static void _manager_process_command(manager_worker_t *worker,
                                     const manager_command_t *command,
                                     bool *stop);
static void _manager_continue_policy(manager_worker_t *worker);
static void _manager_cycle_start(manager_worker_t *worker,
                                 connectivity_manager_operation_id_t operation_id,
                                 manager_command_type_t command_type,
                                 uint8_t start_index);
static void _manager_cycle_scan_complete(manager_worker_t *worker,
        const wifi_service_scan_snapshot_t *scan);
static esp_err_t _manager_cycle_request_scan(manager_worker_t *worker);
static void _manager_cycle_scan_failed(manager_worker_t *worker,
                                       esp_err_t result);
static void _manager_attempt_end(manager_worker_t *worker);

static manager_defer_result_t _manager_defer_foreground(
    manager_worker_t *worker, const manager_command_t *command)
{
    if (worker->operation_kind == MANAGER_OPERATION_NONE && !worker->suspended)
    {
        return MANAGER_DEFER_NONE;
    }
    const unsigned command_priority =
        _manager_command_priority(command->type);
    const bool scan_preempts_cycle =
        command->type == MANAGER_COMMAND_SCAN && worker->cycle_active &&
        !worker->pending_command_valid;
    if (!scan_preempts_cycle &&
            (command_priority <
             _manager_command_priority(worker->active_command) ||
             (worker->pending_command_valid && command_priority <
              _manager_command_priority(worker->pending_command.type))))
    {
        _manager_publish_command_terminal(
            worker, command, ESP_ERR_INVALID_STATE,
            CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
        return MANAGER_DEFER_REJECTED;
    }

    if (worker->pending_command_valid)
    {
        _manager_publish_command_terminal(
            worker, &worker->pending_command, ESP_ERR_NOT_FINISHED,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        _manager_secure_zero(&worker->pending_command,
                             sizeof(worker->pending_command));
        worker->pending_command_valid = false;
    }

    worker->pending_command = *command;
    worker->pending_command_valid = true;
    if (scan_preempts_cycle &&
            worker->operation_kind == MANAGER_OPERATION_CONNECT)
    {
        /* Do not abort a connect that may already be passing association or
         * DHCP. The scan runs immediately after its terminal. */
        return MANAGER_DEFER_QUEUED;
    }
    if (command->type == MANAGER_COMMAND_DISCONNECT ||
            command->type == MANAGER_COMMAND_FORGET)
    {
        worker->resume_cycle_after_scan = false;
        worker->cycle_active = false;
        worker->cycle_scan_pending = false;
        if (!worker->target_candidate)
        {
            worker->retry_pending = false;
        }
    }
    if (command->type == MANAGER_COMMAND_SCAN && worker->cycle_active)
    {
        worker->resume_cycle_after_scan = true;
        worker->cycle_active = false;
        worker->cycle_scan_pending = false;
        worker->retry_pending = false;
        _manager_attempt_end(worker);
    }
    if (worker->operation_kind == MANAGER_OPERATION_NONE)
    {
        return MANAGER_DEFER_QUEUED;
    }
    if (scan_preempts_cycle)
    {
        worker->resume_cycle_after_scan = true;
    }
    if (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
            (worker->connect_resume_pending ||
             (worker->retry_pending && worker->service_operation_id == 0U)))
    {
        worker->retry_pending = false;
        worker->status.state = worker->suspended ? CONNECTIVITY_MANAGER_STATE_SUSPENDED :
                               CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.ipv4_address = 0U;
        _manager_clear_candidate(worker);
        _manager_set_target_status(worker);
        _manager_complete_status_operation(
            worker, ESP_ERR_NOT_FINISHED,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        return MANAGER_DEFER_QUEUED;
    }
    if (worker->active_cancel_requested)
    {
        return MANAGER_DEFER_QUEUED;
    }
    wifi_service_cancel_disposition_t disposition =
        WIFI_SERVICE_CANCEL_FAILURE;
    const esp_err_t result = wifi_service_cancel(
                                 worker->session_id,
                                 worker->service_operation_id,
                                 &disposition);
    if (result != ESP_OK || disposition == WIFI_SERVICE_CANCEL_NOT_FOUND ||
            disposition == WIFI_SERVICE_CANCEL_FAILURE)
    {
        const esp_err_t terminal_result = result == ESP_OK ?
                                          ESP_ERR_NOT_FOUND : result;

        _manager_publish_command_terminal(
            worker, &worker->pending_command, terminal_result,
            CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
        _manager_secure_zero(&worker->pending_command,
                             sizeof(worker->pending_command));
        worker->pending_command_valid = false;
        return MANAGER_DEFER_REJECTED;
    }
    worker->active_cancel_requested =
        disposition == WIFI_SERVICE_CANCEL_ACCEPTED;
    return MANAGER_DEFER_QUEUED;
}


/**
 * @brief Report whether the worker still has an untried saved candidate.
 *
 * The cycle keeps `attempt_active` set across per-network failures; only the
 * last matched candidate ends the cycle.
 */
static bool _manager_attempt_has_next(const manager_worker_t *worker)
{
    return worker->attempt_active &&
           worker->attempt_position < worker->profile_count;
}

static void _manager_attempt_end(manager_worker_t *worker);

/** @brief Load the next matched saved candidate of the running cycle. */
static bool _manager_attempt_next(manager_worker_t *worker,
                                  manager_profile_entry_t *entry)
{
    while (_manager_attempt_has_next(worker))
    {
        const uint8_t index = worker->attempt_order[worker->attempt_position];

        ++worker->attempt_position;
        if (index >= worker->profile_count ||
                (worker->scan_matched_mask & (uint8_t)(1U << index)) == 0U)
        {
            continue;
        }
        ++worker->attempt_ordinal;
        *entry = worker->profiles[index];
        /* Only `target` follows the cycle. `active_index` keeps the user's
         * choice, so an exhausted cycle cannot silently reselect the last
         * network tried. */
        return true;
    }
    _manager_attempt_end(worker);
    return false;
}

/** @brief Start a cycle over the saved networks present in the scan. */
static void _manager_attempt_begin(manager_worker_t *worker,
                                   uint8_t start_index)
{
    worker->attempt_active = worker->profile_count > 0U;
    worker->attempt_position = 0U;
    worker->attempt_ordinal = 0U;
    if (!worker->attempt_active)
    {
        return;
    }
    if (start_index < worker->profile_count)
    {
        _manager_touch_profile(worker, start_index);
    }
}

static void _manager_attempt_end(manager_worker_t *worker)
{
    worker->attempt_active = false;
    worker->attempt_position = 0U;
    worker->attempt_ordinal = 0U;
}

/**
 * @brief Report whether one attempt failure should advance to the next saved
 *        network instead of ending the whole operation.
 *
 * Authentication is excluded on purpose: a wrong password must surface to the
 * caller instead of silently connecting somewhere else. A resource-level
 * failure ends the sweep because the radio, not the candidate, is at fault.
 */

static bool _manager_failure_retryable(
    connectivity_manager_failure_t failure)
{
    return failure == CONNECTIVITY_MANAGER_FAILURE_AP_NOT_FOUND ||
           failure == CONNECTIVITY_MANAGER_FAILURE_ASSOCIATION_TIMEOUT ||
           failure == CONNECTIVITY_MANAGER_FAILURE_DHCP_TIMEOUT ||
           failure == CONNECTIVITY_MANAGER_FAILURE_LINK_LOST;
}

static esp_err_t _manager_open_session(manager_worker_t *worker)
{
    esp_err_t result = wifi_service_session_open(&worker->session_id);
    if (result == ESP_OK)
    {
        worker->service_operation_id = 0U;
        worker->service_status_generation = 0U;
        worker->service_scan_generation = 0U;
    }
    return result;
}

static esp_err_t _manager_radio_stop(manager_worker_t *worker,
                                     uint32_t timeout_ms)
{
    if (!worker->radio_initialized)
    {
        worker->status.radio_available = false;
        return ESP_OK;
    }

    const esp_err_t suspend_result = wifi_service_suspend(timeout_ms);
    wifi_service_status_snapshot_t radio;
    memset(&radio, 0, sizeof(radio));
    const bool have_radio = wifi_service_get_status(&radio) == ESP_OK;
    const bool stopped = have_radio &&
                         (radio.state == WIFI_SERVICE_STATE_SUSPENDED ||
                          radio.state == WIFI_SERVICE_STATE_OFFLINE);
    worker->status.radio_available = stopped && radio.available;
    if (!stopped)
    {
        worker->status.state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
        return suspend_result != ESP_OK ? suspend_result :
               ESP_ERR_INVALID_STATE;
    }
    return suspend_result;
}

static esp_err_t _manager_radio_start(manager_worker_t *worker)
{
    if (worker->radio_initialized)
    {
        wifi_service_status_snapshot_t status;

        if (wifi_service_get_status(&status) == ESP_OK)
        {
            if (status.available && status.state == WIFI_SERVICE_STATE_SUSPENDED)
            {
                esp_err_t result = wifi_service_resume(
                                       CONNECTIVITY_MANAGER_RADIO_WAIT_MS);

                if (result != ESP_OK)
                {
                    worker->status.radio_available = false;
                    return result;
                }
                worker->status.radio_available = true;
                return worker->session_id != 0U ? ESP_OK :
                       _manager_open_session(worker);
            }
            if (status.available && status.state != WIFI_SERVICE_STATE_OFFLINE)
            {
                worker->status.radio_available = true;
                return worker->session_id != 0U ? ESP_OK :
                       _manager_open_session(worker);
            }
        }
        if (wifi_service_is_cleanup_pending())
        {
            if (worker->session_id != 0U)
            {
                (void)wifi_service_session_close(worker->session_id);
                worker->session_id = 0U;
            }
            esp_err_t cleanup = wifi_service_deinit(
                                    CONNECTIVITY_MANAGER_RADIO_WAIT_MS);
            if (cleanup != ESP_OK)
            {
                worker->status.radio_available = false;
                return cleanup;
            }
            worker->radio_initialized = false;
        }
    }

    const wifi_service_config_t config =
    {
        .task_priority = s_manager.config.wifi_task_priority,
    };
    esp_err_t result = wifi_service_init_with_timeout(
                           &config, CONNECTIVITY_MANAGER_RADIO_INIT_WAIT_MS);
    if (result == ESP_OK)
    {
        worker->radio_initialized = true;
        result = _manager_open_session(worker);
    }
    worker->status.radio_available = result == ESP_OK;
    return result;
}

static void _manager_cycle_schedule_attempt(manager_worker_t *worker,
        uint32_t delay_ms)
{
    if (!worker->cycle_active || worker->suspended)
    {
        worker->retry_pending = false;
        return;
    }
    TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
    if (delay_ticks == 0U)
    {
        delay_ticks = 1U;
    }
    const TickType_t now = xTaskGetTickCount();
    if (worker->cycle_active)
    {
        const TickType_t remaining = worker->cycle_retry_deadline - now;
        if ((int32_t)remaining < (int32_t)delay_ticks)
        {
            delay_ticks = remaining > 0U ? remaining : 1U;
        }
    }
    worker->retry_deadline = now + delay_ticks;
    worker->retry_pending = true;
    worker->status.state = CONNECTIVITY_MANAGER_STATE_RETRY_WAIT;
    worker->status.retry_delay_ms = (uint32_t)(delay_ticks * portTICK_PERIOD_MS);
    worker->status.retry_count = worker->cycle_attempt_count;
    _manager_cache_status(worker);
}

static void _manager_schedule_retry(manager_worker_t *worker)
{
    _manager_cycle_schedule_attempt(worker,
                                    CONNECTIVITY_MANAGER_ATTEMPT_RETRY_DELAY_MS);
}

static esp_err_t _manager_start_connect(manager_worker_t *worker,
                                        connectivity_manager_operation_id_t operation_id,
                                        manager_command_type_t command_type)
{
    worker->operation_kind = MANAGER_OPERATION_CONNECT;
    worker->active_command = command_type;
    worker->operation_id = operation_id;
    worker->service_operation_id = 0U;
    worker->active_cancel_requested = false;
    if (!worker->target_valid || !worker->target_reconnectable ||
            (!worker->target_candidate && worker->profile_count == 0U))
    {
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_INTERNAL;
        worker->status.last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = _manager_radio_start(worker);
    if (result != ESP_OK)
    {
        worker->status.radio_available = false;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
        worker->status.last_error = result;
        return result;
    }
    const manager_profile_entry_t *target = _manager_target_profile(worker);
    const wifi_service_credentials_t credentials =
    {
        .ssid = (const char *)target->ssid,
        .ssid_length = target->ssid_length,
        .password = (const char *)target->password,
        .password_length = target->password_length,
        .security = target->security ==
        CONNECTIVITY_MANAGER_SECURITY_PERSONAL ?
        WIFI_SERVICE_SECURITY_PERSONAL :
        WIFI_SERVICE_SECURITY_OPEN,
    };
    /* The policy layer owns candidate ordering and attempt counting, so every
     * cycle attempt is exactly one association/DHCP phase in the executor. */
    const wifi_service_connect_options_t attempt_budget =
    {
        .retry_limit = CONNECTIVITY_MANAGER_ATTEMPT_BUDGET_RETRY_LIMIT,
        .association_timeout_ms =
        CONNECTIVITY_MANAGER_ATTEMPT_BUDGET_ASSOCIATION_TIMEOUT_MS,
        .dhcp_timeout_ms = CONNECTIVITY_MANAGER_ATTEMPT_BUDGET_DHCP_TIMEOUT_MS,
    };

    result = wifi_service_request_connect_with_options(worker->session_id,
        &credentials, &attempt_budget, &worker->service_operation_id);
    if (result == ESP_OK)
    {
        worker->retry_pending = false;
        worker->status.operation_id = operation_id;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_CONNECTING;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        worker->status.last_error = ESP_OK;
        worker->status.retry_delay_ms = 0U;
        _manager_set_target_status(worker);
        _manager_cache_status(worker);
    }
    else
    {
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_INTERNAL;
        worker->status.last_error = result;
    }
    return result;
}

static void _manager_handle_connected(manager_worker_t *worker,
                                      const wifi_service_status_snapshot_t *status)
{
    worker->retry_pending = false;
    worker->retry_count = 0U;
    worker->status.retry_count = 0U;
    worker->status.retry_delay_ms = 0U;
    worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    worker->status.last_error = ESP_OK;
    if (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
            worker->active_cancel_requested)
    {
        _manager_attempt_end(worker);
        _manager_clear_candidate(worker);
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.ipv4_address = 0U;
        _manager_set_target_status(worker);
        _manager_complete_status_operation(
            worker, ESP_ERR_NOT_FINISHED,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        return;
    }
    /* A successful link always makes its saved entry the selected profile. */
    _manager_attempt_end(worker);
    worker->cycle_active = false;
    worker->cycle_scan_pending = false;
    const uint8_t connected_index = _manager_find_profile(
                                        worker, worker->target.ssid,
                                        worker->target.ssid_length);

    if (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
            worker->target_candidate)
    {
        manager_profile_entry_t saved = worker->target;
        const uint64_t sync_id = worker->active_sync ?
                                 worker->active_client_sync_id : 0U;
        const uint8_t sync_valid = worker->active_sync ? 1U : 0U;
        const uint8_t previous_count = worker->profile_count;
        const uint8_t previous_active = worker->active_index;
        const uint64_t previous_revision = worker->profile_revision;
        const uint64_t previous_sync_id = worker->last_client_sync_id;
        const uint8_t previous_sync_valid = worker->sync_window_valid;
        uint8_t previous_identity[CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES];
        uint8_t previous_order[CONNECTIVITY_MANAGER_PERSISTED_PROFILES];
        /* The replaced entry is the only one a failed write may need to undo. */
        manager_profile_entry_t replaced;
        uint8_t replaced_index = CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE;
        uint8_t identity[CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES];
        esp_err_t result = ESP_OK;
        connectivity_manager_failure_t persistence_failure =
            CONNECTIVITY_MANAGER_FAILURE_STORAGE;

        memset(identity, 0, sizeof(identity));
        memset(previous_identity, 0, sizeof(previous_identity));
        memset(&replaced, 0, sizeof(replaced));
        memcpy(previous_identity, worker->last_sync_identity,
               sizeof(previous_identity));
        memcpy(previous_order, worker->attempt_order, sizeof(previous_order));
        if (worker->active_sync)
        {
            memcpy(identity, worker->active_sync_identity, sizeof(identity));
        }
        if (worker->profile_revision == UINT64_MAX)
        {
            result = ESP_ERR_INVALID_STATE;
            persistence_failure = CONNECTIVITY_MANAGER_FAILURE_INTERNAL;
        }
        if (result == ESP_OK)
        {
            const uint8_t index = _manager_find_profile(
                                      worker, saved.ssid, saved.ssid_length);

            if (index != CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE)
            {
                replaced = worker->profiles[index];
                replaced_index = index;
                worker->profiles[index] = saved;
                worker->active_index = index;
            }
            else if (worker->profile_count < CONNECTIVITY_MANAGER_MAX_PROFILES)
            {
                worker->profiles[worker->profile_count] = saved;
                worker->active_index = worker->profile_count;
                ++worker->profile_count;
                _manager_rebuild_order(worker);
            }
            else
            {
                result = ESP_ERR_NO_MEM;
                persistence_failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
            }
        }
        if (result == ESP_OK)
        {
            worker->last_client_sync_id = sync_id;
            worker->sync_window_valid = sync_valid;
            memcpy(worker->last_sync_identity, identity,
                   sizeof(worker->last_sync_identity));
            ++worker->profile_revision;
            _manager_touch_profile(worker, worker->active_index);
            result = _manager_profile_store(worker);
            if (result == ESP_ERR_INVALID_ARG)
            {
                persistence_failure = CONNECTIVITY_MANAGER_FAILURE_INTERNAL;
            }
        }
        if (result == ESP_OK)
        {
            _manager_set_saved_target(worker);
            _manager_publish_profiles(worker);
        }
        else
        {
            /* A refused write leaves the durable list untouched, so the RAM
             * list must not advertise the new entry either. The link itself is
             * preserved through the ephemeral target below. */
            worker->profile_count = previous_count;
            worker->active_index = previous_active;
            worker->profile_revision = previous_revision;
            worker->last_client_sync_id = previous_sync_id;
            worker->sync_window_valid = previous_sync_valid;
            memcpy(worker->last_sync_identity, previous_identity,
                   sizeof(worker->last_sync_identity));
            memcpy(worker->attempt_order, previous_order,
                   sizeof(worker->attempt_order));
            if (replaced_index != CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE)
            {
                worker->profiles[replaced_index] = replaced;
            }
            else if (previous_count < CONNECTIVITY_MANAGER_MAX_PROFILES)
            {
                _manager_secure_zero(&worker->profiles[previous_count],
                                     sizeof(worker->profiles[previous_count]));
            }
            worker->status.failure = persistence_failure;
            worker->status.last_error = result;
            _manager_set_ephemeral_link(worker, &saved);
        }
        _manager_secure_zero(&replaced, sizeof(replaced));
        _manager_secure_zero(previous_order, sizeof(previous_order));
        _manager_secure_zero(previous_identity, sizeof(previous_identity));
        _manager_secure_zero(identity, sizeof(identity));
        _manager_secure_zero(&saved, sizeof(saved));
    }
    else if (connected_index != CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE)
    {
        worker->active_index = connected_index;
        _manager_touch_profile(worker, connected_index);
        _manager_set_saved_target(worker);
        _manager_publish_profiles(worker);
    }
    worker->status.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    worker->status.ipv4_address = status->ipv4_address;
    _manager_set_target_status(worker);
    if (worker->operation_kind == MANAGER_OPERATION_CONNECT)
    {
        _manager_complete_status_operation(
            worker, worker->status.last_error, worker->status.failure);
    }
}

static esp_err_t _manager_store_wifi_policy(manager_worker_t *worker)
{
    if (worker->profile_count == 0U)
    {
        return ESP_OK;
    }
    return _manager_profile_store(worker);
}

static esp_err_t _manager_erase_all_profiles(manager_worker_t *worker)
{
    esp_err_t result = nv_storage_erase_key(
                           CONNECTIVITY_MANAGER_PROFILE_KEY);
    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        result = ESP_OK;
    }
    if (result != ESP_OK)
    {
        return result;
    }
    _manager_worker_clear_profiles(worker);
    _manager_clear_target(worker);
    worker->wifi_enabled = 0U;
    worker->status.manual_hold = true;
    worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    worker->status.ipv4_address = 0U;
    _manager_set_target_status(worker);
    _manager_publish_profiles(worker);
    return ESP_OK;
}

static void _manager_cycle_finish_failure(manager_worker_t *worker,
        esp_err_t result, connectivity_manager_failure_t failure)
{
    worker->retry_pending = false;
    worker->cycle_active = false;
    worker->cycle_scan_pending = false;
    worker->cycle_scan_retry_pending = false;
    worker->resume_cycle_after_scan = false;
    _manager_attempt_end(worker);
    if (worker->profile_count > 0U && !worker->target_candidate)
    {
        _manager_set_saved_target(worker);
    }
    worker->wifi_enabled = 0U;
    const esp_err_t stop_result =
        _manager_radio_stop(worker, CONNECTIVITY_MANAGER_RADIO_STOP_WAIT_MS);
    const esp_err_t policy_result = _manager_store_wifi_policy(worker);
    worker->policy_dirty = policy_result != ESP_OK;
    if (stop_result != ESP_OK)
    {
        result = stop_result;
        failure = CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
    }
    else if (policy_result != ESP_OK)
    {
        result = policy_result;
        failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
    }
    worker->status.manual_hold = true;
    worker->status.state = stop_result == ESP_OK ?
                           CONNECTIVITY_MANAGER_STATE_IDLE :
                           CONNECTIVITY_MANAGER_STATE_OFFLINE;
    worker->status.ipv4_address = 0U;
    worker->status.failure = failure;
    worker->status.last_error = result;
    worker->status.retry_count = worker->cycle_attempt_count;
    _manager_set_target_status(worker);
    _manager_complete_status_operation(worker, result, failure);
}

static void _manager_pause_cycle_for_scan(manager_worker_t *worker)
{
    worker->retry_pending = false;
    worker->cycle_active = false;
    worker->cycle_scan_pending = false;
    worker->resume_cycle_after_scan = true;
    _manager_attempt_end(worker);
    worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    worker->status.ipv4_address = 0U;
    _manager_set_saved_target(worker);
    /* The cycle operation stays pending under cycle_operation_id. The scan
     * command owns the operation slot while it runs. */
    _manager_clear_active_operation(worker);
    _manager_set_target_status(worker);
    _manager_cache_status(worker);
}

static void _manager_handle_connect_terminal(manager_worker_t *worker,
        const wifi_service_status_snapshot_t *status)
{
    const bool succeeded = status->operation_result == ESP_OK &&
                           status->operation_failure == WIFI_SERVICE_FAILURE_NONE;
    if (succeeded && status->state == WIFI_SERVICE_STATE_IP_READY &&
            status->ipv4_address != 0U)
    {
        _manager_handle_connected(worker, status);
        return;
    }
    const bool canceled = worker->active_cancel_requested &&
                          status->operation_canceled;
    const connectivity_manager_failure_t failure = canceled ?
        CONNECTIVITY_MANAGER_FAILURE_NONE : succeeded ?
        CONNECTIVITY_MANAGER_FAILURE_INTERNAL :
        _manager_map_failure(status->operation_failure);
    const esp_err_t result = canceled ? ESP_ERR_NOT_FINISHED :
                             succeeded ? ESP_ERR_INVALID_STATE :
                             status->operation_result;
    worker->status.state = status->state == WIFI_SERVICE_STATE_OFFLINE ?
                           CONNECTIVITY_MANAGER_STATE_OFFLINE :
                           CONNECTIVITY_MANAGER_STATE_IDLE;
    worker->status.ipv4_address = 0U;
    worker->service_operation_id = 0U;
    if (!worker->cycle_active)
    {
        _manager_complete_status_operation(worker, result, failure);
        return;
    }
    if (canceled)
    {
        _manager_cycle_finish_failure(worker, result,
                                      CONNECTIVITY_MANAGER_FAILURE_NONE);
        return;
    }
    if (failure == CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE)
    {
        _manager_cycle_finish_failure(worker, result, failure);
        return;
    }
    if (worker->pending_command_valid &&
            worker->pending_command.type == MANAGER_COMMAND_SCAN)
    {
        _manager_pause_cycle_for_scan(worker);
        return;
    }
    if (failure == CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION)
    {
        worker->cycle_attempt_count = CONNECTIVITY_MANAGER_MAX_CONNECT_FAILURES;
    }
    else if (worker->cycle_attempt_count < UINT8_MAX)
    {
        ++worker->cycle_attempt_count;
    }
    worker->status.failure = failure;
    worker->status.last_error = result;
    worker->status.retry_count = worker->cycle_attempt_count;
    if (worker->cycle_attempt_count <
            CONNECTIVITY_MANAGER_MAX_CONNECT_FAILURES &&
            _manager_failure_retryable(failure))
    {
        _manager_set_target_status(worker);
        _manager_schedule_retry(worker);
        return;
    }
    manager_profile_entry_t next;
    memset(&next, 0, sizeof(next));
    if (_manager_attempt_next(worker, &next))
    {
        worker->target = next;
        worker->target_valid = true;
        worker->target_candidate = false;
        worker->target_persisted = true;
        worker->target_reconnectable = true;
        _manager_secure_zero(&next, sizeof(next));
        worker->cycle_attempt_count = 0U;
        worker->status.retry_count = 0U;
        worker->retry_pending = false;
        if (_manager_start_connect(worker, worker->operation_id,
                                   worker->active_command) == ESP_OK)
        {
            return;
        }
    }
    _manager_cycle_finish_failure(worker, result, failure);
}

static void _manager_poll_status(manager_worker_t *worker)
{
    if (!worker->radio_initialized)
    {
        return;
    }
    wifi_service_status_snapshot_t status;
    if (wifi_service_get_status(&status) != ESP_OK ||
            status.generation == worker->service_status_generation)
    {
        return;
    }
    worker->service_status_generation = status.generation;
    worker->status.radio_available = status.available;
    /* A policy-off manager deliberately leaves the executor in SUSPENDED
     * state. That is a physical lifecycle fact, not a system-standby request,
     * so keep the user-facing policy state IDLE. */
    if (!(worker->status.manual_hold &&
            status.state == WIFI_SERVICE_STATE_SUSPENDED))
    {
        worker->status.state = _manager_map_state(status.state);
    }
    const bool terminal_pending = worker->status.operation_complete &&
                                  worker->status.operation_id != 0U;
    if (!terminal_pending)
    {
        worker->status.failure = _manager_map_failure(status.failure);
        worker->status.last_error = status.last_error;
    }
    worker->status.ipv4_address = status.ipv4_address;
    if (status.state != WIFI_SERVICE_STATE_IP_READY &&
            status.failure == WIFI_SERVICE_FAILURE_LINK_LOST)
    {
        _manager_clear_ephemeral_link(worker);
    }
    const bool executor_retry_allowed =
        worker->cycle_active && worker->wifi_enabled != 0U;
    if (worker->operation_kind == MANAGER_OPERATION_NONE &&
            (!executor_retry_allowed || worker->status.manual_hold) &&
            (status.state == WIFI_SERVICE_STATE_CONNECTING ||
             status.state == WIFI_SERVICE_STATE_WAITING_IP ||
             status.state == WIFI_SERVICE_STATE_RETRY_WAIT))
    {
        wifi_service_operation_id_t operation_id = 0U;
        if (wifi_service_request_disconnect(worker->session_id,
                                            &operation_id) == ESP_OK)
        {
            worker->operation_kind = MANAGER_OPERATION_DISCONNECT;
            worker->active_command = MANAGER_COMMAND_AUTO;
            worker->operation_id = 0U;
            worker->service_operation_id = operation_id;
            worker->active_cancel_requested = false;
        }
    }
    const bool matching_terminal = worker->service_operation_id != 0U &&
                                   status.session_id == worker->session_id &&
                                   status.operation_id == worker->service_operation_id &&
                                   status.operation_complete;
    if (status.state == WIFI_SERVICE_STATE_IP_READY &&
            worker->operation_kind == MANAGER_OPERATION_NONE)
    {
        _manager_handle_connected(worker, &status);
    }
    else if (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
             matching_terminal)
    {
        _manager_handle_connect_terminal(worker, &status);
        return;
    }
    else if (worker->operation_kind == MANAGER_OPERATION_DISCONNECT &&
             matching_terminal)
    {
        const bool canceled = worker->active_cancel_requested &&
                              status.operation_canceled;
        esp_err_t result = canceled ? ESP_ERR_NOT_FINISHED :
                           status.operation_result;
        connectivity_manager_failure_t failure = canceled ?
            CONNECTIVITY_MANAGER_FAILURE_NONE :
            _manager_map_failure(status.operation_failure);

        if (result == ESP_OK &&
                worker->active_command == MANAGER_COMMAND_FORGET)
        {
            result = nv_storage_erase_key(CONNECTIVITY_MANAGER_PROFILE_KEY);
            if (result == ESP_ERR_NVS_NOT_FOUND)
            {
                result = ESP_OK;
            }
            if (result == ESP_OK)
            {
                _manager_attempt_end(worker);
                worker->pending_removal_valid = false;
                _manager_worker_clear_profiles(worker);
                _manager_clear_target(worker);
                _manager_publish_profiles(worker);
            }
            else
            {
                failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
            }
        }
        if (worker->pending_removal_valid)
        {
            const connectivity_manager_operation_id_t removal_id =
                worker->operation_id;

            _manager_clear_active_operation(worker);
            if (result == ESP_OK && !canceled)
            {
                _manager_apply_pending_removal(worker, removal_id);
            }
            else
            {
                /* A canceled or failed disconnect keeps the entry and reports
                 * the disconnect outcome instead of a silent removal. */
                worker->pending_removal_valid = false;
                worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
                worker->status.ipv4_address = 0U;
                _manager_set_target_status(worker);
                _manager_publish_status_terminal(worker, removal_id, result,
                                                 failure);
            }
            return;
        }
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.ipv4_address = 0U;
        _manager_set_target_status(worker);
        _manager_complete_status_operation(worker, result, failure);
        return;
    }
    else if (worker->operation_kind == MANAGER_OPERATION_NONE &&
             status.state == WIFI_SERVICE_STATE_IDLE &&
             worker->wifi_enabled != 0U && !worker->cycle_active &&
             !worker->status.manual_hold &&
             status.failure != WIFI_SERVICE_FAILURE_NONE)
    {
        _manager_cycle_start(worker, 0U, MANAGER_COMMAND_AUTO, worker->active_index);
    }
    else if (worker->operation_kind == MANAGER_OPERATION_SCAN &&
             status.state == WIFI_SERVICE_STATE_IDLE &&
             status.failure != WIFI_SERVICE_FAILURE_NONE &&
             worker->wifi_enabled != 0U)
    {
        worker->resume_cycle_after_scan = true;
    }
    _manager_set_target_status(worker);
    _manager_cache_status(worker);
}

static void _manager_poll_scan(manager_worker_t *worker)
{
    if (!worker->radio_initialized)
    {
        return;
    }
    wifi_service_scan_snapshot_t scan;
    if (wifi_service_get_scan_snapshot(&scan) != ESP_OK ||
            scan.generation == worker->service_scan_generation)
    {
        return;
    }
    worker->service_scan_generation = scan.generation;
    if (worker->operation_kind == MANAGER_OPERATION_SCAN &&
            scan.operation_id != worker->service_operation_id)
    {
        return;
    }
    worker->scan.operation_id = worker->operation_id;
    worker->scan.last_error = scan.operation_canceled ?
                              ESP_ERR_NOT_FINISHED : scan.last_error;
    worker->scan.running = scan.state == WIFI_SERVICE_SCAN_RUNNING;
    worker->scan.matched_mask = scan.matched_mask;
    worker->scan.truncated = scan.truncated;
    worker->scan.record_count = scan.record_count <
                                CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS ?
                                scan.record_count :
                                CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS;
    memset(worker->scan.records, 0, sizeof(worker->scan.records));
    for (size_t index = 0U; index < worker->scan.record_count; ++index)
    {
        connectivity_manager_scan_record_t *record =
            &worker->scan.records[index];
        memcpy(record->ssid, scan.records[index].ssid,
               sizeof(record->ssid));
        record->rssi = scan.records[index].rssi;
        record->channel = scan.records[index].channel;
        record->security = _manager_map_security(scan.records[index].security);
        /* Any saved entry marks the network, not only the selected one. */
        record->saved = _manager_scan_record_saved(worker, record->ssid,
            record->security);
    }
    if (worker->operation_kind == MANAGER_OPERATION_SCAN &&
            scan.operation_id == worker->service_operation_id &&
            scan.state != WIFI_SERVICE_SCAN_RUNNING)
    {
        if (worker->cycle_scan_pending)
        {
            _manager_cycle_scan_complete(worker, &scan);
            return;
        }
        bool status_terminal_published = false;
        if (!worker->resume_cycle_after_scan &&
                worker->cycle_operation_id != 0U &&
                worker->operation_id == worker->cycle_operation_id)
        {
            /* A pending Disable/Forget detached the cycle before its scan
             * terminal arrived. Complete the original enable operation now;
             * the pending command publishes its own terminal afterwards. */
            _manager_publish_status_terminal(
                worker, worker->operation_id, ESP_ERR_NOT_FINISHED,
                CONNECTIVITY_MANAGER_FAILURE_NONE);
            status_terminal_published = true;
        }
        _manager_clear_active_operation(worker);
        if (!worker->wifi_enabled && worker->radio_initialized)
        {
            const esp_err_t stop_result = _manager_radio_stop(
                                              worker,
                                              CONNECTIVITY_MANAGER_RADIO_STOP_WAIT_MS);
            worker->status.manual_hold = true;
            worker->status.state = stop_result == ESP_OK ?
                                   CONNECTIVITY_MANAGER_STATE_IDLE :
                                   CONNECTIVITY_MANAGER_STATE_OFFLINE;
        }
        if (!status_terminal_published)
        {
            _manager_cache_status(worker);
        }
        _manager_cache_scan(worker);
        return;
    }
    _manager_cache_scan(worker);
}

static esp_err_t _manager_cycle_request_scan(manager_worker_t *worker)
{
    worker->cycle_scan_retry_pending = false;
    worker->cycle_scan_pending = true;
    worker->operation_kind = MANAGER_OPERATION_SCAN;
    worker->status.operation_id = worker->cycle_operation_id;
    worker->status.operation_complete = false;
    worker->status.state = CONNECTIVITY_MANAGER_STATE_SCANNING;
    worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    worker->status.last_error = ESP_OK;
    _manager_set_target_status(worker);
    _manager_cache_status(worker);

    if (worker->profile_count == 0U)
    {
        return ESP_ERR_NOT_FOUND;
    }
    wifi_service_scan_target_t targets[CONNECTIVITY_MANAGER_MAX_PROFILES];
    memset(targets, 0, sizeof(targets));
    for (uint8_t index = 0U; index < worker->profile_count; ++index)
    {
        targets[index].ssid_length = worker->profiles[index].ssid_length;
        memcpy(targets[index].ssid, worker->profiles[index].ssid,
               worker->profiles[index].ssid_length);
        targets[index].security =
            (wifi_service_security_t)worker->profiles[index].security;
    }
    const esp_err_t result = wifi_service_request_scan_targets(
                                 worker->session_id, targets,
                                 worker->profile_count,
                                 &worker->service_operation_id);
    wifi_service_secure_zero(targets, sizeof(targets));
    return result;
}

static void _manager_cycle_scan_failed(manager_worker_t *worker,
                                       esp_err_t result)
{
    worker->cycle_scan_pending = false;
    worker->service_operation_id = 0U;
    worker->operation_kind = MANAGER_OPERATION_NONE;
    if (worker->cycle_scan_attempts < CONNECTIVITY_MANAGER_SCAN_ATTEMPTS)
    {
        TickType_t delay = pdMS_TO_TICKS(
                               CONNECTIVITY_MANAGER_SCAN_RETRY_DELAY_MS);
        if (delay == 0U)
        {
            delay = 1U;
        }
        worker->cycle_scan_retry_pending = true;
        worker->cycle_scan_retry_deadline = xTaskGetTickCount() + delay;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_SCANNING;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
        worker->status.last_error = result;
        _manager_set_target_status(worker);
        _manager_cache_status(worker);
        return;
    }
    _manager_cycle_finish_failure(
        worker, result, CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
}

static void _manager_cycle_start(manager_worker_t *worker,
                                 connectivity_manager_operation_id_t operation_id,
                                 manager_command_type_t command_type,
                                 uint8_t start_index)
{
    /* Install the operation before any fallible policy or radio work. A
     * storage failure below must be able to publish this operation's terminal. */
    worker->operation_kind = MANAGER_OPERATION_SCAN;
    worker->active_command = command_type;
    worker->operation_id = operation_id;
    worker->service_operation_id = 0U;
    worker->active_cancel_requested = false;
    worker->status.operation_id = operation_id;
    worker->status.operation_complete = false;
    worker->status.operation_canceled = false;
    worker->status.operation_conflict = false;
    worker->cycle_operation_id = operation_id;
    worker->cycle_start_index = start_index;
    if (worker->wifi_enabled == 0U)
    {
        worker->wifi_enabled = 1U;
        const esp_err_t store_result = _manager_store_wifi_policy(worker);
        if (store_result != ESP_OK)
        {
            worker->wifi_enabled = 0U;
            worker->policy_dirty = false;
            _manager_cycle_finish_failure(
                worker, store_result,
                CONNECTIVITY_MANAGER_FAILURE_STORAGE);
            return;
        }
    }
    worker->status.manual_hold = false;
    worker->cycle_active = true;
    worker->cycle_scan_pending = true;
    worker->resume_cycle_after_scan = false;
    TickType_t cycle_ticks = pdMS_TO_TICKS(CONNECTIVITY_MANAGER_CYCLE_MAX_MS);
    if (cycle_ticks == 0U)
    {
        cycle_ticks = 1U;
    }
    worker->cycle_retry_deadline = xTaskGetTickCount() + cycle_ticks;
    worker->cycle_attempt_count = 0U;
    worker->cycle_scan_attempts = 0U;
    worker->cycle_scan_retry_pending = false;
    worker->scan_matched_mask = 0U;
    worker->retry_pending = false;
    worker->connect_resume_pending = false;
    _manager_attempt_end(worker);
    /* A saved network cycle always reports the selected profile. The candidate
     * cursor is tracked separately through attempt_index/ordinal. */
    _manager_set_saved_target(worker);
    worker->status.state = CONNECTIVITY_MANAGER_STATE_SCANNING;
    worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    worker->status.last_error = ESP_OK;
    worker->status.ipv4_address = 0U;
    worker->status.retry_count = 0U;
    _manager_set_target_status(worker);
    _manager_cache_status(worker);

    if (worker->profile_count == 0U)
    {
        _manager_cycle_finish_failure(worker, ESP_ERR_NOT_FOUND,
                                      CONNECTIVITY_MANAGER_FAILURE_NONE);
        return;
    }
    esp_err_t result = _manager_radio_start(worker);
    if (result != ESP_OK)
    {
        _manager_cycle_finish_failure(
            worker, result, CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
        return;
    }
    ++worker->cycle_scan_attempts;
    result = _manager_cycle_request_scan(worker);
    if (result != ESP_OK)
    {
        _manager_cycle_scan_failed(worker, result);
    }
}

static void _manager_cycle_scan_complete(manager_worker_t *worker,
        const wifi_service_scan_snapshot_t *scan)
{
    worker->cycle_scan_pending = false;
    worker->service_operation_id = 0U;
    worker->scan_matched_mask = scan->matched_mask;
    worker->operation_kind = MANAGER_OPERATION_NONE;
    worker->scan.operation_id = worker->operation_id;
    worker->scan.last_error = scan->operation_canceled ?
                              ESP_ERR_NOT_FINISHED : scan->last_error;
    worker->scan.running = false;
    worker->scan.truncated = scan->truncated;
    worker->scan.matched_mask = scan->matched_mask;
    worker->scan.record_count = scan->record_count <
                                CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS ?
                                scan->record_count :
                                CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS;
    memset(worker->scan.records, 0, sizeof(worker->scan.records));
    for (size_t index = 0U; index < worker->scan.record_count; ++index)
    {
        connectivity_manager_scan_record_t *record =
            &worker->scan.records[index];

        memcpy(record->ssid, scan->records[index].ssid, sizeof(record->ssid));
        record->rssi = scan->records[index].rssi;
        record->channel = scan->records[index].channel;
        record->security = _manager_map_security(scan->records[index].security);
        record->saved = _manager_scan_record_saved(worker, record->ssid,
            record->security);
    }
    _manager_cache_scan(worker);
    if (scan->operation_canceled)
    {
        if (worker->suspended)
        {
            worker->resume_cycle_after_scan = true;
            worker->status.state = CONNECTIVITY_MANAGER_STATE_SUSPENDED;
            _manager_cache_status(worker);
            return;
        }
        if (worker->active_cancel_requested)
        {
            worker->cycle_active = false;
            worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
            worker->status.ipv4_address = 0U;
            _manager_set_target_status(worker);
            _manager_complete_status_operation(
                worker, ESP_ERR_NOT_FINISHED,
                CONNECTIVITY_MANAGER_FAILURE_NONE);
            return;
        }
    }
    if (scan->last_error != ESP_OK)
    {
        _manager_cycle_scan_failed(worker, scan->last_error);
        return;
    }
    manager_profile_entry_t next;
    memset(&next, 0, sizeof(next));
    _manager_attempt_begin(worker, worker->cycle_start_index);
    if (!_manager_attempt_next(worker, &next))
    {
        _manager_cycle_finish_failure(
            worker, ESP_ERR_NOT_FOUND, CONNECTIVITY_MANAGER_FAILURE_AP_NOT_FOUND);
        return;
    }
    worker->target = next;
    worker->target_valid = true;
    worker->target_candidate = false;
    worker->target_persisted = true;
    worker->target_reconnectable = true;
    _manager_secure_zero(&next, sizeof(next));
    worker->cycle_attempt_count = 0U;
    worker->status.retry_count = 0U;
    if (_manager_start_connect(worker, worker->operation_id,
                               worker->active_command) != ESP_OK)
    {
        _manager_cycle_finish_failure(
            worker, ESP_ERR_INVALID_STATE,
            CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
    }
}

static void _manager_command_scan(manager_worker_t *worker,
                                  const manager_command_t *command)
{
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);
    if (defer_result != MANAGER_DEFER_NONE)
    {
        return;
    }
    if (worker->cycle_active)
    {
        worker->resume_cycle_after_scan = true;
    }
    if (worker->retry_pending)
    {
        worker->resume_cycle_after_scan = true;
        worker->retry_pending = false;
    }
    worker->retry_pending = false;
    worker->cycle_active = false;
    worker->cycle_scan_pending = false;
    _manager_attempt_end(worker);
    esp_err_t result = _manager_radio_start(worker);
    if (result == ESP_OK)
    {
        result = wifi_service_request_scan(worker->session_id,
                                           &worker->service_operation_id);
    }
    if (result == ESP_OK)
    {
        worker->operation_kind = MANAGER_OPERATION_SCAN;
        worker->active_command = MANAGER_COMMAND_SCAN;
        worker->operation_id = command->operation_id;
        worker->active_cancel_requested = false;
        worker->scan.operation_id = command->operation_id;
        worker->scan.running = true;
        worker->scan.last_error = ESP_OK;
        _manager_cache_scan(worker);
    }
    else
    {
        worker->status.radio_available = false;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
        worker->status.last_error = result;
        _manager_set_target_status(worker);
        _manager_cache_status(worker);
        _manager_publish_scan_terminal(worker, command->operation_id, result);
    }
}

static void _manager_command_connect(manager_worker_t *worker,
                                     const manager_command_t *command)
{
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);
    if (defer_result == MANAGER_DEFER_REJECTED)
    {
        return;
    }
    worker->resume_cycle_after_scan = false;
    if (defer_result == MANAGER_DEFER_QUEUED)
    {
        return;
    }
    worker->retry_pending = false;
    worker->retry_count = 0U;
    _manager_attempt_end(worker);
    _manager_secure_zero(&worker->target, sizeof(worker->target));
    worker->target = command->credentials;
    worker->target_valid = true;
    worker->target_candidate = true;
    worker->target_persisted = false;
    worker->target_reconnectable = true;
    worker->active_sync = false;
    worker->active_client_sync_id = 0U;
    _manager_secure_zero(worker->active_sync_identity,
                         sizeof(worker->active_sync_identity));
    worker->retry_count = 0U;
    worker->wifi_enabled = 1U;
    worker->status.manual_hold = false;
    const esp_err_t start_result = _manager_start_connect(
                                       worker, command->operation_id,
                                       MANAGER_COMMAND_CONNECT);
    if (start_result != ESP_OK)
    {
        _manager_cycle_finish_failure(
            worker, start_result,
            CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
    }
}

static void _manager_command_sync_profile(
    manager_worker_t *worker, const manager_command_t *command)
{
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);
    if (defer_result != MANAGER_DEFER_NONE)
    {
        return;
    }
    uint8_t identity[CONNECTIVITY_MANAGER_SYNC_DIGEST_BYTES];
    esp_err_t identity_result = _manager_profile_sync_identity(
                                    &command->credentials, false, identity);

    if (identity_result != ESP_OK)
    {
        worker->status.last_error = identity_result;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_INTERNAL;
        _manager_publish_status_terminal(
            worker, command->operation_id, identity_result,
            CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
        _manager_secure_zero(identity, sizeof(identity));
        return;
    }
    if (worker->profile_count > 0U && worker->sync_window_valid != 0U &&
            worker->last_client_sync_id == command->client_sync_id)
    {
        if (memcmp(worker->last_sync_identity, identity,
                   sizeof(identity)) != 0)
        {
            worker->status.last_error = ESP_ERR_INVALID_STATE;
            worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
            _manager_publish_status_terminal(
                worker, command->operation_id, ESP_ERR_INVALID_STATE,
                CONNECTIVITY_MANAGER_FAILURE_CONFLICT);
            _manager_secure_zero(identity, sizeof(identity));
            return;
        }
        _manager_set_target_status(worker);
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_OK,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        _manager_secure_zero(identity, sizeof(identity));
        return;
    }
    worker->retry_pending = false;
    worker->retry_count = 0U;
    _manager_attempt_end(worker);
    _manager_secure_zero(&worker->target, sizeof(worker->target));
    worker->target = command->credentials;
    worker->target_valid = true;
    worker->target_candidate = true;
    worker->target_persisted = false;
    worker->target_reconnectable = true;
    worker->active_sync = true;
    worker->active_client_sync_id = command->client_sync_id;
    memcpy(worker->active_sync_identity, identity,
           sizeof(worker->active_sync_identity));
    _manager_secure_zero(identity, sizeof(identity));
    worker->wifi_enabled = 1U;
    worker->status.manual_hold = false;
    const esp_err_t start_result = _manager_start_connect(
                                       worker, command->operation_id,
                                       MANAGER_COMMAND_SYNC_PROFILE);
    if (start_result != ESP_OK)
    {
        _manager_cycle_finish_failure(
            worker, start_result,
            CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
    }
}

static void _manager_command_save_profile(
    manager_worker_t *worker, const manager_command_t *command)
{
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);

    if (defer_result != MANAGER_DEFER_NONE)
    {
        return;
    }
    manager_profile_entry_t entry = command->credentials;
    const uint8_t index = _manager_find_profile(worker, entry.ssid,
        entry.ssid_length);

    if (index == CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE &&
            worker->profile_count >= CONNECTIVITY_MANAGER_MAX_PROFILES)
    {
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_ERR_NO_MEM,
            CONNECTIVITY_MANAGER_FAILURE_STORAGE);
        _manager_secure_zero(&entry, sizeof(entry));
        return;
    }
    const uint8_t previous_count = worker->profile_count;
    const uint8_t previous_active = worker->active_index;
    const uint64_t previous_revision = worker->profile_revision;
    uint8_t previous_entry[sizeof(manager_profile_entry_t)];
    bool replaced = false;

    memset(previous_entry, 0, sizeof(previous_entry));
    if (index != CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE)
    {
        memcpy(previous_entry, &worker->profiles[index],
               sizeof(previous_entry));
        replaced = true;
        worker->profiles[index] = entry;
    }
    else
    {
        worker->profiles[worker->profile_count] = entry;
        ++worker->profile_count;
        _manager_rebuild_order(worker);
    }
    worker->active_index = index != CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE ?
                           index : (uint8_t)(worker->profile_count - 1U);
    if (worker->profile_revision == UINT64_MAX)
    {
        worker->profile_count = previous_count;
        worker->active_index = previous_active;
        if (replaced && previous_count > 0U)
        {
            memcpy(&worker->profiles[worker->active_index], previous_entry,
                   sizeof(previous_entry));
        }
        _manager_secure_zero(previous_entry, sizeof(previous_entry));
        _manager_secure_zero(&entry, sizeof(entry));
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_ERR_INVALID_STATE,
            CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
        return;
    }
    ++worker->profile_revision;
    _manager_touch_profile(worker, worker->active_index);
    const esp_err_t result = _manager_profile_store(worker);

    if (result == ESP_OK)
    {
        _manager_set_saved_target(worker);
        _manager_set_target_status(worker);
        _manager_publish_profiles(worker);
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_OK,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
    }
    else
    {
        worker->profile_count = previous_count;
        worker->active_index = previous_active;
        worker->profile_revision = previous_revision;
        if (replaced && index < CONNECTIVITY_MANAGER_MAX_PROFILES)
        {
            memcpy(&worker->profiles[index], previous_entry,
                   sizeof(previous_entry));
        }
        else
        {
            _manager_secure_zero(&worker->profiles[previous_count],
                                 sizeof(worker->profiles[previous_count]));
            _manager_rebuild_order(worker);
        }
        _manager_publish_status_terminal(
            worker, command->operation_id, result,
            result == ESP_ERR_INVALID_ARG ?
            CONNECTIVITY_MANAGER_FAILURE_INTERNAL :
            CONNECTIVITY_MANAGER_FAILURE_STORAGE);
    }
    _manager_secure_zero(previous_entry, sizeof(previous_entry));
    _manager_secure_zero(&entry, sizeof(entry));
}

static void _manager_command_wifi_disable(manager_worker_t *worker,
        const manager_command_t *command)
{
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);
    if (defer_result == MANAGER_DEFER_REJECTED)
    {
        return;
    }
    if (defer_result == MANAGER_DEFER_QUEUED)
    {
        _manager_set_target_status(worker);
        _manager_cache_status(worker);
        return;
    }
    worker->wifi_enabled = 0U;
    worker->status.manual_hold = true;
    worker->resume_cycle_after_scan = false;
    worker->retry_pending = false;
    worker->cycle_active = false;
    worker->cycle_scan_pending = false;
    worker->cycle_scan_retry_pending = false;
    _manager_attempt_end(worker);
    _manager_clear_candidate(worker);
    /* Radio safety is independent of policy persistence: a failed NVS write
     * must never leave an authorized radio running while the UI says "off". */
    const esp_err_t stop_result =
        _manager_radio_stop(worker, CONNECTIVITY_MANAGER_RADIO_STOP_WAIT_MS);
    const esp_err_t policy_result = _manager_store_wifi_policy(worker);
    worker->policy_dirty = policy_result != ESP_OK;
    const esp_err_t result = stop_result != ESP_OK ? stop_result : policy_result;
    const connectivity_manager_failure_t failure =
        stop_result != ESP_OK ? CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE :
        (policy_result != ESP_OK ? CONNECTIVITY_MANAGER_FAILURE_STORAGE :
         CONNECTIVITY_MANAGER_FAILURE_NONE);
    worker->status.state = stop_result == ESP_OK ?
                           CONNECTIVITY_MANAGER_STATE_IDLE :
                           CONNECTIVITY_MANAGER_STATE_OFFLINE;
    worker->status.ipv4_address = 0U;
    worker->status.last_error = result;
    worker->status.failure = failure;
    _manager_set_target_status(worker);
    _manager_publish_status_terminal(
        worker, command->operation_id, result, failure);
}

static void _manager_command_wifi_enable(manager_worker_t *worker,
        const manager_command_t *command)
{
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);
    if (defer_result == MANAGER_DEFER_REJECTED)
    {
        return;
    }
    if (defer_result == MANAGER_DEFER_QUEUED)
    {
        return;
    }
    if (worker->status.state == CONNECTIVITY_MANAGER_STATE_IP_READY &&
            worker->status.radio_available && !worker->status.manual_hold &&
            worker->wifi_enabled != 0U)
    {
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_OK,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        return;
    }
    if (worker->profile_count == 0U)
    {
        worker->status.last_error = ESP_ERR_NOT_FOUND;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_ERR_NOT_FOUND,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        return;
    }
    _manager_cycle_start(worker, command->operation_id,
                         command->type, worker->active_index);
}

static void _manager_command_forget(manager_worker_t *worker,
                                    const manager_command_t *command)
{
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);
    if (defer_result == MANAGER_DEFER_REJECTED)
    {
        return;
    }
    worker->resume_cycle_after_scan = false;
    worker->retry_pending = false;
    _manager_clear_candidate(worker);
    if (defer_result == MANAGER_DEFER_QUEUED)
    {
        _manager_set_target_status(worker);
        _manager_cache_status(worker);
        return;
    }
    if (worker->profile_count == 0U)
    {
        worker->status.last_error = ESP_ERR_NOT_FOUND;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_ERR_NOT_FOUND,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        return;
    }

    const uint8_t previous_wifi_enabled = worker->wifi_enabled;
    esp_err_t result = _manager_radio_stop(worker,
                                           CONNECTIVITY_MANAGER_RADIO_STOP_WAIT_MS);
    const bool radio_stopped = result == ESP_OK;
    if (result == ESP_OK)
    {
        result = _manager_erase_all_profiles(worker);
    }
    if (result == ESP_OK)
    {
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        worker->status.last_error = ESP_OK;
    }
    else
    {
        worker->wifi_enabled = previous_wifi_enabled;
        worker->status.manual_hold = true;
        worker->status.state = radio_stopped ?
                               CONNECTIVITY_MANAGER_STATE_IDLE :
                               CONNECTIVITY_MANAGER_STATE_OFFLINE;
        worker->status.ipv4_address = 0U;
        worker->status.failure = radio_stopped ?
                                 CONNECTIVITY_MANAGER_FAILURE_STORAGE :
                                 CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
        worker->status.last_error = result;
    }
    _manager_set_target_status(worker);
    _manager_publish_status_terminal(
        worker, command->operation_id, result, worker->status.failure);
}

static void _manager_command_connect_network(manager_worker_t *worker,
        const manager_command_t *command)
{
    const uint8_t index = _manager_find_profile(worker,
        command->credentials.ssid,
        command->credentials.ssid_length);

    if (index == CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE)
    {
        worker->status.last_error = ESP_ERR_NOT_FOUND;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_ERR_NOT_FOUND,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        return;
    }
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);
    if (defer_result == MANAGER_DEFER_REJECTED)
    {
        return;
    }
    worker->resume_cycle_after_scan = false;
    if (defer_result == MANAGER_DEFER_QUEUED)
    {
        return;
    }
    /* A name-addressed connect starts the cycle at the named network without
     * making it the selected profile; a successful link selects it. */
    _manager_set_saved_target_index(worker, index);
    _manager_cycle_start(worker, command->operation_id,
                         MANAGER_COMMAND_CONNECT_NETWORK, index);
}

static void _manager_command_forget_network(manager_worker_t *worker,
        const manager_command_t *command)
{
    const uint8_t index = _manager_find_profile(worker,
        command->credentials.ssid,
        command->credentials.ssid_length);

    if (index == CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE)
    {
        worker->status.last_error = ESP_ERR_NOT_FOUND;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_ERR_NOT_FOUND,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        return;
    }
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);

    if (defer_result != MANAGER_DEFER_NONE)
    {
        return;
    }
    /* Removing the network in use needs the link torn down first so the driver
     * never keeps running with credentials that no longer exist. A sweep may
     * already be using an entry that is not the selected one, so the target
     * counts as in use too. */
    const bool link_active =
        worker->status.state == CONNECTIVITY_MANAGER_STATE_IP_READY ||
        worker->status.state == CONNECTIVITY_MANAGER_STATE_CONNECTING ||
        worker->status.state == CONNECTIVITY_MANAGER_STATE_WAITING_IP ||
        worker->status.state == CONNECTIVITY_MANAGER_STATE_RETRY_WAIT ||
        (worker->status.state == CONNECTIVITY_MANAGER_STATE_SCANNING &&
         worker->scan.running && worker->wifi_enabled != 0U);
    const bool target_active = worker->target_valid &&
                               !worker->target_candidate &&
                               worker->target.ssid_length ==
                               worker->profiles[index].ssid_length &&
                               memcmp(worker->target.ssid,
                                      worker->profiles[index].ssid,
                                      worker->target.ssid_length) == 0;
    const bool active = link_active &&
                        (index == worker->active_index || target_active);

    worker->pending_removal = index;
    worker->pending_removal_valid = true;
    worker->pending_removal_drops_link = active;
    if (!active)
    {
        /* Removing an unused entry must not put the radio into the manual-offline
         * hold: the established link and the automatic-connect policy stay as
         * they are, and only the list changes. */
        _manager_apply_pending_removal(worker, command->operation_id);
        return;
    }
    worker->status.manual_hold = true;
    worker->resume_cycle_after_scan = false;
    worker->retry_pending = false;
    _manager_attempt_end(worker);
    esp_err_t result = _manager_radio_start(worker);
    if (result == ESP_OK)
    {
        result = wifi_service_request_disconnect(
                     worker->session_id, &worker->service_operation_id);
    }
    if (result != ESP_OK)
    {
        worker->pending_removal_valid = false;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.ipv4_address = 0U;
        worker->status.last_error = result;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_INTERNAL;
        _manager_publish_status_terminal(
            worker, command->operation_id, result,
            CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
        _manager_set_target_status(worker);
        return;
    }
    worker->operation_kind = MANAGER_OPERATION_DISCONNECT;
    worker->active_command = MANAGER_COMMAND_FORGET_NETWORK;
    worker->operation_id = command->operation_id;
    worker->active_cancel_requested = false;
    worker->status.last_error = ESP_OK;
    worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    _manager_set_target_status(worker);
    _manager_cache_status(worker);
}

/**
 * @brief Remove one saved entry and persist the shortened list.
 *
 * The removed entry is overwritten so the remaining entries stay compact; the
 * attempt order and the selected index follow the shift.
 */
static void _manager_remove_profile(manager_worker_t *worker, uint8_t index)
{
    if (index >= worker->profile_count)
    {
        return;
    }
    for (uint8_t step = (uint8_t)(index + 1U);
            step < worker->profile_count; ++step)
    {
        worker->profiles[step - 1U] = worker->profiles[step];
    }
    --worker->profile_count;
    _manager_secure_zero(&worker->profiles[worker->profile_count],
                         sizeof(worker->profiles[worker->profile_count]));
    if (worker->profile_count == 0U)
    {
        worker->active_index = CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE;
    }
    else if (index < worker->active_index)
    {
        --worker->active_index;
    }
    else if (worker->active_index >= worker->profile_count)
    {
        worker->active_index = (uint8_t)(worker->profile_count - 1U);
    }
    for (size_t step = 0U; step < CONNECTIVITY_MANAGER_PERSISTED_PROFILES;
            ++step)
    {
        uint8_t order = worker->attempt_order[step];

        if (order == index)
        {
            order = CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE;
        }
        else if (order > index &&
                 order < CONNECTIVITY_MANAGER_PERSISTED_PROFILES)
        {
            --order;
        }
        worker->attempt_order[step] = order;
    }
    _manager_rebuild_order(worker);
}

/**
 * @brief Reinsert one saved entry so a refused removal can be undone.
 *
 * This is the inverse of _manager_remove_profile() for the shift only; the
 * caller restores the selected index, revision, and attempt order it captured
 * before the removal.
 */
static void _manager_insert_profile(manager_worker_t *worker, uint8_t index,
                                    const manager_profile_entry_t *entry)
{
    if (entry == NULL || index > worker->profile_count ||
            worker->profile_count >= CONNECTIVITY_MANAGER_MAX_PROFILES)
    {
        return;
    }
    for (uint8_t step = worker->profile_count; step > index; --step)
    {
        worker->profiles[step] = worker->profiles[step - 1U];
    }
    worker->profiles[index] = *entry;
    ++worker->profile_count;
}

/**
 * @brief Apply a pending single-entry removal and publish its terminal.
 * @param operation_id identifies the admitted FORGET_NETWORK operation.
 */
static void _manager_apply_pending_removal(manager_worker_t *worker,
        connectivity_manager_operation_id_t operation_id)
{
    esp_err_t result = ESP_OK;
    connectivity_manager_failure_t failure =
        CONNECTIVITY_MANAGER_FAILURE_NONE;
    /* A link that is still established keeps its state; only a removal that
     * tore the link down reports IDLE with no address. */
    const bool removal_kept_link =
        worker->status.state == CONNECTIVITY_MANAGER_STATE_IP_READY;
    const bool drops_link = worker->pending_removal_drops_link;
    worker->pending_removal_drops_link = false;

    if (worker->pending_removal_valid)
    {
        const uint8_t removed_index = worker->pending_removal;
        const uint8_t previous_active = worker->active_index;
        const uint64_t previous_revision = worker->profile_revision;
        const uint8_t previous_wifi_enabled = worker->wifi_enabled;
        const bool previous_manual_hold = worker->status.manual_hold;
        const connectivity_manager_state_t previous_state = worker->status.state;
        const uint32_t previous_ipv4 = worker->status.ipv4_address;
        manager_profile_entry_t removed;
        uint8_t previous_order[CONNECTIVITY_MANAGER_PERSISTED_PROFILES];

        memset(&removed, 0, sizeof(removed));
        memcpy(previous_order, worker->attempt_order, sizeof(previous_order));
        if (removed_index < worker->profile_count)
        {
            removed = worker->profiles[removed_index];
        }
        worker->pending_removal_valid = false;
        _manager_remove_profile(worker, removed_index);
        if (worker->profile_count == 0U)
        {
            worker->wifi_enabled = 0U;
            worker->status.manual_hold = true;
            worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
            worker->status.ipv4_address = 0U;
            result = nv_storage_erase_key(CONNECTIVITY_MANAGER_PROFILE_KEY);
            if (result == ESP_ERR_NVS_NOT_FOUND)
            {
                result = ESP_OK;
            }
        }
        else if (worker->profile_revision != UINT64_MAX)
        {
            if (drops_link)
            {
                worker->wifi_enabled = 0U;
            }
            ++worker->profile_revision;
            result = _manager_profile_store(worker);
        }
        else
        {
            result = ESP_ERR_INVALID_STATE;
        }
        if (result != ESP_OK)
        {
            /* A refused write never reached flash, so the RAM list must not
             * advertise the removal either. */
            _manager_insert_profile(worker, removed_index, &removed);
            worker->active_index = previous_active;
            worker->profile_revision = previous_revision;
            worker->wifi_enabled = previous_wifi_enabled;
            worker->status.manual_hold = previous_manual_hold;
            worker->status.state = previous_state;
            worker->status.ipv4_address = previous_ipv4;
            memcpy(worker->attempt_order, previous_order,
                   sizeof(worker->attempt_order));
            _manager_rebuild_order(worker);
            failure = result == ESP_ERR_INVALID_ARG ?
                      CONNECTIVITY_MANAGER_FAILURE_INTERNAL :
                      CONNECTIVITY_MANAGER_FAILURE_STORAGE;
        }
        _manager_secure_zero(&removed, sizeof(removed));
        _manager_secure_zero(previous_order, sizeof(previous_order));
        if (drops_link && result == ESP_OK && worker->profile_count > 0U)
        {
            worker->status.manual_hold = true;
        }
        _manager_set_saved_target(worker);
        _manager_publish_profiles(worker);
    }
    /* Removing an entry that is not carrying the link must not disturb it; the
     * terminal still reports the removal result. */
    if (!removal_kept_link)
    {
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.ipv4_address = 0U;
    }
    worker->status.last_error = result;
    worker->status.failure = failure;
    _manager_set_target_status(worker);
    _manager_publish_status_terminal(worker, operation_id, result, failure);
}

static void _manager_complete_cancel_failure(manager_worker_t *worker,
        esp_err_t result)
{
    const connectivity_manager_operation_id_t operation_id =
        worker->operation_id;

    if (worker->operation_kind == MANAGER_OPERATION_SCAN)
    {
        _manager_publish_scan_terminal(worker, operation_id, result);
        _manager_clear_active_operation(worker);
    }
    else
    {
        _manager_complete_status_operation(
            worker, result, CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
    }
}

static void _manager_command_cancel(manager_worker_t *worker,
                                    const manager_command_t *command)
{
    if (worker->pending_command_valid &&
            worker->pending_command.operation_id == command->operation_id)
    {
        manager_command_t canceled = worker->pending_command;
        _manager_secure_zero(&worker->pending_command,
                             sizeof(worker->pending_command));
        worker->pending_command_valid = false;
        _manager_publish_command_terminal(
            worker, &canceled, ESP_ERR_NOT_FINISHED,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        _manager_secure_zero(&canceled, sizeof(canceled));
        return;
    }
    if (worker->operation_id != command->operation_id)
    {
        return;
    }
    if (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
            (worker->connect_resume_pending ||
             (worker->retry_pending && worker->service_operation_id == 0U)))
    {
        worker->retry_pending = false;
        worker->status.state = worker->suspended ? CONNECTIVITY_MANAGER_STATE_SUSPENDED :
                               CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.ipv4_address = 0U;
        _manager_clear_candidate(worker);
        _manager_set_target_status(worker);
        _manager_complete_status_operation(
            worker, ESP_ERR_NOT_FINISHED,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        return;
    }
    if (worker->service_operation_id == 0U)
    {
        _manager_complete_cancel_failure(worker, ESP_ERR_INVALID_STATE);
        return;
    }
    wifi_service_cancel_disposition_t disposition =
        WIFI_SERVICE_CANCEL_FAILURE;
    const esp_err_t cancel_result = wifi_service_cancel(
                                        worker->session_id,
                                        worker->service_operation_id,
                                        &disposition);

    if (cancel_result != ESP_OK ||
            disposition == WIFI_SERVICE_CANCEL_NOT_FOUND ||
            disposition == WIFI_SERVICE_CANCEL_FAILURE)
    {
        _manager_complete_cancel_failure(
            worker, cancel_result == ESP_OK ?
            ESP_ERR_NOT_FOUND : cancel_result);
        return;
    }
    if (disposition == WIFI_SERVICE_CANCEL_ACCEPTED)
    {
        worker->active_cancel_requested = true;
        _manager_clear_candidate(worker);
    }
}

static void _manager_run_pending(manager_worker_t *worker)
{
    if (!worker->pending_command_valid)
    {
        return;
    }
    manager_command_t command = worker->pending_command;
    _manager_secure_zero(&worker->pending_command,
                         sizeof(worker->pending_command));
    worker->pending_command_valid = false;
    bool stop = false;
    _manager_process_command(worker, &command, &stop);
    _manager_secure_zero(&command, sizeof(command));
}

static void _manager_continue_policy(manager_worker_t *worker)
{
    if (s_manager.terminal_outbox_count > 0U || worker->suspended ||
            atomic_load_explicit(&s_manager_lifecycle, memory_order_acquire) !=
            MANAGER_LIFECYCLE_RUNNING)
    {
        return;
    }
    if (worker->cycle_active &&
            _manager_tick_reached(xTaskGetTickCount(),
                                  worker->cycle_retry_deadline))
    {
        _manager_cycle_finish_failure(
            worker, ESP_ERR_TIMEOUT,
            CONNECTIVITY_MANAGER_FAILURE_ASSOCIATION_TIMEOUT);
        return;
    }
    if (worker->cycle_active && worker->cycle_scan_retry_pending &&
            _manager_tick_reached(xTaskGetTickCount(),
                                  worker->cycle_scan_retry_deadline))
    {
        worker->cycle_scan_retry_pending = false;
        ++worker->cycle_scan_attempts;
        const esp_err_t scan_result = _manager_cycle_request_scan(worker);
        if (scan_result != ESP_OK)
        {
            _manager_cycle_scan_failed(worker, scan_result);
        }
        return;
    }
    if (worker->operation_kind == MANAGER_OPERATION_NONE && worker->pending_command_valid)
    {
        _manager_run_pending(worker);
        return;
    }
    if (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
            worker->connect_resume_pending)
    {
        worker->connect_resume_pending = false;
        const esp_err_t start_result = _manager_start_connect(
                                           worker, worker->operation_id,
                                           worker->active_command);
        if (start_result != ESP_OK)
        {
            if (worker->cycle_active)
            {
                _manager_cycle_finish_failure(
                    worker, start_result,
                    CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
            }
            else
            {
                _manager_complete_status_operation(
                    worker, start_result,
                    CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
            }
        }
        return;
    }
    if (worker->operation_kind == MANAGER_OPERATION_NONE &&
            worker->resume_cycle_after_scan)
    {
        worker->resume_cycle_after_scan = false;
        if (worker->wifi_enabled != 0U && worker->profile_count > 0U)
        {
            _manager_cycle_start(worker, worker->cycle_operation_id,
                                 MANAGER_COMMAND_AUTO,
                                 worker->cycle_start_index);
        }
        return;
    }
    if (worker->retry_pending &&
            worker->cycle_active &&
            worker->operation_kind == MANAGER_OPERATION_CONNECT &&
            worker->service_operation_id == 0U &&
            _manager_tick_reached(xTaskGetTickCount(), worker->retry_deadline))
    {
        worker->retry_pending = false;
        const esp_err_t start_result = _manager_start_connect(
                                           worker, worker->operation_id,
                                           worker->active_command);
        if (start_result != ESP_OK)
        {
            _manager_cycle_finish_failure(
                worker, start_result,
                CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
        }
    }
}


static esp_err_t _manager_worker_init(manager_worker_t *worker)
{
    memset(worker, 0, sizeof(*worker));
    worker->status.available = true;
    worker->status.state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
    worker->scan.last_error = ESP_ERR_INVALID_STATE;
    esp_err_t storage_result = _manager_profile_load(worker);
    if (storage_result != ESP_OK)
    {
        worker->status.failure = storage_result == ESP_ERR_INVALID_RESPONSE ?
                                 CONNECTIVITY_MANAGER_FAILURE_INTERNAL :
                                 CONNECTIVITY_MANAGER_FAILURE_STORAGE;
        worker->status.last_error = storage_result;
    }
    /* Radio initialization is deliberately deferred until a command actually
     * needs it. On this board the first LVGL frame needs internal DMA memory;
     * bringing up the Wi-Fi driver during boot competes with that allocation
     * and can make app_runtime fail with ESP_ERR_NO_MEM. */
    worker->status.radio_available = false;
    worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    worker->status.manual_hold = worker->profile_count == 0U ||
                                 worker->wifi_enabled == 0U;
    if (storage_result == ESP_OK)
    {
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        worker->status.last_error = ESP_OK;
    }
    _manager_set_target_status(worker);
    _manager_cache_status(worker);
    _manager_cache_scan(worker);
    _manager_publish_profiles(worker);
    return ESP_OK;
}

static esp_err_t _manager_worker_suspend(manager_worker_t *worker,
        uint32_t timeout_ms)
{
    if (worker->suspended)
    {
        return ESP_OK;
    }
    _manager_poll_status(worker);
    const bool resume_cycle = worker->wifi_enabled != 0U &&
                              !worker->retry_pending;
    const esp_err_t result = worker->radio_initialized ?
                             wifi_service_suspend(timeout_ms) : ESP_OK;
    wifi_service_status_snapshot_t radio = { 0 };
    const bool have_radio = worker->radio_initialized &&
                            wifi_service_get_status(&radio) == ESP_OK;
    if ((!worker->radio_initialized && result == ESP_OK) ||
            (have_radio && radio.state == WIFI_SERVICE_STATE_SUSPENDED))
    {
        worker->suspended = true;
        if (worker->operation_kind == MANAGER_OPERATION_CONNECT)
        {
            worker->connect_resume_pending = true;
            worker->service_operation_id = 0U;
        }
        if (resume_cycle && worker->cycle_active &&
                worker->operation_kind == MANAGER_OPERATION_SCAN)
        {
            worker->resume_cycle_after_scan = true;
        }
        worker->status.state = CONNECTIVITY_MANAGER_STATE_SUSPENDED;
        worker->status.ipv4_address = 0U;
        _manager_poll_scan(worker);
        _manager_cache_status(worker);
    }
    else if (have_radio && radio.state == WIFI_SERVICE_STATE_OFFLINE)
    {
        _manager_poll_status(worker);
    }
    return result;
}

static esp_err_t _manager_worker_resume(manager_worker_t *worker,
                                        uint32_t timeout_ms)
{
    if (!worker->suspended)
    {
        return ESP_OK;
    }
    if (worker->wifi_enabled == 0U)
    {
        worker->suspended = false;
        worker->status.manual_hold = true;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.ipv4_address = 0U;
        _manager_cache_status(worker);
        return ESP_OK;
    }
    wifi_service_status_snapshot_t radio;
    const bool suspended_radio = worker->radio_initialized &&
                                 wifi_service_get_status(&radio) == ESP_OK &&
                                 radio.state == WIFI_SERVICE_STATE_SUSPENDED;
    const esp_err_t result = suspended_radio ? wifi_service_resume(timeout_ms) :
                             _manager_radio_start(worker);
    if (result == ESP_OK)
    {
        worker->suspended = false;
        worker->status.manual_hold = false;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.radio_available = true;
        bool resumed_connect = false;
        if (worker->connect_resume_pending)
        {
            worker->connect_resume_pending = false;
            if (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
                    worker->target_valid)
            {
                resumed_connect = true;
                if (_manager_start_connect(worker, worker->operation_id,
                                           worker->active_command) != ESP_OK)
                {
                    if (worker->cycle_active)
                    {
                        _manager_cycle_finish_failure(
                            worker, ESP_ERR_INVALID_STATE,
                            CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
                    }
                    else
                    {
                        _manager_complete_status_operation(
                            worker, ESP_ERR_INVALID_STATE,
                            CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
                    }
                }
            }
        }
        _manager_cache_status(worker);
        if (!resumed_connect && worker->resume_cycle_after_scan &&
                worker->wifi_enabled != 0U && worker->profile_count > 0U)
        {
            worker->resume_cycle_after_scan = false;
            _manager_cycle_start(worker, worker->cycle_operation_id,
                                 MANAGER_COMMAND_AUTO,
                                 worker->cycle_start_index);
        }
        else if (!resumed_connect && worker->wifi_enabled != 0U &&
                 worker->profile_count > 0U)
        {
            _manager_cycle_start(worker, 0U, MANAGER_COMMAND_AUTO,
                                 worker->active_index);
        }
    }
    return result;
}

static esp_err_t _manager_worker_deinit(manager_worker_t *worker,
                                        uint32_t timeout_ms)
{
    const manager_deadline_t deadline = _manager_deadline(timeout_ms);
    worker->retry_pending = false;
    worker->resume_cycle_after_scan = false;
    worker->connect_resume_pending = false;
    if (worker->operation_kind != MANAGER_OPERATION_NONE)
    {
        _manager_clear_candidate(worker);
        _manager_set_target_status(worker);
        if (worker->operation_kind == MANAGER_OPERATION_SCAN)
        {
            _manager_publish_scan_terminal(
                worker, worker->operation_id, ESP_ERR_NOT_FINISHED);
        }
        else
        {
            _manager_publish_status_terminal(
                worker, worker->operation_id, ESP_ERR_NOT_FINISHED,
                CONNECTIVITY_MANAGER_FAILURE_NONE);
        }
        _manager_clear_active_operation(worker);
    }
    if (worker->pending_command_valid)
    {
        _manager_publish_command_terminal(
            worker, &worker->pending_command, ESP_ERR_NOT_FINISHED,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        _manager_secure_zero(&worker->pending_command,
                             sizeof(worker->pending_command));
        worker->pending_command_valid = false;
    }
    while (s_manager.terminal_outbox_count > 0U)
    {
        if (_manager_flush_terminal_outbox())
        {
            break;
        }
        if (_manager_deadline_remaining(&deadline) == 0U)
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1U);
    }
    if (worker->session_id != 0U)
    {
        (void)wifi_service_session_close(worker->session_id);
        worker->session_id = 0U;
    }
    const bool radio_owned = worker->radio_initialized ||
                             wifi_service_is_available() ||
                             wifi_service_is_cleanup_pending();
    esp_err_t result = radio_owned ? wifi_service_deinit(
                           _manager_deadline_remaining_ms(&deadline)) : ESP_OK;
    if (result == ESP_OK)
    {
        worker->radio_initialized = false;
    }
    _manager_attempt_end(worker);
    _manager_worker_clear_profiles(worker);
    _manager_secure_zero(&worker->target, sizeof(worker->target));
    _manager_secure_zero(&worker->pending_command,
                         sizeof(worker->pending_command));
    worker->target_valid = false;
    worker->target_candidate = false;
    worker->target_persisted = false;
    worker->target_reconnectable = false;
    worker->status.available = false;
    worker->status.radio_available = false;
    worker->status.state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
    worker->status.ipv4_address = 0U;
    _manager_cache_status(worker);
    return result;
}

static void _manager_process_command(manager_worker_t *worker,
                                     const manager_command_t *command,
                                     bool *stop)
{
    esp_err_t control_result = ESP_OK;
    switch (command->type)
    {
    case MANAGER_COMMAND_SCAN:
        _manager_command_scan(worker, command);
        break;
    case MANAGER_COMMAND_CONNECT:
        _manager_command_connect(worker, command);
        break;
    case MANAGER_COMMAND_SYNC_PROFILE:
        _manager_command_sync_profile(worker, command);
        break;
    case MANAGER_COMMAND_SAVE_PROFILE:
        _manager_command_save_profile(worker, command);
        break;
    case MANAGER_COMMAND_DISCONNECT:
        _manager_command_wifi_disable(worker, command);
        break;
    case MANAGER_COMMAND_RECONNECT:
        _manager_command_wifi_enable(worker, command);
        break;
    case MANAGER_COMMAND_FORGET:
        _manager_command_forget(worker, command);
        break;
    case MANAGER_COMMAND_CONNECT_NETWORK:
        _manager_command_connect_network(worker, command);
        break;
    case MANAGER_COMMAND_FORGET_NETWORK:
        _manager_command_forget_network(worker, command);
        break;
    case MANAGER_COMMAND_CANCEL:
        _manager_command_cancel(worker, command);
        break;
    case MANAGER_COMMAND_SUSPEND:
        control_result = _manager_worker_suspend(
                             worker, command->timeout_ms);
        atomic_store_explicit(&s_manager.control_result, control_result,
                              memory_order_relaxed);
        atomic_store_explicit(&s_manager.control_completed_generation,
                              command->control_generation,
                              memory_order_release);
        xSemaphoreGive(s_manager.control_done);
        break;
    case MANAGER_COMMAND_RESUME:
        control_result = _manager_worker_resume(
                             worker, command->timeout_ms);
        atomic_store_explicit(&s_manager.control_result, control_result,
                              memory_order_relaxed);
        atomic_store_explicit(&s_manager.control_completed_generation,
                              command->control_generation,
                              memory_order_release);
        xSemaphoreGive(s_manager.control_done);
        break;
    case MANAGER_COMMAND_DEINIT:
        control_result = _manager_worker_deinit(worker, command->timeout_ms);
        atomic_store_explicit(&s_manager.control_result, control_result,
                              memory_order_relaxed);
        atomic_store_explicit(&s_manager.control_completed_generation,
                              command->control_generation,
                              memory_order_release);
        *stop = control_result == ESP_OK;
        if (*stop)
        {
            atomic_store_explicit(&s_manager.worker_stopping, true,
                                  memory_order_release);
        }
        xSemaphoreGive(s_manager.control_done);
        break;
    case MANAGER_COMMAND_INIT:
    case MANAGER_COMMAND_AUTO:
        break;
    }
}

static void _manager_wake(void)
{
    if (s_manager.worker != NULL)
    {
        (void)xTaskNotifyGive(s_manager.worker);
    }
}

static TickType_t _manager_ticks_until(TickType_t deadline)
{
    const TickType_t now = xTaskGetTickCount();
    const int32_t remaining = (int32_t)(deadline - now);
    return remaining <= 0 ? 0U : (TickType_t)remaining;
}

static TickType_t _manager_next_wait_ticks(const manager_worker_t *worker)
{
    if (s_manager.terminal_outbox_count > 0U)
    {
        return 1U;
    }
    if (worker->suspended)
    {
        return portMAX_DELAY;
    }
    TickType_t wait = portMAX_DELAY;
    if (worker->cycle_active)
    {
        wait = _manager_ticks_until(worker->cycle_retry_deadline);
    }
    if (worker->retry_pending)
    {
        const TickType_t retry = _manager_ticks_until(worker->retry_deadline);
        if (retry < wait)
        {
            wait = retry;
        }
    }
    if (worker->cycle_active && worker->cycle_scan_retry_pending)
    {
        const TickType_t scan_retry =
            _manager_ticks_until(worker->cycle_scan_retry_deadline);
        if (scan_retry < wait)
        {
            wait = scan_retry;
        }
    }
    if (worker->radio_initialized)
    {
        const TickType_t fallback =
            pdMS_TO_TICKS(CONNECTIVITY_MANAGER_FALLBACK_POLL_MS);
        if (fallback < wait)
        {
            wait = fallback;
        }
    }
    return wait;
}

static void _manager_wifi_event(event_bus_msg_id_t message_id, uint32_t subtype,
                                const void *payload, size_t payload_size,
                                void *user_data)
{
    (void)payload;
    (void)payload_size;
    (void)user_data;
    if (message_id != WIFI_SERVICE_MSG ||
            (subtype != WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT &&
             subtype != WIFI_SERVICE_MSG_SUB_TYPE_SCAN_SNAPSHOT))
    {
        return;
    }
    _manager_wake();
}

static esp_err_t _manager_subscribe_wifi_events(void)
{
    esp_err_t result = event_bus_subscribe(
                           WIFI_SERVICE_MSG,
                           WIFI_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                           _manager_wifi_event, NULL,
                           EVENT_BUS_DISPATCH_PUBLISHER,
                           &s_manager.wifi_status_subscription);
    if (result != ESP_OK)
    {
        return result;
    }
    result = event_bus_subscribe(
                 WIFI_SERVICE_MSG,
                 WIFI_SERVICE_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                 _manager_wifi_event, NULL,
                 EVENT_BUS_DISPATCH_PUBLISHER,
                 &s_manager.wifi_scan_subscription);
    if (result != ESP_OK)
    {
        (void)event_bus_unsubscribe(s_manager.wifi_status_subscription);
        s_manager.wifi_status_subscription = EVENT_BUS_SUB_HANDLE_INVALID;
    }
    return result;
}

static void _manager_unsubscribe_wifi_events(void)
{
    if (s_manager.wifi_status_subscription != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        (void)event_bus_unsubscribe_sync(
            s_manager.wifi_status_subscription, 1000U);
        s_manager.wifi_status_subscription = EVENT_BUS_SUB_HANDLE_INVALID;
    }
    if (s_manager.wifi_scan_subscription != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        (void)event_bus_unsubscribe_sync(
            s_manager.wifi_scan_subscription, 1000U);
        s_manager.wifi_scan_subscription = EVENT_BUS_SUB_HANDLE_INVALID;
    }
}

static void _manager_worker_run(void *argument)
{
    (void)argument;
    manager_worker_t *worker = &s_manager.worker_state;
    const esp_err_t init_result = _manager_worker_init(worker);
    atomic_store_explicit(&s_manager.control_result, init_result,
                          memory_order_release);
    if (init_result != ESP_OK)
    {
        atomic_store_explicit(&s_manager.worker_stopping, true,
                              memory_order_release);
    }
    xSemaphoreGive(s_manager.control_done);
    bool stop = init_result != ESP_OK;
    while (!stop)
    {
        if (!_manager_flush_terminal_outbox())
        {
            vTaskDelay(1U);
            continue;
        }
        _manager_continue_policy(worker);
        if (s_manager.terminal_outbox_count > 0U)
        {
            continue;
        }
        (void)ulTaskNotifyTake(pdTRUE, _manager_next_wait_ticks(worker));
        manager_command_t *queued_command = NULL;
        while (xQueueReceive(s_manager.queue, &queued_command, 0U) == pdTRUE)
        {
            manager_command_t command = *queued_command;
            _manager_command_pool_release(queued_command);
            _manager_process_command(worker, &command, &stop);
            _manager_secure_zero(&command, sizeof(command));
            if (stop)
            {
                break;
            }
        }
        if (s_manager.terminal_outbox_count > 0U || stop)
        {
            continue;
        }
        _manager_poll_status(worker);
        if (s_manager.terminal_outbox_count > 0U)
        {
            continue;
        }
        _manager_poll_scan(worker);
        if (s_manager.terminal_outbox_count > 0U)
        {
            continue;
        }
        _manager_continue_policy(worker);
    }
    _manager_secure_zero(worker, sizeof(*worker));
    vTaskSuspend(NULL);
    vTaskDelete(NULL);
}

static esp_err_t _manager_submit(manager_command_t *command,
                                 connectivity_manager_operation_id_t *out_operation_id)
{
    if (!_manager_api_acquire())
    {
        _manager_secure_zero(command, sizeof(*command));
        return ESP_ERR_INVALID_STATE;
    }
    if (out_operation_id != NULL && command->type != MANAGER_COMMAND_CANCEL)
    {
        command->operation_id = _manager_next_generation();
    }
    manager_command_t *queued_command =
        _manager_command_pool_acquire(command);
    esp_err_t result = queued_command != NULL &&
                       xQueueSend(s_manager.queue, &queued_command, 0) == pdTRUE ?
                       ESP_OK : ESP_ERR_NO_MEM;
    if (result != ESP_OK && queued_command != NULL)
    {
        _manager_command_pool_release(queued_command);
    }
    if (result == ESP_OK && out_operation_id != NULL)
    {
        *out_operation_id = command->operation_id;
    }
    if (result == ESP_OK)
    {
        _manager_wake();
    }
    _manager_secure_zero(command, sizeof(*command));
    _manager_api_release();
    return result;
}

static esp_err_t _manager_control(manager_command_type_t type,
                                  uint32_t timeout_ms)
{
    const manager_deadline_t deadline = _manager_deadline(timeout_ms);
    if (xSemaphoreTake(s_manager.control_mutex,
                       _manager_deadline_remaining(&deadline)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    if (s_manager.control_inflight)
    {
        const manager_command_type_t inflight_type = s_manager.control_type;
        const uint64_t generation = s_manager.control_generation;
        do
        {
            if (xSemaphoreTake(
                        s_manager.control_done,
                        _manager_deadline_remaining(&deadline)) != pdTRUE)
            {
                result = ESP_ERR_TIMEOUT;
                goto exit;
            }
        }
        while (atomic_load_explicit(
                    &s_manager.control_completed_generation,
                    memory_order_acquire) != generation);
        result = (esp_err_t)atomic_load_explicit(
                     &s_manager.control_result, memory_order_acquire);
        s_manager.control_inflight = false;
        if (inflight_type == type && result == ESP_OK)
        {
            goto exit;
        }
    }

    manager_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = type;
    command.timeout_ms = timeout_ms;
    command.control_generation = _manager_next_generation();
    (void)xSemaphoreTake(s_manager.control_done, 0U);
    s_manager.control_type = type;
    s_manager.control_generation = command.control_generation;
    s_manager.control_inflight = true;
    atomic_store_explicit(&s_manager.control_completed_generation, 0U,
                          memory_order_release);
    manager_command_t *queued_command =
        _manager_command_pool_acquire(&command);
    if (queued_command == NULL ||
            xQueueSend(s_manager.queue, &queued_command, 0) != pdTRUE)
    {
        if (queued_command != NULL)
        {
            _manager_command_pool_release(queued_command);
        }
        s_manager.control_inflight = false;
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
    _manager_wake();
    do
    {
        if (xSemaphoreTake(s_manager.control_done,
                           _manager_deadline_remaining(&deadline)) != pdTRUE)
        {
            result = ESP_ERR_TIMEOUT;
            goto exit;
        }
    }
    while (atomic_load_explicit(&s_manager.control_completed_generation,
                                memory_order_acquire) !=
            command.control_generation);
    result = (esp_err_t)atomic_load_explicit(
                 &s_manager.control_result, memory_order_acquire);
    s_manager.control_inflight = false;

exit:
    xSemaphoreGive(s_manager.control_mutex);
    return result;
}

static void _manager_release_resources(void)
{
    _manager_unsubscribe_wifi_events();
    if (s_manager.worker != NULL)
    {
        vTaskDelete(s_manager.worker);
    }
    if (s_manager.queue != NULL)
    {
        vQueueDelete(s_manager.queue);
    }
    if (s_manager.control_done != NULL)
    {
        vSemaphoreDelete(s_manager.control_done);
    }
    if (s_manager.control_mutex != NULL)
    {
        vSemaphoreDelete(s_manager.control_mutex);
    }
    if (s_manager.mutex != NULL)
    {
        vSemaphoreDelete(s_manager.mutex);
    }
    s_manager.worker = NULL;
    s_manager.queue = NULL;
    s_manager.control_done = NULL;
    s_manager.control_mutex = NULL;
    s_manager.mutex = NULL;
    _manager_secure_zero(&s_manager.queue_storage,
                         sizeof(s_manager.queue_storage));
    _manager_secure_zero(s_manager.command_pool,
                         sizeof(s_manager.command_pool));
    memset(s_manager.command_pool_used, 0,
           sizeof(s_manager.command_pool_used));
}

esp_err_t connectivity_manager_clear_persisted_profile(void)
{
    int expected = MANAGER_LIFECYCLE_OFFLINE;

    if (!atomic_compare_exchange_strong_explicit(
                &s_manager_lifecycle, &expected,
                MANAGER_LIFECYCLE_STORAGE_RESETTING,
                memory_order_acq_rel, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = nv_storage_erase_key(
                           CONNECTIVITY_MANAGER_PROFILE_KEY);

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        result = ESP_OK;
    }
    atomic_store_explicit(&s_manager_lifecycle, MANAGER_LIFECYCLE_OFFLINE,
                          memory_order_release);
    return result;
}

esp_err_t connectivity_manager_init(
    const connectivity_manager_config_t *config)
{
    if (config == NULL || config->task_priority == 0U ||
            config->task_priority >= configMAX_PRIORITIES ||
            config->wifi_task_priority == 0U ||
            config->wifi_task_priority >= configMAX_PRIORITIES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    int expected = MANAGER_LIFECYCLE_OFFLINE;
    if (!atomic_compare_exchange_strong_explicit(
                &s_manager_lifecycle, &expected,
                MANAGER_LIFECYCLE_INITIALIZING,
                memory_order_acq_rel, memory_order_acquire))
    {
        return expected == MANAGER_LIFECYCLE_RUNNING &&
               memcmp(&s_manager.config, config, sizeof(*config)) == 0 ?
               ESP_OK : ESP_ERR_INVALID_STATE;
    }

    memset(&s_manager, 0, sizeof(s_manager));
    atomic_init(&s_manager.generation, 0U);
    atomic_init(&s_manager.control_completed_generation, 0U);
    atomic_init(&s_manager.control_result, ESP_ERR_INVALID_STATE);
    atomic_init(&s_manager.worker_stopping, false);
    s_manager.config = *config;
    s_manager.mutex = xSemaphoreCreateMutexStatic(&s_manager.mutex_control);
    s_manager.control_mutex = xSemaphoreCreateMutexStatic(
                                  &s_manager.control_mutex_control);
    s_manager.control_done = xSemaphoreCreateBinaryStatic(
                                 &s_manager.control_done_control);
    s_manager.queue = xQueueCreateStatic(
                          CONFIG_CONNECTIVITY_MANAGER_QUEUE_DEPTH,
                          sizeof(manager_command_t *),
                          s_manager.queue_storage.bytes,
                          &s_manager.queue_control);
    if (s_manager.mutex == NULL || s_manager.control_mutex == NULL ||
            s_manager.control_done == NULL ||
            s_manager.queue == NULL)
    {
        _manager_release_resources();
        atomic_store_explicit(&s_manager_lifecycle,
                              MANAGER_LIFECYCLE_OFFLINE,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    s_manager.worker = xTaskCreateStaticPinnedToCore(
                           _manager_worker_run, "connectivity",
                           CONFIG_CONNECTIVITY_MANAGER_TASK_STACK, NULL,
                           config->task_priority, s_manager.worker_stack,
                           &s_manager.worker_control,
                           CONFIG_MAIN_PROJECT_TASK_CORE_ID);
    if (s_manager.worker == NULL)
    {
        _manager_release_resources();
        atomic_store_explicit(&s_manager_lifecycle,
                              MANAGER_LIFECYCLE_OFFLINE,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU0 || \
    CONFIG_MAIN_PROJECT_TASK_AFFINITY_CPU1
    LOG_I("task affinity name=connectivity core=%d",
          (int)xTaskGetCoreID(s_manager.worker));
#endif
    if (xSemaphoreTake(s_manager.control_done, portMAX_DELAY) != pdTRUE)
    {
        _manager_release_resources();
        atomic_store_explicit(&s_manager_lifecycle,
                              MANAGER_LIFECYCLE_OFFLINE,
                              memory_order_release);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = (esp_err_t)atomic_load_explicit(
                           &s_manager.control_result, memory_order_acquire);
    if (result == ESP_OK)
    {
        if (_manager_subscribe_wifi_events() != ESP_OK)
        {
            LOG_W("wifi event subscription unavailable; using fallback poll");
            s_manager.wifi_status_subscription = EVENT_BUS_SUB_HANDLE_INVALID;
            s_manager.wifi_scan_subscription = EVENT_BUS_SUB_HANDLE_INVALID;
        }
        atomic_store_explicit(&s_manager_lifecycle,
                              MANAGER_LIFECYCLE_RUNNING,
                              memory_order_release);
    }
    else
    {
        const manager_deadline_t stop_deadline = _manager_deadline(
                CONNECTIVITY_MANAGER_WAIT_FOREVER);
        (void)_manager_wait_worker_suspended(&stop_deadline);
        _manager_release_resources();
        atomic_store_explicit(&s_manager_lifecycle,
                              MANAGER_LIFECYCLE_OFFLINE,
                              memory_order_release);
    }
    return result;
}

esp_err_t connectivity_manager_deinit(uint32_t timeout_ms)
{
    const manager_deadline_t deadline = _manager_deadline(timeout_ms);
    int lifecycle = atomic_load_explicit(&s_manager_lifecycle,
                                         memory_order_acquire);
    if (lifecycle == MANAGER_LIFECYCLE_OFFLINE)
    {
        return ESP_OK;
    }
    if (lifecycle == MANAGER_LIFECYCLE_RUNNING)
    {
        if (!atomic_compare_exchange_strong_explicit(
                    &s_manager_lifecycle, &lifecycle,
                    MANAGER_LIFECYCLE_STOPPING,
                    memory_order_acq_rel, memory_order_acquire) &&
                lifecycle != MANAGER_LIFECYCLE_STOPPING)
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    else if (lifecycle != MANAGER_LIFECYCLE_STOPPING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_exchange_explicit(&s_manager_deinit_active, true,
                                 memory_order_acq_rel))
    {
        return ESP_ERR_INVALID_STATE;
    }
    while (atomic_load_explicit(&s_manager_api_users,
                                memory_order_acquire) != 0U)
    {
        if (_manager_deadline_remaining(&deadline) == 0U)
        {
            atomic_store_explicit(&s_manager_deinit_active, false,
                                  memory_order_release);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1U);
    }
    esp_err_t result = ESP_OK;
    if (!atomic_load_explicit(&s_manager.worker_stopping,
                              memory_order_acquire))
    {
        result = _manager_control(
                     MANAGER_COMMAND_DEINIT,
                     _manager_deadline_remaining_ms(&deadline));
    }
    if (result == ESP_OK)
    {
        result = _manager_wait_worker_suspended(&deadline);
    }
    if (result == ESP_OK)
    {
        _manager_release_resources();
        atomic_store_explicit(&s_manager_deinit_active, false,
                              memory_order_release);
        atomic_store_explicit(&s_manager_lifecycle,
                              MANAGER_LIFECYCLE_OFFLINE,
                              memory_order_release);
        return result;
    }
    atomic_store_explicit(&s_manager_deinit_active, false,
                          memory_order_release);
    return result;
}

esp_err_t connectivity_manager_suspend(uint32_t timeout_ms)
{
    if (atomic_load_explicit(&s_manager_lifecycle, memory_order_acquire) ==
            MANAGER_LIFECYCLE_OFFLINE)
    {
        return ESP_OK;
    }
    if (!_manager_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = _manager_control(MANAGER_COMMAND_SUSPEND,
        timeout_ms);
    _manager_api_release();
    return result;
}

esp_err_t connectivity_manager_resume(uint32_t timeout_ms)
{
    if (atomic_load_explicit(&s_manager_lifecycle, memory_order_acquire) ==
            MANAGER_LIFECYCLE_OFFLINE)
    {
        return ESP_OK;
    }
    if (!_manager_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = _manager_control(MANAGER_COMMAND_RESUME,
        timeout_ms);
    _manager_api_release();
    return result;
}

esp_err_t connectivity_manager_request_scan(
    connectivity_manager_operation_id_t *out_operation_id)
{
    if (out_operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    manager_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = MANAGER_COMMAND_SCAN;
    return _manager_submit(&command, out_operation_id);
}

static bool _manager_credentials_valid(
    const connectivity_manager_credentials_t *credentials)
{
    return credentials != NULL && _manager_credentials_policy_valid(
               credentials->ssid, credentials->ssid_length,
               credentials->password, credentials->password_length,
               credentials->security);
}

/** @brief Copy caller credentials into one command entry. */
static void _manager_entry_from_credentials(
    manager_profile_entry_t *entry,
    const connectivity_manager_credentials_t *credentials)
{
    memset(entry, 0, sizeof(*entry));
    entry->security = (uint8_t)credentials->security;
    entry->ssid_length = (uint8_t)credentials->ssid_length;
    entry->password_length = (uint8_t)credentials->password_length;
    memcpy(entry->ssid, credentials->ssid, credentials->ssid_length);
    if (credentials->password_length > 0U)
    {
        memcpy(entry->password, credentials->password,
               credentials->password_length);
    }
}

esp_err_t connectivity_manager_request_connect(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_operation_id_t *out_operation_id)
{
    if (!_manager_credentials_valid(credentials) || out_operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    manager_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = MANAGER_COMMAND_CONNECT;
    _manager_entry_from_credentials(&command.credentials, credentials);
    return _manager_submit(&command, out_operation_id);
}

esp_err_t connectivity_manager_request_sync_profile(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_client_sync_id_t client_sync_id,
    connectivity_manager_operation_id_t *out_operation_id)
{
    if (!_manager_credentials_valid(credentials) || client_sync_id == 0U ||
            out_operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    manager_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = MANAGER_COMMAND_SYNC_PROFILE;
    command.client_sync_id = client_sync_id;
    _manager_entry_from_credentials(&command.credentials, credentials);
    return _manager_submit(&command, out_operation_id);
}

esp_err_t connectivity_manager_request_save_profile(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_operation_id_t *out_operation_id)
{
    if (!_manager_credentials_valid(credentials) || out_operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    manager_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = MANAGER_COMMAND_SAVE_PROFILE;
    _manager_entry_from_credentials(&command.credentials, credentials);
    return _manager_submit(&command, out_operation_id);
}

/** @brief Validate a borrowed SSID passed to a name-addressed command. */
static bool _manager_ssid_argument_valid(const char *ssid, size_t ssid_length)
{
    return ssid != NULL && ssid_length > 0U &&
           ssid_length <= CONNECTIVITY_MANAGER_SSID_MAX_BYTES &&
           memchr(ssid, '\0', ssid_length) == NULL;
}

esp_err_t connectivity_manager_request_connect_network(
    const char *ssid, size_t ssid_length,
    connectivity_manager_operation_id_t *out_operation_id)
{
    if (!_manager_ssid_argument_valid(ssid, ssid_length) ||
            out_operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    manager_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = MANAGER_COMMAND_CONNECT_NETWORK;
    command.credentials.ssid_length = (uint8_t)ssid_length;
    memcpy(command.credentials.ssid, ssid, ssid_length);
    return _manager_submit(&command, out_operation_id);
}

esp_err_t connectivity_manager_request_forget_network(
    const char *ssid, size_t ssid_length,
    connectivity_manager_operation_id_t *out_operation_id)
{
    if (!_manager_ssid_argument_valid(ssid, ssid_length) ||
            out_operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    manager_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = MANAGER_COMMAND_FORGET_NETWORK;
    command.credentials.ssid_length = (uint8_t)ssid_length;
    memcpy(command.credentials.ssid, ssid, ssid_length);
    return _manager_submit(&command, out_operation_id);
}

static esp_err_t _manager_submit_simple(manager_command_type_t type,
                                        connectivity_manager_operation_id_t *out_operation_id)
{
    if (out_operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    manager_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = type;
    return _manager_submit(&command, out_operation_id);
}

esp_err_t connectivity_manager_request_wifi_disable(
    connectivity_manager_operation_id_t *out_operation_id)
{
    return _manager_submit_simple(MANAGER_COMMAND_DISCONNECT,
                                  out_operation_id);
}

esp_err_t connectivity_manager_request_wifi_enable(
    connectivity_manager_operation_id_t *out_operation_id)
{
    return _manager_submit_simple(MANAGER_COMMAND_RECONNECT,
                                  out_operation_id);
}

esp_err_t connectivity_manager_request_forget(
    connectivity_manager_operation_id_t *out_operation_id)
{
    return _manager_submit_simple(MANAGER_COMMAND_FORGET, out_operation_id);
}

esp_err_t connectivity_manager_cancel(
    connectivity_manager_operation_id_t operation_id)
{
    if (operation_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    manager_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = MANAGER_COMMAND_CANCEL;
    command.operation_id = operation_id;
    return _manager_submit(&command, NULL);
}

esp_err_t connectivity_manager_get_status(
    connectivity_manager_status_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_manager_api_acquire())
    {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
        snapshot->failure =
            CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
        snapshot->last_error = ESP_ERR_INVALID_STATE;
        return ESP_OK;
    }
    xSemaphoreTake(s_manager.mutex, portMAX_DELAY);
    *snapshot = s_manager.status_cache;
    xSemaphoreGive(s_manager.mutex);
    _manager_api_release();
    return ESP_OK;
}

esp_err_t connectivity_manager_get_scan_snapshot(
    connectivity_manager_scan_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_manager_api_acquire())
    {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->last_error = ESP_ERR_INVALID_STATE;
        return ESP_OK;
    }
    xSemaphoreTake(s_manager.mutex, portMAX_DELAY);
    *snapshot = s_manager.scan_cache;
    xSemaphoreGive(s_manager.mutex);
    _manager_api_release();
    return ESP_OK;
}

bool connectivity_manager_is_available(void)
{
    return atomic_load_explicit(&s_manager_lifecycle,
                                memory_order_acquire) ==
           MANAGER_LIFECYCLE_RUNNING;
}

esp_err_t connectivity_manager_get_profiles(
    connectivity_manager_profile_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_manager_api_acquire())
    {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->active_index = CONNECTIVITY_MANAGER_NO_ACTIVE_PROFILE;
        return ESP_OK;
    }
    xSemaphoreTake(s_manager.mutex, portMAX_DELAY);
    *snapshot = s_manager.profile_cache;
    xSemaphoreGive(s_manager.mutex);
    _manager_api_release();
    return ESP_OK;
}
