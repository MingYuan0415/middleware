idf_component_register(
    SRCS
        "src/device_link_service.c"
        "src/device_link_wifi_adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ble_runtime
        device_link
        connectivity_manager
        event_bus
        nv_storage
        wifi_service
    PRIV_REQUIRES
        esp_hw_support
        freertos
        mt_log
        nvs_flash
)
