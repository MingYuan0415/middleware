#define DBG_TAG "connectivity"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "connectivity_manager.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nv_storage.h"
#include "wifi_service.h"

#ifndef CONNECTIVITY_MANAGER_RETRY_DELAY_1_MS
    #define CONNECTIVITY_MANAGER_RETRY_DELAY_1_MS 30000U
#endif
#ifndef CONNECTIVITY_MANAGER_RETRY_DELAY_2_MS
    #define CONNECTIVITY_MANAGER_RETRY_DELAY_2_MS 120000U
#endif
#ifndef CONNECTIVITY_MANAGER_RETRY_DELAY_3_MS
    #define CONNECTIVITY_MANAGER_RETRY_DELAY_3_MS 600000U
#endif
#ifndef CONNECTIVITY_MANAGER_RETRY_DELAY_MAX_MS
    #define CONNECTIVITY_MANAGER_RETRY_DELAY_MAX_MS 1800000U
#endif

#define CONNECTIVITY_MANAGER_POLL_MS        20U
#define CONNECTIVITY_MANAGER_PROFILE_KEY    "wifi_profile"
#define CONNECTIVITY_MANAGER_PROFILE_MAGIC  UINT32_C(0x57465031)
#define CONNECTIVITY_MANAGER_PROFILE_VERSION UINT16_C(1)

EVENT_BUS_DEFINE_ID(CONNECTIVITY_MANAGER_MSG);

typedef enum
{
    MANAGER_COMMAND_INIT = 0,
    MANAGER_COMMAND_SCAN,
    MANAGER_COMMAND_CONNECT,
    MANAGER_COMMAND_DISCONNECT,
    MANAGER_COMMAND_RECONNECT,
    MANAGER_COMMAND_FORGET,
    MANAGER_COMMAND_SET_AUTO_CONNECT,
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

typedef struct manager_profile
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
    uint8_t trailing_reserved[4];
} manager_profile_t;

typedef struct manager_command
{
    manager_command_type_t type;
    connectivity_manager_operation_id_t operation_id;
    uint64_t control_generation;
    uint32_t timeout_ms;
    bool enabled;
    manager_profile_t credentials;
} manager_command_t;

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
    manager_profile_t profile;
    manager_profile_t target;
    bool profile_valid;
    bool target_valid;
    bool target_candidate;
    bool target_persisted;
    bool target_reconnectable;
    bool radio_initialized;
    bool suspended;
    bool retry_pending;
    bool resume_connect_pending;
    connectivity_manager_operation_id_t resume_operation_id;
    bool pending_command_valid;
    bool resume_auto_after_scan;
    manager_command_t pending_command;
    TickType_t retry_deadline;
    uint8_t retry_count;
    connectivity_manager_status_snapshot_t status;
    connectivity_manager_scan_snapshot_t scan;
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
                      sizeof(manager_command_t)];
        uintptr_t alignment;
    } queue_storage;
    TaskHandle_t worker;
    StaticTask_t worker_control;
    StackType_t worker_stack[CONFIG_CONNECTIVITY_MANAGER_TASK_STACK];
    connectivity_manager_config_t config;
    bool control_inflight;
    manager_command_type_t control_type;
    uint64_t control_generation;
    atomic_uint_fast64_t control_completed_generation;
    atomic_int control_result;
    connectivity_manager_status_snapshot_t status_cache;
    connectivity_manager_scan_snapshot_t scan_cache;
} manager_shared_t;

static manager_shared_t s_manager;
static atomic_int s_manager_lifecycle =
    ATOMIC_VAR_INIT(MANAGER_LIFECYCLE_OFFLINE);
static atomic_uint s_manager_api_users = ATOMIC_VAR_INIT(0U);
static atomic_bool s_manager_deinit_active = ATOMIC_VAR_INIT(false);

_Static_assert(CONNECTIVITY_MANAGER_SSID_MAX_BYTES ==
               WIFI_SERVICE_SSID_MAX_BYTES, "SSID limits must match");
_Static_assert(CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES ==
               WIFI_SERVICE_PASSWORD_MAX_BYTES, "password limits must match");
_Static_assert(CONNECTIVITY_MANAGER_MAX_SCAN_RECORDS ==
               WIFI_SERVICE_MAX_SCAN_RECORDS, "scan limits must match");
_Static_assert(sizeof(manager_profile_t) == 112U,
               "Wi-Fi profile record size changed");
_Static_assert(sizeof(connectivity_manager_status_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE, "status snapshot too large");
_Static_assert(sizeof(connectivity_manager_scan_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE, "scan snapshot too large");

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

static bool _manager_profile_valid(const manager_profile_t *profile)
{
    if (profile->magic != CONNECTIVITY_MANAGER_PROFILE_MAGIC ||
            profile->version != CONNECTIVITY_MANAGER_PROFILE_VERSION ||
            profile->auto_connect > 1U || profile->ssid_length == 0U ||
            profile->ssid_length > CONNECTIVITY_MANAGER_SSID_MAX_BYTES ||
            profile->security > CONNECTIVITY_MANAGER_SECURITY_PERSONAL ||
            !_manager_bytes_are_zero(profile->reserved,
                                     sizeof(profile->reserved)) ||
            !_manager_bytes_are_zero(profile->trailing_reserved,
                                     sizeof(profile->trailing_reserved)) ||
            memchr(profile->ssid, '\0', profile->ssid_length) != NULL)
    {
        return false;
    }
    if (profile->security == CONNECTIVITY_MANAGER_SECURITY_OPEN)
    {
        return profile->password_length == 0U &&
               _manager_bytes_are_zero(profile->ssid + profile->ssid_length,
                                       sizeof(profile->ssid) -
                                       profile->ssid_length) &&
               _manager_bytes_are_zero(profile->password,
                                       sizeof(profile->password));
    }
    return profile->password_length >= 8U &&
           profile->password_length <=
           CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES &&
           memchr(profile->password, '\0', profile->password_length) == NULL &&
           _manager_bytes_are_zero(profile->ssid + profile->ssid_length,
                                   sizeof(profile->ssid) -
                                   profile->ssid_length) &&
           _manager_bytes_are_zero(
               profile->password + profile->password_length,
               sizeof(profile->password) - profile->password_length);
}

static esp_err_t _manager_profile_load(manager_worker_t *worker)
{
    manager_profile_t profile;
    memset(&profile, 0, sizeof(profile));
    size_t size = sizeof(profile);
    esp_err_t result = nv_storage_get_blob(CONNECTIVITY_MANAGER_PROFILE_KEY,
                                           &profile, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }
    if (result != ESP_OK || size != sizeof(profile) ||
            !_manager_profile_valid(&profile))
    {
        _manager_secure_zero(&profile, sizeof(profile));
        return result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result;
    }
    worker->profile = profile;
    worker->profile_valid = true;
    _manager_secure_zero(&profile, sizeof(profile));
    return ESP_OK;
}

static esp_err_t _manager_profile_store(const manager_profile_t *profile)
{
    return nv_storage_set_blob(CONNECTIVITY_MANAGER_PROFILE_KEY, profile,
                               sizeof(*profile));
}

static void _manager_publish_status(
    manager_worker_t *worker,
    const connectivity_manager_status_snapshot_t *snapshot, uint32_t flags)
{
    connectivity_manager_status_snapshot_t published = *snapshot;
    published.generation = _manager_next_generation();
    worker->status.generation = published.generation;
    xSemaphoreTake(s_manager.mutex, portMAX_DELAY);
    s_manager.status_cache = published;
    xSemaphoreGive(s_manager.mutex);
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
    worker->status.operation_id =
        worker->operation_id != 0U ? worker->operation_id : 0U;
    worker->status.operation_complete = false;
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
    terminal.last_error = result;
    terminal.failure = failure;
    _manager_publish_status(worker, &terminal, 0U);
}

static void _manager_publish_scan(
    manager_worker_t *worker,
    const connectivity_manager_scan_snapshot_t *snapshot, uint32_t flags)
{
    connectivity_manager_scan_snapshot_t published = *snapshot;
    published.generation = _manager_next_generation();
    worker->scan.generation = published.generation;
    xSemaphoreTake(s_manager.mutex, portMAX_DELAY);
    s_manager.scan_cache = published;
    xSemaphoreGive(s_manager.mutex);
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
    _manager_publish_scan(worker, &worker->scan,
                          worker->scan.running ?
                          EVENT_BUS_PUBLISH_FLAG_UI_LATEST : 0U);
}

static void _manager_publish_scan_terminal(
    manager_worker_t *worker,
    connectivity_manager_operation_id_t operation_id, esp_err_t result)
{
    connectivity_manager_scan_snapshot_t terminal;
    memset(&terminal, 0, sizeof(terminal));
    terminal.operation_id = operation_id;
    terminal.last_error = result;
    _manager_publish_scan(worker, &terminal, 0U);
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
    worker->status.saved_profile = worker->profile_valid;
    worker->status.auto_connect = worker->profile_valid &&
                                  worker->profile.auto_connect != 0U;
    worker->status.profile_persisted = worker->target_valid &&
                                       worker->target_persisted;
    if (worker->target_valid)
    {
        _manager_copy_ssid(worker->status.ssid, worker->target.ssid,
                           worker->target.ssid_length);
    }
    else if (worker->profile_valid)
    {
        _manager_copy_ssid(worker->status.ssid, worker->profile.ssid,
                           worker->profile.ssid_length);
    }
    else
    {
        worker->status.ssid[0] = '\0';
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
        return 4U;
    case MANAGER_COMMAND_DISCONNECT:
        return 3U;
    case MANAGER_COMMAND_CONNECT:
    case MANAGER_COMMAND_RECONNECT:
        return 2U;
    case MANAGER_COMMAND_SCAN:
        return 1U;
    default:
        return 0U;
    }
}

static manager_defer_result_t _manager_defer_foreground(
    manager_worker_t *worker, const manager_command_t *command)
{
    if (worker->operation_kind == MANAGER_OPERATION_NONE)
    {
        return MANAGER_DEFER_NONE;
    }
    const unsigned command_priority =
        _manager_command_priority(command->type);
    if (command_priority <
            _manager_command_priority(worker->active_command) ||
            (worker->pending_command_valid && command_priority <
             _manager_command_priority(worker->pending_command.type)))
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
    if (command->type == MANAGER_COMMAND_SCAN &&
            worker->active_command == MANAGER_COMMAND_AUTO)
    {
        worker->resume_auto_after_scan = true;
    }
    if (worker->active_cancel_requested)
    {
        return MANAGER_DEFER_QUEUED;
    }
    const esp_err_t result = wifi_service_cancel(
                                 worker->session_id,
                                 worker->service_operation_id);
    if (result != ESP_OK)
    {
        _manager_publish_command_terminal(
            worker, &worker->pending_command, result,
            CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
        _manager_secure_zero(&worker->pending_command,
                             sizeof(worker->pending_command));
        worker->pending_command_valid = false;
        return MANAGER_DEFER_REJECTED;
    }
    worker->active_cancel_requested = true;
    return MANAGER_DEFER_QUEUED;
}

static void _manager_run_pending(manager_worker_t *worker);
static void _manager_continue_policy(manager_worker_t *worker,
                                     bool schedule_retry);

static bool _manager_auto_connect_eligible(const manager_worker_t *worker)
{
    return !worker->suspended && !worker->status.manual_hold &&
           worker->target_valid && worker->target_reconnectable &&
           (worker->target_candidate ||
            (worker->profile_valid && worker->profile.auto_connect != 0U));
}

static esp_err_t _manager_open_session(manager_worker_t *worker)
{
    esp_err_t result = wifi_service_session_open(&worker->session_id);
    if (result == ESP_OK)
    {
        worker->service_operation_id = 0U;
    }
    return result;
}

static esp_err_t _manager_radio_start(manager_worker_t *worker)
{
    if (worker->radio_initialized && wifi_service_is_available())
    {
        return worker->session_id != 0U ? ESP_OK :
               _manager_open_session(worker);
    }

    const wifi_service_config_t config =
    {
        .task_priority = s_manager.config.wifi_task_priority,
    };
    esp_err_t result = wifi_service_init(&config);
    if (result == ESP_OK)
    {
        worker->radio_initialized = true;
        result = _manager_open_session(worker);
    }
    worker->status.radio_available = result == ESP_OK;
    return result;
}

static uint32_t _manager_retry_delay(uint8_t retry_count)
{
    static const uint32_t delays[] =
    {
        CONNECTIVITY_MANAGER_RETRY_DELAY_1_MS,
        CONNECTIVITY_MANAGER_RETRY_DELAY_2_MS,
        CONNECTIVITY_MANAGER_RETRY_DELAY_3_MS,
        CONNECTIVITY_MANAGER_RETRY_DELAY_MAX_MS,
    };
    size_t index = retry_count;
    if (index >= sizeof(delays) / sizeof(delays[0]))
    {
        index = sizeof(delays) / sizeof(delays[0]) - 1U;
    }
    return delays[index];
}

static void _manager_schedule_retry(manager_worker_t *worker)
{
    if (!_manager_auto_connect_eligible(worker))
    {
        worker->retry_pending = false;
        return;
    }
    const uint32_t delay_ms = _manager_retry_delay(worker->retry_count);
    worker->retry_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    worker->retry_pending = true;
    if (worker->retry_count < UINT8_MAX)
    {
        ++worker->retry_count;
    }
    worker->status.state = CONNECTIVITY_MANAGER_STATE_RETRY_WAIT;
    worker->status.retry_delay_ms = delay_ms;
    worker->status.retry_count = worker->retry_count;
    _manager_cache_status(worker);
}

static esp_err_t _manager_start_connect(manager_worker_t *worker,
                                        connectivity_manager_operation_id_t operation_id,
                                        manager_command_type_t command_type)
{
    if (!worker->target_valid || !worker->target_reconnectable)
    {
        _manager_publish_status_terminal(
            worker, operation_id, ESP_ERR_INVALID_STATE,
            CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = _manager_radio_start(worker);
    if (result != ESP_OK)
    {
        worker->status.radio_available = false;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_OFFLINE;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
        worker->status.last_error = result;
        _manager_set_target_status(worker);
        _manager_schedule_retry(worker);
        if (!worker->retry_pending)
        {
            _manager_cache_status(worker);
        }
        _manager_publish_status_terminal(
            worker, operation_id, result,
            CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE);
        return result;
    }
    const wifi_service_credentials_t credentials =
    {
        .ssid = (const char *)worker->target.ssid,
        .ssid_length = worker->target.ssid_length,
        .password = (const char *)worker->target.password,
        .password_length = worker->target.password_length,
        .security = worker->target.security ==
        CONNECTIVITY_MANAGER_SECURITY_PERSONAL ?
        WIFI_SERVICE_SECURITY_PERSONAL :
        WIFI_SERVICE_SECURITY_OPEN,
    };
    result = wifi_service_request_connect(worker->session_id, &credentials,
                                          &worker->service_operation_id);
    if (result == ESP_OK)
    {
        worker->operation_kind = MANAGER_OPERATION_CONNECT;
        worker->active_command = command_type;
        worker->operation_id = operation_id;
        worker->active_cancel_requested = false;
        worker->retry_pending = false;
        worker->status.operation_id = operation_id;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_CONNECTING;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        worker->status.last_error = ESP_OK;
        worker->status.retry_delay_ms = 0U;
        worker->status.manual_hold = false;
        _manager_set_target_status(worker);
        _manager_cache_status(worker);
    }
    else
    {
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_INTERNAL;
        worker->status.last_error = result;
        _manager_set_target_status(worker);
        _manager_schedule_retry(worker);
        if (!worker->retry_pending)
        {
            _manager_cache_status(worker);
        }
        _manager_publish_status_terminal(
            worker, operation_id, result,
            CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
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
        if (worker->target_candidate)
        {
            _manager_secure_zero(&worker->target, sizeof(worker->target));
            worker->target_valid = false;
            worker->target_candidate = false;
            worker->target_persisted = false;
            worker->target_reconnectable = false;
        }
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.ipv4_address = 0U;
        _manager_set_target_status(worker);
        _manager_complete_status_operation(
            worker, ESP_ERR_NOT_FINISHED,
            CONNECTIVITY_MANAGER_FAILURE_NONE);
        _manager_continue_policy(worker, false);
        return;
    }
    if (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
            worker->target_candidate)
    {
        manager_profile_t saved = worker->target;
        saved.magic = CONNECTIVITY_MANAGER_PROFILE_MAGIC;
        saved.version = CONNECTIVITY_MANAGER_PROFILE_VERSION;
        saved.auto_connect = 1U;
        esp_err_t result = _manager_profile_store(&saved);
        if (result == ESP_OK)
        {
            _manager_secure_zero(&worker->profile, sizeof(worker->profile));
            worker->profile = saved;
            worker->profile_valid = true;
            worker->target_candidate = false;
            worker->target_persisted = true;
            worker->target_reconnectable = true;
        }
        else
        {
            worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
            worker->status.last_error = result;
            _manager_secure_zero(worker->target.password,
                                 sizeof(worker->target.password));
            worker->target_candidate = false;
            worker->target_persisted = false;
            worker->target_reconnectable = false;
        }
        _manager_secure_zero(&saved, sizeof(saved));
    }
    worker->status.state = CONNECTIVITY_MANAGER_STATE_IP_READY;
    worker->status.ipv4_address = status->ipv4_address;
    _manager_set_target_status(worker);
    if (worker->operation_kind == MANAGER_OPERATION_CONNECT)
    {
        _manager_complete_status_operation(
            worker, worker->status.last_error, worker->status.failure);
        _manager_continue_policy(worker, false);
    }
}

static void _manager_handle_connect_terminal(manager_worker_t *worker,
        const wifi_service_status_snapshot_t *status)
{
    const bool canceled = worker->active_cancel_requested;
    const connectivity_manager_failure_t failure = canceled ?
        CONNECTIVITY_MANAGER_FAILURE_NONE :
        _manager_map_failure(status->failure);
    const esp_err_t result = canceled ? ESP_ERR_NOT_FINISHED :
                             status->last_error;
    worker->status.state = status->state == WIFI_SERVICE_STATE_OFFLINE ?
                           CONNECTIVITY_MANAGER_STATE_OFFLINE :
                           CONNECTIVITY_MANAGER_STATE_IDLE;
    worker->status.ipv4_address = 0U;
    if (failure == CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION)
    {
        worker->retry_pending = false;
        if (worker->target_candidate)
        {
            _manager_secure_zero(&worker->target, sizeof(worker->target));
            worker->target_valid = false;
            worker->target_candidate = false;
            worker->target_persisted = false;
            worker->target_reconnectable = false;
        }
        _manager_set_target_status(worker);
    }
    else
    {
        _manager_set_target_status(worker);
    }
    const bool should_retry = !canceled &&
                              failure !=
                              CONNECTIVITY_MANAGER_FAILURE_AUTHENTICATION &&
                              !worker->pending_command_valid &&
                              !worker->resume_auto_after_scan;
    if (should_retry)
    {
        _manager_schedule_retry(worker);
    }
    _manager_complete_status_operation(worker, result, failure);
    _manager_continue_policy(worker, false);
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
    worker->status.state = _manager_map_state(status.state);
    worker->status.failure = _manager_map_failure(status.failure);
    worker->status.last_error = status.last_error;
    worker->status.ipv4_address = status.ipv4_address;
    const bool executor_retry_allowed =
        _manager_auto_connect_eligible(worker);
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
    if (status.state == WIFI_SERVICE_STATE_IP_READY &&
            (worker->operation_kind == MANAGER_OPERATION_NONE ||
             (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
              status.operation_id == worker->service_operation_id)))
    {
        const bool active_connect =
            worker->operation_kind == MANAGER_OPERATION_CONNECT;
        _manager_handle_connected(worker, &status);
        if (active_connect)
        {
            return;
        }
    }
    else if (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
             status.operation_id == worker->service_operation_id &&
             ((status.state == WIFI_SERVICE_STATE_IDLE &&
               !status.desired_connected) ||
              status.state == WIFI_SERVICE_STATE_OFFLINE))
    {
        _manager_handle_connect_terminal(worker, &status);
        return;
    }
    else if (worker->operation_kind == MANAGER_OPERATION_DISCONNECT &&
             status.operation_id == worker->service_operation_id &&
             (status.state == WIFI_SERVICE_STATE_IDLE ||
              status.state == WIFI_SERVICE_STATE_OFFLINE))
    {
        const bool canceled = worker->active_cancel_requested;
        const esp_err_t result = canceled ? ESP_ERR_NOT_FINISHED :
                                 status.last_error;
        const connectivity_manager_failure_t failure = canceled ?
            CONNECTIVITY_MANAGER_FAILURE_NONE :
            _manager_map_failure(status.failure);
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.ipv4_address = 0U;
        _manager_set_target_status(worker);
        _manager_complete_status_operation(worker, result, failure);
        _manager_continue_policy(worker, false);
        return;
    }
    else if (worker->operation_kind == MANAGER_OPERATION_NONE &&
             status.state == WIFI_SERVICE_STATE_IDLE &&
             _manager_auto_connect_eligible(worker) &&
             status.failure != WIFI_SERVICE_FAILURE_NONE)
    {
        _manager_schedule_retry(worker);
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
    worker->scan.last_error = worker->active_cancel_requested ||
                              scan.state == WIFI_SERVICE_SCAN_CANCELED ?
                              ESP_ERR_NOT_FINISHED : scan.last_error;
    worker->scan.running = scan.state == WIFI_SERVICE_SCAN_RUNNING;
    worker->scan.truncated = scan.truncated;
    worker->scan.record_count = scan.record_count;
    memset(worker->scan.records, 0, sizeof(worker->scan.records));
    for (size_t index = 0U; index < scan.record_count; ++index)
    {
        connectivity_manager_scan_record_t *record =
            &worker->scan.records[index];
        memcpy(record->ssid, scan.records[index].ssid,
               sizeof(record->ssid));
        record->rssi = scan.records[index].rssi;
        record->channel = scan.records[index].channel;
        record->security = _manager_map_security(scan.records[index].security);
        const size_t ssid_length = strlen(record->ssid);
        record->saved = worker->profile_valid &&
                        ssid_length == worker->profile.ssid_length &&
                        memcmp(record->ssid, worker->profile.ssid,
                               ssid_length) == 0;
    }
    if (worker->operation_kind == MANAGER_OPERATION_SCAN &&
            scan.operation_id == worker->service_operation_id &&
            scan.state != WIFI_SERVICE_SCAN_RUNNING)
    {
        _manager_cache_scan(worker);
        _manager_clear_active_operation(worker);
        _manager_continue_policy(worker, false);
        return;
    }
    _manager_cache_scan(worker);
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
    if (worker->retry_pending)
    {
        worker->resume_auto_after_scan = true;
        worker->retry_pending = false;
    }
    worker->retry_pending = false;
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
        _manager_publish_scan_terminal(worker, command->operation_id, result);
        _manager_continue_policy(worker, false);
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
    worker->resume_auto_after_scan = false;
    if (defer_result == MANAGER_DEFER_QUEUED)
    {
        return;
    }
    worker->retry_pending = false;
    worker->retry_count = 0U;
    _manager_secure_zero(&worker->target, sizeof(worker->target));
    worker->target = command->credentials;
    worker->target_valid = true;
    worker->target_candidate = true;
    worker->target_persisted = false;
    worker->target_reconnectable = true;
    worker->retry_count = 0U;
    worker->status.manual_hold = false;
    (void)_manager_start_connect(worker, command->operation_id,
                                 MANAGER_COMMAND_CONNECT);
}

static void _manager_command_disconnect(manager_worker_t *worker,
                                        const manager_command_t *command)
{
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);
    if (defer_result == MANAGER_DEFER_REJECTED)
    {
        return;
    }
    worker->resume_auto_after_scan = false;
    worker->retry_pending = false;
    worker->status.manual_hold = true;
    if (worker->target_candidate)
    {
        _manager_secure_zero(&worker->target, sizeof(worker->target));
        worker->target_valid = false;
        worker->target_candidate = false;
        worker->target_persisted = false;
        worker->target_reconnectable = false;
    }
    if (defer_result == MANAGER_DEFER_QUEUED)
    {
        _manager_set_target_status(worker);
        _manager_cache_status(worker);
        return;
    }
    esp_err_t result = _manager_radio_start(worker);
    if (result == ESP_OK)
    {
        result = wifi_service_request_disconnect(
                     worker->session_id, &worker->service_operation_id);
    }
    if (result == ESP_OK)
    {
        worker->operation_kind = MANAGER_OPERATION_DISCONNECT;
        worker->active_command = command->type;
        worker->operation_id = command->operation_id;
        worker->active_cancel_requested = false;
        worker->status.last_error = ESP_OK;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
        _manager_set_target_status(worker);
        _manager_cache_status(worker);
    }
    else
    {
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.ipv4_address = 0U;
        worker->status.last_error = result;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_INTERNAL;
        _manager_set_target_status(worker);
        _manager_publish_status_terminal(
            worker, command->operation_id, result,
            CONNECTIVITY_MANAGER_FAILURE_INTERNAL);
    }
}

static void _manager_command_reconnect(manager_worker_t *worker,
                                       const manager_command_t *command)
{
    const manager_defer_result_t defer_result =
        _manager_defer_foreground(worker, command);
    if (defer_result == MANAGER_DEFER_REJECTED)
    {
        return;
    }
    worker->resume_auto_after_scan = false;
    if (defer_result == MANAGER_DEFER_QUEUED)
    {
        return;
    }
    if (!worker->profile_valid)
    {
        worker->status.last_error = ESP_ERR_NOT_FOUND;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_ERR_NOT_FOUND,
            CONNECTIVITY_MANAGER_FAILURE_STORAGE);
        return;
    }
    worker->retry_pending = false;
    worker->retry_count = 0U;
    _manager_secure_zero(&worker->target, sizeof(worker->target));
    worker->target = worker->profile;
    worker->target_valid = true;
    worker->target_candidate = false;
    worker->target_persisted = true;
    worker->target_reconnectable = true;
    worker->retry_count = 0U;
    worker->status.manual_hold = false;
    (void)_manager_start_connect(worker, command->operation_id,
                                 MANAGER_COMMAND_RECONNECT);
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
    worker->resume_auto_after_scan = false;
    worker->retry_pending = false;
    if (worker->target_candidate)
    {
        _manager_secure_zero(&worker->target, sizeof(worker->target));
        worker->target_valid = false;
        worker->target_candidate = false;
        worker->target_persisted = false;
        worker->target_reconnectable = false;
    }
    if (defer_result == MANAGER_DEFER_QUEUED)
    {
        _manager_set_target_status(worker);
        _manager_cache_status(worker);
        return;
    }

    esp_err_t result = nv_storage_erase_key(CONNECTIVITY_MANAGER_PROFILE_KEY);
    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        result = ESP_OK;
    }
    if (result != ESP_OK)
    {
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
        worker->status.last_error = result;
        _manager_publish_status_terminal(
            worker, command->operation_id, result,
            CONNECTIVITY_MANAGER_FAILURE_STORAGE);
        return;
    }
    _manager_secure_zero(&worker->profile, sizeof(worker->profile));
    _manager_secure_zero(&worker->target, sizeof(worker->target));
    worker->profile_valid = false;
    worker->target_valid = false;
    worker->target_candidate = false;
    worker->target_persisted = false;
    worker->target_reconnectable = false;
    _manager_command_disconnect(worker, command);
}

static void _manager_command_set_auto(manager_worker_t *worker,
                                      const manager_command_t *command)
{
    if (!worker->profile_valid)
    {
        worker->status.last_error = ESP_ERR_NOT_FOUND;
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
        _manager_publish_status_terminal(
            worker, command->operation_id, ESP_ERR_NOT_FOUND,
            CONNECTIVITY_MANAGER_FAILURE_STORAGE);
        return;
    }
    manager_profile_t changed = worker->profile;
    changed.auto_connect = command->enabled ? 1U : 0U;
    esp_err_t result = _manager_profile_store(&changed);
    if (result == ESP_OK)
    {
        worker->profile = changed;
        worker->retry_pending = false;
        if (command->enabled)
        {
            worker->status.manual_hold = false;
        }
        else
        {
            worker->resume_auto_after_scan = false;
            if (worker->active_command == MANAGER_COMMAND_AUTO &&
                    wifi_service_cancel(worker->session_id,
                                        worker->service_operation_id) == ESP_OK)
            {
                worker->active_cancel_requested = true;
            }
        }
    }
    worker->status.last_error = result;
    worker->status.failure = result == ESP_OK ?
                             CONNECTIVITY_MANAGER_FAILURE_NONE :
                             CONNECTIVITY_MANAGER_FAILURE_STORAGE;
    _manager_set_target_status(worker);
    _manager_publish_status_terminal(
        worker, command->operation_id, result, worker->status.failure);
    if (result == ESP_OK && command->enabled &&
            worker->status.state != CONNECTIVITY_MANAGER_STATE_IP_READY)
    {
        if (worker->operation_kind == MANAGER_OPERATION_NONE)
        {
            _manager_secure_zero(&worker->target, sizeof(worker->target));
            worker->target = worker->profile;
            worker->target_valid = true;
            worker->target_candidate = false;
            worker->target_persisted = true;
            worker->target_reconnectable = true;
            (void)_manager_start_connect(worker, 0U, MANAGER_COMMAND_AUTO);
        }
        else if (worker->operation_kind == MANAGER_OPERATION_SCAN)
        {
            worker->resume_auto_after_scan = true;
        }
    }
    _manager_secure_zero(&changed, sizeof(changed));
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
    if (worker->operation_id != command->operation_id ||
            worker->service_operation_id == 0U)
    {
        return;
    }
    if (wifi_service_cancel(worker->session_id,
                            worker->service_operation_id) != ESP_OK)
    {
        return;
    }
    worker->active_cancel_requested = true;
    if (worker->operation_kind == MANAGER_OPERATION_CONNECT &&
            worker->target_candidate)
    {
        _manager_secure_zero(&worker->target, sizeof(worker->target));
        worker->target_valid = false;
        worker->target_candidate = false;
        worker->target_persisted = false;
        worker->target_reconnectable = false;
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
    switch (command.type)
    {
    case MANAGER_COMMAND_SCAN:
        _manager_command_scan(worker, &command);
        break;
    case MANAGER_COMMAND_CONNECT:
        _manager_command_connect(worker, &command);
        break;
    case MANAGER_COMMAND_RECONNECT:
        _manager_command_reconnect(worker, &command);
        break;
    case MANAGER_COMMAND_DISCONNECT:
        _manager_command_disconnect(worker, &command);
        break;
    case MANAGER_COMMAND_FORGET:
        _manager_command_forget(worker, &command);
        break;
    default:
        break;
    }
    _manager_secure_zero(&command, sizeof(command));
}

static void _manager_continue_policy(manager_worker_t *worker,
                                     bool schedule_retry)
{
    if (worker->pending_command_valid)
    {
        _manager_run_pending(worker);
        return;
    }
    if (worker->resume_auto_after_scan)
    {
        if (worker->suspended)
        {
            return;
        }
        worker->resume_auto_after_scan = false;
        if (_manager_auto_connect_eligible(worker))
        {
            (void)_manager_start_connect(worker, 0U, MANAGER_COMMAND_AUTO);
        }
        return;
    }
    if (schedule_retry)
    {
        _manager_schedule_retry(worker);
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
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_STORAGE;
        worker->status.last_error = storage_result;
    }
    esp_err_t radio_result = _manager_radio_start(worker);
    worker->status.radio_available = radio_result == ESP_OK;
    if (radio_result == ESP_OK)
    {
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        if (storage_result == ESP_OK)
        {
            worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
            worker->status.last_error = ESP_OK;
        }
    }
    else
    {
        worker->status.failure = CONNECTIVITY_MANAGER_FAILURE_RADIO_UNAVAILABLE;
        worker->status.last_error = radio_result;
    }
    _manager_set_target_status(worker);
    _manager_cache_status(worker);
    _manager_cache_scan(worker);
    if (worker->profile_valid && worker->profile.auto_connect != 0U)
    {
        worker->target = worker->profile;
        worker->target_valid = true;
        worker->target_persisted = true;
        worker->target_reconnectable = true;
        (void)_manager_start_connect(worker, 0U, MANAGER_COMMAND_AUTO);
    }
    return ESP_OK;
}

static esp_err_t _manager_worker_suspend(manager_worker_t *worker,
        uint32_t timeout_ms)
{
    const bool reconnect_after_resume = !worker->retry_pending &&
                                        _manager_auto_connect_eligible(worker);
    if (worker->operation_kind == MANAGER_OPERATION_SCAN &&
            reconnect_after_resume)
    {
        worker->resume_auto_after_scan = true;
    }
    worker->resume_connect_pending = reconnect_after_resume &&
                                     worker->operation_kind !=
                                     MANAGER_OPERATION_SCAN;
    worker->resume_operation_id = worker->resume_connect_pending &&
                                  worker->operation_kind ==
                                  MANAGER_OPERATION_CONNECT ?
                                  worker->operation_id : 0U;
    esp_err_t result = worker->radio_initialized ?
                       wifi_service_suspend(timeout_ms) : ESP_OK;
    if (result == ESP_OK)
    {
        worker->suspended = true;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_SUSPENDED;
        worker->status.ipv4_address = 0U;
        _manager_cache_status(worker);
    }
    return result;
}

static esp_err_t _manager_worker_resume(manager_worker_t *worker,
                                        uint32_t timeout_ms)
{
    esp_err_t result = worker->radio_initialized ?
                       wifi_service_resume(timeout_ms) :
                       _manager_radio_start(worker);
    if (result == ESP_OK)
    {
        worker->suspended = false;
        worker->status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
        worker->status.radio_available = true;
        _manager_cache_status(worker);
        if (worker->retry_pending &&
                _manager_tick_reached(xTaskGetTickCount(),
                                      worker->retry_deadline))
        {
            worker->retry_pending = false;
            (void)_manager_start_connect(worker, 0U, MANAGER_COMMAND_AUTO);
        }
        else if (worker->resume_connect_pending)
        {
            worker->resume_connect_pending = false;
            const connectivity_manager_operation_id_t operation_id =
                worker->resume_operation_id;
            worker->resume_operation_id = 0U;
            const manager_command_type_t command_type = operation_id == 0U ?
                MANAGER_COMMAND_AUTO : worker->active_command;
            _manager_clear_active_operation(worker);
            (void)_manager_start_connect(worker, operation_id, command_type);
        }
        else if (worker->operation_kind == MANAGER_OPERATION_NONE &&
                 worker->resume_auto_after_scan)
        {
            _manager_continue_policy(worker, false);
        }
    }
    return result;
}

static esp_err_t _manager_worker_deinit(manager_worker_t *worker,
                                        uint32_t timeout_ms)
{
    worker->retry_pending = false;
    if (worker->operation_kind != MANAGER_OPERATION_NONE)
    {
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
    if (worker->session_id != 0U)
    {
        (void)wifi_service_session_close(worker->session_id);
        worker->session_id = 0U;
    }
    const bool radio_owned = worker->radio_initialized ||
                             wifi_service_is_available() ||
                             wifi_service_is_cleanup_pending();
    esp_err_t result = radio_owned ? wifi_service_deinit(timeout_ms) : ESP_OK;
    if (result == ESP_OK)
    {
        worker->radio_initialized = false;
    }
    _manager_secure_zero(&worker->profile, sizeof(worker->profile));
    _manager_secure_zero(&worker->target, sizeof(worker->target));
    _manager_secure_zero(&worker->pending_command,
                         sizeof(worker->pending_command));
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
    case MANAGER_COMMAND_DISCONNECT:
        _manager_command_disconnect(worker, command);
        break;
    case MANAGER_COMMAND_RECONNECT:
        _manager_command_reconnect(worker, command);
        break;
    case MANAGER_COMMAND_FORGET:
        _manager_command_forget(worker, command);
        break;
    case MANAGER_COMMAND_SET_AUTO_CONNECT:
        _manager_command_set_auto(worker, command);
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
        xSemaphoreGive(s_manager.control_done);
        *stop = control_result == ESP_OK;
        break;
    case MANAGER_COMMAND_INIT:
    case MANAGER_COMMAND_AUTO:
        break;
    }
}

static void _manager_worker_run(void *argument)
{
    (void)argument;
    manager_worker_t worker;
    atomic_store_explicit(&s_manager.control_result,
                          _manager_worker_init(&worker),
                          memory_order_release);
    xSemaphoreGive(s_manager.control_done);
    bool stop = atomic_load_explicit(&s_manager.control_result,
                                     memory_order_acquire) != ESP_OK;
    while (!stop)
    {
        manager_command_t command;
        if (xQueueReceive(s_manager.queue, &command,
                          pdMS_TO_TICKS(CONNECTIVITY_MANAGER_POLL_MS)) == pdTRUE)
        {
            _manager_process_command(&worker, &command, &stop);
            _manager_secure_zero(&command, sizeof(command));
        }
        _manager_poll_status(&worker);
        _manager_poll_scan(&worker);
        if (!worker.suspended && worker.retry_pending &&
                _manager_tick_reached(xTaskGetTickCount(),
                                      worker.retry_deadline))
        {
            worker.retry_pending = false;
            (void)_manager_start_connect(&worker, 0U, MANAGER_COMMAND_AUTO);
        }
    }
    while (true)
    {
        vTaskDelay(portMAX_DELAY);
    }
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
    esp_err_t result = xQueueSend(s_manager.queue, command, 0) == pdTRUE ?
                       ESP_OK : ESP_ERR_NO_MEM;
    if (result == ESP_OK && out_operation_id != NULL)
    {
        *out_operation_id = command->operation_id;
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
    if (xQueueSend(s_manager.queue, &command, 0) != pdTRUE)
    {
        s_manager.control_inflight = false;
        result = ESP_ERR_NO_MEM;
        goto exit;
    }
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
    s_manager.config = *config;
    s_manager.mutex = xSemaphoreCreateMutexStatic(&s_manager.mutex_control);
    s_manager.control_mutex = xSemaphoreCreateMutexStatic(
                                  &s_manager.control_mutex_control);
    s_manager.control_done = xSemaphoreCreateBinaryStatic(
                                 &s_manager.control_done_control);
    s_manager.queue = xQueueCreateStatic(
                          CONFIG_CONNECTIVITY_MANAGER_QUEUE_DEPTH,
                          sizeof(manager_command_t),
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
    s_manager.worker = xTaskCreateStatic(
                           _manager_worker_run, "connectivity",
                           CONFIG_CONNECTIVITY_MANAGER_TASK_STACK, NULL,
                           config->task_priority, s_manager.worker_stack,
                           &s_manager.worker_control);
    if (s_manager.worker == NULL)
    {
        _manager_release_resources();
        atomic_store_explicit(&s_manager_lifecycle,
                              MANAGER_LIFECYCLE_OFFLINE,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
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
        atomic_store_explicit(&s_manager_lifecycle,
                              MANAGER_LIFECYCLE_RUNNING,
                              memory_order_release);
    }
    else
    {
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
    esp_err_t result = _manager_control(
                           MANAGER_COMMAND_DEINIT,
                           _manager_deadline_remaining_ms(&deadline));
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
    if (credentials == NULL || credentials->ssid == NULL ||
            credentials->ssid_length == 0U ||
            credentials->ssid_length > CONNECTIVITY_MANAGER_SSID_MAX_BYTES ||
            memchr(credentials->ssid, '\0',
                   credentials->ssid_length) != NULL ||
            credentials->security > CONNECTIVITY_MANAGER_SECURITY_PERSONAL)
    {
        return false;
    }
    if (credentials->security == CONNECTIVITY_MANAGER_SECURITY_OPEN)
    {
        return credentials->password_length == 0U;
    }
    return credentials->password != NULL &&
           credentials->password_length >= 8U &&
           credentials->password_length <=
           CONNECTIVITY_MANAGER_PASSWORD_MAX_BYTES &&
           memchr(credentials->password, '\0',
                  credentials->password_length) == NULL;
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
    command.credentials.magic = CONNECTIVITY_MANAGER_PROFILE_MAGIC;
    command.credentials.version = CONNECTIVITY_MANAGER_PROFILE_VERSION;
    command.credentials.security = (uint8_t)credentials->security;
    command.credentials.auto_connect = 1U;
    command.credentials.ssid_length = (uint8_t)credentials->ssid_length;
    command.credentials.password_length =
        (uint8_t)credentials->password_length;
    memcpy(command.credentials.ssid, credentials->ssid,
           credentials->ssid_length);
    if (credentials->password_length > 0U)
    {
        memcpy(command.credentials.password, credentials->password,
               credentials->password_length);
    }
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

esp_err_t connectivity_manager_request_disconnect(
    connectivity_manager_operation_id_t *out_operation_id)
{
    return _manager_submit_simple(MANAGER_COMMAND_DISCONNECT,
                                  out_operation_id);
}

esp_err_t connectivity_manager_request_reconnect_saved(
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

esp_err_t connectivity_manager_set_auto_connect(
    bool enabled, connectivity_manager_operation_id_t *out_operation_id)
{
    if (out_operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    manager_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = MANAGER_COMMAND_SET_AUTO_CONNECT;
    command.enabled = enabled;
    return _manager_submit(&command, out_operation_id);
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
