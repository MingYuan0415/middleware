idf_component_register(
    SRCS
        "src/time_service.c"
        "src/time_service_core.c"
        "src/time_service_port.c"
    INCLUDE_DIRS "include"
    REQUIRES
        event_bus
    PRIV_REQUIRES
        mt_log
        nv_storage
        esp_netif
        lwip
)
