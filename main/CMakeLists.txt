if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "Metalio E-Ink 4 requires ESP32-S3. Run idf.py set-target esp32s3")
endif()

idf_component_register(SRCS "app_main.c" "ui/home_screen.c"
                            "recording/recorder.c"
                       PRIV_REQUIRES board lvgl_port audio_capture recording_storage esp_timer
                       PRIV_INCLUDE_DIRS "ui" "recording")
