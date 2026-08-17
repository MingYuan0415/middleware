#include "wifi_service_port.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WIFI_SERVICE_EVENT_DRAIN_TIMEOUT_MS 1000U

ESP_EVENT_DEFINE_BASE(WIFI_SERVICE_PORT_BARRIER_EVENT);

enum
{
    WIFI_SERVICE_PORT_BARRIER_ID = 1,
};

typedef struct wifi_idf_state
{
    esp_netif_t *netif;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    esp_event_handler_instance_t barrier_handler;
    bool attach_attempted;
    bool default_handlers_ready;
    bool wifi_init_attempted;
    bool wifi_initialized;
    bool wifi_handler_ready;
    bool ip_handler_ready;
    bool barrier_handler_ready;
    bool started;
    bool scan_list_owned;
} wifi_idf_state_t;

static wifi_idf_state_t s_state;
static atomic_uintptr_t s_event_netif;
static atomic_uint_fast32_t s_event_epoch = 1U;
static atomic_uint_fast32_t s_barrier_request;
static atomic_uint_fast32_t s_barrier_completed;
static atomic_bool s_events_enabled;

static uint32_t _wifi_service_port_next_epoch(void)
{
    uint32_t epoch = atomic_fetch_add_explicit(&s_event_epoch, 1U,
                     memory_order_acq_rel) + 1U;
    if (epoch == 0)
    {
        epoch = atomic_fetch_add_explicit(&s_event_epoch, 1U,
                                          memory_order_acq_rel) + 1U;
    }
    return epoch;
}

uint64_t wifi_service_port_get_epoch(void)
{
    return (uint64_t)atomic_load_explicit(&s_event_epoch,
                                          memory_order_acquire);
}

static void _wifi_service_port_barrier_handler(void *argument,
        esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)argument;
    if (event_base == WIFI_SERVICE_PORT_BARRIER_EVENT &&
            event_id == WIFI_SERVICE_PORT_BARRIER_ID && event_data != NULL)
    {
        uint32_t generation;
        memcpy(&generation, event_data, sizeof(generation));
        atomic_store_explicit(&s_barrier_completed, generation,
                              memory_order_release);
    }
}

static esp_err_t _wifi_service_port_drain_event_loop(void)
{
    if (!s_state.barrier_handler_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t generation = atomic_fetch_add_explicit(
                              &s_barrier_request, 1U,
                              memory_order_relaxed) + 1U;
    if (generation == 0)
    {
        generation = atomic_fetch_add_explicit(
                         &s_barrier_request, 1U,
                         memory_order_relaxed) + 1U;
    }
    esp_err_t result = esp_event_post(WIFI_SERVICE_PORT_BARRIER_EVENT,
                                      WIFI_SERVICE_PORT_BARRIER_ID,
                                      &generation, sizeof(generation), 0);
    if (result != ESP_OK)
    {
        return result;
    }

    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(
                             WIFI_SERVICE_EVENT_DRAIN_TIMEOUT_MS);
    if (timeout == 0)
    {
        timeout = 1;
    }
    do
    {
        if (atomic_load_explicit(&s_barrier_completed,
                                 memory_order_acquire) == generation)
        {
            return ESP_OK;
        }
        vTaskDelay(1);
    }
    while ((xTaskGetTickCount() - started) < timeout);
    return ESP_ERR_TIMEOUT;
}

static bool _wifi_service_port_inactive_error(esp_err_t result)
{
    return result == ESP_ERR_WIFI_NOT_INIT ||
           result == ESP_ERR_WIFI_NOT_STARTED;
}

static wifi_service_security_t _wifi_service_port_map_security(
    wifi_auth_mode_t auth_mode)
{
    wifi_service_security_t security;
    switch (auth_mode)
    {
    case WIFI_AUTH_OPEN:
        security = WIFI_SERVICE_SECURITY_OPEN;
        break;
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        security = WIFI_SERVICE_SECURITY_PERSONAL;
        break;
    default:
        security = WIFI_SERVICE_SECURITY_UNSUPPORTED;
        break;
    }
    return security;
}

static int _wifi_service_port_compare_scan_record(
    const wifi_service_port_scan_record_t *left,
    const wifi_service_port_scan_record_t *right)
{
    if (left->rssi != right->rssi)
    {
        return left->rssi > right->rssi ? -1 : 1;
    }
    const size_t common_length = left->ssid_length < right->ssid_length ?
                                 left->ssid_length : right->ssid_length;
    const int ssid_order = memcmp(left->ssid, right->ssid, common_length);
    if (ssid_order != 0)
    {
        return ssid_order;
    }
    if (left->ssid_length != right->ssid_length)
    {
        return left->ssid_length < right->ssid_length ? -1 : 1;
    }
    if (left->security != right->security)
    {
        return left->security < right->security ? -1 : 1;
    }
    return 0;
}

static bool _wifi_service_port_scan_key_equal(
    const wifi_service_port_scan_record_t *left,
    const wifi_service_port_scan_record_t *right)
{
    return left->ssid_length == right->ssid_length &&
           left->security == right->security &&
           memcmp(left->ssid, right->ssid, left->ssid_length) == 0;
}

static void _wifi_service_port_sort_scan_records(
    wifi_service_port_scan_record_t *records, size_t count)
{
    for (size_t index = 1U; index < count; ++index)
    {
        wifi_service_port_scan_record_t record = records[index];
        size_t position = index;
        while (position > 0U &&
                _wifi_service_port_compare_scan_record(
                    &record, &records[position - 1U]) < 0)
        {
            records[position] = records[position - 1U];
            --position;
        }
        records[position] = record;
    }
}

static void _wifi_service_port_retain_scan_record(
    wifi_service_port_scan_record_t *records, size_t capacity,
    size_t *count, bool *truncated,
    const wifi_service_port_scan_record_t *candidate)
{
    for (size_t index = 0U; index < *count; ++index)
    {
        if (_wifi_service_port_scan_key_equal(&records[index], candidate))
        {
            if (candidate->rssi > records[index].rssi)
            {
                records[index] = *candidate;
            }
            return;
        }
    }
    if (*count < capacity)
    {
        records[*count] = *candidate;
        ++(*count);
        return;
    }

    *truncated = true;
    size_t weakest = 0U;
    for (size_t index = 1U; index < *count; ++index)
    {
        if (_wifi_service_port_compare_scan_record(
                    &records[weakest], &records[index]) < 0)
        {
            weakest = index;
        }
    }
    if (_wifi_service_port_compare_scan_record(
                candidate, &records[weakest]) < 0)
    {
        records[weakest] = *candidate;
    }
}

static wifi_service_failure_t _wifi_service_port_map_disconnect_failure(
    uint16_t reason)
{
    wifi_service_failure_t failure = WIFI_SERVICE_FAILURE_LINK_LOST;
    switch (reason)
    {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_NOT_AUTHED:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        failure = WIFI_SERVICE_FAILURE_AUTHENTICATION;
        break;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        failure = WIFI_SERVICE_FAILURE_AP_NOT_FOUND;
        break;
    default:
        break;
    }
    return failure;
}

static bool _wifi_service_port_map_wifi_event(
    int32_t event_id, const void *event_data,
    wifi_service_port_event_t *event)
{
    bool mapped = true;
    switch (event_id)
    {
    case WIFI_EVENT_SCAN_DONE:
        event->type = WIFI_SERVICE_PORT_EVENT_SCAN_DONE;
        if (event_data != NULL)
        {
            const wifi_event_sta_scan_done_t *scan = event_data;
            event->status = (int32_t)scan->status;
            event->scan_id = scan->scan_id;
        }
        break;
    case WIFI_EVENT_STA_CONNECTED:
        event->type = WIFI_SERVICE_PORT_EVENT_STA_CONNECTED;
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        event->type = WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED;
        event->status = ESP_FAIL;
        if (event_data != NULL)
        {
            const wifi_event_sta_disconnected_t *disconnected = event_data;
            event->disconnect_reason = disconnected->reason;
            event->failure = _wifi_service_port_map_disconnect_failure(
                                 disconnected->reason);
        }
        break;
    default:
        mapped = false;
        break;
    }
    return mapped;
}

static bool _wifi_service_port_map_ip_event(
    int32_t event_id, const void *event_data,
    wifi_service_port_event_t *event)
{
    bool mapped = event_data != NULL &&
                  (event_id == IP_EVENT_STA_GOT_IP ||
                   event_id == IP_EVENT_STA_LOST_IP);
    if (mapped)
    {
        const ip_event_got_ip_t *got_ip = event_data;
        esp_netif_t *expected = (esp_netif_t *)atomic_load_explicit(
                                    &s_event_netif, memory_order_acquire);
        if (expected == NULL || got_ip->esp_netif != expected)
        {
            mapped = false;
        }
        else if (event_id == IP_EVENT_STA_GOT_IP)
        {
            event->type = WIFI_SERVICE_PORT_EVENT_GOT_IP;
            event->ipv4_address = got_ip->ip_info.ip.addr;
        }
        else
        {
            event->type = WIFI_SERVICE_PORT_EVENT_LOST_IP;
        }
    }
    return mapped;
}

static void _wifi_service_port_event_handler(void *argument,
        esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    const uint32_t handler_epoch = (uint32_t)(uintptr_t)argument;
    const uint32_t epoch = atomic_load_explicit(&s_event_epoch,
                           memory_order_acquire);
    wifi_service_port_event_t event;
    memset(&event, 0, sizeof(event));
    event.epoch = epoch;
    bool submit = atomic_load_explicit(&s_events_enabled,
                                       memory_order_acquire) &&
                  handler_epoch != 0 && handler_epoch == epoch;

    if (submit && event_base == WIFI_EVENT)
    {
        submit = _wifi_service_port_map_wifi_event(
                     event_id, event_data, &event);
    }
    else if (submit && event_base == IP_EVENT)
    {
        submit = _wifi_service_port_map_ip_event(event_id, event_data, &event);
    }
    else
    {
        submit = false;
    }

    if (submit &&
            (!atomic_load_explicit(&s_events_enabled, memory_order_acquire) ||
             atomic_load_explicit(&s_event_epoch,
                                  memory_order_acquire) != epoch))
    {
        submit = false;
    }
    if (submit)
    {
        (void)wifi_service_port_submit_event(&event);
    }
}

static esp_err_t _wifi_service_port_register_handlers(void)
{
    if (s_state.wifi_handler_ready && s_state.ip_handler_ready)
    {
        atomic_store_explicit(&s_events_enabled, true,
                              memory_order_release);
        return ESP_OK;
    }
    atomic_store_explicit(&s_event_netif, (uintptr_t)s_state.netif,
                          memory_order_release);
    uint32_t epoch = atomic_load_explicit(&s_event_epoch,
                                          memory_order_acquire);
    void *handler_argument = (void *)(uintptr_t)epoch;
    if (!s_state.wifi_handler_ready)
    {
        esp_err_t result = esp_event_handler_instance_register(
                               WIFI_EVENT, ESP_EVENT_ANY_ID,
                               _wifi_service_port_event_handler, handler_argument,
                               &s_state.wifi_handler);
        if (result != ESP_OK)
        {
            return result;
        }
        s_state.wifi_handler_ready = true;
    }
    if (!s_state.ip_handler_ready)
    {
        esp_err_t result = esp_event_handler_instance_register(
                               IP_EVENT, ESP_EVENT_ANY_ID,
                               _wifi_service_port_event_handler, handler_argument,
                               &s_state.ip_handler);
        if (result != ESP_OK)
        {
            return result;
        }
        s_state.ip_handler_ready = true;
    }
    atomic_store_explicit(&s_events_enabled, true, memory_order_release);
    return ESP_OK;
}

static esp_err_t _wifi_service_port_unregister_handlers(void)
{
    atomic_store_explicit(&s_events_enabled, false, memory_order_release);
    esp_err_t first_error = ESP_OK;
    if (s_state.ip_handler_ready)
    {
        esp_err_t result = esp_event_handler_instance_unregister(
                               IP_EVENT, ESP_EVENT_ANY_ID,
                               s_state.ip_handler);
        if (result == ESP_OK)
        {
            s_state.ip_handler_ready = false;
            s_state.ip_handler = NULL;
        }
        else
        {
            first_error = result;
        }
    }
    if (s_state.wifi_handler_ready)
    {
        esp_err_t result = esp_event_handler_instance_unregister(
                               WIFI_EVENT, ESP_EVENT_ANY_ID,
                               s_state.wifi_handler);
        if (result == ESP_OK)
        {
            s_state.wifi_handler_ready = false;
            s_state.wifi_handler = NULL;
        }
        else if (first_error == ESP_OK)
        {
            first_error = result;
        }
    }
    if (!s_state.wifi_handler_ready && !s_state.ip_handler_ready)
    {
        atomic_store_explicit(&s_event_netif, (uintptr_t)NULL,
                              memory_order_release);
    }
    return first_error;
}

esp_err_t wifi_service_port_start(void)
{
    const wifi_service_port_state_t state = wifi_service_port_get_state();
    if (state == WIFI_SERVICE_PORT_STATE_STARTED)
    {
        return ESP_OK;
    }
    if (state != WIFI_SERVICE_PORT_STATE_STOPPED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    (void)_wifi_service_port_next_epoch();
    esp_err_t result = _wifi_service_port_register_handlers();
    if (result != ESP_OK)
    {
        atomic_store_explicit(&s_events_enabled, false,
                              memory_order_release);
        esp_err_t cleanup = _wifi_service_port_unregister_handlers();
        (void)_wifi_service_port_next_epoch();
        if (cleanup != ESP_OK)
        {
            result = cleanup;
        }
        return result;
    }
    result = esp_wifi_start();
    if (result != ESP_OK)
    {
        const esp_err_t start_result = result;
        esp_err_t stop_result = esp_wifi_stop();
        if (stop_result != ESP_OK &&
                !_wifi_service_port_inactive_error(stop_result))
        {
            s_state.started = true;
            atomic_store_explicit(&s_events_enabled, false,
                                  memory_order_release);
            return stop_result;
        }
        s_state.started = false;
        s_state.scan_list_owned = false;
        atomic_store_explicit(&s_events_enabled, false,
                              memory_order_release);
        esp_err_t drain_result = _wifi_service_port_drain_event_loop();
        if (drain_result != ESP_OK)
        {
            return drain_result;
        }
        esp_err_t cleanup = _wifi_service_port_unregister_handlers();
        if (cleanup == ESP_OK)
        {
            (void)_wifi_service_port_next_epoch();
            result = start_result;
        }
        else
        {
            result = cleanup;
        }
        return result;
    }
    s_state.started = true;
    return ESP_OK;
}

esp_err_t wifi_service_port_stop(void)
{
    esp_err_t result = ESP_OK;
    if (!s_state.started)
    {
        if (!s_state.wifi_handler_ready && !s_state.ip_handler_ready)
        {
            result = wifi_service_port_get_state() ==
                     WIFI_SERVICE_PORT_STATE_STOPPED ? ESP_OK :
                     ESP_ERR_INVALID_STATE;
            return result;
        }
        atomic_store_explicit(&s_events_enabled, false,
                              memory_order_release);
        esp_err_t drain_result = _wifi_service_port_drain_event_loop();
        if (drain_result != ESP_OK)
        {
            return drain_result;
        }
        esp_err_t unregister_result =
            _wifi_service_port_unregister_handlers();
        if (unregister_result == ESP_OK)
        {
            (void)_wifi_service_port_next_epoch();
        }
        return unregister_result;
    }
    atomic_store_explicit(&s_events_enabled, false, memory_order_release);
    result = esp_wifi_stop();
    if (result == ESP_OK || _wifi_service_port_inactive_error(result))
    {
        s_state.started = false;
        s_state.scan_list_owned = false;
        esp_err_t drain_result = _wifi_service_port_drain_event_loop();
        if (drain_result != ESP_OK)
        {
            return drain_result;
        }
        esp_err_t unregister_result =
            _wifi_service_port_unregister_handlers();
        if (unregister_result == ESP_OK)
        {
            (void)_wifi_service_port_next_epoch();
        }
        return unregister_result;
    }

    const esp_err_t stop_result = result;
    esp_err_t rollback = esp_wifi_start();
    if (rollback != ESP_OK && rollback != ESP_ERR_WIFI_NOT_STOPPED)
    {
        return rollback;
    }
    s_state.started = true;
    esp_err_t drain_result = _wifi_service_port_drain_event_loop();
    if (drain_result != ESP_OK)
    {
        return drain_result;
    }
    atomic_store_explicit(&s_events_enabled, true, memory_order_release);
    return stop_result;
}

esp_err_t wifi_service_port_init(void)
{
    if (!wifi_service_port_is_clean())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    s_state.netif = esp_netif_new(&netif_config);
    if (s_state.netif == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    s_state.attach_attempted = true;
    esp_err_t result = esp_netif_attach_wifi_station(s_state.netif);
    if (result != ESP_OK)
    {
        return result;
    }
    result = esp_wifi_set_default_wifi_sta_handlers();
    if (result != ESP_OK)
    {
        return result;
    }
    s_state.default_handlers_ready = true;

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    s_state.wifi_init_attempted = true;
    result = esp_wifi_init(&config);
    wifi_service_secure_zero(&config, sizeof(config));
    if (result != ESP_OK)
    {
        return result;
    }
    s_state.wifi_initialized = true;
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK)
    {
        return result;
    }
    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK)
    {
        return result;
    }
    result = esp_event_handler_instance_register(
                 WIFI_SERVICE_PORT_BARRIER_EVENT,
                 WIFI_SERVICE_PORT_BARRIER_ID,
                 _wifi_service_port_barrier_handler, NULL,
                 &s_state.barrier_handler);
    if (result != ESP_OK)
    {
        return result;
    }
    s_state.barrier_handler_ready = true;
    return wifi_service_port_start();
}

esp_err_t wifi_service_port_scan_abort(void)
{
    esp_err_t result = ESP_OK;
    if (!s_state.scan_list_owned)
    {
        return ESP_OK;
    }
    if (s_state.started)
    {
        esp_err_t stop_result = esp_wifi_scan_stop();
        if (stop_result != ESP_OK &&
                !_wifi_service_port_inactive_error(stop_result))
        {
            return stop_result;
        }
    }
    result = esp_wifi_clear_ap_list();
    if (result == ESP_OK || _wifi_service_port_inactive_error(result))
    {
        s_state.scan_list_owned = false;
        result = ESP_OK;
    }
    return result;
}

esp_err_t wifi_service_port_deinit(void)
{
    esp_err_t result = ESP_OK;
    esp_err_t first_error = ESP_OK;
    (void)wifi_service_port_scan_abort();
    (void)wifi_service_port_clear_credentials();

    if (s_state.started || s_state.wifi_handler_ready ||
            s_state.ip_handler_ready)
    {
        esp_err_t stop_result = wifi_service_port_stop();
        if (stop_result != ESP_OK)
        {
            return stop_result;
        }
    }

    if (s_state.wifi_init_attempted)
    {
        result = esp_wifi_deinit();
        if (result == ESP_OK || result == ESP_ERR_WIFI_NOT_INIT)
        {
            s_state.wifi_init_attempted = false;
            s_state.wifi_initialized = false;
            s_state.scan_list_owned = false;
        }
        else
        {
            return result;
        }
    }

    if (s_state.attach_attempted && s_state.netif != NULL)
    {
        result = esp_wifi_clear_default_wifi_driver_and_handlers(
                     s_state.netif);
        /* This API destroys the interface driver before returning its error. */
        s_state.attach_attempted = false;
        s_state.default_handlers_ready = false;
        if (result != ESP_OK && first_error == ESP_OK)
        {
            first_error = result;
        }
    }
    if (s_state.netif != NULL)
    {
        esp_netif_destroy(s_state.netif);
        s_state.netif = NULL;
    }
    if (s_state.barrier_handler_ready)
    {
        result = esp_event_handler_instance_unregister(
                     WIFI_SERVICE_PORT_BARRIER_EVENT,
                     WIFI_SERVICE_PORT_BARRIER_ID,
                     s_state.barrier_handler);
        if (result == ESP_OK)
        {
            s_state.barrier_handler_ready = false;
            s_state.barrier_handler = NULL;
        }
        else if (first_error == ESP_OK)
        {
            first_error = result;
        }
    }
    atomic_store_explicit(&s_events_enabled, false, memory_order_release);
    atomic_store_explicit(&s_event_netif, (uintptr_t)NULL,
                          memory_order_release);
    return first_error;
}

bool wifi_service_port_is_clean(void)
{
    return s_state.netif == NULL && !s_state.attach_attempted &&
           !s_state.default_handlers_ready &&
           !s_state.wifi_init_attempted && !s_state.wifi_initialized &&
           !s_state.wifi_handler_ready && !s_state.ip_handler_ready &&
           !s_state.barrier_handler_ready && !s_state.started &&
           !s_state.scan_list_owned;
}

wifi_service_port_state_t wifi_service_port_get_state(void)
{
    wifi_service_port_state_t state = WIFI_SERVICE_PORT_STATE_PARTIAL;
    if (wifi_service_port_is_clean())
    {
        state = WIFI_SERVICE_PORT_STATE_CLEAN;
    }
    else
    {
        const bool base_ready = s_state.netif != NULL &&
                                s_state.attach_attempted &&
                                s_state.default_handlers_ready &&
                                s_state.wifi_init_attempted &&
                                s_state.wifi_initialized &&
                                s_state.barrier_handler_ready;
        const bool events_enabled = atomic_load_explicit(
                                        &s_events_enabled, memory_order_acquire);
        if (base_ready && s_state.started && s_state.wifi_handler_ready &&
                s_state.ip_handler_ready && events_enabled)
        {
            state = WIFI_SERVICE_PORT_STATE_STARTED;
        }
        else if (base_ready && !s_state.started &&
                 !s_state.wifi_handler_ready && !s_state.ip_handler_ready &&
                 !events_enabled && !s_state.scan_list_owned)
        {
            state = WIFI_SERVICE_PORT_STATE_STOPPED;
        }
    }
    return state;
}

bool wifi_service_port_scan_is_owned(void)
{
    return s_state.scan_list_owned;
}

esp_err_t wifi_service_port_scan_start(void)
{
    if (wifi_service_port_get_state() != WIFI_SERVICE_PORT_STATE_STARTED ||
            s_state.scan_list_owned)
    {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_scan_config_t config;
    memset(&config, 0, sizeof(config));
    config.show_hidden = false;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_err_t result = esp_wifi_scan_start(&config, false);
    wifi_service_secure_zero(&config, sizeof(config));
    if (result == ESP_OK)
    {
        s_state.scan_list_owned = true;
    }
    return result;
}

esp_err_t wifi_service_port_scan_finish(
    wifi_service_port_scan_record_t *records, size_t capacity,
    size_t *out_count, bool *out_truncated)
{
    esp_err_t result = ESP_OK;
    wifi_ap_record_t idf_record;
    if (records == NULL || capacity == 0 ||
            capacity > WIFI_SERVICE_MAX_SCAN_RECORDS || out_count == NULL ||
            out_truncated == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.scan_list_owned)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *out_count = 0;
    *out_truncated = false;
    memset(records, 0, capacity * sizeof(*records));

    uint16_t total = 0U;
    result = esp_wifi_scan_get_ap_num(&total);
    for (uint16_t index = 0U; result == ESP_OK && index < total; ++index)
    {
        memset(&idf_record, 0, sizeof(idf_record));
        result = esp_wifi_scan_get_ap_record(&idf_record);
        if (result == ESP_OK)
        {
            wifi_service_port_scan_record_t candidate;
            memset(&candidate, 0, sizeof(candidate));
            const size_t length = strnlen((const char *)idf_record.ssid,
                                          WIFI_SERVICE_SSID_MAX_BYTES);
            if (length > 0U)
            {
                memcpy(candidate.ssid, idf_record.ssid, length);
                candidate.ssid_length = (uint8_t)length;
                candidate.rssi = idf_record.rssi;
                candidate.channel = idf_record.primary;
                candidate.security = _wifi_service_port_map_security(
                                         idf_record.authmode);
                _wifi_service_port_retain_scan_record(
                    records, capacity, out_count, out_truncated, &candidate);
            }
            wifi_service_secure_zero(&candidate, sizeof(candidate));
        }
        wifi_service_secure_zero(&idf_record, sizeof(idf_record));
    }

    const esp_err_t clear_result = esp_wifi_clear_ap_list();
    if (clear_result == ESP_OK ||
            _wifi_service_port_inactive_error(clear_result))
    {
        s_state.scan_list_owned = false;
    }
    if (clear_result != ESP_OK &&
            !_wifi_service_port_inactive_error(clear_result))
    {
        result = clear_result;
    }
    if (result == ESP_OK)
    {
        _wifi_service_port_sort_scan_records(records, *out_count);
    }
    return result;
}

esp_err_t wifi_service_port_clear_credentials(void)
{
    esp_err_t result = ESP_OK;
    if (!s_state.wifi_initialized)
    {
        return ESP_OK;
    }
    wifi_config_t config;
    memset(&config, 0, sizeof(config));
    result = esp_wifi_set_config(WIFI_IF_STA, &config);
    wifi_service_secure_zero(&config, sizeof(config));
    return result;
}

esp_err_t wifi_service_port_set_credentials(
    const wifi_service_port_credentials_t *credentials)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    wifi_config_t config;
    if (credentials == NULL || !s_state.wifi_initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const wifi_service_credentials_t policy =
    {
        .ssid = (const char *)credentials->ssid,
        .ssid_length = credentials->ssid_length,
        .password = (const char *)credentials->password,
        .password_length = credentials->password_length,
        .security = credentials->security,
    };

    if (!wifi_service_credentials_valid(&policy))
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&config, 0, sizeof(config));
    memcpy(config.sta.ssid, credentials->ssid,
           credentials->ssid_length);
    if (credentials->password_length > 0)
    {
        memcpy(config.sta.password, credentials->password,
               credentials->password_length);
    }
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode =
        credentials->security == WIFI_SERVICE_SECURITY_PERSONAL ?
        WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    result = esp_wifi_set_config(WIFI_IF_STA, &config);
    wifi_service_secure_zero(&config, sizeof(config));
    return result;
}

esp_err_t wifi_service_port_connect(void)
{
    return s_state.started ? esp_wifi_connect() : ESP_ERR_INVALID_STATE;
}

esp_err_t wifi_service_port_disconnect(void)
{
    if (!s_state.started)
    {
        return ESP_OK;
    }
    esp_err_t result = esp_wifi_disconnect();
    if (result == ESP_OK || result == ESP_ERR_WIFI_NOT_CONNECT ||
            _wifi_service_port_inactive_error(result))
    {
        result = ESP_OK;
    }
    return result;
}
