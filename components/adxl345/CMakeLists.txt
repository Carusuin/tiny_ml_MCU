idf_component_register(SRCS "src/adxl345.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_i2c esp_timer freertos)
