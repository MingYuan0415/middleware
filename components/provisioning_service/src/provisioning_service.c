#define DBG_TAG "provisioning"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "provisioning_service.h"

#include "provisioning_protocol.h"

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_srp.h"
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
    #include "esp_timer.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "protocomm.h"
#include "protocomm_ble.h"
#include "protocomm_security2.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROVISIONING_SERVICE_ENDPOINT_VERSION "proto-ver"
#define PROVISIONING_SERVICE_ENDPOINT_SESSION "prov-session"
#define PROVISIONING_SERVICE_ENDPOINT_CONTROL "mt-prov"
#define PROVISIONING_SERVICE_USERNAME         "microtech"
#define PROVISIONING_SERVICE_DEVICE_PREFIX    "MT-"
#define PROVISIONING_SERVICE_POP_RAW_BYTES    16U
#define PROVISIONING_SERVICE_POP_TEXT_BYTES   22U
#define PROVISIONING_SERVICE_SALT_BYTES       16U
#define PROVISIONING_SERVICE_WORKER_POLL_MS   100U
EVENT_BUS_DEFINE_ID(PROVISIONING_SERVICE_MSG);

typedef enum provisioning_lifecycle
{
    PROVISIONING_LIFECYCLE_STOPPED = 0,
    PROVISIONING_LIFECYCLE_STARTING,
    PROVISIONING_LIFECYCLE_RUNNING,
    PROVISIONING_LIFECYCLE_STOPPING,
} provisioning_lifecycle_t;

typedef enum provisioning_command_type
{
    PROVISIONING_COMMAND_RECONCILE = 0,
    PROVISIONING_COMMAND_PUBLISH,
    PROVISIONING_COMMAND_DEINIT,
} provisioning_command_type_t;

typedef struct provisioning_command
{
    provisioning_command_type_t type;
} provisioning_command_t;

typedef struct provisioning_service_context
{
    provisioning_service_config_t config;
    provisioning_service_snapshot_t snapshot;
    provisioning_protocol_context_t protocol;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t stopped;
    QueueHandle_t queue;
    TaskHandle_t task;
    StaticSemaphore_t mutex_control;
    StaticSemaphore_t stopped_control;
    StaticQueue_t queue_control;
    union provisioning_queue_storage
    {
        max_align_t alignment;
        uint8_t bytes[CONFIG_PROVISIONING_SERVICE_QUEUE_DEPTH *
                      sizeof(provisioning_command_t)];
    } queue_storage;
    StaticTask_t task_control;
    StackType_t task_stack[CONFIG_PROVISIONING_SERVICE_TASK_STACK];
    event_bus_sub_handle_t status_subscription;
    event_bus_sub_handle_t scan_subscription;
    esp_event_handler_instance_t ble_event_handler;
    protocomm_t *protocomm;
    char qr[PROVISIONING_SERVICE_QR_MAX_BYTES];
    char *salt;
    char *verifier;
    int verifier_length;
    TickType_t window_deadline;
    TickType_t finish_deadline;
    TickType_t success_deadline;
    uint32_t last_remaining_seconds;
    bool ble_event_registered;
    atomic_bool transport_started;
    bool transport_stop_attempted;
    bool transport_accepting;
    atomic_bool transport_faulted;
    esp_err_t transport_fault_error;
    bool desired_open;
    bool desired_cancel_operation;
    bool reconcile_queued;
    bool publish_pending;
    bool publish_command_queued;
    bool publish_failure_reported;
    bool finish_pending;
    bool success_pending;
    bool suspended;
    atomic_bool worker_stopping;
} provisioning_service_context_t;

static provisioning_service_context_t s_service;
static atomic_int s_lifecycle = ATOMIC_VAR_INIT(
                                    PROVISIONING_LIFECYCLE_STOPPED);
static atomic_uint s_api_users = ATOMIC_VAR_INIT(0U);
static atomic_uint s_transport_users = ATOMIC_VAR_INIT(0U);
static atomic_bool s_active = ATOMIC_VAR_INIT(false);
static atomic_int s_worker_result = ATOMIC_VAR_INIT(ESP_OK);
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
    static atomic_uint_fast64_t s_protected_request_count;
    static atomic_uint_fast64_t s_protected_success_count;
    static atomic_uint_fast64_t s_protected_failure_count;
    static atomic_uint_fast64_t s_snapshot_success_count;
    static atomic_uint_fast64_t s_last_snapshot_request_id;
    static atomic_int_fast64_t s_last_snapshot_success_us;
#endif

static const char s_protocol_version[] =
    "{\"prov\":{\"ver\":\"v1.0\",\"sec_ver\":2,"
    "\"sec_patch_ver\":1,\"cap\":[\"mt-prov-v1\"]}}";

static const uint8_t s_service_uuid[BLE_UUID128_VAL_LENGTH] =
{
    0x6b, 0x0e, 0x39, 0x9e, 0x97, 0x73, 0x21, 0x8c,
    0x9f, 0x40, 0x7e, 0xb4, 0x36, 0xc8, 0xf1, 0xd8,
};

#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
static void _provisioning_service_reset_diagnostics(void)
{
    atomic_store_explicit(&s_protected_request_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_protected_success_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_protected_failure_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_snapshot_success_count, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_last_snapshot_request_id, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_last_snapshot_success_us, 0,
                          memory_order_relaxed);
}

static void _provisioning_service_record_request(
    esp_err_t result, const provisioning_protocol_result_t *request)
{
    atomic_fetch_add_explicit(&s_protected_request_count, 1U,
                              memory_order_relaxed);
    if (result != ESP_OK || request == NULL || !request->request_succeeded)
    {
        atomic_fetch_add_explicit(&s_protected_failure_count, 1U,
                                  memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&s_protected_success_count, 1U,
                              memory_order_relaxed);
    if (request->get_snapshot)
    {
        atomic_fetch_add_explicit(&s_snapshot_success_count, 1U,
                                  memory_order_relaxed);
        atomic_store_explicit(&s_last_snapshot_request_id,
                              request->request_id, memory_order_relaxed);
        atomic_store_explicit(&s_last_snapshot_success_us,
                              esp_timer_get_time(), memory_order_release);
    }
}
#endif

static bool _provisioning_service_api_acquire(void)
{
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) !=
            PROVISIONING_LIFECYCLE_RUNNING)
    {
        return false;
    }
    atomic_fetch_add_explicit(&s_api_users, 1U, memory_order_acq_rel);
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) ==
            PROVISIONING_LIFECYCLE_RUNNING)
    {
        return true;
    }
    atomic_fetch_sub_explicit(&s_api_users, 1U, memory_order_release);
    return false;
}

static void _provisioning_service_api_release(void)
{
    atomic_fetch_sub_explicit(&s_api_users, 1U, memory_order_release);
}

static bool _provisioning_service_tick_reached(TickType_t now,
        TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static uint32_t _provisioning_service_remaining_ms(TickType_t deadline)
{
    const TickType_t now = xTaskGetTickCount();
    if (_provisioning_service_tick_reached(now, deadline))
    {
        return 0U;
    }
    const TickType_t remaining = deadline - now;
    const uint64_t milliseconds =
        ((uint64_t)remaining * 1000U + configTICK_RATE_HZ - 1U) /
        configTICK_RATE_HZ;
    return milliseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)milliseconds;
}

static TickType_t _provisioning_service_timeout_remaining(
    TickType_t started, TickType_t timeout)
{
    if (timeout == portMAX_DELAY)
    {
        return portMAX_DELAY;
    }
    const TickType_t elapsed = xTaskGetTickCount() - started;
    return elapsed >= timeout ? 0U : timeout - elapsed;
}

static esp_err_t _provisioning_service_wait_worker_suspended(
    TickType_t started, TickType_t timeout)
{
    while (s_service.task != NULL &&
            eTaskGetState(s_service.task) != eSuspended)
    {
        if (_provisioning_service_timeout_remaining(started, timeout) == 0U)
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1U);
    }
    return ESP_OK;
}

static void _provisioning_service_next_snapshot_locked(
    provisioning_service_snapshot_t *snapshot)
{
    ++s_service.snapshot.generation;
    if (s_service.snapshot.generation == 0U)
    {
        ++s_service.snapshot.generation;
    }
    if (snapshot != NULL)
    {
        *snapshot = s_service.snapshot;
    }
}

static esp_err_t _provisioning_service_publish_now(
    const provisioning_service_snapshot_t *snapshot)
{
    return event_bus_publish(
               PROVISIONING_SERVICE_MSG,
               PROVISIONING_SERVICE_MSG_SUB_TYPE_STATUS_SNAPSHOT,
               snapshot, sizeof(*snapshot),
               EVENT_BUS_PUBLISH_FLAG_UI_LATEST);
}

static void _provisioning_service_request_publish_locked(void)
{
    s_service.publish_pending = true;
    if (s_service.publish_command_queued)
    {
        return;
    }
    const provisioning_command_t command =
    {
        .type = PROVISIONING_COMMAND_PUBLISH,
    };
    if (xQueueSend(s_service.queue, &command, 0U) == pdTRUE)
    {
        s_service.publish_command_queued = true;
    }
}

static void _provisioning_service_flush_pending_publish(void)
{
    provisioning_service_snapshot_t snapshot;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    const bool pending = s_service.publish_pending;
    if (pending)
    {
        snapshot = s_service.snapshot;
    }
    xSemaphoreGive(s_service.mutex);
    if (!pending)
    {
        return;
    }

    const esp_err_t result = _provisioning_service_publish_now(&snapshot);
    bool report_failure = false;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (result == ESP_OK)
    {
        if (s_service.snapshot.generation == snapshot.generation)
        {
            s_service.publish_pending = false;
        }
        s_service.publish_failure_reported = false;
    }
    else if (result != ESP_ERR_INVALID_STATE &&
             !s_service.publish_failure_reported)
    {
        s_service.publish_failure_reported = true;
        report_failure = true;
    }
    xSemaphoreGive(s_service.mutex);
    if (report_failure)
    {
        LOG_W("status publish failed: %s", esp_err_to_name(result));
    }
}

static void _provisioning_service_worker_publish(
    const provisioning_service_snapshot_t *snapshot)
{
    const esp_err_t result = _provisioning_service_publish_now(snapshot);
    bool report_failure = false;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (result == ESP_OK)
    {
        if (s_service.snapshot.generation == snapshot->generation)
        {
            s_service.publish_pending = false;
        }
        s_service.publish_failure_reported = false;
    }
    else
    {
        if (s_service.snapshot.generation == snapshot->generation)
        {
            s_service.publish_pending = true;
        }
        if (result != ESP_ERR_INVALID_STATE &&
                !s_service.publish_failure_reported)
        {
            s_service.publish_failure_reported = true;
            report_failure = true;
        }
    }
    xSemaphoreGive(s_service.mutex);
    if (report_failure)
    {
        LOG_W("status publish failed: %s", esp_err_to_name(result));
    }
}

static void _provisioning_service_sync_operation_locked(void)
{
    s_service.snapshot.wifi_operation_id =
        provisioning_protocol_active_operation(&s_service.protocol);
    s_service.snapshot.wifi_operation_active =
        s_service.snapshot.wifi_operation_id != 0U;
}

static void _provisioning_service_zero_secrets(void)
{
    if (s_service.salt != NULL)
    {
        mbedtls_platform_zeroize(s_service.salt,
                                 PROVISIONING_SERVICE_SALT_BYTES);
        free(s_service.salt);
        s_service.salt = NULL;
    }
    if (s_service.verifier != NULL)
    {
        mbedtls_platform_zeroize(s_service.verifier,
                                 (size_t)s_service.verifier_length);
        free(s_service.verifier);
        s_service.verifier = NULL;
    }
    s_service.verifier_length = 0;
    mbedtls_platform_zeroize(s_service.qr, sizeof(s_service.qr));
}

static void _provisioning_service_encode_pop(
    const uint8_t input[PROVISIONING_SERVICE_POP_RAW_BYTES],
    char output[PROVISIONING_SERVICE_POP_TEXT_BYTES + 1U])
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t input_index = 0U;
    size_t output_index = 0U;
    while (input_index + 3U <= PROVISIONING_SERVICE_POP_RAW_BYTES)
    {
        const uint32_t value = ((uint32_t)input[input_index] << 16) |
                               ((uint32_t)input[input_index + 1U] << 8) |
                               input[input_index + 2U];
        output[output_index++] = alphabet[(value >> 18) & 0x3fU];
        output[output_index++] = alphabet[(value >> 12) & 0x3fU];
        output[output_index++] = alphabet[(value >> 6) & 0x3fU];
        output[output_index++] = alphabet[value & 0x3fU];
        input_index += 3U;
    }
    const uint32_t tail = ((uint32_t)input[input_index] << 16);
    output[output_index++] = alphabet[(tail >> 18) & 0x3fU];
    output[output_index++] = alphabet[(tail >> 12) & 0x3fU];
    output[PROVISIONING_SERVICE_POP_TEXT_BYTES] = '\0';
}

static esp_err_t _provisioning_service_generate_secrets(void)
{
    uint8_t raw_pop[PROVISIONING_SERVICE_POP_RAW_BYTES];
    char pop[PROVISIONING_SERVICE_POP_TEXT_BYTES + 1U];
    esp_fill_random(raw_pop, sizeof(raw_pop));
    _provisioning_service_encode_pop(raw_pop, pop);
    mbedtls_platform_zeroize(raw_pop, sizeof(raw_pop));

    const int qr_length = snprintf(
                              s_service.qr, sizeof(s_service.qr),
                              "{\"ver\":\"v1\",\"name\":\"MT-%s\","
                              "\"transport\":\"ble\",\"security\":2,"
                              "\"username\":\"microtech\",\"pop\":\"%s\","
                              "\"service\":\"d8f1c836-b47e-409f-8c21-73979e390e6b\","
                              "\"device_id\":\"%s\"}",
                              s_service.protocol.device_id, pop,
                              s_service.protocol.device_id);
    esp_err_t result = qr_length > 0 &&
                       (size_t)qr_length < sizeof(s_service.qr) ?
                       ESP_OK : ESP_ERR_INVALID_SIZE;
    if (result == ESP_OK)
    {
        result = esp_srp_gen_salt_verifier(
                     PROVISIONING_SERVICE_USERNAME,
                     (int)strlen(PROVISIONING_SERVICE_USERNAME),
                     pop, PROVISIONING_SERVICE_POP_TEXT_BYTES,
                     &s_service.salt, PROVISIONING_SERVICE_SALT_BYTES,
                     &s_service.verifier, &s_service.verifier_length);
    }
    mbedtls_platform_zeroize(pop, sizeof(pop));
    if (result != ESP_OK)
    {
        _provisioning_service_zero_secrets();
    }
    return result;
}

static esp_err_t _provisioning_service_endpoint(
    uint32_t session_id, const uint8_t *input, ssize_t input_length,
    uint8_t **output, ssize_t *output_length, void *private_data)
{
    (void)session_id;
    (void)private_data;
    if (input == NULL || input_length <= 0 || output == NULL ||
            output_length == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *mutable_input = (uint8_t *)(uintptr_t)input;
    if ((size_t)input_length >
            PROVISIONING_PROTOCOL_MAX_PLAINTEXT_REQUEST_BYTES)
    {
        mbedtls_platform_zeroize(mutable_input, (size_t)input_length);
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
        _provisioning_service_record_request(ESP_ERR_INVALID_SIZE, NULL);
#endif
        return ESP_ERR_INVALID_SIZE;
    }
    if (!_provisioning_service_api_acquire())
    {
        mbedtls_platform_zeroize(mutable_input, (size_t)input_length);
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
        _provisioning_service_record_request(ESP_ERR_INVALID_STATE, NULL);
#endif
        return ESP_ERR_INVALID_STATE;
    }
    atomic_fetch_add_explicit(&s_transport_users, 1U,
                              memory_order_acq_rel);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    uint8_t *packed = NULL;
    size_t packed_size = 0U;
    provisioning_protocol_result_t request_result = {0};

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (s_service.transport_accepting)
    {
        result = provisioning_protocol_handle(
                     &s_service.protocol, mutable_input,
                     (size_t)input_length, &packed, &packed_size,
                     &request_result);
        if (result == ESP_OK)
        {
            if (request_result.finish_session)
            {
                s_service.finish_pending = true;
                s_service.finish_deadline = xTaskGetTickCount() +
                                            pdMS_TO_TICKS(
                                                s_service.config.finish_close_delay_ms);
            }
            _provisioning_service_sync_operation_locked();
            _provisioning_service_next_snapshot_locked(NULL);
            _provisioning_service_request_publish_locked();
        }
    }
    xSemaphoreGive(s_service.mutex);
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
    _provisioning_service_record_request(result, &request_result);
#endif
    mbedtls_platform_zeroize(mutable_input, (size_t)input_length);

    if (result == ESP_OK)
    {
        *output = packed;
        *output_length = (ssize_t)packed_size;
    }
    else
    {
        free(packed);
        *output = NULL;
        *output_length = 0;
    }
    atomic_fetch_sub_explicit(&s_transport_users, 1U,
                              memory_order_release);
    _provisioning_service_api_release();
    return result;
}

static esp_err_t _provisioning_service_transport_stop(void);

static esp_err_t _provisioning_service_transport_start(void)
{
    if (s_service.transport_faulted)
    {
        return s_service.transport_fault_error;
    }
    esp_err_t result = _provisioning_service_generate_secrets();
    if (result != ESP_OK)
    {
        return result;
    }

    s_service.protocomm = protocomm_new();
    if (s_service.protocomm == NULL)
    {
        _provisioning_service_zero_secrets();
        return ESP_ERR_NO_MEM;
    }
    protocomm_ble_name_uuid_t endpoints[] =
    {
        {.name = PROVISIONING_SERVICE_ENDPOINT_VERSION, .uuid = 0xff50U},
        {.name = PROVISIONING_SERVICE_ENDPOINT_SESSION, .uuid = 0xff51U},
        {.name = PROVISIONING_SERVICE_ENDPOINT_CONTROL, .uuid = 0xff52U},
    };
    protocomm_ble_config_t config;
    memset(&config, 0, sizeof(config));
    (void)snprintf(config.device_name, sizeof(config.device_name), "%s%s",
                   PROVISIONING_SERVICE_DEVICE_PREFIX,
                   s_service.protocol.device_id);
    memcpy(config.service_uuid, s_service_uuid, sizeof(s_service_uuid));
    config.nu_lookup_count = sizeof(endpoints) / sizeof(endpoints[0]);
    config.nu_lookup = endpoints;
    config.ble_bonding = 0U;
    config.ble_sm_sc = 0U;
    config.ble_link_encryption = 0U;
    config.ble_notify = 0U;

    s_service.transport_stop_attempted = false;
    result = protocomm_ble_start(s_service.protocomm, &config);
    if (result == ESP_OK)
    {
        s_service.transport_started = true;
        const protocomm_security2_params_t security =
        {
            .salt = s_service.salt,
            .salt_len = PROVISIONING_SERVICE_SALT_BYTES,
            .verifier = s_service.verifier,
            .verifier_len = (uint16_t)s_service.verifier_length,
        };
        result = protocomm_set_security(
                     s_service.protocomm,
                     PROVISIONING_SERVICE_ENDPOINT_SESSION,
                     &protocomm_security2, &security);
    }
    if (result == ESP_OK)
    {
        result = protocomm_set_version(
                     s_service.protocomm,
                     PROVISIONING_SERVICE_ENDPOINT_VERSION,
                     s_protocol_version);
    }
    if (result == ESP_OK)
    {
        result = protocomm_add_endpoint(
                     s_service.protocomm,
                     PROVISIONING_SERVICE_ENDPOINT_CONTROL,
                     _provisioning_service_endpoint, &s_service);
    }
    if (result != ESP_OK)
    {
        if (s_service.transport_started)
        {
            const esp_err_t stop_result =
                _provisioning_service_transport_stop();
            if (stop_result != ESP_OK)
            {
                return stop_result;
            }
        }
        else
        {
            protocomm_delete(s_service.protocomm);
            s_service.protocomm = NULL;
            _provisioning_service_zero_secrets();
        }
    }
    return result;
}

static esp_err_t _provisioning_service_transport_stop(void)
{
    if (s_service.transport_faulted)
    {
        return s_service.transport_fault_error;
    }
    esp_err_t result = ESP_OK;
    if (s_service.transport_started && s_service.protocomm != NULL &&
            !s_service.transport_stop_attempted)
    {
        s_service.transport_stop_attempted = true;
        result = protocomm_ble_stop(s_service.protocomm);
        s_service.transport_started = false;
    }
    if (s_service.protocomm != NULL)
    {
        protocomm_delete(s_service.protocomm);
        s_service.protocomm = NULL;
    }
    _provisioning_service_zero_secrets();
    if (result != ESP_OK)
    {
        s_service.transport_fault_error = result;
        atomic_store_explicit(&s_service.transport_faulted, true,
                              memory_order_release);
        s_service.transport_accepting = false;
        mbedtls_platform_zeroize(&s_service.protocol,
                                 sizeof(s_service.protocol));
    }
    return result;
}

static void _provisioning_service_wait_transport_users(void)
{
    while (atomic_load_explicit(&s_transport_users,
                                memory_order_acquire) != 0U)
    {
        vTaskDelay(1U);
    }
}

static void _provisioning_service_release_resources(void)
{
    if (s_service.task != NULL)
    {
        vTaskDelete(s_service.task);
    }
    if (s_service.queue != NULL)
    {
        vQueueDelete(s_service.queue);
    }
    if (s_service.stopped != NULL)
    {
        vSemaphoreDelete(s_service.stopped);
    }
    if (s_service.mutex != NULL)
    {
        vSemaphoreDelete(s_service.mutex);
    }
    s_service.task = NULL;
    s_service.queue = NULL;
    s_service.stopped = NULL;
    s_service.mutex = NULL;
}

static esp_err_t _provisioning_service_worker_start(void)
{
    provisioning_service_snapshot_t snapshot;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    s_service.snapshot.state = PROVISIONING_SERVICE_STATE_OPENING;
    s_service.snapshot.active = true;
    s_service.snapshot.last_error = ESP_OK;
    s_service.snapshot.client_connected = false;
    s_service.snapshot.qr_ready = false;
    _provisioning_service_next_snapshot_locked(&snapshot);
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_worker_publish(&snapshot);

    const esp_err_t result = _provisioning_service_transport_start();
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (result == ESP_OK)
    {
        s_service.transport_accepting = true;
        s_service.finish_pending = false;
        s_service.success_pending = false;
        s_service.window_deadline = xTaskGetTickCount() +
                                    pdMS_TO_TICKS(s_service.config.window_ms);
        s_service.last_remaining_seconds = UINT32_MAX;
        s_service.snapshot.state = PROVISIONING_SERVICE_STATE_ADVERTISING;
        s_service.snapshot.qr_ready = true;
        s_service.snapshot.window_remaining_ms = s_service.config.window_ms;
        LOG_I("window started: device=%s", s_service.protocol.device_id);
    }
    else
    {
        atomic_store_explicit(&s_active, s_service.transport_faulted,
                              memory_order_release);
        s_service.snapshot.state = PROVISIONING_SERVICE_STATE_ERROR;
        s_service.snapshot.active = s_service.transport_faulted;
        s_service.snapshot.last_error = result;
        s_service.snapshot.window_remaining_ms = 0U;
        LOG_W("window start failed: %s", esp_err_to_name(result));
    }
    _provisioning_service_next_snapshot_locked(&snapshot);
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_worker_publish(&snapshot);
    return result;
}

static esp_err_t _provisioning_service_worker_stop(bool cancel_operation)
{
    if (s_service.transport_faulted)
    {
        return s_service.transport_fault_error;
    }
    if (!atomic_load_explicit(&s_active, memory_order_acquire) &&
            !s_service.transport_started && s_service.protocomm == NULL)
    {
        return ESP_OK;
    }

    provisioning_service_snapshot_t snapshot;
    uint64_t operation_id = 0U;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    s_service.transport_accepting = false;
    s_service.snapshot.state = PROVISIONING_SERVICE_STATE_CLOSING;
    s_service.snapshot.qr_ready = false;
    if (cancel_operation)
    {
        operation_id = provisioning_protocol_active_operation(
                           &s_service.protocol);
    }
    _provisioning_service_next_snapshot_locked(&snapshot);
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_worker_publish(&snapshot);

    if (operation_id != 0U)
    {
        (void)connectivity_manager_cancel(operation_id);
    }
    _provisioning_service_wait_transport_users();
    const esp_err_t result = _provisioning_service_transport_stop();

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    s_service.finish_pending = false;
    s_service.success_pending = false;
    const bool remain_active = result != ESP_OK || s_service.desired_open;
    s_service.snapshot.active = remain_active;
    s_service.snapshot.client_connected = false;
    s_service.snapshot.qr_ready = false;
    s_service.snapshot.window_remaining_ms = 0U;
    s_service.snapshot.state = result == ESP_OK ?
                               PROVISIONING_SERVICE_STATE_IDLE :
                               PROVISIONING_SERVICE_STATE_ERROR;
    s_service.snapshot.last_error = result;
    _provisioning_service_sync_operation_locked();
    _provisioning_service_next_snapshot_locked(&snapshot);
    xSemaphoreGive(s_service.mutex);
    atomic_store_explicit(&s_active, remain_active, memory_order_release);
    _provisioning_service_worker_publish(&snapshot);
    if (result != ESP_OK)
    {
        LOG_W("window stop failed: %s", esp_err_to_name(result));
    }
    return result;
}

static void _provisioning_service_worker_reconcile(void)
{
    while (true)
    {
        bool desired_open;
        bool cancel_operation;
        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        desired_open = s_service.desired_open;
        cancel_operation = s_service.desired_cancel_operation;
        const bool faulted = s_service.transport_faulted;
        if (faulted)
        {
            s_service.reconcile_queued = false;
        }
        xSemaphoreGive(s_service.mutex);
        if (faulted)
        {
            return;
        }

        esp_err_t result = ESP_OK;
        if (desired_open && !s_service.transport_started)
        {
            result = _provisioning_service_worker_start();
        }
        else if (!desired_open &&
                 (atomic_load_explicit(&s_active, memory_order_acquire) ||
                  s_service.transport_started || s_service.protocomm != NULL))
        {
            result = _provisioning_service_worker_stop(cancel_operation);
        }

        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        if (result != ESP_OK || s_service.transport_faulted)
        {
            if (!s_service.transport_faulted)
            {
                s_service.desired_open = false;
                atomic_store_explicit(&s_active, false,
                                      memory_order_release);
            }
            s_service.reconcile_queued = false;
            xSemaphoreGive(s_service.mutex);
            return;
        }
        const bool actual_open = s_service.transport_started;
        if (s_service.desired_open == actual_open)
        {
            s_service.reconcile_queued = false;
            atomic_store_explicit(&s_active, actual_open,
                                  memory_order_release);
            xSemaphoreGive(s_service.mutex);
            return;
        }
        xSemaphoreGive(s_service.mutex);
    }
}

static void _provisioning_service_worker_tick(void)
{
    if (!atomic_load_explicit(&s_active, memory_order_acquire) ||
            !s_service.transport_started)
    {
        return;
    }
    const TickType_t now = xTaskGetTickCount();
    bool finish = false;
    bool success = false;
    bool timeout = false;
    provisioning_service_snapshot_t snapshot;
    bool publish = false;

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    finish = s_service.finish_pending &&
             _provisioning_service_tick_reached(
                 now, s_service.finish_deadline);
    success = s_service.success_pending &&
              _provisioning_service_tick_reached(
                  now, s_service.success_deadline);
    timeout = _provisioning_service_tick_reached(
                  now, s_service.window_deadline);
    const uint32_t remaining = _provisioning_service_remaining_ms(
                                   s_service.window_deadline);
    const uint32_t seconds = (remaining + 999U) / 1000U;
    if (seconds != s_service.last_remaining_seconds)
    {
        s_service.last_remaining_seconds = seconds;
        s_service.snapshot.window_remaining_ms = remaining;
        _provisioning_service_next_snapshot_locked(&snapshot);
        publish = true;
    }
    xSemaphoreGive(s_service.mutex);
    if (publish)
    {
        _provisioning_service_worker_publish(&snapshot);
    }
    if (finish || success || timeout)
    {
        xSemaphoreTake(s_service.mutex, portMAX_DELAY);
        s_service.desired_open = false;
        s_service.desired_cancel_operation = timeout;
        s_service.reconcile_queued = true;
        xSemaphoreGive(s_service.mutex);
        _provisioning_service_worker_reconcile();
    }
}

static void _provisioning_service_worker(void *argument)
{
    (void)argument;
    bool running = true;
    while (running)
    {
        provisioning_command_t command;
        const BaseType_t received = xQueueReceive(
                                        s_service.queue, &command,
                                        pdMS_TO_TICKS(
                                            PROVISIONING_SERVICE_WORKER_POLL_MS));
        if (received == pdTRUE)
        {
            switch (command.type)
            {
            case PROVISIONING_COMMAND_RECONCILE:
                _provisioning_service_worker_reconcile();
                break;
            case PROVISIONING_COMMAND_PUBLISH:
                xSemaphoreTake(s_service.mutex, portMAX_DELAY);
                s_service.publish_command_queued = false;
                xSemaphoreGive(s_service.mutex);
                break;
            case PROVISIONING_COMMAND_DEINIT:
            {
                xSemaphoreTake(s_service.mutex, portMAX_DELAY);
                s_service.desired_open = false;
                s_service.desired_cancel_operation = true;
                s_service.reconcile_queued = false;
                s_service.publish_pending = false;
                s_service.publish_command_queued = false;
                s_service.publish_failure_reported = false;
                xSemaphoreGive(s_service.mutex);
                const esp_err_t result =
                    _provisioning_service_worker_stop(true);
                atomic_store_explicit(&s_worker_result, result,
                                      memory_order_release);
                running = result != ESP_OK;
                if (!running)
                {
                    atomic_store_explicit(&s_service.worker_stopping, true,
                                          memory_order_release);
                }
                xSemaphoreGive(s_service.stopped);
                break;
            }
            }
        }
        if (running)
        {
            _provisioning_service_flush_pending_publish();
            _provisioning_service_worker_tick();
            _provisioning_service_flush_pending_publish();
        }
    }
    vTaskSuspend(NULL);
    vTaskDelete(NULL);
}

static void _provisioning_service_ble_event(
    void *argument, esp_event_base_t event_base,
    int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_data;
    if (event_base != PROTOCOMM_TRANSPORT_BLE_EVENT ||
            !_provisioning_service_api_acquire())
    {
        return;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (s_service.transport_accepting)
    {
        if (event_id == PROTOCOMM_TRANSPORT_BLE_CONNECTED)
        {
            s_service.snapshot.client_connected = true;
            s_service.snapshot.state = PROVISIONING_SERVICE_STATE_CONNECTED;
            _provisioning_service_next_snapshot_locked(NULL);
            _provisioning_service_request_publish_locked();
        }
        else if (event_id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED)
        {
            s_service.snapshot.client_connected = false;
            s_service.snapshot.state = PROVISIONING_SERVICE_STATE_ADVERTISING;
            _provisioning_service_next_snapshot_locked(NULL);
            _provisioning_service_request_publish_locked();
        }
    }
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_api_release();
}

static void _provisioning_service_connectivity_status(
    event_bus_msg_id_t message_id, uint32_t subtype,
    const void *payload, size_t payload_size, void *user_data)
{
    (void)user_data;
    if (message_id != CONNECTIVITY_MANAGER_MSG ||
            subtype != CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT ||
            payload == NULL ||
            payload_size != sizeof(connectivity_manager_status_snapshot_t) ||
            !_provisioning_service_api_acquire())
    {
        return;
    }
    const connectivity_manager_status_snapshot_t *status = payload;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (provisioning_protocol_ingest_status(&s_service.protocol, status) &&
            s_service.transport_started)
    {
        s_service.success_pending = true;
        s_service.success_deadline = xTaskGetTickCount() +
                                     pdMS_TO_TICKS(
                                         s_service.config.success_grace_ms);
    }
    _provisioning_service_sync_operation_locked();
    _provisioning_service_next_snapshot_locked(NULL);
    _provisioning_service_request_publish_locked();
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_api_release();
}

static void _provisioning_service_connectivity_scan(
    event_bus_msg_id_t message_id, uint32_t subtype,
    const void *payload, size_t payload_size, void *user_data)
{
    (void)user_data;
    if (message_id != CONNECTIVITY_MANAGER_MSG ||
            subtype != CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT ||
            payload == NULL ||
            payload_size != sizeof(connectivity_manager_scan_snapshot_t) ||
            !_provisioning_service_api_acquire())
    {
        return;
    }
    const connectivity_manager_scan_snapshot_t *scan = payload;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    provisioning_protocol_ingest_scan(&s_service.protocol, scan);
    _provisioning_service_sync_operation_locked();
    _provisioning_service_next_snapshot_locked(NULL);
    _provisioning_service_request_publish_locked();
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_api_release();
}

static void _provisioning_service_refresh_connectivity(void)
{
    connectivity_manager_status_snapshot_t status;
    connectivity_manager_scan_snapshot_t scan;
    const esp_err_t status_result = connectivity_manager_get_status(&status);
    const esp_err_t scan_result = connectivity_manager_get_scan_snapshot(&scan);
    if (status_result != ESP_OK && scan_result != ESP_OK)
    {
        LOG_W("connectivity refresh failed: status=%s scan=%s",
              esp_err_to_name(status_result), esp_err_to_name(scan_result));
        return;
    }

    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (status_result == ESP_OK)
    {
        (void)provisioning_protocol_ingest_status(
            &s_service.protocol, &status);
    }
    if (scan_result == ESP_OK)
    {
        provisioning_protocol_ingest_scan(&s_service.protocol, &scan);
    }
    _provisioning_service_sync_operation_locked();
    _provisioning_service_next_snapshot_locked(NULL);
    _provisioning_service_request_publish_locked();
    xSemaphoreGive(s_service.mutex);
}

static bool _provisioning_service_config_valid(
    const provisioning_service_config_t *config)
{
    return config != NULL && config->task_priority > 0U &&
           config->task_priority < configMAX_PRIORITIES &&
           config->window_ms > 0U && config->success_grace_ms > 0U &&
           config->success_grace_ms < config->window_ms &&
           config->finish_close_delay_ms > 0U;
}

static esp_err_t _provisioning_service_queue_reconcile_locked(void)
{
    if (s_service.reconcile_queued ||
            s_service.desired_open == s_service.transport_started)
    {
        return ESP_OK;
    }
    const provisioning_command_t command =
    {
        .type = PROVISIONING_COMMAND_RECONCILE,
    };
    if (xQueueSend(s_service.queue, &command, 0U) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    s_service.reconcile_queued = true;
    return ESP_OK;
}

esp_err_t provisioning_service_init(
    const provisioning_service_config_t *config)
{
    if (!_provisioning_service_config_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load_explicit(&s_lifecycle, memory_order_acquire) ==
            PROVISIONING_LIFECYCLE_RUNNING)
    {
        return memcmp(&s_service.config, config, sizeof(*config)) == 0 ?
               ESP_OK : ESP_ERR_INVALID_STATE;
    }
    int expected = PROVISIONING_LIFECYCLE_STOPPED;
    if (!atomic_compare_exchange_strong_explicit(
                &s_lifecycle, &expected, PROVISIONING_LIFECYCLE_STARTING,
                memory_order_acq_rel, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_service, 0, sizeof(s_service));
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
    _provisioning_service_reset_diagnostics();
#endif
    atomic_init(&s_service.transport_started, false);
    atomic_init(&s_service.transport_faulted, false);
    atomic_init(&s_service.worker_stopping, false);
    s_service.config = *config;
    s_service.status_subscription = EVENT_BUS_SUB_HANDLE_INVALID;
    s_service.scan_subscription = EVENT_BUS_SUB_HANDLE_INVALID;
    s_service.mutex = xSemaphoreCreateMutexStatic(&s_service.mutex_control);
    s_service.stopped = xSemaphoreCreateBinaryStatic(
                            &s_service.stopped_control);
    s_service.queue = xQueueCreateStatic(
                          CONFIG_PROVISIONING_SERVICE_QUEUE_DEPTH,
                          sizeof(provisioning_command_t),
                          s_service.queue_storage.bytes,
                          &s_service.queue_control);
    esp_err_t result = s_service.mutex != NULL && s_service.stopped != NULL &&
                       s_service.queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;

    uint8_t mac[6];
    char device_id[PROVISIONING_PROTOCOL_DEVICE_ID_BYTES + 1U];
    memset(mac, 0, sizeof(mac));
    memset(device_id, 0, sizeof(device_id));
    if (result == ESP_OK)
    {
        result = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    }
    if (result == ESP_OK)
    {
        (void)snprintf(device_id, sizeof(device_id), "%02X%02X%02X",
                       mac[3], mac[4], mac[5]);
        connectivity_manager_status_snapshot_t status;
        result = connectivity_manager_get_status(&status);
        if (result == ESP_OK)
        {
            const esp_app_desc_t *description = esp_app_get_description();
            result = provisioning_protocol_init(
                         &s_service.protocol, device_id,
                         description != NULL ? description->version : "unknown",
                         &status);
        }
    }
    mbedtls_platform_zeroize(mac, sizeof(mac));
    if (result == ESP_OK)
    {
        result = event_bus_subscribe(
                     CONNECTIVITY_MANAGER_MSG,
                     CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
                     _provisioning_service_connectivity_status, NULL,
                     EVENT_BUS_DISPATCH_PUBLISHER,
                     &s_service.status_subscription);
    }
    if (result == ESP_OK)
    {
        result = event_bus_subscribe(
                     CONNECTIVITY_MANAGER_MSG,
                     CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
                     _provisioning_service_connectivity_scan, NULL,
                     EVENT_BUS_DISPATCH_PUBLISHER,
                     &s_service.scan_subscription);
    }
    if (result == ESP_OK)
    {
        result = esp_event_handler_instance_register(
                     PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
                     _provisioning_service_ble_event, NULL,
                     &s_service.ble_event_handler);
        s_service.ble_event_registered = result == ESP_OK;
    }
    if (result == ESP_OK)
    {
        s_service.snapshot.generation = 1U;
        s_service.snapshot.available = true;
        s_service.snapshot.state = PROVISIONING_SERVICE_STATE_IDLE;
        (void)snprintf(s_service.snapshot.device_name,
                       sizeof(s_service.snapshot.device_name), "%s%s",
                       PROVISIONING_SERVICE_DEVICE_PREFIX, device_id);
        atomic_store_explicit(&s_worker_result, ESP_OK,
                              memory_order_release);
        s_service.task = xTaskCreateStatic(
                             _provisioning_service_worker, "provisioning",
                             CONFIG_PROVISIONING_SERVICE_TASK_STACK, NULL,
                             config->task_priority,
                             s_service.task_stack, &s_service.task_control);
        result = s_service.task != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (result == ESP_OK)
    {
        atomic_store_explicit(&s_active, false, memory_order_release);
        atomic_store_explicit(&s_lifecycle,
                              PROVISIONING_LIFECYCLE_RUNNING,
                              memory_order_release);
        _provisioning_service_refresh_connectivity();
        LOG_I("ready: device=%s, window=%u ms",
              s_service.protocol.device_id,
              (unsigned)config->window_ms);
        return ESP_OK;
    }

    if (s_service.ble_event_registered)
    {
        (void)esp_event_handler_instance_unregister(
            PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
            s_service.ble_event_handler);
    }
    if (s_service.scan_subscription != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        (void)event_bus_unsubscribe(s_service.scan_subscription);
    }
    if (s_service.status_subscription != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        (void)event_bus_unsubscribe(s_service.status_subscription);
    }
    _provisioning_service_release_resources();
    memset(&s_service, 0, sizeof(s_service));
    atomic_store_explicit(&s_lifecycle, PROVISIONING_LIFECYCLE_STOPPED,
                          memory_order_release);
    return result;
}

esp_err_t provisioning_service_deinit(uint32_t timeout_ms)
{
    int lifecycle = atomic_load_explicit(&s_lifecycle, memory_order_acquire);
    bool command_admitted = lifecycle == PROVISIONING_LIFECYCLE_STOPPING;
    if (lifecycle == PROVISIONING_LIFECYCLE_STOPPED)
    {
        return ESP_OK;
    }
    if (!command_admitted)
    {
        int expected = PROVISIONING_LIFECYCLE_RUNNING;
        if (!atomic_compare_exchange_strong_explicit(
                    &s_lifecycle, &expected, PROVISIONING_LIFECYCLE_STOPPING,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    const TickType_t started = xTaskGetTickCount();
    TickType_t timeout = timeout_ms == PROVISIONING_SERVICE_WAIT_FOREVER ?
                         portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0U && timeout == 0U)
    {
        timeout = 1U;
    }
    while (!command_admitted &&
            atomic_load_explicit(&s_api_users, memory_order_acquire) != 0U)
    {
        if (timeout != portMAX_DELAY &&
                xTaskGetTickCount() - started >= timeout)
        {
            atomic_store_explicit(&s_lifecycle,
                                  PROVISIONING_LIFECYCLE_RUNNING,
                                  memory_order_release);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1U);
    }

    if (!command_admitted)
    {
        const provisioning_command_t command =
        {
            .type = PROVISIONING_COMMAND_DEINIT,
        };
        const TickType_t remaining =
            _provisioning_service_timeout_remaining(started, timeout);
        if (xQueueSend(s_service.queue, &command, remaining) != pdTRUE)
        {
            atomic_store_explicit(&s_lifecycle,
                                  PROVISIONING_LIFECYCLE_RUNNING,
                                  memory_order_release);
            return ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t result = ESP_OK;
    if (!atomic_load_explicit(&s_service.worker_stopping,
                              memory_order_acquire))
    {
        const TickType_t remaining =
            _provisioning_service_timeout_remaining(started, timeout);
        if (xSemaphoreTake(s_service.stopped, remaining) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }
        result = atomic_load_explicit(&s_worker_result,
                                      memory_order_acquire);
    }
    if (result != ESP_OK)
    {
        atomic_store_explicit(&s_lifecycle, PROVISIONING_LIFECYCLE_RUNNING,
                              memory_order_release);
        return result;
    }
    result = _provisioning_service_wait_worker_suspended(started, timeout);
    if (result != ESP_OK)
    {
        return result;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    s_service.publish_pending = false;
    s_service.publish_command_queued = false;
    s_service.publish_failure_reported = false;
    xSemaphoreGive(s_service.mutex);
    if (s_service.ble_event_registered)
    {
        const esp_err_t unregister_result =
            esp_event_handler_instance_unregister(
                PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
                s_service.ble_event_handler);
        if (result == ESP_OK && unregister_result != ESP_OK &&
                unregister_result != ESP_ERR_NOT_FOUND)
        {
            result = unregister_result;
        }
    }
    if (s_service.scan_subscription != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        const esp_err_t unsubscribe_result =
            event_bus_unsubscribe(s_service.scan_subscription);
        if (result == ESP_OK && unsubscribe_result != ESP_OK &&
                unsubscribe_result != ESP_ERR_NOT_FOUND)
        {
            result = unsubscribe_result;
        }
    }
    if (s_service.status_subscription != EVENT_BUS_SUB_HANDLE_INVALID)
    {
        const esp_err_t unsubscribe_result =
            event_bus_unsubscribe(s_service.status_subscription);
        if (result == ESP_OK && unsubscribe_result != ESP_OK &&
                unsubscribe_result != ESP_ERR_NOT_FOUND)
        {
            result = unsubscribe_result;
        }
    }
    _provisioning_service_release_resources();
    memset(&s_service, 0, sizeof(s_service));
    atomic_store_explicit(&s_active, false, memory_order_release);
    atomic_store_explicit(&s_lifecycle, PROVISIONING_LIFECYCLE_STOPPED,
                          memory_order_release);
    return result;
}

esp_err_t provisioning_service_open_window(void)
{
    if (!_provisioning_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (s_service.transport_faulted)
    {
        result = s_service.transport_fault_error;
    }
    else if (s_service.suspended)
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else
    {
        const bool previous_desired = s_service.desired_open;
        const bool previous_cancel = s_service.desired_cancel_operation;
        const bool previous_active = atomic_load_explicit(
                                         &s_active, memory_order_acquire);
#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
        if (!previous_active)
        {
            _provisioning_service_reset_diagnostics();
        }
#endif
        s_service.desired_open = true;
        s_service.desired_cancel_operation = false;
        atomic_store_explicit(&s_active, true, memory_order_release);
        result = _provisioning_service_queue_reconcile_locked();
        if (result != ESP_OK)
        {
            s_service.desired_open = previous_desired;
            s_service.desired_cancel_operation = previous_cancel;
            atomic_store_explicit(&s_active, previous_active,
                                  memory_order_release);
        }
    }
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_api_release();
    return result;
}

esp_err_t provisioning_service_close_window(void)
{
    if (!_provisioning_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (s_service.transport_faulted)
    {
        result = s_service.transport_fault_error;
    }
    else
    {
        const bool previous_desired = s_service.desired_open;
        const bool previous_cancel = s_service.desired_cancel_operation;
        s_service.desired_open = false;
        s_service.desired_cancel_operation = true;
        result = _provisioning_service_queue_reconcile_locked();
        if (result != ESP_OK)
        {
            s_service.desired_open = previous_desired;
            s_service.desired_cancel_operation = previous_cancel;
        }
        else if (!s_service.desired_open &&
                 !s_service.transport_started &&
                 !s_service.reconcile_queued)
        {
            atomic_store_explicit(&s_active, false, memory_order_release);
        }
    }
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_api_release();
    return result;
}

esp_err_t provisioning_service_suspend(uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!_provisioning_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (s_service.transport_faulted)
    {
        result = s_service.transport_fault_error;
    }
    else if (atomic_load_explicit(&s_active, memory_order_acquire))
    {
        result = ESP_ERR_INVALID_STATE;
    }
    else if (!s_service.suspended)
    {
        s_service.suspended = true;
        s_service.snapshot.state = PROVISIONING_SERVICE_STATE_SUSPENDED;
        _provisioning_service_next_snapshot_locked(NULL);
        _provisioning_service_request_publish_locked();
    }
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_api_release();
    return result;
}

esp_err_t provisioning_service_resume(uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!_provisioning_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (s_service.transport_faulted)
    {
        result = s_service.transport_fault_error;
    }
    else if (s_service.suspended)
    {
        s_service.suspended = false;
        s_service.snapshot.state = PROVISIONING_SERVICE_STATE_IDLE;
        _provisioning_service_next_snapshot_locked(NULL);
        _provisioning_service_request_publish_locked();
    }
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_api_release();
    return result;
}

esp_err_t provisioning_service_get_status(
    provisioning_service_status_t *snapshot)
{
    if (snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_provisioning_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    *snapshot = s_service.snapshot;
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_api_release();
    return ESP_OK;
}

#if CONFIG_PROVISIONING_SERVICE_DIAGNOSTICS
esp_err_t provisioning_service_get_diagnostics(
    provisioning_service_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_provisioning_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    diagnostics->protected_request_count = atomic_load_explicit(
            &s_protected_request_count, memory_order_relaxed);
    diagnostics->protected_success_count = atomic_load_explicit(
            &s_protected_success_count, memory_order_relaxed);
    diagnostics->protected_failure_count = atomic_load_explicit(
            &s_protected_failure_count, memory_order_relaxed);
    diagnostics->snapshot_success_count = atomic_load_explicit(
            &s_snapshot_success_count, memory_order_relaxed);
    diagnostics->last_snapshot_request_id = atomic_load_explicit(
            &s_last_snapshot_request_id, memory_order_relaxed);
    diagnostics->last_snapshot_success_us = atomic_load_explicit(
            &s_last_snapshot_success_us, memory_order_acquire);
    diagnostics->worker_found = s_service.task != NULL;
    diagnostics->worker_stack_high_water = diagnostics->worker_found ?
                                           (uint32_t)uxTaskGetStackHighWaterMark(s_service.task) : 0U;
    _provisioning_service_api_release();
    return ESP_OK;
}
#endif

esp_err_t provisioning_service_copy_qr(char *buffer, size_t capacity,
                                       size_t *out_length)
{
    if (buffer == NULL || capacity == 0U || out_length == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!_provisioning_service_api_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    xSemaphoreTake(s_service.mutex, portMAX_DELAY);
    if (!s_service.snapshot.qr_ready)
    {
        result = ESP_ERR_NOT_FOUND;
    }
    else
    {
        const size_t length = strnlen(s_service.qr, sizeof(s_service.qr));
        if (length + 1U > capacity)
        {
            result = ESP_ERR_INVALID_SIZE;
        }
        else
        {
            memcpy(buffer, s_service.qr, length + 1U);
            *out_length = length;
        }
    }
    xSemaphoreGive(s_service.mutex);
    _provisioning_service_api_release();
    return result;
}

bool provisioning_service_is_active(void)
{
    return atomic_load_explicit(&s_active, memory_order_acquire);
}

_Static_assert(sizeof(provisioning_service_snapshot_t) <=
               EVENT_BUS_MAX_UI_PAYLOAD_SIZE,
               "provisioning snapshot exceeds event-bus payload");
