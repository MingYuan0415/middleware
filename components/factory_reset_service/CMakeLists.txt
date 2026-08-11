idf_component_register(
    SRCS "src/factory_reset_service.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_storage
    PRIV_REQUIRES
        mt_log
        nvs_flash
)
