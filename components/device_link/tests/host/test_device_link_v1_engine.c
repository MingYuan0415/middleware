#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "device_link_v1_engine.h"

static device_link_v1_snapshot_t _idle(void)
{
    device_link_v1_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.state = DEVICE_LINK_V1_WIFI_IDLE;
    snapshot.failure = DEVICE_LINK_V1_WIFI_FAILURE_NONE;
    return snapshot;
}

static device_link_v1_snapshot_t _home_connected(void)
{
    device_link_v1_snapshot_t snapshot = _idle();

    snapshot.state = DEVICE_LINK_V1_WIFI_CONNECTED;
    snapshot.profile_ssid[0] = 'H';
    snapshot.profile_ssid[1] = 'o';
    snapshot.profile_ssid[2] = 'm';
    snapshot.profile_ssid[3] = 'e';
    snapshot.profile_ssid_length = 4U;
    return snapshot;
}

static void _confirm_response(device_link_v1_engine_t *engine)
{
    assert(engine->in_flight == DEVICE_LINK_V1_TX_RESPONSE);
    device_link_v1_engine_confirm_tx(engine);
}

static void _start_id(device_link_v1_engine_t *engine,
                      device_link_v1_operation_t operation, uint32_t id,
                      device_link_v1_status_t expect)
{
    const device_link_v1_status_t status =
        device_link_v1_engine_start_with_id(engine, operation, 1U, id);

    assert(status == expect);
    if (status == DEVICE_LINK_V1_STATUS_ACCEPTED)
    {
        device_link_v1_engine_arm_response(engine, 1U, true, false);
        _confirm_response(engine);
    }
}

static void _complete_home_connect(device_link_v1_engine_t *engine, uint32_t id)
{
    const device_link_v1_snapshot_t snapshot = _home_connected();

    assert(device_link_v1_engine_complete(
               engine, id, DEVICE_LINK_V1_WIFI_FAILURE_NONE, NULL, 0U,
               &snapshot));
}

static void _emit_ordinary_then_terminal(device_link_v1_engine_t *engine)
{
    assert(device_link_v1_engine_next_tx(engine) ==
           DEVICE_LINK_V1_TX_ORDINARY_STATUS);
    device_link_v1_engine_confirm_tx(engine);
    assert(device_link_v1_engine_next_tx(engine) == DEVICE_LINK_V1_TX_TERMINAL);
    device_link_v1_engine_confirm_tx(engine);
}

static void _ack_ok(device_link_v1_engine_t *engine, uint32_t id)
{
    assert(device_link_v1_engine_ack(engine, id, 2U) == DEVICE_LINK_V1_STATUS_OK);
    device_link_v1_engine_arm_response(engine, 2U, false, true);
    _confirm_response(engine);
}

static void _test_connected_terminal_and_ack_order(void)
{
    device_link_v1_engine_t engine;
    device_link_v1_operation_record_t record;
    const device_link_v1_snapshot_t idle = _idle();

    device_link_v1_engine_init(&engine, &idle);
    _start_id(&engine, DEVICE_LINK_V1_OPERATION_CONNECT, 1U,
              DEVICE_LINK_V1_STATUS_ACCEPTED);
    assert(device_link_v1_engine_get_operation(&engine,
            &record) == DEVICE_LINK_V1_STATUS_OK);
    assert(record.phase == DEVICE_LINK_V1_OPERATION_ACTIVE);
    assert(device_link_v1_engine_start_with_id(
               &engine, DEVICE_LINK_V1_OPERATION_SCAN, 3U, 2U) ==
           DEVICE_LINK_V1_STATUS_BUSY);
    assert(device_link_v1_engine_ack(&engine, 1U,
                                     4U) == DEVICE_LINK_V1_STATUS_BUSY);
    _complete_home_connect(&engine, 1U);
    assert(device_link_v1_engine_next_tx(&engine) ==
           DEVICE_LINK_V1_TX_ORDINARY_STATUS);
    assert(device_link_v1_engine_write_blocked(&engine));
    assert(device_link_v1_engine_ack(&engine, 1U,
                                     5U) == DEVICE_LINK_V1_STATUS_INTERNAL);
    device_link_v1_engine_confirm_tx(&engine);
    assert(device_link_v1_engine_next_tx(&engine) == DEVICE_LINK_V1_TX_TERMINAL);
    assert(device_link_v1_engine_write_blocked(&engine));
    device_link_v1_engine_confirm_tx(&engine);
    assert(device_link_v1_engine_ack(&engine, 99U,
                                     6U) == DEVICE_LINK_V1_STATUS_NOT_FOUND);
    assert(device_link_v1_engine_start_with_id(
               &engine, DEVICE_LINK_V1_OPERATION_SCAN, 7U, 3U) ==
           DEVICE_LINK_V1_STATUS_BUSY);
    _ack_ok(&engine, 1U);
    assert(device_link_v1_engine_get_operation(&engine,
            &record) == DEVICE_LINK_V1_STATUS_NOT_FOUND);
}

static void _test_ack_omits_terminal(void)
{
    device_link_v1_engine_t engine;
    const device_link_v1_snapshot_t idle = _idle();
    device_link_v1_operation_record_t record;

    device_link_v1_engine_init(&engine, &idle);
    _start_id(&engine, DEVICE_LINK_V1_OPERATION_SCAN, 71U,
              DEVICE_LINK_V1_STATUS_ACCEPTED);
    assert(device_link_v1_engine_complete(
               &engine, 71U, DEVICE_LINK_V1_WIFI_FAILURE_NONE, NULL, 0U, &idle));
    assert(device_link_v1_engine_get_operation(&engine,
            &record) == DEVICE_LINK_V1_STATUS_OK);
    assert(record.phase == DEVICE_LINK_V1_OPERATION_SUCCEEDED);
    _ack_ok(&engine, 71U);
    assert(device_link_v1_engine_next_tx(&engine) == DEVICE_LINK_V1_TX_NONE);
    assert(device_link_v1_engine_get_operation(&engine,
            &record) == DEVICE_LINK_V1_STATUS_NOT_FOUND);
}

static void _test_disconnect_keeps_record(void)
{
    device_link_v1_engine_t engine;
    device_link_v1_operation_record_t record;
    const device_link_v1_snapshot_t idle = _idle();

    device_link_v1_engine_init(&engine, &idle);
    _start_id(&engine, DEVICE_LINK_V1_OPERATION_SCAN, 10U,
              DEVICE_LINK_V1_STATUS_ACCEPTED);
    assert(device_link_v1_engine_complete(
               &engine, 10U, DEVICE_LINK_V1_WIFI_FAILURE_NONE, NULL, 0U, &idle));
    device_link_v1_engine_disconnect(&engine);
    device_link_v1_engine_connect(&engine);
    assert(device_link_v1_engine_next_tx(&engine) == DEVICE_LINK_V1_TX_NONE);
    assert(device_link_v1_engine_get_operation(&engine,
            &record) == DEVICE_LINK_V1_STATUS_OK);
    assert(record.operation_id == 10U);
    _ack_ok(&engine, 10U);
}

static void _test_unconfirmed_accepted_omits_terminal(void)
{
    device_link_v1_engine_t engine;
    device_link_v1_operation_record_t record;
    const device_link_v1_snapshot_t idle = _idle();

    device_link_v1_engine_init(&engine, &idle);
    assert(device_link_v1_engine_start_with_id(
               &engine, DEVICE_LINK_V1_OPERATION_SCAN, 1U, 20U) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    device_link_v1_engine_arm_response(&engine, 1U, true, false);
    device_link_v1_engine_disconnect(&engine);
    assert(device_link_v1_engine_complete(
               &engine, 20U, DEVICE_LINK_V1_WIFI_FAILURE_NONE, NULL, 0U, &idle));
    device_link_v1_engine_connect(&engine);
    assert(device_link_v1_engine_next_tx(&engine) == DEVICE_LINK_V1_TX_NONE);
    assert(device_link_v1_engine_get_operation(&engine,
            &record) == DEVICE_LINK_V1_STATUS_OK);
    _ack_ok(&engine, 20U);
}

static void _test_ordinary_coalesce_and_id_exhaustion(void)
{
    device_link_v1_engine_t engine;
    device_link_v1_snapshot_t connecting = _home_connected();
    const device_link_v1_snapshot_t idle = _idle();
    uint32_t operation_id = 0U;

    connecting.state = DEVICE_LINK_V1_WIFI_CONNECTING;
    device_link_v1_engine_init(&engine, &idle);
    const device_link_v1_snapshot_t connected = _home_connected();

    device_link_v1_engine_observe_snapshot(&engine, &connecting);
    device_link_v1_engine_observe_snapshot(&engine, &connected);
    assert(device_link_v1_engine_next_tx(&engine) ==
           DEVICE_LINK_V1_TX_ORDINARY_STATUS);
    device_link_v1_engine_disconnect(&engine);
    assert(engine.in_flight == DEVICE_LINK_V1_TX_NONE);
    device_link_v1_engine_connect(&engine);
    assert(device_link_v1_engine_next_tx(&engine) == DEVICE_LINK_V1_TX_NONE);

    device_link_v1_engine_init(&engine, &idle);
    _start_id(&engine, DEVICE_LINK_V1_OPERATION_SCAN, UINT32_MAX,
              DEVICE_LINK_V1_STATUS_ACCEPTED);
    assert(device_link_v1_engine_complete(
               &engine, UINT32_MAX, DEVICE_LINK_V1_WIFI_FAILURE_NONE, NULL, 0U,
               &idle));
    assert(device_link_v1_engine_next_tx(&engine) == DEVICE_LINK_V1_TX_TERMINAL);
    device_link_v1_engine_confirm_tx(&engine);
    _ack_ok(&engine, UINT32_MAX);
    device_link_v1_engine_test_exhaust_ids(&engine);
    assert(device_link_v1_engine_start(
               &engine, DEVICE_LINK_V1_OPERATION_SCAN, 1U, &operation_id) ==
           DEVICE_LINK_V1_STATUS_INTERNAL);
    device_link_v1_engine_test_reboot(&engine);
    device_link_v1_engine_connect(&engine);
    assert(device_link_v1_engine_start(
               &engine, DEVICE_LINK_V1_OPERATION_CONNECT, 1U, &operation_id) ==
           DEVICE_LINK_V1_STATUS_ACCEPTED);
    assert(operation_id == 1U);
}

static void _test_scan_payload_correlation(void)
{
    device_link_v1_engine_t engine;
    device_link_v1_network_t network;
    device_link_v1_operation_record_t record;
    const device_link_v1_snapshot_t idle = _idle();

    memset(&network, 0, sizeof(network));
    network.ssid[0] = 'H';
    network.ssid[1] = 'o';
    network.ssid[2] = 'm';
    network.ssid[3] = 'e';
    network.ssid_length = 4U;
    network.security = DEVICE_LINK_V1_WIFI_PERSONAL;
    network.rssi_dbm = -42;
    device_link_v1_engine_init(&engine, &idle);
    _start_id(&engine, DEVICE_LINK_V1_OPERATION_SCAN, 7U,
              DEVICE_LINK_V1_STATUS_ACCEPTED);
    assert(device_link_v1_engine_complete(
               &engine, 7U, DEVICE_LINK_V1_WIFI_FAILURE_NONE, &network, 1U,
               &idle));
    assert(device_link_v1_engine_get_operation(&engine,
            &record) == DEVICE_LINK_V1_STATUS_OK);
    assert(record.count == 1U);
    assert(record.networks[0].rssi_dbm == -42);
    assert(device_link_v1_engine_next_tx(&engine) == DEVICE_LINK_V1_TX_TERMINAL);
    device_link_v1_engine_confirm_tx(&engine);
    _ack_ok(&engine, 7U);
}

static void _test_connect_authentication(void)
{
    device_link_v1_engine_t engine;
    device_link_v1_snapshot_t snapshot = _home_connected();
    const device_link_v1_snapshot_t idle = _idle();

    snapshot.state = DEVICE_LINK_V1_WIFI_ERROR;
    snapshot.failure = DEVICE_LINK_V1_WIFI_FAILURE_AUTHENTICATION;
    device_link_v1_engine_init(&engine, &idle);
    _start_id(&engine, DEVICE_LINK_V1_OPERATION_CONNECT, 8U,
              DEVICE_LINK_V1_STATUS_ACCEPTED);
    assert(device_link_v1_engine_complete(
               &engine, 8U, DEVICE_LINK_V1_WIFI_FAILURE_AUTHENTICATION, NULL, 0U,
               &snapshot));
    _emit_ordinary_then_terminal(&engine);
    _ack_ok(&engine, 8U);
}

int main(void)
{
    _test_connected_terminal_and_ack_order();
    _test_ack_omits_terminal();
    _test_disconnect_keeps_record();
    _test_unconfirmed_accepted_omits_terminal();
    _test_ordinary_coalesce_and_id_exhaustion();
    _test_scan_payload_correlation();
    _test_connect_authentication();
    return 0;
}
