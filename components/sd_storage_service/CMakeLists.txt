idf_component_register(
    SRCS "src/sd_storage_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mt_log freertos
)
