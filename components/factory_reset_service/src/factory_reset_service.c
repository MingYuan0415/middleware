#define DBG_TAG "factory_reset"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "factory_reset_service.h"

#include "nv_storage.h"

#include "nvs.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define FACTORY_RESET_SERVICE_STORAGE_KEY "factory.reset"
#define FACTORY_RESET_SERVICE_MARKER_MAGIC UINT32_C(0x46525354)
#define FACTORY_RESET_SERVICE_MARKER_VERSION UINT16_C(1)
#define FACTORY_RESET_SERVICE_MARKER_INTEGRITY UINT32_C(0xA9174EC3)
#define FACTORY_RESET_SERVICE_LIFECYCLE_MASK UINT32_C(0x00000003)
#define FACTORY_RESET_SERVICE_API_USER_INCREMENT UINT32_C(0x00000004)
#define FACTORY_RESET_SERVICE_API_USER_MASK UINT32_C(0x00000FFC)
#define FACTORY_RESET_SERVICE_INSTANCE_INCREMENT UINT32_C(0x00001000)
#define FACTORY_RESET_SERVICE_INSTANCE_MASK UINT32_C(0xFFFFF000)

typedef enum factory_reset_service_lifecycle
{
    FACTORY_RESET_SERVICE_STOPPED = 0,
    FACTORY_RESET_SERVICE_INITIALIZING,
    FACTORY_RESET_SERVICE_RUNNING,
    FACTORY_RESET_SERVICE_STOPPING,
} factory_reset_service_lifecycle_t;

typedef struct factory_reset_service_marker
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t integrity;
    uint32_t trailing_reserved;
} factory_reset_service_marker_t;

_Static_assert(sizeof(factory_reset_service_marker_t) == 16U,
               "Factory-reset journal layout must remain stable");

static factory_reset_service_config_t s_config;
/* A single CAS-visible word prevents an API that sampled an old RUNNING
 * instance from crossing a complete deinit/reinit ABA cycle. */
static atomic_uint s_api_state = ATOMIC_VAR_INIT(
                                     FACTORY_RESET_SERVICE_STOPPED);
static atomic_bool s_request_admitted = ATOMIC_VAR_INIT(false);
static atomic_flag s_operation_busy = ATOMIC_FLAG_INIT;
#ifdef UNIT_TEST_HOST
    static factory_reset_service_test_api_acquire_hook_t s_api_acquire_hook;
    static void *s_api_acquire_hook_arg;
#endif

static bool _factory_reset_service_api_acquire(void)
{
    unsigned state = atomic_load_explicit(&s_api_state, memory_order_acquire);
#ifdef UNIT_TEST_HOST
    if (s_api_acquire_hook != NULL)
    {
        s_api_acquire_hook(s_api_acquire_hook_arg);
    }
#endif
    const unsigned instance = state & FACTORY_RESET_SERVICE_INSTANCE_MASK;

    while ((state & FACTORY_RESET_SERVICE_INSTANCE_MASK) == instance)
    {
        if ((state & FACTORY_RESET_SERVICE_LIFECYCLE_MASK) !=
                FACTORY_RESET_SERVICE_RUNNING)
        {
            return false;
        }
        if ((state & FACTORY_RESET_SERVICE_API_USER_MASK) ==
                FACTORY_RESET_SERVICE_API_USER_MASK)
        {
            return false;
        }
        const unsigned desired = state +
                                 FACTORY_RESET_SERVICE_API_USER_INCREMENT;

        if (atomic_compare_exchange_weak_explicit(
                    &s_api_state, &state, desired,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

static void _factory_reset_service_api_release(void)
{
    atomic_fetch_sub_explicit(&s_api_state,
                              FACTORY_RESET_SERVICE_API_USER_INCREMENT,
                              memory_order_release);
}

#ifdef UNIT_TEST_HOST
void factory_reset_service_test_set_api_acquire_hook(
    factory_reset_service_test_api_acquire_hook_t hook, void *arg)
{
    s_api_acquire_hook = hook;
    s_api_acquire_hook_arg = arg;
}
#endif

static bool _factory_reset_service_operation_begin(void)
{
    return !atomic_flag_test_and_set_explicit(&s_operation_busy,
            memory_order_acquire);
}

static void _factory_reset_service_operation_end(void)
{
    atomic_flag_clear_explicit(&s_operation_busy, memory_order_release);
}

static factory_reset_service_marker_t _factory_reset_service_marker(void)
{
    const factory_reset_service_marker_t marker =
    {
        .magic = FACTORY_RESET_SERVICE_MARKER_MAGIC,
        .version = FACTORY_RESET_SERVICE_MARKER_VERSION,
        .integrity = FACTORY_RESET_SERVICE_MARKER_INTEGRITY,
    };
    return marker;
}

static bool _factory_reset_service_marker_valid(
    const factory_reset_service_marker_t *marker)
{
    return marker != NULL &&
           marker->magic == FACTORY_RESET_SERVICE_MARKER_MAGIC &&
           marker->version == FACTORY_RESET_SERVICE_MARKER_VERSION &&
           marker->reserved == 0U &&
           marker->integrity == FACTORY_RESET_SERVICE_MARKER_INTEGRITY &&
           marker->trailing_reserved == 0U;
}

static esp_err_t _factory_reset_service_persist_marker(void)
{
    const factory_reset_service_marker_t marker =
        _factory_reset_service_marker();

    return nv_storage_set_blob(FACTORY_RESET_SERVICE_STORAGE_KEY,
                               &marker, sizeof(marker));
}

esp_err_t factory_reset_service_init(
    const factory_reset_service_config_t *config)
{
    if (config == NULL || config->restart == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    unsigned expected = atomic_load_explicit(&s_api_state,
                        memory_order_acquire);

    if ((expected & FACTORY_RESET_SERVICE_LIFECYCLE_MASK) !=
            FACTORY_RESET_SERVICE_STOPPED ||
            (expected & FACTORY_RESET_SERVICE_API_USER_MASK) != 0U ||
            (expected & FACTORY_RESET_SERVICE_INSTANCE_MASK) ==
            FACTORY_RESET_SERVICE_INSTANCE_MASK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const unsigned initializing =
        ((expected & FACTORY_RESET_SERVICE_INSTANCE_MASK) +
         FACTORY_RESET_SERVICE_INSTANCE_INCREMENT) |
        FACTORY_RESET_SERVICE_INITIALIZING;

    if (!atomic_compare_exchange_strong_explicit(
                &s_api_state, &expected, initializing,
                memory_order_acq_rel, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_config = *config;
    atomic_store_explicit(&s_request_admitted, false, memory_order_release);
    atomic_flag_clear_explicit(&s_operation_busy, memory_order_release);
    atomic_store_explicit(&s_api_state,
                          (initializing & FACTORY_RESET_SERVICE_INSTANCE_MASK) |
                          FACTORY_RESET_SERVICE_RUNNING,
                          memory_order_release);
    return ESP_OK;
}

esp_err_t factory_reset_service_deinit(void)
{
    unsigned expected = atomic_load_explicit(&s_api_state,
                        memory_order_acquire);

    if ((expected & FACTORY_RESET_SERVICE_LIFECYCLE_MASK) ==
            FACTORY_RESET_SERVICE_STOPPED)
    {
        return ESP_OK;
    }
    if ((expected & FACTORY_RESET_SERVICE_LIFECYCLE_MASK) !=
            FACTORY_RESET_SERVICE_RUNNING ||
            (expected & FACTORY_RESET_SERVICE_API_USER_MASK) != 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const unsigned stopping =
        (expected & FACTORY_RESET_SERVICE_INSTANCE_MASK) |
        FACTORY_RESET_SERVICE_STOPPING;

    if (!atomic_compare_exchange_strong_explicit(
                &s_api_state, &expected, stopping,
                memory_order_acq_rel, memory_order_acquire))
    {
        return (expected & FACTORY_RESET_SERVICE_LIFECYCLE_MASK) ==
               FACTORY_RESET_SERVICE_STOPPED ? ESP_OK :
               ESP_ERR_INVALID_STATE;
    }
    memset(&s_config, 0, sizeof(s_config));
    atomic_store_explicit(&s_request_admitted, false, memory_order_release);
    atomic_flag_clear_explicit(&s_operation_busy, memory_order_release);
    atomic_store_explicit(&s_api_state,
                          (stopping & FACTORY_RESET_SERVICE_INSTANCE_MASK) |
                          FACTORY_RESET_SERVICE_STOPPED,
                          memory_order_release);
    return ESP_OK;
}

esp_err_t factory_reset_service_request(void)
{
    if (!_factory_reset_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_factory_reset_service_operation_begin())
    {
        _factory_reset_service_api_release();
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;

    if (!atomic_compare_exchange_strong_explicit(
                &s_request_admitted, &expected, true,
                memory_order_acq_rel, memory_order_acquire))
    {
        _factory_reset_service_operation_end();
        _factory_reset_service_api_release();
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = _factory_reset_service_persist_marker();

    if (result != ESP_OK)
    {
        atomic_store_explicit(&s_request_admitted, false,
                              memory_order_release);
        _factory_reset_service_operation_end();
        _factory_reset_service_api_release();
        return result;
    }
    const factory_reset_service_restart_fn restart = s_config.restart;
    void *const restart_context = s_config.restart_context;

    restart(restart_context);
    _factory_reset_service_operation_end();
    _factory_reset_service_api_release();
    return ESP_OK;
}

esp_err_t factory_reset_service_recovery_pending(bool *pending)
{
    if (pending == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *pending = false;
    if (!_factory_reset_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_factory_reset_service_operation_begin())
    {
        _factory_reset_service_api_release();
        return ESP_ERR_INVALID_STATE;
    }
    factory_reset_service_marker_t marker;
    size_t size = sizeof(marker);
    memset(&marker, 0, sizeof(marker));
    const esp_err_t result = nv_storage_get_blob(
                                 FACTORY_RESET_SERVICE_STORAGE_KEY,
                                 &marker, &size);

    if (result == ESP_ERR_NVS_NOT_FOUND)
    {
        _factory_reset_service_operation_end();
        _factory_reset_service_api_release();
        return ESP_OK;
    }
    if (result != ESP_OK)
    {
        _factory_reset_service_operation_end();
        _factory_reset_service_api_release();
        return result;
    }
    if (size != sizeof(marker) ||
            !_factory_reset_service_marker_valid(&marker))
    {
        _factory_reset_service_operation_end();
        _factory_reset_service_api_release();
        return ESP_ERR_INVALID_RESPONSE;
    }
    *pending = true;
    atomic_store_explicit(&s_request_admitted, true, memory_order_release);
    _factory_reset_service_operation_end();
    _factory_reset_service_api_release();
    return ESP_OK;
}

esp_err_t factory_reset_service_complete_recovery(void)
{
    if (!_factory_reset_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_factory_reset_service_operation_begin())
    {
        _factory_reset_service_api_release();
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = nv_storage_erase_key(
                                 FACTORY_RESET_SERVICE_STORAGE_KEY);

    if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND)
    {
        atomic_store_explicit(&s_request_admitted, false,
                              memory_order_release);
        _factory_reset_service_operation_end();
        _factory_reset_service_api_release();
        return ESP_OK;
    }
    _factory_reset_service_operation_end();
    _factory_reset_service_api_release();
    return result;
}
