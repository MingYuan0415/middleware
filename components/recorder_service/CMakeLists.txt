idf_component_register(
    SRCS "src/recorder_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES audio_service esp_timer fatfs freertos mt_log
                  sd_storage_service
)
