idf_component_register(
    SRCS "board.c" "board_i2c.c" "board_display.c" "board_sdcard.c" "board_vibration.c" "board_audio.c" "board_clock.c" "board_battery.c" "board_camera.c" "board_nt26.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "../config"
    REQUIRES esp_lcd cst816s_touch i2s_microphone
    PRIV_REQUIRES tca9555 ssd1677 sdcard fatfs vibration esp_driver_spi esp_driver_gpio esp_driver_i2c esp_driver_uart
                  freertos usb usb_host_uvc hal 78__uart-uhci espressif__iot_eth
)
