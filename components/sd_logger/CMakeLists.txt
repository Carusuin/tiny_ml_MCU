idf_component_register(
    SRCS "src/sd_logger.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_sdspi esp_driver_spi
)
