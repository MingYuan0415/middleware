idf_build_get_property(_ble_target IDF_TARGET)
set(_ble_radio_requires)
if(_ble_target STREQUAL "esp32p4")
    # The CP owns the BT controller; the host runs NimBLE and the hosted
    # stack adapter binds it to the SDIO HCI byte pipe.
    list(APPEND _ble_radio_requires esp_hosted)
endif()

idf_component_register(
    SRCS
    "src/ble_runtime.c"
    "src/ble_gatt_registry.c"
    "src/ble_nimble_port.c"
    "src/ble_nimble_host.c"
    "src/ble_nimble_link_events.c"
    "src/ble_nimble_adv_start.c"
    "src/ble_nimble_peer_identity.c"
    "src/ble_nimble_pairing_gate.c"
    "src/ble_nimble_port_revoke_journal.c"
    "src/ble_nimble_smp_policy.c"
    "src/ble_nimble_smp_io.c"
    "src/ble_nimble_store_guard.c"
    "src/ble_nimble_store_restore_audit.c"
    "src/ble_nimble_store_reset.c"
    "src/ble_gap_manager.c"
    "src/ble_adv_manager.c"
    "src/ble_tx_scheduler.c"
    "src/ble_link_cleanup_obligation.c"
    "src/ble_link_timer_deadline.c"
    "src/ble_nimble_tx_tracker.c"
    "src/ble_link_session.c"
    "src/ble_link_sec_state.c"
    "src/ble_link_service.c"
    "src/ble_link_gatt.c"
    "src/ble_event_router.c"
    INCLUDE_DIRS "include"
    REQUIRES
    bt
    device_link
    ${_ble_radio_requires}
    PRIV_REQUIRES
    freertos
    mt_log
    nvs_flash
    nv_storage
)
