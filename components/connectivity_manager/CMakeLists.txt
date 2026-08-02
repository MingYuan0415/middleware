idf_component_register(
    SRCS "src/connectivity_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES event_bus
    PRIV_REQUIRES freertos mt_log nvs_flash nv_storage wifi_service
)
