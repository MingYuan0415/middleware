idf_component_register(
    SRCS
        "src/chore_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        mt_log
        esp_timer
        freertos
        heap
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wframe-larger-than=2048)
