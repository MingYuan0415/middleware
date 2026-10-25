#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "sd_storage_service.h"

#define SD_STORAGE_SERVICE_PATH_MAX (64U)
#define SD_STORAGE_SERVICE_LEASE_CAPACITY (8U)

/**
 * @brief Reported faults needed before the medium is declared unusable.
 *
 * One failed transfer is not evidence of a dead card: a preemption, a clock
 * change during light sleep, or a marginal contact produces the same error and
 * the next access succeeds. Unmounting on the first report turned any such
 * transient into "card removed" plus a manual remount requirement.
 *
 * The count is per content epoch and resets on any successful medium access,
 * so it tracks a persisting failure rather than the total number of errors.
 */
#define SD_STORAGE_SERVICE_FAULT_CONFIRMATIONS (2U)

typedef enum sd_storage_service_state
{
    SD_STORAGE_SERVICE_STATE_STOPPED = 0,
    SD_STORAGE_SERVICE_STATE_IDLE,
    SD_STORAGE_SERVICE_STATE_MOUNTING,
    SD_STORAGE_SERVICE_STATE_MOUNTED,
    SD_STORAGE_SERVICE_STATE_UNMOUNTING,
    SD_STORAGE_SERVICE_STATE_FORMATTING,
    SD_STORAGE_SERVICE_STATE_ERROR,
    SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING,
} sd_storage_service_state_t;

typedef struct sd_storage_service_lease
{
    uint32_t token;
    uint32_t generation;
    sd_storage_service_usage_kind_t kind;
} sd_storage_service_lease_t;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static sd_storage_service_mount_ops_t s_ops;
static bool s_ops_registered;
static sd_storage_service_state_t s_state;
static void *s_handle;
static char s_mount_path[SD_STORAGE_SERVICE_PATH_MAX];
static sd_storage_service_config_t s_config;
static uint32_t s_generation;
static uint32_t s_revision;
static uint32_t s_next_token;
static esp_err_t s_last_error;
static bool s_faulted;
static uint32_t s_fault_streak; /**< Fault reports since the last medium success. */
static sd_storage_service_lease_t s_leases[SD_STORAGE_SERVICE_LEASE_CAPACITY];

static bool _config_valid(const sd_storage_service_config_t *config)
{
    if (config == NULL || config->mount_path == NULL ||
            config->mount_path[0] != '/' ||
            strlen(config->mount_path) >= sizeof(s_mount_path) ||
            config->max_files <= 0)
    {
        return false;
    }
    const size_t size = config->allocation_unit_size;
    return size == 0U || (size >= 512U && size <= 65536U &&
                          (size & (size - 1U)) == 0U);
}

static bool _config_equal_locked(const sd_storage_service_config_t *config)
{
    return strcmp(s_config.mount_path, config->mount_path) == 0 &&
           s_config.max_files == config->max_files &&
           s_config.allocation_unit_size == config->allocation_unit_size;
}

static bool _transition_locked(void)
{
    return s_state == SD_STORAGE_SERVICE_STATE_MOUNTING ||
           s_state == SD_STORAGE_SERVICE_STATE_UNMOUNTING ||
           s_state == SD_STORAGE_SERVICE_STATE_FORMATTING;
}

static bool _lease_table_empty_locked(void)
{
    for (size_t i = 0U; i < SD_STORAGE_SERVICE_LEASE_CAPACITY; ++i)
    {
        if (s_leases[i].token != 0U)
        {
            return false;
        }
    }
    return true;
}

static uint32_t _lease_count_locked(sd_storage_service_usage_kind_t kind)
{
    uint32_t count = 0U;
    for (size_t i = 0U; i < SD_STORAGE_SERVICE_LEASE_CAPACITY; ++i)
    {
        if (s_leases[i].token != 0U && s_leases[i].kind == kind)
        {
            ++count;
        }
    }
    return count;
}

static sd_storage_service_lease_t *_lease_find_locked(
    const sd_storage_service_usage_t *usage)
{
    for (size_t i = 0U; i < SD_STORAGE_SERVICE_LEASE_CAPACITY; ++i)
    {
        if (s_leases[i].token == usage->token &&
                s_leases[i].generation == usage->generation)
        {
            return &s_leases[i];
        }
    }
    return NULL;
}

static bool _usage_valid_locked(const sd_storage_service_usage_t *usage)
{
    return s_state == SD_STORAGE_SERVICE_STATE_MOUNTED && !s_faulted &&
           usage->token != 0U && usage->generation == s_generation &&
           _lease_find_locked(usage) != NULL;
}

esp_err_t sd_storage_service_register_mount_ops(
    const sd_storage_service_mount_ops_t *ops)
{
    if (ops == NULL || ops->mount == NULL || ops->unmount == NULL ||
            ops->format == NULL || ops->get_space == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != SD_STORAGE_SERVICE_STATE_STOPPED)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ops = *ops;
    s_ops_registered = true;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static esp_err_t _mount(const sd_storage_service_config_t *requested,
                        sd_storage_service_mount_mode_t mode,
                        bool check_generation, uint32_t expected_generation)
{
    if (!_config_valid(requested))
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (!s_ops_registered || _transition_locked() || !_lease_table_empty_locked() ||
            (check_generation && expected_generation != s_generation))
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == SD_STORAGE_SERVICE_STATE_MOUNTED && !s_faulted)
    {
        const bool equal = _config_equal_locked(requested);
        taskEXIT_CRITICAL(&s_state_lock);
        return equal ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    const sd_storage_service_mount_ops_t ops = s_ops;
    void *old_handle = s_handle;
    s_state = SD_STORAGE_SERVICE_STATE_MOUNTING;
    taskEXIT_CRITICAL(&s_state_lock);

    /* Reserve the lifecycle before touching the old adapter. A failed release
     * retains exactly that handle; a retry never mounts over live resources. */
    if (old_handle != NULL)
    {
        const esp_err_t cleanup = ops.unmount(ops.context, old_handle);
        if (cleanup != ESP_OK)
        {
            taskENTER_CRITICAL(&s_state_lock);
            s_last_error = cleanup;
            s_state = SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING;
            taskEXIT_CRITICAL(&s_state_lock);
            return cleanup;
        }
        taskENTER_CRITICAL(&s_state_lock);
        s_handle = NULL;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    if (mode == SD_STORAGE_SERVICE_MOUNT_RECOVER_FORMAT)
    {
        /* Recovery may erase even if its remount fails. Invalidate the
         * confirmation before handing ownership to the destructive adapter. */
        taskENTER_CRITICAL(&s_state_lock);
        ++s_generation;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    memmove(s_mount_path, requested->mount_path, strlen(requested->mount_path) + 1U);
    const sd_storage_service_config_t config =
    {
        .mount_path = s_mount_path,
        .max_files = requested->max_files,
        .allocation_unit_size = requested->allocation_unit_size,
    };
    void *handle = NULL;
    esp_err_t result = ops.mount(ops.context, &config, mode, &handle);
    if (result == ESP_OK && (handle == NULL ||
                             (ops.is_mounted != NULL && !ops.is_mounted(ops.context, handle))))
    {
        result = ESP_ERR_INVALID_STATE;
    }
    esp_err_t cleanup = ESP_OK;
    if (result != ESP_OK && handle != NULL)
    {
        cleanup = ops.unmount(ops.context, handle);
        if (cleanup == ESP_OK)
        {
            handle = NULL;
        }
    }
    taskENTER_CRITICAL(&s_state_lock);
    s_config = config;
    s_handle = handle;
    s_last_error = cleanup == ESP_OK ? result : cleanup;
    if (result == ESP_OK)
    {
        s_state = SD_STORAGE_SERVICE_STATE_MOUNTED;
        s_faulted = false;
        s_fault_streak = 0U;
        ++s_generation;
    }
    else
    {
        s_state = handle == NULL ? SD_STORAGE_SERVICE_STATE_ERROR :
                  SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return cleanup == ESP_OK ? result : cleanup;
}

esp_err_t sd_storage_service_init(const sd_storage_service_config_t *config)
{
    return _mount(config, SD_STORAGE_SERVICE_MOUNT_NORMAL, false, 0U);
}

esp_err_t sd_storage_service_recover_and_mount(
    const sd_storage_service_config_t *config, uint32_t expected_generation)
{
    return _mount(config, SD_STORAGE_SERVICE_MOUNT_RECOVER_FORMAT, true,
                  expected_generation);
}

static esp_err_t _unmount(bool stop, bool check_generation,
                          uint32_t expected_generation)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (_transition_locked() || !_lease_table_empty_locked() ||
            (check_generation && expected_generation != s_generation))
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const sd_storage_service_mount_ops_t ops = s_ops;
    void *handle = s_handle;
    if (s_state == SD_STORAGE_SERVICE_STATE_MOUNTED)
    {
        ++s_generation;
    }
    s_state = SD_STORAGE_SERVICE_STATE_UNMOUNTING;
    taskEXIT_CRITICAL(&s_state_lock);

    const esp_err_t result = handle == NULL ? ESP_OK :
                             ops.unmount(ops.context, handle);
    taskENTER_CRITICAL(&s_state_lock);
    s_last_error = result;
    if (result == ESP_OK)
    {
        s_handle = NULL;
        s_faulted = false;
        s_fault_streak = 0U;
        s_state = stop ? SD_STORAGE_SERVICE_STATE_STOPPED : SD_STORAGE_SERVICE_STATE_IDLE;
    }
    else
    {
        s_state = SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return result;
}

esp_err_t sd_storage_service_unmount(uint32_t expected_generation)
{
    return _unmount(false, true, expected_generation);
}

esp_err_t sd_storage_service_deinit(void)
{
    return _unmount(true, false, 0U);
}

esp_err_t sd_storage_service_format(const sd_storage_service_config_t *config,
                                    uint32_t expected_generation)
{
    if (config != NULL && !_config_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != SD_STORAGE_SERVICE_STATE_MOUNTED || s_faulted ||
            expected_generation != s_generation || !_lease_table_empty_locked())
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (config != NULL && !_config_equal_locked(config))
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_ARG;
    }
    const sd_storage_service_mount_ops_t ops = s_ops;
    void *handle = s_handle;
    s_state = SD_STORAGE_SERVICE_STATE_FORMATTING;
    ++s_generation;
    taskEXIT_CRITICAL(&s_state_lock);

    esp_err_t result = ops.format(ops.context, handle, &s_config);
    if (result == ESP_OK && ops.is_mounted != NULL &&
            !ops.is_mounted(ops.context, handle))
    {
        result = ESP_FAIL;
    }
    taskENTER_CRITICAL(&s_state_lock);
    s_last_error = result;
    s_state = result == ESP_OK ? SD_STORAGE_SERVICE_STATE_MOUNTED :
              SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING;
    if (result == ESP_OK)
    {
        s_fault_streak = 0U;
        ++s_revision;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return result;
}

esp_err_t sd_storage_service_get_space(const sd_storage_service_usage_t *usage,
                                       uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (usage == NULL || total_bytes == NULL || free_bytes == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (!_usage_valid_locked(usage))
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const sd_storage_service_mount_ops_t ops = s_ops;
    void *handle = s_handle;
    taskEXIT_CRITICAL(&s_state_lock);

    uint64_t total;
    uint64_t available;
    const esp_err_t result = ops.get_space(ops.context, handle, &total, &available);
    if (result != ESP_OK)
    {
        return result;
    }
    taskENTER_CRITICAL(&s_state_lock);
    const bool valid = _usage_valid_locked(usage);
    if (valid)
    {
        /* The medium answered, so any earlier failed transfer was transient.
         * Reset here rather than on a timer: this is the only real proof that
         * the card still responds, and it runs on every capacity query. */
        s_fault_streak = 0U;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (!valid)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *total_bytes = total;
    *free_bytes = available;
    return ESP_OK;
}

esp_err_t sd_storage_service_start(const sd_storage_service_config_t *config)
{
    return sd_storage_service_init(config);
}

esp_err_t sd_storage_service_stop(void)
{
    return sd_storage_service_deinit();
}

bool sd_storage_service_is_mounted(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool mounted = s_state == SD_STORAGE_SERVICE_STATE_MOUNTED && !s_faulted;
    taskEXIT_CRITICAL(&s_state_lock);
    return mounted;
}

const char *sd_storage_service_get_mount_path(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const char *path = s_state == SD_STORAGE_SERVICE_STATE_MOUNTED && !s_faulted ?
                       s_mount_path : NULL;
    taskEXIT_CRITICAL(&s_state_lock);
    return path;
}

esp_err_t sd_storage_service_get_snapshot(sd_storage_service_snapshot_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    snapshot->mounted = s_state == SD_STORAGE_SERVICE_STATE_MOUNTED && !s_faulted;
    switch (s_state)
    {
    case SD_STORAGE_SERVICE_STATE_MOUNTING:
        snapshot->phase = SD_STORAGE_SERVICE_PHASE_MOUNTING;
        break;
    case SD_STORAGE_SERVICE_STATE_MOUNTED:
        snapshot->phase = SD_STORAGE_SERVICE_PHASE_MOUNTED;
        break;
    case SD_STORAGE_SERVICE_STATE_UNMOUNTING:
        snapshot->phase = SD_STORAGE_SERVICE_PHASE_UNMOUNTING;
        break;
    case SD_STORAGE_SERVICE_STATE_FORMATTING:
        snapshot->phase = SD_STORAGE_SERVICE_PHASE_FORMATTING;
        break;
    case SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING:
    case SD_STORAGE_SERVICE_STATE_ERROR:
        snapshot->phase = SD_STORAGE_SERVICE_PHASE_ERROR;
        break;
    default:
        snapshot->phase = SD_STORAGE_SERVICE_PHASE_UNMOUNTED;
        break;
    }
    snapshot->generation = s_generation;
    snapshot->revision = s_revision;
    snapshot->mount_path = snapshot->mounted ? s_mount_path : NULL;
    snapshot->last_error = s_last_error;
    snapshot->faulted = s_faulted;
    snapshot->access_users = _lease_count_locked(SD_STORAGE_SERVICE_USAGE_ACCESS);
    snapshot->session_users = _lease_count_locked(SD_STORAGE_SERVICE_USAGE_SESSION);
    snapshot->maintenance = _lease_count_locked(SD_STORAGE_SERVICE_USAGE_MAINTENANCE) != 0U;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t sd_storage_service_get_config(sd_storage_service_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (s_handle == NULL || _transition_locked())
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *config = s_config;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

uint32_t sd_storage_service_content_revision(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const uint32_t revision = s_revision;
    taskEXIT_CRITICAL(&s_state_lock);
    return revision;
}

esp_err_t sd_storage_service_note_content_change(const sd_storage_service_usage_t *usage)
{
    if (usage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    const bool valid = _usage_valid_locked(usage);
    if (valid)
    {
        /* A completed mutation proves the medium accepted a write. */
        s_fault_streak = 0U;
        ++s_revision;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t sd_storage_service_usage_acquire(sd_storage_service_usage_kind_t kind,
        sd_storage_service_usage_t *usage)
{
    if (usage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (kind != SD_STORAGE_SERVICE_USAGE_ACCESS &&
            kind != SD_STORAGE_SERVICE_USAGE_SESSION &&
            kind != SD_STORAGE_SERVICE_USAGE_MAINTENANCE)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != SD_STORAGE_SERVICE_STATE_MOUNTED || s_faulted ||
            (kind == SD_STORAGE_SERVICE_USAGE_MAINTENANCE ? !_lease_table_empty_locked() :
             _lease_count_locked(SD_STORAGE_SERVICE_USAGE_MAINTENANCE) != 0U))
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0U; i < SD_STORAGE_SERVICE_LEASE_CAPACITY; ++i)
    {
        if (s_leases[i].token == 0U)
        {
            /* Avoid a live token collision when the counter wraps. */
            bool duplicate;
            do
            {
                ++s_next_token;
                duplicate = s_next_token == 0U;
                for (size_t j = 0U; j < SD_STORAGE_SERVICE_LEASE_CAPACITY; ++j)
                {
                    duplicate |= s_leases[j].token == s_next_token;
                }
            }
            while (duplicate);
            s_leases[i] = (sd_storage_service_lease_t)
            {
                s_next_token, s_generation, kind
            };
            *usage = (sd_storage_service_usage_t)
            {
                s_next_token, s_generation
            };
            taskEXIT_CRITICAL(&s_state_lock);
            return ESP_OK;
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t sd_storage_service_usage_release(const sd_storage_service_usage_t *usage)
{
    if (usage == NULL || usage->token == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    sd_storage_service_lease_t *lease = _lease_find_locked(usage);
    if (lease != NULL)
    {
        memset(lease, 0, sizeof(*lease));
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t sd_storage_service_usage_report_fault(const sd_storage_service_usage_t *usage,
        esp_err_t error)
{
    if (usage == NULL || usage->token == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    const bool valid = _usage_valid_locked(usage);
    if (valid)
    {
        /* A single failed transfer does not condemn the medium: the same
         * error comes from a preemption or a marginal contact, and the next
         * access succeeds. Only a failure that survives a successful access
         * in between is a real fault, so the count is what confirms it. */
        if (s_fault_streak < SD_STORAGE_SERVICE_FAULT_CONFIRMATIONS)
        {
            ++s_fault_streak;
        }
        s_last_error = error == ESP_OK ? ESP_FAIL : error;
        if (s_fault_streak >= SD_STORAGE_SERVICE_FAULT_CONFIRMATIONS)
        {
            s_faulted = true;
            s_state = SD_STORAGE_SERVICE_STATE_CLEANUP_PENDING;
            ++s_generation;
            /* Keep original token+generation entries until their owners
             * release. The invalidated epoch cannot be mounted over live file
             * handles. */
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}
