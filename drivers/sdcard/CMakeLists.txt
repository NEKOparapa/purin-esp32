idf_component_register(
    SRCS "sdcard.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc vfs
)
