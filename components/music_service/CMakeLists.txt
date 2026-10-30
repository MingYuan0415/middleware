idf_component_register(
    SRCS
        "src/music_service.c"
        "src/music_net.c"
        "src/music_stream.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        event_bus
    PRIV_REQUIRES
        audio_service
        esp_http_client
        esp_timer
        espressif__cjson
        espressif__esp_audio_codec
        espressif__esp_new_jpeg
        freertos
        heap
        mbedtls
        mt_log
        nvs_flash
        nv_storage
        time_service
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wframe-larger-than=4096)
