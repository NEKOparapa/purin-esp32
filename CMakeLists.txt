cmake_minimum_required(VERSION 3.16)

# 按职责组织源码目录；只有实际驱动、板级层和 LVGL 适配层注册为组件。
set(EXTRA_COMPONENT_DIRS
    "board"
    "drivers/ssd1677"
    "drivers/cst816s_touch"
    "drivers/tca9555"
    "drivers/sdcard"
    "middleware/lvgl_port"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(wintermute-esp32)
