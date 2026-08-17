idf_component_register(
    SRCS
        "src/wifi_service.c"
        "src/wifi_service_credentials.c"
        "src/wifi_service_worker.c"
        "src/wifi_service_worker_ops.c"
        "src/wifi_service_idf_port.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES event_bus
    PRIV_REQUIRES esp_event esp_netif esp_wifi freertos mt_log
)
