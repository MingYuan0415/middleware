#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ble_link_service.h"
#include "ble_link_session.h"

#define DBG_TAG "ble_link_service"
#define DBG_LVL DBG_WARN
#include "mt_log.h"

struct ble_link_work
{
    bool used;
    uint16_t att_mtu;
    uint32_t generation;
    bool encrypted;
    bool authenticated;
    uint8_t value[DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES];
    uint16_t length;
};

typedef struct ble_link_service
{
    bool initialized;
    uint64_t boot_id;
    ble_link_service_output_t output;
    void *output_arg;
    ble_link_v1_owner_ops_t owner_ops;
    void *owner_arg;
    device_link_v1_engine_t engine;
    ble_link_work_t work[BLE_LINK_SERVICE_WORK_SLOTS];
    uint8_t tx_value[DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES];
    size_t tx_length;
    uint32_t tx_flow_id;
    uint32_t next_flow_id;
    uint16_t att_mtu;
    bool rx_reserved;
    ble_link_work_t *rx_owner;
    bool pairing_window_open;
    bool confirmation_pending;
    bool connection_valid;
    uint32_t connection_generation;
    uint16_t conn_handle;
    uint64_t confirmation_token;
    uint32_t passkey;
    uint64_t next_token;
    ble_link_service_wake_fn_t wake;
    void *wake_arg;
} ble_link_service_t;

static ble_link_service_t s_service;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_control;

static void _ble_link_service_lock(void)
{
    if (s_lock != NULL)
    {
        (void)xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

static void _ble_link_service_unlock(void)
{
    if (s_lock != NULL)
    {
        (void)xSemaphoreGiveRecursive(s_lock);
    }
}

static void _ble_link_service_wake(void)
{
    if (s_service.wake != NULL)
    {
        s_service.wake(s_service.wake_arg);
    }
}

static void _ble_link_service_drop_reservation(void)
{
    s_service.rx_reserved = false;
    s_service.rx_owner = NULL;
}

static void _ble_link_service_clear_reservation(ble_link_work_t *work)
{
    if (s_service.rx_owner == work)
    {
        _ble_link_service_drop_reservation();
    }
}

static ble_link_work_t *_ble_link_service_alloc_work(void)
{
    for (size_t i = 0U; i < BLE_LINK_SERVICE_WORK_SLOTS; ++i)
    {
        if (!s_service.work[i].used)
        {
            s_service.work[i].used = true;
            return &s_service.work[i];
        }
    }
    return NULL;
}

static bool _ble_link_service_identity_is_current(
    const ble_link_operation_identity_t *identity)
{
    return identity != NULL &&
           s_service.connection_valid &&
           identity->generation == s_service.connection_generation &&
           identity->conn_handle == s_service.conn_handle;
}

static size_t _ble_link_service_encode_app_error(
    const device_link_v1_route_result_t *route, uint8_t *out, size_t capacity)
{
    if (route->status == DEVICE_LINK_V1_STATUS_UNSUPPORTED)
    {
        return device_link_v1_encode_application_error(
                   route->request.request_id, route->offending_opcode,
                   out, capacity);
    }
    return device_link_v1_encode_response(
               route->request.opcode != 0U ? route->request.opcode :
               DEVICE_LINK_V1_GET_INFO,
               route->request.request_id, route->status, NULL, out, capacity);
}

static size_t _ble_link_service_handle_admitted(
    const device_link_v1_request_t *request, uint8_t *out, size_t capacity)
{
    device_link_v1_info_t info;
    device_link_v1_operation_record_t record;
    uint32_t operation_id = 0U;
    device_link_v1_status_t status;
    bool accepted = false;
    bool ack = false;

    memset(&info, 0, sizeof(info));
    switch (request->opcode)
    {
    case DEVICE_LINK_V1_GET_INFO:
        info.pairing_window_open =
            (ble_link_session_get_state_flags() &
             BLE_LINK_STATE_FLAG_BINDABLE) != 0U;
        if (s_service.owner_ops.fill_info != NULL)
        {
            s_service.owner_ops.fill_info(&info, s_service.owner_arg);
            info.pairing_window_open =
                (ble_link_session_get_state_flags() &
                 BLE_LINK_STATE_FLAG_BINDABLE) != 0U;
        }
        return device_link_v1_encode_info_response(request->request_id, &info,
                out, capacity);
    case DEVICE_LINK_V1_GET_STATUS:
        return device_link_v1_encode_status_response(
                   request->request_id, device_link_v1_engine_snapshot(
                       &s_service.engine), out, capacity);
    case DEVICE_LINK_V1_GET_OPERATION:
        status = device_link_v1_engine_get_operation(&s_service.engine, &record);
        return device_link_v1_encode_operation_response(
                   request->request_id, status,
                   status == DEVICE_LINK_V1_STATUS_OK ? &record : NULL,
                   out, capacity);
    case DEVICE_LINK_V1_ACK_OPERATION:
        status = device_link_v1_engine_ack(&s_service.engine,
                                           request->payload.operation_id,
                                           request->request_id);
        ack = status == DEVICE_LINK_V1_STATUS_OK;
        device_link_v1_engine_arm_response(&s_service.engine,
                                           request->request_id, false, ack);
        return device_link_v1_encode_response(
                   request->opcode, request->request_id, status, NULL, out,
                   capacity);
    case DEVICE_LINK_V1_SCAN:
    case DEVICE_LINK_V1_SET_CREDENTIALS:
    case DEVICE_LINK_V1_CONNECT:
    case DEVICE_LINK_V1_DISCONNECT:
    case DEVICE_LINK_V1_FORGET:
        if (s_service.owner_ops.submit_operation == NULL)
        {
            device_link_v1_engine_arm_response(&s_service.engine,
                                               request->request_id, false, false);
            return device_link_v1_encode_response(
                       request->opcode, request->request_id,
                       DEVICE_LINK_V1_STATUS_INTERNAL, NULL, out, capacity);
        }
        status = device_link_v1_engine_start(
                     &s_service.engine, (device_link_v1_operation_t)request->opcode,
                     request->request_id,
                     request->opcode == DEVICE_LINK_V1_SET_CREDENTIALS ?
                     &request->payload.credentials : NULL,
                     &operation_id);
        if (status == DEVICE_LINK_V1_STATUS_ACCEPTED)
        {
            const device_link_v1_status_t submitted =
                s_service.owner_ops.submit_operation(
                    (device_link_v1_operation_t)request->opcode,
                    request->opcode == DEVICE_LINK_V1_SET_CREDENTIALS ?
                    &request->payload.credentials : NULL,
                    operation_id, s_service.owner_arg);

            if (submitted != DEVICE_LINK_V1_STATUS_ACCEPTED)
            {
                (void)device_link_v1_engine_rollback(&s_service.engine);
                status = (submitted == DEVICE_LINK_V1_STATUS_INVALID_ARGUMENT) ?
                         DEVICE_LINK_V1_STATUS_INVALID_ARGUMENT :
                         DEVICE_LINK_V1_STATUS_INTERNAL;
            }
            else
            {
                accepted = true;
                device_link_v1_engine_arm_deadline(
                    &s_service.engine,
                    (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                    DEVICE_LINK_V1_OPERATION_TIMEOUT_MS);
            }
        }
        device_link_v1_engine_arm_response(&s_service.engine,
                                           request->request_id, accepted, false);
        return device_link_v1_encode_response(
                   request->opcode, request->request_id, status,
                   accepted ? &operation_id : NULL, out, capacity);
    default:
        return 0U;
    }
}

static esp_err_t _ble_link_service_submit_tx(const uint8_t *value, size_t length)
{
    if (s_service.output == NULL || length == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_service.next_flow_id == 0U)
    {
        s_service.next_flow_id = 1U;
    }
    s_service.tx_flow_id = s_service.next_flow_id;
    s_service.next_flow_id++;
    if (s_service.next_flow_id == 0U)
    {
        s_service.next_flow_id = 1U;
    }
    return s_service.output(value, length, BLE_LINK_SERVICE_TX_SESSION, true,
                            s_service.tx_flow_id, s_service.output_arg);
}

static esp_err_t _ble_link_service_pump_locked(void)
{
    uint8_t value[DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES];
    size_t length = 0U;
    device_link_v1_tx_kind_t kind;

    if (s_service.rx_reserved)
    {
        return ESP_OK;
    }
    kind = device_link_v1_engine_next_tx(&s_service.engine);

    if (kind == DEVICE_LINK_V1_TX_NONE)
    {
        return ESP_OK;
    }
    if (kind == DEVICE_LINK_V1_TX_ORDINARY_STATUS)
    {
        length = device_link_v1_encode_wifi_status(
                     device_link_v1_engine_snapshot(&s_service.engine),
                     value, sizeof(value));
    }
    else if (kind == DEVICE_LINK_V1_TX_TERMINAL)
    {
        length = device_link_v1_encode_terminal(
                     device_link_v1_engine_record(&s_service.engine),
                     value, sizeof(value));
    }
    else
    {
        return ESP_OK;
    }
    const uint16_t att_mtu = s_service.att_mtu >= DEVICE_LINK_V1_MINIMUM_ATT_MTU ?
                             s_service.att_mtu : DEVICE_LINK_V1_MINIMUM_ATT_MTU;
    const size_t max_value = (size_t)att_mtu - 3U;

    if (length == 0U || length > max_value)
    {
        device_link_v1_engine_reject_tx(&s_service.engine);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_service.tx_value, value, length);
    s_service.tx_length = length;
    {
        const esp_err_t result = _ble_link_service_submit_tx(value, length);

        if (result != ESP_OK)
        {
            device_link_v1_engine_abort_tx(&s_service.engine);
        }
        return result;
    }
}

esp_err_t ble_link_service_set_domain_descriptors(
    const void *domains, size_t domain_count)
{
    (void)domains;
    (void)domain_count;
    return ESP_OK;
}

void ble_link_service_init(
    uint64_t boot_id, ble_link_service_output_t output, void *arg,
    const void *security, size_t max_pending_frames)
{
    device_link_v1_snapshot_t snapshot;

    (void)security;
    (void)max_pending_frames;
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_control);
    }
    _ble_link_service_lock();
    memset(&s_service, 0, sizeof(s_service));
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.state = DEVICE_LINK_V1_WIFI_IDLE;
    snapshot.failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    s_service.initialized = true;
    s_service.boot_id = boot_id;
    s_service.output = output;
    s_service.output_arg = arg;
    s_service.next_token = 1U;
    s_service.next_flow_id = 1U;
    s_service.att_mtu = DEVICE_LINK_V1_MINIMUM_ATT_MTU;
    device_link_v1_engine_init(&s_service.engine, &snapshot);
    _ble_link_service_unlock();
}

void ble_link_service_reset(void)
{
    _ble_link_service_lock();
    device_link_v1_engine_disconnect(&s_service.engine);
    s_service.connection_valid = false;
    s_service.connection_generation = 0U;
    s_service.conn_handle = 0U;
    s_service.confirmation_pending = false;
    s_service.confirmation_token = 0U;
    s_service.passkey = 0U;
    s_service.tx_flow_id = 0U;
    s_service.tx_length = 0U;
    _ble_link_service_drop_reservation();
    _ble_link_service_unlock();
}

void ble_link_service_set_v1_ops(const ble_link_v1_owner_ops_t *ops, void *arg)
{
    _ble_link_service_lock();
    if (ops != NULL)
    {
        s_service.owner_ops = *ops;
    }
    else
    {
        memset(&s_service.owner_ops, 0, sizeof(s_service.owner_ops));
    }
    s_service.owner_arg = arg;
    _ble_link_service_unlock();
}

void ble_link_service_set_pairing_window(bool open)
{
    _ble_link_service_lock();
    s_service.pairing_window_open = open;
    _ble_link_service_unlock();
}

void ble_link_service_observe_snapshot(const device_link_v1_snapshot_t *snapshot)
{
    _ble_link_service_lock();
    device_link_v1_engine_observe_snapshot(&s_service.engine, snapshot);
    _ble_link_service_unlock();
    _ble_link_service_wake();
}

esp_err_t ble_link_service_complete_operation(
    uint32_t operation_id, device_link_v1_wifi_failure_t failure,
    const device_link_v1_network_t *networks, uint8_t count,
    const device_link_v1_snapshot_t *snapshot)
{
    _ble_link_service_lock();
    const bool ok = device_link_v1_engine_complete(
                        &s_service.engine, operation_id, failure, networks, count,
                        snapshot);

    _ble_link_service_unlock();
    if (!ok)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_service_wake();
    return ESP_OK;
}

esp_err_t ble_link_service_accept(
    const ble_link_service_facts_t *facts,
    ble_link_service_rx_channel_t channel,
    const uint8_t *value, size_t len,
    ble_link_work_t **out_work)
{
    (void)channel;
    if (facts == NULL || value == NULL || out_work == NULL ||
            len > DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_service_lock();
    if (s_service.rx_reserved ||
            device_link_v1_engine_write_blocked(&s_service.engine))
    {
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ble_link_work_t *work = _ble_link_service_alloc_work();

    if (work == NULL)
    {
        _ble_link_service_unlock();
        return ESP_ERR_NO_MEM;
    }
    s_service.rx_reserved = true;
    s_service.rx_owner = work;
    work->att_mtu = (uint16_t)facts->preferred_att_mtu;
    work->generation = facts->connection_generation;
    if (facts->preferred_att_mtu >= DEVICE_LINK_V1_MINIMUM_ATT_MTU)
    {
        s_service.att_mtu = (uint16_t)facts->preferred_att_mtu;
    }
    work->encrypted = facts->encrypted;
    work->authenticated = facts->secure_connections_bond_verified ||
                          facts->session_authenticated;
    work->length = (uint16_t)len;
    memcpy(work->value, value, len);
    *out_work = work;
    _ble_link_service_unlock();
    return ESP_OK;
}

esp_err_t ble_link_service_execute(ble_link_work_t *work)
{
    device_link_v1_route_input_t input;
    device_link_v1_route_result_t route;
    uint8_t response[DEVICE_LINK_V1_MAX_ATT_VALUE_BYTES];
    size_t length = 0U;

    if (work == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    _ble_link_service_lock();
    if (!s_service.connection_valid ||
            work->generation != s_service.connection_generation)
    {
        _ble_link_service_clear_reservation(work);
        _ble_link_service_unlock();
        return ESP_OK;
    }
    if (device_link_v1_engine_write_blocked(&s_service.engine))
    {
        _ble_link_service_clear_reservation(work);
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(&input, 0, sizeof(input));
    input.att_mtu = work->att_mtu;
    input.att_value_length = work->length;
    input.encrypted = work->encrypted;
    input.authenticated = work->authenticated;
    input.subscription_enabled = true;
    input.indication_outstanding = false;
    input.slot_occupied = device_link_v1_engine_slot_occupied(&s_service.engine);
    input.profile_present = device_link_v1_engine_profile_present(
                                &s_service.engine);
    input.value = work->value;
    device_link_v1_route_write(&input, &route);
    if (route.kind == DEVICE_LINK_V1_ROUTE_ATT)
    {
        _ble_link_service_clear_reservation(work);
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (route.kind == DEVICE_LINK_V1_ROUTE_APP)
    {
        length = _ble_link_service_encode_app_error(&route, response,
                 sizeof(response));
    }
    else
    {
        length = _ble_link_service_handle_admitted(&route.request, response,
                 sizeof(response));
    }
    if (!device_link_v1_engine_write_blocked(&s_service.engine))
    {
        device_link_v1_engine_arm_response(&s_service.engine,
                                           route.request.request_id, false, false);
    }
    _ble_link_service_clear_reservation(work);
    if (length == 0U)
    {
        device_link_v1_engine_abort_tx(&s_service.engine);
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_service.tx_value, response, length);
    s_service.tx_length = length;
    const esp_err_t result = _ble_link_service_submit_tx(response, length);

    if (result != ESP_OK)
    {
        device_link_v1_engine_abort_tx(&s_service.engine);
    }
    _ble_link_service_unlock();
    return result;
}

void ble_link_service_release_work(ble_link_work_t *work)
{
    if (work == NULL)
    {
        return;
    }
    _ble_link_service_lock();
    _ble_link_service_clear_reservation(work);
    memset(work, 0, sizeof(*work));
    _ble_link_service_unlock();
}

void ble_link_service_set_worker_wake(
    ble_link_service_wake_fn_t wake, void *arg)
{
    _ble_link_service_lock();
    s_service.wake = wake;
    s_service.wake_arg = arg;
    _ble_link_service_unlock();
}

void ble_link_service_wake_owner(void)
{
    _ble_link_service_wake();
}

esp_err_t ble_link_service_pump_tx(void)
{
    _ble_link_service_lock();
    const esp_err_t result = _ble_link_service_pump_locked();

    _ble_link_service_unlock();
    return result;
}

void ble_link_service_tick(uint32_t now_ms)
{
    _ble_link_service_lock();
    const bool timed_out = device_link_v1_engine_tick(&s_service.engine,
                           now_ms);

    _ble_link_service_unlock();
    if (timed_out)
    {
        _ble_link_service_wake();
    }
}

uint32_t ble_link_service_operation_timeout_remaining_ms(void)
{
    _ble_link_service_lock();
    const uint32_t remaining = device_link_v1_engine_deadline_remaining_ms(
                                   &s_service.engine,
                                   (uint32_t)(xTaskGetTickCount() *
                                       portTICK_PERIOD_MS));

    _ble_link_service_unlock();
    return remaining;
}

void ble_link_service_set_att_mtu(uint16_t att_mtu)
{
    _ble_link_service_lock();
    s_service.att_mtu = att_mtu >= DEVICE_LINK_V1_MINIMUM_ATT_MTU ?
                        att_mtu : DEVICE_LINK_V1_MINIMUM_ATT_MTU;
    _ble_link_service_unlock();
}

esp_err_t ble_link_service_response_completed(uint32_t flow_id, bool is_last)
{
    (void)is_last;
    _ble_link_service_lock();
    if (flow_id != s_service.tx_flow_id)
    {
        _ble_link_service_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    device_link_v1_engine_confirm_tx(&s_service.engine);
    s_service.tx_flow_id = 0U;
    s_service.tx_length = 0U;
    _ble_link_service_unlock();
    _ble_link_service_wake();
    return ESP_OK;
}

bool ble_link_service_write_blocked(void)
{
    _ble_link_service_lock();
    const bool blocked = s_service.rx_reserved ||
                         device_link_v1_engine_write_blocked(&s_service.engine);

    _ble_link_service_unlock();
    return blocked;
}

bool ble_link_service_response_in_flight(void)
{
    return ble_link_service_write_blocked();
}

void ble_link_service_abort_transactions(void)
{
    _ble_link_service_lock();
    device_link_v1_engine_disconnect(&s_service.engine);
    device_link_v1_engine_connect(&s_service.engine);
    _ble_link_service_drop_reservation();
    _ble_link_service_unlock();
}

void ble_link_service_on_connect(uint32_t generation, uint16_t conn_handle)
{
    _ble_link_service_lock();
    s_service.connection_generation = generation;
    s_service.conn_handle = conn_handle;
    s_service.connection_valid = generation != 0U;
    device_link_v1_engine_connect(&s_service.engine);
    _ble_link_service_unlock();
}

void ble_link_service_clear_session_state(void)
{
    _ble_link_service_lock();
    device_link_v1_engine_disconnect(&s_service.engine);
    s_service.connection_valid = false;
    s_service.connection_generation = 0U;
    s_service.conn_handle = 0U;
    s_service.confirmation_pending = false;
    s_service.confirmation_token = 0U;
    s_service.passkey = 0U;
    _ble_link_service_drop_reservation();
    _ble_link_service_unlock();
}

esp_err_t ble_link_service_clear_session_state_if_current(
    const ble_link_operation_identity_t *identity)
{
    _ble_link_service_lock();
    if (!_ble_link_service_identity_is_current(identity))
    {
        _ble_link_service_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    _ble_link_service_unlock();
    ble_link_service_clear_session_state();
    return ESP_OK;
}

esp_err_t ble_link_service_abort_tx_if_current(
    const ble_link_operation_identity_t *identity)
{
    _ble_link_service_lock();
    if (!_ble_link_service_identity_is_current(identity))
    {
        _ble_link_service_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    device_link_v1_engine_abort_tx(&s_service.engine);
    _ble_link_service_drop_reservation();
    _ble_link_service_unlock();
    return ESP_OK;
}

esp_err_t ble_link_service_confirm_binding(uint64_t token, bool accept)
{
    _ble_link_service_lock();
    if (!s_service.confirmation_pending ||
            token == 0U || token != s_service.confirmation_token)
    {
        _ble_link_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_service.confirmation_pending = false;
    s_service.confirmation_token = 0U;
    s_service.passkey = 0U;
    _ble_link_service_unlock();
    (void)accept;
    return ESP_OK;
}

bool ble_link_service_pending_confirmation(void)
{
    _ble_link_service_lock();
    const bool pending = s_service.confirmation_pending;

    _ble_link_service_unlock();
    return pending;
}

uint64_t ble_link_service_confirmation_token(void)
{
    _ble_link_service_lock();
    const uint64_t token = s_service.confirmation_token;

    _ble_link_service_unlock();
    return token;
}

uint32_t ble_link_service_numeric_comparison_value(void)
{
    _ble_link_service_lock();
    const uint32_t passkey = s_service.passkey;

    _ble_link_service_unlock();
    return passkey;
}

esp_err_t ble_link_service_offer_numeric_comparison(uint32_t passkey)
{
    _ble_link_service_lock();
    s_service.passkey = passkey;
    s_service.confirmation_pending = true;
    if (s_service.next_token == 0U)
    {
        s_service.next_token = 1U;
    }
    s_service.confirmation_token = s_service.next_token;
    ++s_service.next_token;
    _ble_link_service_unlock();
    _ble_link_service_wake();
    return ESP_OK;
}

esp_err_t ble_link_service_register_remote_replacement(
    const ble_link_operation_identity_t *identity)
{
    (void)identity;
    return ESP_OK;
}

bool ble_link_service_delayed_replacement_pending(uint32_t generation)
{
    (void)generation;
    return false;
}

uint32_t ble_link_service_retained_retry_remaining_ms(void)
{
    return UINT32_MAX;
}

bool ble_link_service_retained_cleanup_pending(void)
{
    return false;
}

void ble_link_service_idle_timeout(uint32_t generation)
{
    (void)generation;
}

void ble_link_service_idle_timeout_epoch(uint32_t generation, uint32_t epoch)
{
    (void)generation;
    (void)epoch;
}

uint32_t ble_link_service_auth_expiry_remaining_ms(void)
{
    return UINT32_MAX;
}

esp_err_t ble_link_service_auth_expiry_tick(void)
{
    return ESP_OK;
}
