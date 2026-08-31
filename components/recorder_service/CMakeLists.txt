idf_component_register(
    SRCS "src/recorder_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES audio_service sd_storage_service esp_timer fatfs freertos
                  mt_log
)
