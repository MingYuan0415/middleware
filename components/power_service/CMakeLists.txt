idf_component_register(
    SRCS "src/power_service.c"
    INCLUDE_DIRS "include"
    REQUIRES event_bus
    PRIV_REQUIRES mt_log esp_timer
)
