idf_component_register(
    SRCS
        "src/ble_runtime.c"
        "src/ble_gatt_registry.c"
        "src/ble_nimble_port.c"
        "src/ble_gap_manager.c"
        "src/ble_adv_manager.c"
        "src/ble_tx_scheduler.c"
        "src/ble_response_cache.c"
        "src/ble_link_codec.c"
        "src/ble_link_dispatcher.c"
        "src/ble_event_router.c"
    INCLUDE_DIRS "include"
    REQUIRES bt
    PRIV_REQUIRES
        freertos
        mt_log
)
