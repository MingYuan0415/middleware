idf_component_register(
    SRCS "src/file_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer fatfs freertos heap mt_log sd_storage_service
                  vfs
)
