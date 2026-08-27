#include <string.h>

#include "device_link_v1_engine.h"

static bool _device_link_v1_engine_ready(const device_link_v1_engine_t *engine)
{
    return engine != NULL;
}

static bool _device_link_v1_engine_terminal_ready(
    const device_link_v1_engine_t *engine)
{
    return engine->connected &&
           engine->record_present &&
           engine->accepted_confirmed &&
           engine->record.phase != DEVICE_LINK_V1_OPERATION_ACTIVE &&
           !engine->terminal_emitted &&
           !engine->terminal_omitted &&
           !engine->suppress_indications;
}

static bool _device_link_v1_engine_success_snapshot(
    device_link_v1_operation_t operation,
    const device_link_v1_snapshot_t *before,
    const device_link_v1_snapshot_t *after)
{
    switch (operation)
    {
    case DEVICE_LINK_V1_OPERATION_SCAN:
        return device_link_v1_snapshot_equal(before, after);
    case DEVICE_LINK_V1_OPERATION_SET_CREDENTIALS:
        return after->state == before->state &&
               after->failure == before->failure &&
               after->profile_ssid_length > 0U;
    case DEVICE_LINK_V1_OPERATION_CONNECT:
        return after->state == DEVICE_LINK_V1_WIFI_CONNECTED &&
               after->failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE &&
               after->profile_ssid_length > 0U;
    case DEVICE_LINK_V1_OPERATION_DISCONNECT:
        return after->state == DEVICE_LINK_V1_WIFI_IDLE &&
               after->failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE &&
               after->profile_ssid_length == before->profile_ssid_length &&
               memcmp(after->profile_ssid, before->profile_ssid,
                      after->profile_ssid_length) == 0;
    case DEVICE_LINK_V1_OPERATION_FORGET:
        return after->state == DEVICE_LINK_V1_WIFI_IDLE &&
               after->failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE &&
               after->profile_ssid_length == 0U;
    default:
        return false;
    }
}

static void _device_link_v1_engine_retain_ordinary(
    device_link_v1_engine_t *engine,
    const device_link_v1_snapshot_t *snapshot)
{
    engine->ordinary_snapshot = *snapshot;
    engine->ordinary_present = true;
}

static void _device_link_v1_engine_clear_record(device_link_v1_engine_t *engine)
{
    memset(&engine->record, 0, sizeof(engine->record));
    memset(&engine->pre_snapshot, 0, sizeof(engine->pre_snapshot));
    engine->record_present = false;
    engine->accepted_confirmed = false;
    engine->terminal_emitted = false;
    engine->terminal_omitted = false;
    engine->suppress_indications = false;
    engine->pending_accepted = false;
    engine->pending_ack = false;
}

static device_link_v1_status_t _device_link_v1_engine_admit(
    device_link_v1_engine_t *engine, device_link_v1_operation_t operation,
    uint8_t request_id, uint32_t operation_id)
{
    if (!_device_link_v1_engine_ready(engine) || request_id == 0U ||
            operation_id == 0U)
    {
        return DEVICE_LINK_V1_STATUS_INVALID_ARGUMENT;
    }
    if (!engine->connected || engine->in_flight != DEVICE_LINK_V1_TX_NONE)
    {
        return DEVICE_LINK_V1_STATUS_INTERNAL;
    }
    if (engine->record_present)
    {
        return DEVICE_LINK_V1_STATUS_BUSY;
    }
    if (engine->ids_exhausted)
    {
        return DEVICE_LINK_V1_STATUS_INTERNAL;
    }
    memset(&engine->record, 0, sizeof(engine->record));
    engine->record.operation_id = operation_id;
    engine->record.operation = operation;
    engine->record.phase = DEVICE_LINK_V1_OPERATION_ACTIVE;
    engine->record.failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    engine->pre_snapshot = engine->current_snapshot;
    engine->record_present = true;
    engine->accepted_confirmed = false;
    engine->terminal_emitted = false;
    engine->terminal_omitted = false;
    engine->suppress_indications = false;
    engine->response_request_id = request_id;
    engine->pending_accepted = true;
    engine->pending_ack = false;
    if (operation_id == UINT32_MAX)
    {
        engine->ids_exhausted = true;
        engine->next_operation_id = UINT32_MAX;
    }
    else if (operation_id >= engine->next_operation_id)
    {
        engine->next_operation_id = operation_id + 1U;
    }
    return DEVICE_LINK_V1_STATUS_ACCEPTED;
}

void device_link_v1_engine_init(device_link_v1_engine_t *engine,
                                const device_link_v1_snapshot_t *snapshot)
{
    if (engine == NULL)
    {
        return;
    }
    memset(engine, 0, sizeof(*engine));
    engine->next_operation_id = 1U;
    engine->connected = true;
    if (snapshot != NULL && device_link_v1_snapshot_valid(snapshot))
    {
        engine->current_snapshot = *snapshot;
    }
    else
    {
        engine->current_snapshot.state = DEVICE_LINK_V1_WIFI_IDLE;
        engine->current_snapshot.failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    }
}

bool device_link_v1_engine_write_blocked(const device_link_v1_engine_t *engine)
{
    return engine == NULL || engine->in_flight != DEVICE_LINK_V1_TX_NONE;
}

bool device_link_v1_engine_slot_occupied(const device_link_v1_engine_t *engine)
{
    return engine != NULL && engine->record_present;
}

bool device_link_v1_engine_profile_present(const device_link_v1_engine_t *engine)
{
    return engine != NULL && engine->current_snapshot.profile_ssid_length > 0U;
}

device_link_v1_status_t device_link_v1_engine_start(
    device_link_v1_engine_t *engine, device_link_v1_operation_t operation,
    uint8_t request_id, uint32_t *operation_id)
{
    if (operation_id == NULL)
    {
        return DEVICE_LINK_V1_STATUS_INVALID_ARGUMENT;
    }
    if (engine != NULL && engine->ids_exhausted)
    {
        *operation_id = 0U;
        return DEVICE_LINK_V1_STATUS_INTERNAL;
    }
    const uint32_t assigned = engine != NULL ? engine->next_operation_id : 0U;
    const device_link_v1_status_t status = _device_link_v1_engine_admit(
            engine, operation, request_id, assigned);

    *operation_id = (status == DEVICE_LINK_V1_STATUS_ACCEPTED) ? assigned : 0U;
    return status;
}

device_link_v1_status_t device_link_v1_engine_ack(
    device_link_v1_engine_t *engine, uint32_t operation_id,
    uint8_t request_id)
{
    if (!_device_link_v1_engine_ready(engine) || request_id == 0U ||
            operation_id == 0U)
    {
        return DEVICE_LINK_V1_STATUS_INVALID_ARGUMENT;
    }
    if (engine->in_flight != DEVICE_LINK_V1_TX_NONE)
    {
        return DEVICE_LINK_V1_STATUS_INTERNAL;
    }
    if (!engine->record_present ||
            engine->record.operation_id != operation_id)
    {
        return DEVICE_LINK_V1_STATUS_NOT_FOUND;
    }
    if (engine->record.phase == DEVICE_LINK_V1_OPERATION_ACTIVE)
    {
        return DEVICE_LINK_V1_STATUS_BUSY;
    }
    engine->response_request_id = request_id;
    engine->pending_ack = true;
    engine->pending_accepted = false;
    if (!engine->terminal_emitted)
    {
        engine->terminal_omitted = true;
    }
    return DEVICE_LINK_V1_STATUS_OK;
}

device_link_v1_status_t device_link_v1_engine_get_operation(
    device_link_v1_engine_t *engine,
    device_link_v1_operation_record_t *record)
{
    if (!_device_link_v1_engine_ready(engine) || record == NULL)
    {
        return DEVICE_LINK_V1_STATUS_INVALID_ARGUMENT;
    }
    if (!engine->record_present)
    {
        return DEVICE_LINK_V1_STATUS_NOT_FOUND;
    }
    *record = engine->record;
    return DEVICE_LINK_V1_STATUS_OK;
}

bool device_link_v1_engine_complete(
    device_link_v1_engine_t *engine, uint32_t operation_id,
    device_link_v1_wifi_failure_t failure,
    const device_link_v1_network_t *networks, uint8_t count,
    const device_link_v1_snapshot_t *snapshot)
{
    if (!_device_link_v1_engine_ready(engine) || snapshot == NULL ||
            !engine->record_present ||
            engine->record.operation_id != operation_id ||
            engine->record.phase != DEVICE_LINK_V1_OPERATION_ACTIVE ||
            !device_link_v1_failure_allowed(engine->record.operation, failure) ||
            !device_link_v1_snapshot_valid(snapshot) ||
            count > DEVICE_LINK_V1_MAX_SCAN_NETWORKS)
    {
        return false;
    }
    if (failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE &&
            !_device_link_v1_engine_success_snapshot(
                engine->record.operation, &engine->pre_snapshot, snapshot))
    {
        return false;
    }
    if (engine->record.operation == DEVICE_LINK_V1_OPERATION_SCAN)
    {
        if (failure != DEVICE_LINK_V1_WIFI_FAILURE_NONE && count != 0U)
        {
            return false;
        }
        if (count > 0U && networks == NULL)
        {
            return false;
        }
        engine->record.count = count;
        if (count > 0U)
        {
            memcpy(engine->record.networks, networks,
                   (size_t)count * sizeof(networks[0]));
        }
    }
    else if (count != 0U)
    {
        return false;
    }
    engine->record.failure = failure;
    engine->record.phase = (failure == DEVICE_LINK_V1_WIFI_FAILURE_NONE) ?
                           DEVICE_LINK_V1_OPERATION_SUCCEEDED :
                           DEVICE_LINK_V1_OPERATION_FAILED;
    const bool snapshot_changed = !device_link_v1_snapshot_equal(
                                      &engine->current_snapshot, snapshot);

    engine->current_snapshot = *snapshot;
    if (engine->connected && (snapshot_changed || engine->ordinary_present))
    {
        _device_link_v1_engine_retain_ordinary(engine, snapshot);
    }
    return true;
}

void device_link_v1_engine_observe_snapshot(
    device_link_v1_engine_t *engine,
    const device_link_v1_snapshot_t *snapshot)
{
    if (!_device_link_v1_engine_ready(engine) || snapshot == NULL ||
            !device_link_v1_snapshot_valid(snapshot))
    {
        return;
    }
    if (device_link_v1_snapshot_equal(&engine->current_snapshot, snapshot))
    {
        return;
    }
    engine->current_snapshot = *snapshot;
    if (engine->connected)
    {
        _device_link_v1_engine_retain_ordinary(engine, snapshot);
    }
}

void device_link_v1_engine_arm_response(
    device_link_v1_engine_t *engine, uint8_t request_id,
    bool accepted, bool ack)
{
    if (!_device_link_v1_engine_ready(engine) ||
            engine->in_flight != DEVICE_LINK_V1_TX_NONE)
    {
        return;
    }
    engine->response_request_id = request_id;
    engine->pending_accepted = accepted;
    engine->pending_ack = ack;
    engine->in_flight = DEVICE_LINK_V1_TX_RESPONSE;
}

device_link_v1_tx_kind_t device_link_v1_engine_next_tx(
    device_link_v1_engine_t *engine)
{
    if (!_device_link_v1_engine_ready(engine) ||
            engine->in_flight != DEVICE_LINK_V1_TX_NONE ||
            !engine->connected)
    {
        return engine != NULL ? engine->in_flight : DEVICE_LINK_V1_TX_NONE;
    }
    if (engine->ordinary_present &&
            device_link_v1_snapshot_equal(&engine->ordinary_snapshot,
                                          &engine->current_snapshot))
    {
        engine->in_flight = DEVICE_LINK_V1_TX_ORDINARY_STATUS;
        return DEVICE_LINK_V1_TX_ORDINARY_STATUS;
    }
    if (_device_link_v1_engine_terminal_ready(engine))
    {
        engine->in_flight = DEVICE_LINK_V1_TX_TERMINAL;
        return DEVICE_LINK_V1_TX_TERMINAL;
    }
    return DEVICE_LINK_V1_TX_NONE;
}

void device_link_v1_engine_confirm_tx(device_link_v1_engine_t *engine)
{
    if (!_device_link_v1_engine_ready(engine))
    {
        return;
    }
    switch (engine->in_flight)
    {
    case DEVICE_LINK_V1_TX_RESPONSE:
        if (engine->pending_accepted && engine->record_present)
        {
            engine->accepted_confirmed = true;
        }
        if (engine->pending_ack && engine->record_present &&
                engine->record.phase != DEVICE_LINK_V1_OPERATION_ACTIVE)
        {
            _device_link_v1_engine_clear_record(engine);
        }
        engine->pending_accepted = false;
        engine->pending_ack = false;
        break;
    case DEVICE_LINK_V1_TX_ORDINARY_STATUS:
        engine->ordinary_present = false;
        break;
    case DEVICE_LINK_V1_TX_TERMINAL:
        engine->terminal_emitted = true;
        break;
    case DEVICE_LINK_V1_TX_NONE:
    default:
        break;
    }
    engine->in_flight = DEVICE_LINK_V1_TX_NONE;
}

void device_link_v1_engine_abort_tx(device_link_v1_engine_t *engine)
{
    if (!_device_link_v1_engine_ready(engine) ||
            engine->in_flight == DEVICE_LINK_V1_TX_NONE)
    {
        return;
    }
    engine->pending_accepted = false;
    engine->pending_ack = false;
    engine->in_flight = DEVICE_LINK_V1_TX_NONE;
}

void device_link_v1_engine_disconnect(device_link_v1_engine_t *engine)
{
    if (!_device_link_v1_engine_ready(engine) || !engine->connected)
    {
        return;
    }
    if (engine->record_present)
    {
        engine->suppress_indications = true;
        if (engine->in_flight == DEVICE_LINK_V1_TX_RESPONSE &&
                engine->pending_accepted)
        {
            engine->accepted_confirmed = false;
        }
    }
    engine->ordinary_present = false;
    engine->pending_accepted = false;
    engine->pending_ack = false;
    engine->in_flight = DEVICE_LINK_V1_TX_NONE;
    engine->connected = false;
}

void device_link_v1_engine_connect(device_link_v1_engine_t *engine)
{
    if (_device_link_v1_engine_ready(engine))
    {
        engine->connected = true;
    }
}

const device_link_v1_snapshot_t *device_link_v1_engine_snapshot(
    const device_link_v1_engine_t *engine)
{
    return engine != NULL ? &engine->current_snapshot : NULL;
}

const device_link_v1_operation_record_t *device_link_v1_engine_record(
    const device_link_v1_engine_t *engine)
{
    if (engine == NULL || !engine->record_present)
    {
        return NULL;
    }
    return &engine->record;
}

#ifdef UNIT_TEST_HOST
device_link_v1_status_t device_link_v1_engine_start_with_id(
    device_link_v1_engine_t *engine, device_link_v1_operation_t operation,
    uint8_t request_id, uint32_t operation_id)
{
    return _device_link_v1_engine_admit(engine, operation, request_id,
                                        operation_id);
}

void device_link_v1_engine_test_exhaust_ids(device_link_v1_engine_t *engine)
{
    if (engine != NULL)
    {
        engine->ids_exhausted = true;
        engine->next_operation_id = UINT32_MAX;
    }
}

void device_link_v1_engine_test_reboot(device_link_v1_engine_t *engine)
{
    device_link_v1_snapshot_t snapshot;

    if (engine == NULL)
    {
        return;
    }
    snapshot = engine->current_snapshot;
    device_link_v1_engine_init(engine, &snapshot);
    engine->connected = false;
}
#endif
