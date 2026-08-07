idf_component_register(
    SRCS "src/device_link_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ble_runtime
        device_link_security
        event_bus
    PRIV_REQUIRES
        esp_hw_support
        freertos
        mt_log
)
