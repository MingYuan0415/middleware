idf_component_register(
    SRCS "src/nv_storage.c" "src/nv_storage_blob.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        freertos
        nvs_flash
        mt_log
)
