idf_component_register(
    SRCS
        "src/ble_runtime.c"
        "src/ble_gatt_registry.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mt_log
)
