idf_component_register(
    SRCS "src/kwp2000.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_common freertos
)
