idf_component_register(
    SRCS "src/system_pm.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        mt_log
        esp_pm
        esp_driver_gpio
        esp_hw_support
)
