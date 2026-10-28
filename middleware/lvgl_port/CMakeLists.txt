idf_component_register(
    SRCS "lvgl_port.c" "display/epaper_lvgl.c" "input/touch_lvgl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "display" "input"
    REQUIRES lvgl__lvgl
    PRIV_REQUIRES board ssd1677 cst816s_touch esp_timer
)
