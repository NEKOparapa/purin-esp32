if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "Metalio E-Ink 4 requires ESP32-S3. Run idf.py set-target esp32s3")
endif()

idf_component_register(SRCS "app_main.c" "ui/home_screen.c" "ui/agent_screen.c"
                            "agent/terminal_network.c" "agent/terminal_http.c" "ui/notice_screen.c" "agent/notice_file.c" "agent/agent_client.c"
                            "recording/recorder.c" "upload/audio_upload.c"
                       PRIV_REQUIRES board lvgl_port audio_capture recording_storage esp_timer
                                     esp_http_client esp_wifi esp_netif nvs_flash mbedtls esp_event json
                       PRIV_INCLUDE_DIRS "ui" "recording" "upload" "agent" "../config"
                       EMBED_FILES "ui/fonts/chat_sc.ttf")

# 检测本地配置的新增/移除，避免第一次复制凭据文件后增量构建仍沿用空模板。
file(GLOB upload_local_config CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../config/upload_config.local.h")
if(upload_local_config)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WINTERMUTE_LOCAL_UPLOAD_CONFIG=1)
endif()
