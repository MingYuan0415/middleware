idf_component_register(
    SRCS "src/onboarding_service.c"
    INCLUDE_DIRS "include"
    REQUIRES nv_storage nvs_flash
)
