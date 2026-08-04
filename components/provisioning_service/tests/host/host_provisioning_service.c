#include "host_provisioning_service.h"

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_srp.h"
#include "event_bus.h"
#include "protocomm.h"
#include "protocomm_ble.h"
#include "protocomm_security2.h"
#include "provisioning_service.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOST_SUBSCRIPTION_CAPACITY 8U

typedef struct host_subscription
{
    event_bus_msg_id_t message_id;
    uint32_t subtype;
    event_bus_cb_t callback;
    void *user_data;
    uint64_t handle;
    bool active;
} host_subscription_t;

typedef struct host_event_handler
{
    esp_event_base_t event_base;
    int32_t event_id;
    esp_event_handler_t handler;
    void *argument;
    bool active;
} host_event_handler_t;

struct protocomm
{
    protocomm_req_handler_t control_handler;
    void *control_private_data;
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_stop_changed = PTHREAD_COND_INITIALIZER;
static host_subscription_t s_subscriptions[HOST_SUBSCRIPTION_CAPACITY];
static host_event_handler_t s_ble_handler;
static protocomm_t *s_protocomm;
static connectivity_manager_status_snapshot_t s_manager_status;
static connectivity_manager_scan_snapshot_t s_manager_scan;
static connectivity_manager_status_snapshot_t s_staged_status;
static connectivity_manager_scan_snapshot_t s_staged_scan;
static bool s_init_refresh_staged;
static esp_app_desc_t s_app_description;
static uint64_t s_next_operation_id;
static uint64_t s_canceled_operation;
static unsigned s_cancel_count;
static unsigned s_transport_start_count;
static unsigned s_transport_stop_count;
static unsigned s_random_generation;
static esp_err_t s_next_stop_result;
static bool s_transport_started;
static bool s_transport_shape_valid;
static bool s_block_next_stop;
static bool s_stop_blocked;
static bool s_release_stop;
static bool s_salt_zeroized;
static bool s_verifier_zeroized;
static char s_device_name[30];
static char s_protocol_version[128];
static _Thread_local unsigned s_publish_depth;
static unsigned s_max_publish_depth;
static unsigned s_provisioning_publish_count;
static uint64_t s_last_publish_generation;
static uint64_t s_failed_publish_generation;
static esp_err_t s_next_publish_result;

EVENT_BUS_DEFINE_ID(CONNECTIVITY_MANAGER_MSG);
esp_event_base_t PROTOCOMM_TRANSPORT_BLE_EVENT = "PROTOCOMM_BLE";
const protocomm_security_t protocomm_security2 = {.marker = 2U};

int64_t esp_timer_get_time(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * INT64_C(1000000) +
           (int64_t)now.tv_nsec / INT64_C(1000);
}

static void _host_sleep_ms(uint32_t milliseconds)
{
    const struct timespec delay =
    {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

void host_provisioning_reset(void)
{
    (void)pthread_mutex_lock(&s_lock);
    memset(s_subscriptions, 0, sizeof(s_subscriptions));
    memset(&s_ble_handler, 0, sizeof(s_ble_handler));
    memset(&s_manager_status, 0, sizeof(s_manager_status));
    s_manager_status.generation = 1U;
    s_manager_status.available = true;
    s_manager_status.radio_available = true;
    s_manager_status.state = CONNECTIVITY_MANAGER_STATE_IDLE;
    s_manager_status.failure = CONNECTIVITY_MANAGER_FAILURE_NONE;
    memset(&s_manager_scan, 0, sizeof(s_manager_scan));
    s_manager_scan.generation = 1U;
    s_manager_scan.last_error = ESP_ERR_NOT_FOUND;
    memset(&s_staged_status, 0, sizeof(s_staged_status));
    memset(&s_staged_scan, 0, sizeof(s_staged_scan));
    s_init_refresh_staged = false;
    memset(&s_app_description, 0, sizeof(s_app_description));
    memcpy(s_app_description.version, "host-1.0", sizeof("host-1.0"));
    s_protocomm = NULL;
    s_next_operation_id = 1U;
    s_canceled_operation = 0U;
    s_cancel_count = 0U;
    s_transport_start_count = 0U;
    s_transport_stop_count = 0U;
    s_random_generation = 0U;
    s_next_stop_result = ESP_OK;
    s_transport_started = false;
    s_transport_shape_valid = false;
    s_block_next_stop = false;
    s_stop_blocked = false;
    s_release_stop = false;
    s_salt_zeroized = false;
    s_verifier_zeroized = false;
    memset(s_device_name, 0, sizeof(s_device_name));
    memset(s_protocol_version, 0, sizeof(s_protocol_version));
    s_publish_depth = 0U;
    s_max_publish_depth = 0U;
    s_provisioning_publish_count = 0U;
    s_last_publish_generation = 0U;
    s_failed_publish_generation = 0U;
    s_next_publish_result = ESP_OK;
    (void)pthread_mutex_unlock(&s_lock);
}

bool host_provisioning_wait_transport(bool started, uint32_t timeout_ms)
{
    for (uint32_t elapsed = 0U; elapsed <= timeout_ms; ++elapsed)
    {
        (void)pthread_mutex_lock(&s_lock);
        const bool ready = started ?
                           s_transport_started && s_protocomm != NULL &&
                           s_protocomm->control_handler != NULL &&
                           s_protocol_version[0] != '\0' :
                           !s_transport_started && s_protocomm == NULL;
        (void)pthread_mutex_unlock(&s_lock);
        if (ready)
        {
            return true;
        }
        _host_sleep_ms(1U);
    }
    return false;
}

esp_err_t host_provisioning_request(
    uint8_t *input, size_t input_length,
    uint8_t **output, ssize_t *output_length)
{
    if (input == NULL || output == NULL || output_length == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_lock);
    const bool started = s_transport_started && s_protocomm != NULL;
    protocomm_req_handler_t handler = started ?
                                      s_protocomm->control_handler : NULL;
    void *private_data = started ?
                         s_protocomm->control_private_data : NULL;
    (void)pthread_mutex_unlock(&s_lock);
    return handler != NULL ?
           handler(1U, input, (ssize_t)input_length,
                   output, output_length, private_data) :
           ESP_ERR_INVALID_STATE;
}

void host_provisioning_emit_ble(bool connected)
{
    (void)pthread_mutex_lock(&s_lock);
    const host_event_handler_t handler = s_ble_handler;
    (void)pthread_mutex_unlock(&s_lock);
    if (handler.active)
    {
        handler.handler(handler.argument, handler.event_base,
                        connected ? PROTOCOMM_TRANSPORT_BLE_CONNECTED :
                        PROTOCOMM_TRANSPORT_BLE_DISCONNECTED, NULL);
    }
}

void host_provisioning_publish_status(
    const connectivity_manager_status_snapshot_t *status)
{
    if (status == NULL)
    {
        return;
    }
    (void)pthread_mutex_lock(&s_lock);
    s_manager_status = *status;
    (void)pthread_mutex_unlock(&s_lock);
    (void)event_bus_publish(
        CONNECTIVITY_MANAGER_MSG,
        CONNECTIVITY_MANAGER_MSG_SUB_TYPE_STATUS_SNAPSHOT,
        status, sizeof(*status), 0U);
}

void host_provisioning_publish_scan(
    const connectivity_manager_scan_snapshot_t *scan)
{
    if (scan != NULL)
    {
        (void)pthread_mutex_lock(&s_lock);
        s_manager_scan = *scan;
        (void)pthread_mutex_unlock(&s_lock);
        (void)event_bus_publish(
            CONNECTIVITY_MANAGER_MSG,
            CONNECTIVITY_MANAGER_MSG_SUB_TYPE_SCAN_SNAPSHOT,
            scan, sizeof(*scan), 0U);
    }
}

void host_provisioning_stage_init_refresh(
    const connectivity_manager_status_snapshot_t *status,
    const connectivity_manager_scan_snapshot_t *scan)
{
    if (status == NULL || scan == NULL)
    {
        return;
    }
    (void)pthread_mutex_lock(&s_lock);
    s_staged_status = *status;
    s_staged_scan = *scan;
    s_init_refresh_staged = true;
    (void)pthread_mutex_unlock(&s_lock);
}

uint64_t host_provisioning_canceled_operation(void)
{
    (void)pthread_mutex_lock(&s_lock);
    const uint64_t operation_id = s_canceled_operation;
    (void)pthread_mutex_unlock(&s_lock);
    return operation_id;
}

unsigned host_provisioning_cancel_count(void)
{
    (void)pthread_mutex_lock(&s_lock);
    const unsigned count = s_cancel_count;
    (void)pthread_mutex_unlock(&s_lock);
    return count;
}

unsigned host_provisioning_transport_start_count(void)
{
    (void)pthread_mutex_lock(&s_lock);
    const unsigned count = s_transport_start_count;
    (void)pthread_mutex_unlock(&s_lock);
    return count;
}

unsigned host_provisioning_transport_stop_count(void)
{
    (void)pthread_mutex_lock(&s_lock);
    const unsigned count = s_transport_stop_count;
    (void)pthread_mutex_unlock(&s_lock);
    return count;
}

void host_provisioning_fail_next_stop(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_lock);
    s_next_stop_result = result;
    (void)pthread_mutex_unlock(&s_lock);
}

void host_provisioning_block_next_stop(void)
{
    (void)pthread_mutex_lock(&s_lock);
    s_block_next_stop = true;
    s_stop_blocked = false;
    s_release_stop = false;
    (void)pthread_mutex_unlock(&s_lock);
}

bool host_provisioning_wait_stop_blocked(uint32_t timeout_ms)
{
    for (uint32_t elapsed = 0U; elapsed <= timeout_ms; ++elapsed)
    {
        (void)pthread_mutex_lock(&s_lock);
        const bool blocked = s_stop_blocked;
        (void)pthread_mutex_unlock(&s_lock);
        if (blocked)
        {
            return true;
        }
        _host_sleep_ms(1U);
    }
    return false;
}

void host_provisioning_release_stop(void)
{
    (void)pthread_mutex_lock(&s_lock);
    s_release_stop = true;
    (void)pthread_cond_broadcast(&s_stop_changed);
    (void)pthread_mutex_unlock(&s_lock);
}

bool host_provisioning_application_secrets_zeroized(void)
{
    (void)pthread_mutex_lock(&s_lock);
    const bool zeroized = s_salt_zeroized && s_verifier_zeroized;
    (void)pthread_mutex_unlock(&s_lock);
    return zeroized;
}

const char *host_provisioning_device_name(void)
{
    return s_device_name;
}

const char *host_provisioning_protocol_version(void)
{
    return s_protocol_version;
}

bool host_provisioning_transport_shape_valid(void)
{
    (void)pthread_mutex_lock(&s_lock);
    const bool valid = s_transport_shape_valid;
    (void)pthread_mutex_unlock(&s_lock);
    return valid;
}

void host_provisioning_reset_publish_observer(void)
{
    (void)pthread_mutex_lock(&s_lock);
    s_publish_depth = 0U;
    s_max_publish_depth = 0U;
    s_provisioning_publish_count = 0U;
    s_last_publish_generation = 0U;
    s_failed_publish_generation = 0U;
    s_next_publish_result = ESP_OK;
    (void)pthread_mutex_unlock(&s_lock);
}

void host_provisioning_fail_next_publish(esp_err_t result)
{
    (void)pthread_mutex_lock(&s_lock);
    s_next_publish_result = result;
    (void)pthread_mutex_unlock(&s_lock);
}

bool host_provisioning_wait_publish_count(unsigned count, uint32_t timeout_ms)
{
    for (uint32_t elapsed = 0U; elapsed <= timeout_ms; ++elapsed)
    {
        if (host_provisioning_publish_count() >= count)
        {
            return true;
        }
        _host_sleep_ms(1U);
    }
    return false;
}

unsigned host_provisioning_publish_count(void)
{
    (void)pthread_mutex_lock(&s_lock);
    const unsigned count = s_provisioning_publish_count;
    (void)pthread_mutex_unlock(&s_lock);
    return count;
}

unsigned host_provisioning_max_publish_depth(void)
{
    (void)pthread_mutex_lock(&s_lock);
    const unsigned depth = s_max_publish_depth;
    (void)pthread_mutex_unlock(&s_lock);
    return depth;
}

uint64_t host_provisioning_last_publish_generation(void)
{
    (void)pthread_mutex_lock(&s_lock);
    const uint64_t generation = s_last_publish_generation;
    (void)pthread_mutex_unlock(&s_lock);
    return generation;
}

uint64_t host_provisioning_failed_publish_generation(void)
{
    (void)pthread_mutex_lock(&s_lock);
    const uint64_t generation = s_failed_publish_generation;
    (void)pthread_mutex_unlock(&s_lock);
    return generation;
}

esp_err_t event_bus_init(void)
{
    return ESP_OK;
}

esp_err_t event_bus_subscribe(event_bus_msg_id_t message_id, uint32_t subtype,
                              event_bus_cb_t callback, void *user_data,
                              event_bus_dispatch_context_t context,
                              event_bus_sub_handle_t *out_handle)
{
    if (message_id == NULL || callback == NULL || out_handle == NULL ||
            context != EVENT_BUS_DISPATCH_PUBLISHER)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = ESP_ERR_NO_MEM;
    for (size_t index = 0U; index < HOST_SUBSCRIPTION_CAPACITY; ++index)
    {
        if (!s_subscriptions[index].active)
        {
            s_subscriptions[index] = (host_subscription_t)
            {
                .message_id = message_id,
                .subtype = subtype,
                .callback = callback,
                .user_data = user_data,
                .handle = index + 1U,
                .active = true,
            };
            *out_handle = s_subscriptions[index].handle;
            result = ESP_OK;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t event_bus_unsubscribe(event_bus_sub_handle_t handle)
{
    (void)pthread_mutex_lock(&s_lock);
    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (size_t index = 0U; index < HOST_SUBSCRIPTION_CAPACITY; ++index)
    {
        if (s_subscriptions[index].active &&
                s_subscriptions[index].handle == handle)
        {
            memset(&s_subscriptions[index], 0,
                   sizeof(s_subscriptions[index]));
            result = ESP_OK;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t event_bus_publish(event_bus_msg_id_t message_id, uint32_t subtype,
                            const void *payload, size_t payload_size,
                            uint32_t flags)
{
    (void)flags;
    host_subscription_t callbacks[HOST_SUBSCRIPTION_CAPACITY];
    size_t callback_count = 0U;
    ++s_publish_depth;
    (void)pthread_mutex_lock(&s_lock);
    if (s_publish_depth > s_max_publish_depth)
    {
        s_max_publish_depth = s_publish_depth;
    }
    esp_err_t result = ESP_OK;
    if (message_id == PROVISIONING_SERVICE_MSG &&
            payload != NULL &&
            payload_size == sizeof(provisioning_service_snapshot_t))
    {
        const provisioning_service_snapshot_t *snapshot = payload;
        if (s_next_publish_result != ESP_OK)
        {
            result = s_next_publish_result;
            s_next_publish_result = ESP_OK;
            s_failed_publish_generation = snapshot->generation;
        }
        else
        {
            ++s_provisioning_publish_count;
            s_last_publish_generation = snapshot->generation;
        }
    }
    if (result != ESP_OK)
    {
        (void)pthread_mutex_unlock(&s_lock);
        --s_publish_depth;
        return result;
    }
    for (size_t index = 0U; index < HOST_SUBSCRIPTION_CAPACITY; ++index)
    {
        if (s_subscriptions[index].active &&
                s_subscriptions[index].message_id == message_id &&
                (s_subscriptions[index].subtype == subtype ||
                 s_subscriptions[index].subtype == EVENT_BUS_SUB_TYPE_ANY))
        {
            callbacks[callback_count++] = s_subscriptions[index];
        }
    }
    (void)pthread_mutex_unlock(&s_lock);
    for (size_t index = 0U; index < callback_count; ++index)
    {
        callbacks[index].callback(message_id, subtype, payload,
                                  payload_size, callbacks[index].user_data);
    }
    --s_publish_depth;
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_register(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_t handler, void *argument,
    esp_event_handler_instance_t *instance)
{
    if (event_base == NULL || handler == NULL || instance == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_lock);
    if (s_ble_handler.active)
    {
        (void)pthread_mutex_unlock(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_ble_handler = (host_event_handler_t)
    {
        .event_base = event_base,
        .event_id = event_id,
        .handler = handler,
        .argument = argument,
        .active = true,
    };
    *instance = &s_ble_handler;
    (void)pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_unregister(
    esp_event_base_t event_base, int32_t event_id,
    esp_event_handler_instance_t instance)
{
    (void)pthread_mutex_lock(&s_lock);
    if (!s_ble_handler.active || instance != &s_ble_handler ||
            s_ble_handler.event_base != event_base ||
            s_ble_handler.event_id != event_id)
    {
        (void)pthread_mutex_unlock(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    memset(&s_ble_handler, 0, sizeof(s_ble_handler));
    (void)pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    return &s_app_description;
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    if (mac == NULL || type != ESP_MAC_EFUSE_FACTORY)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t factory_mac[6] = {0xFC, 0x01, 0x2C, 0xA1, 0xB2, 0xC3};
    memcpy(mac, factory_mac, sizeof(factory_mac));
    return ESP_OK;
}

void esp_fill_random(void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    (void)pthread_mutex_lock(&s_lock);
    ++s_random_generation;
    const unsigned generation = s_random_generation;
    (void)pthread_mutex_unlock(&s_lock);
    for (size_t index = 0U; index < length; ++index)
    {
        bytes[index] = (uint8_t)(generation + index);
    }
}

void host_mbedtls_platform_zeroize(void *buffer, size_t length)
{
    if (buffer == NULL)
    {
        return;
    }
    const uint8_t *bytes = buffer;
    bool salt = length == 16U;
    bool verifier = length == 32U;
    for (size_t index = 0U; index < length; ++index)
    {
        salt = salt && bytes[index] == 0x5AU;
        verifier = verifier && bytes[index] == 0xA5U;
    }
    (void)pthread_mutex_lock(&s_lock);
    s_salt_zeroized = s_salt_zeroized || salt;
    s_verifier_zeroized = s_verifier_zeroized || verifier;
    (void)pthread_mutex_unlock(&s_lock);
    volatile uint8_t *mutable_bytes = buffer;
    while (length > 0U)
    {
        *mutable_bytes++ = 0U;
        --length;
    }
}

esp_err_t esp_srp_gen_salt_verifier(
    const char *username, int username_length,
    const char *password, int password_length,
    char **salt, int salt_length,
    char **verifier, int *verifier_length)
{
    if (username == NULL || username_length <= 0 || password == NULL ||
            password_length <= 0 || salt == NULL || salt_length <= 0 ||
            verifier == NULL || verifier_length == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *salt = malloc((size_t)salt_length);
    *verifier_length = 32;
    *verifier = malloc((size_t) * verifier_length);
    if (*salt == NULL || *verifier == NULL)
    {
        free(*salt);
        free(*verifier);
        *salt = NULL;
        *verifier = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(*salt, 0x5A, (size_t)salt_length);
    memset(*verifier, 0xA5, (size_t)*verifier_length);
    (void)pthread_mutex_lock(&s_lock);
    s_salt_zeroized = false;
    s_verifier_zeroized = false;
    (void)pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

protocomm_t *protocomm_new(void)
{
    return calloc(1U, sizeof(protocomm_t));
}

void protocomm_delete(protocomm_t *protocomm)
{
    (void)pthread_mutex_lock(&s_lock);
    if (s_protocomm == protocomm)
    {
        s_protocomm = NULL;
    }
    (void)pthread_mutex_unlock(&s_lock);
    free(protocomm);
}

esp_err_t protocomm_ble_start(
    protocomm_t *protocomm, const protocomm_ble_config_t *config)
{
    if (protocomm == NULL || config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    static const uint8_t service_uuid[BLE_UUID128_VAL_LENGTH] =
    {
        0x6B, 0x0E, 0x39, 0x9E, 0x97, 0x73, 0x21, 0x8C,
        0x9F, 0x40, 0x7E, 0xB4, 0x36, 0xC8, 0xF1, 0xD8,
    };
    const bool endpoints_valid = config->nu_lookup_count == 3 &&
                                 strcmp(config->nu_lookup[0].name, "proto-ver") == 0 &&
                                 config->nu_lookup[0].uuid == 0xFF50U &&
                                 strcmp(config->nu_lookup[1].name, "prov-session") == 0 &&
                                 config->nu_lookup[1].uuid == 0xFF51U &&
                                 strcmp(config->nu_lookup[2].name, "mt-prov") == 0 &&
                                 config->nu_lookup[2].uuid == 0xFF52U;
    (void)pthread_mutex_lock(&s_lock);
    s_protocomm = protocomm;
    s_transport_started = true;
    ++s_transport_start_count;
    memcpy(s_device_name, config->device_name, sizeof(s_device_name));
    s_device_name[sizeof(s_device_name) - 1U] = '\0';
    s_transport_shape_valid = endpoints_valid &&
                              memcmp(config->service_uuid, service_uuid, sizeof(service_uuid)) == 0 &&
                              config->ble_bonding == 0U && config->ble_sm_sc == 0U &&
                              config->ble_link_encryption == 0U && config->ble_notify == 0U;
    (void)pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t protocomm_ble_stop(protocomm_t *protocomm)
{
    (void)pthread_mutex_lock(&s_lock);
    const bool valid = protocomm != NULL && protocomm == s_protocomm &&
                       s_transport_started;
    if (valid)
    {
        s_transport_started = false;
        ++s_transport_stop_count;
        if (s_block_next_stop)
        {
            s_block_next_stop = false;
            s_stop_blocked = true;
            (void)pthread_cond_broadcast(&s_stop_changed);
            while (!s_release_stop)
            {
                (void)pthread_cond_wait(&s_stop_changed, &s_lock);
            }
            s_stop_blocked = false;
            s_release_stop = false;
        }
    }
    const esp_err_t result = valid ? s_next_stop_result :
                             ESP_ERR_INVALID_STATE;
    s_next_stop_result = ESP_OK;
    (void)pthread_mutex_unlock(&s_lock);
    return result;
}

esp_err_t protocomm_set_security(
    protocomm_t *protocomm, const char *endpoint,
    const protocomm_security_t *security, const void *parameters)
{
    return protocomm != NULL && endpoint != NULL &&
           strcmp(endpoint, "prov-session") == 0 &&
           security == &protocomm_security2 && parameters != NULL ?
           ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t protocomm_set_version(
    protocomm_t *protocomm, const char *endpoint, const char *version)
{
    if (protocomm == NULL || endpoint == NULL || version == NULL ||
            strcmp(endpoint, "proto-ver") != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_lock);
    (void)strncpy(s_protocol_version, version,
                  sizeof(s_protocol_version) - 1U);
    s_protocol_version[sizeof(s_protocol_version) - 1U] = '\0';
    (void)pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t protocomm_add_endpoint(
    protocomm_t *protocomm, const char *endpoint,
    protocomm_req_handler_t handler, void *private_data)
{
    if (protocomm == NULL || endpoint == NULL || handler == NULL ||
            strcmp(endpoint, "mt-prov") != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_lock);
    protocomm->control_handler = handler;
    protocomm->control_private_data = private_data;
    (void)pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

static esp_err_t _host_admit_operation(
    connectivity_manager_operation_id_t *operation_id)
{
    if (operation_id == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_lock);
    *operation_id = s_next_operation_id++;
    (void)pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t connectivity_manager_get_status(
    connectivity_manager_status_snapshot_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_lock);
    *status = s_manager_status;
    if (s_init_refresh_staged)
    {
        s_manager_status = s_staged_status;
        s_manager_scan = s_staged_scan;
        s_init_refresh_staged = false;
    }
    (void)pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t connectivity_manager_get_scan_snapshot(
    connectivity_manager_scan_snapshot_t *scan)
{
    if (scan == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_lock);
    *scan = s_manager_scan;
    (void)pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t connectivity_manager_request_scan(
    connectivity_manager_operation_id_t *operation_id)
{
    return _host_admit_operation(operation_id);
}

esp_err_t connectivity_manager_request_connect(
    const connectivity_manager_credentials_t *credentials,
    connectivity_manager_operation_id_t *operation_id)
{
    return credentials != NULL ? _host_admit_operation(operation_id) :
           ESP_ERR_INVALID_ARG;
}

esp_err_t connectivity_manager_request_disconnect(
    connectivity_manager_operation_id_t *operation_id)
{
    return _host_admit_operation(operation_id);
}

esp_err_t connectivity_manager_request_reconnect_saved(
    connectivity_manager_operation_id_t *operation_id)
{
    return _host_admit_operation(operation_id);
}

esp_err_t connectivity_manager_request_forget(
    connectivity_manager_operation_id_t *operation_id)
{
    return _host_admit_operation(operation_id);
}

esp_err_t connectivity_manager_set_auto_connect(
    bool enabled, connectivity_manager_operation_id_t *operation_id)
{
    (void)enabled;
    return _host_admit_operation(operation_id);
}

esp_err_t connectivity_manager_cancel(
    connectivity_manager_operation_id_t operation_id)
{
    if (operation_id == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&s_lock);
    s_canceled_operation = operation_id;
    ++s_cancel_count;
    (void)pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}
