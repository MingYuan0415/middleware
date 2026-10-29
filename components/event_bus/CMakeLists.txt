idf_component_register(
    SRCS "src/event_bus.c" "src/event_bus_core.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mt_log
)
