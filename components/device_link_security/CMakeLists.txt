idf_component_register(
    SRCS "src/device_link_security.c"
    INCLUDE_DIRS "include"
    REQUIRES
        protocomm
    PRIV_REQUIRES
        mbedtls
        mt_log
)
