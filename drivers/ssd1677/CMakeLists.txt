idf_component_register(
    SRCS "esp_lcd_panel_ssd1677.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd
    PRIV_REQUIRES esp_driver_gpio esp_timer
)
