idf_component_register(
    SRCS
        "src/music_service.c"
        "src/music_service_cache.c"
        "src/music_snapshot.c"
        "src/music_queue_model.c"
        "src/music_http.c"
        "src/music_media.c"
        "src/music_api.c"
        "src/music_auth.c"
        "src/music_catalog.c"
        "src/music_login_protocol.c"
        "src/music_profile.c"
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
        espressif__libpng
        freertos
        heap
        mbedtls
        mt_log
        nvs_flash
        nv_storage
        time_service
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wframe-larger-than=4096)
