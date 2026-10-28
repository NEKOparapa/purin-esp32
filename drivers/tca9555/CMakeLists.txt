idf_component_register(
    SRCS "tca9555.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES freertos
)
