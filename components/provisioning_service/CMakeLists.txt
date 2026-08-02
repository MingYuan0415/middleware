idf_component_register(
    SRCS
        "src/provisioning_service.c"
        "src/provisioning_protocol.c"
        "src/generated/microtech/provisioning/v1/provisioning.pb-c.c"
        "src/generated/microtech/provisioning/v1/types.pb-c.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "src/generated"
    REQUIRES connectivity_manager event_bus
    PRIV_REQUIRES
        app_update
        bt
        efuse
        esp_event
        esp_timer
        freertos
        mbedtls
        mt_log
        protobuf-c
        protocomm
)

idf_component_get_property(protocomm_lib protocomm COMPONENT_LIB)
target_compile_definitions(${protocomm_lib} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_INFO)
