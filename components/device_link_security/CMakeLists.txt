idf_component_register(
    SRCS "src/device_link_security.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nv_storage
        protocomm
    PRIV_REQUIRES
        mbedtls
        mt_log
        nvs_flash
)
