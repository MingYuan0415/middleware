idf_component_register(
    SRCS "src/audio_service.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp
    PRIV_REQUIRES freertos mt_log
)
