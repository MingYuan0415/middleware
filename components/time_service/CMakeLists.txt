idf_component_register(
    SRCS
        "src/time_service.c"
        "src/time_service_core.c"
        "src/time_service_port.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        mt_log
        event_bus
        nv_storage
        esp_netif
        lwip
)
