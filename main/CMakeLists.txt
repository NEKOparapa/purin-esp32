if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "Metalio E-Ink 4 requires ESP32-S3. Run idf.py set-target esp32s3")
endif()

set(app_sources "app_main.c")
if(CONFIG_WINTERMUTE_DISPLAY_DIAGNOSTIC)
    list(APPEND app_sources "apps/display_diagnostic.c" "tests/display_test_pattern.c")
else()
    list(APPEND app_sources "apps/hardware_test.c" "ui/lvgl_demo_ui.c"
                           "tests/sdcard_test.c" "tests/sd_test_file.c")
endif()
# IDF 预先扫描依赖时尚未加载 CONFIG_*，基础组件依赖不能按模式条件声明。
idf_component_register(SRCS ${app_sources}
                       PRIV_REQUIRES board lvgl_port esp_timer esp_hw_support
                       PRIV_INCLUDE_DIRS "apps" "ui" "tests")

if(CONFIG_WINTERMUTE_DISPLAY_DIAGNOSTIC)
    # ssd1677 已由 board 引入构建；配置加载后，仅给原始诊断开放波形测试接口。
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::ssd1677)
endif()
