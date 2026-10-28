idf_component_register(
    SRCS "cst816s_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c
    PRIV_REQUIRES espressif__esp_lcd_touch_cst816s esp_lcd freertos
)
