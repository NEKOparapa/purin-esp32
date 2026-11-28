if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "Metalio E-Ink 4 requires ESP32-S3. Run idf.py set-target esp32s3")
endif()

idf_component_register(SRCS "app_main.c" "ui/eink_ui.c" "ui/home_screen.c" "ui/status_bar.c"
                            "ui/settings_screen.c" "ui/home_response.c" "ui/home_graphics.c"
                            "ui/fonts/home_controls.c"
                            "agent/terminal_network.c" "agent/terminal_http.c" "agent/terminal_tools.c"
                            "agent/terminal_pairing.c"
                            "agent/terminal_service.c" "apps/device_apps.c" "apps/library.c" "apps/book_cover.c"
                            "apps/reading_progress.c" "apps/day_schedule.c" "ui/schedule_screen.c"
                            "apps/player.c" "ui/device_apps_screen.c" "ui/reader_screen.c" "ui/reader_layout.c"
                            "recording/recorder.c" "upload/audio_upload.c"
                       PRIV_REQUIRES board lvgl_port audio_capture recording_storage qr_scanner esp_timer
                                     esp_http_client esp_wifi esp_netif nvs_flash mbedtls esp_event json spi_flash
                                     bootloader_support
                       PRIV_INCLUDE_DIRS "ui" "recording" "upload" "agent" "apps" "../config"
                       EMBED_FILES "ui/fonts/ui_sc.ttf" "ui/art/home_cat.i1" "ui/art/status_icons.i1")

# 检测本地配置的新增/移除，避免第一次复制凭据文件后增量构建仍沿用空模板。
file(GLOB upload_local_config CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../config/upload_config.local.h")
if(upload_local_config)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PURIN_LOCAL_UPLOAD_CONFIG=1)
endif()
