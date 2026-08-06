idf_component_register(
    SRCS
        "src/ble_runtime.c"
        "src/ble_gatt_registry.c"
        "src/ble_nimble_port.c"
    INCLUDE_DIRS "include"
    REQUIRES bt
    PRIV_REQUIRES
        freertos
        mt_log
)
