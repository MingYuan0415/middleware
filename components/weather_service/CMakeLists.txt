idf_component_register(
    SRCS
        "src/weather_service.c"
        "src/weather_service_cache.c"
        "src/weather_service_parse.c"
        "src/weather_service_port.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        event_bus
    PRIV_REQUIRES
        mt_log
        esp_http_client
        mbedtls
        espressif__cjson
        freertos
        heap
)
