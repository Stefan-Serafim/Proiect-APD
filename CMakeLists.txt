cmake_minimum_required(VERSION 3.22)

set(APD_NODE "a" CACHE STRING "ESP32 node profile (a or b)")
if(NOT APD_NODE MATCHES "^[ab]$")
    message(FATAL_ERROR "APD_NODE must be a or b")
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/embedded/src")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig.node_${APD_NODE}")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/embedded/profiles/node_${APD_NODE}.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(apd_esp32_chat)

spiffs_create_partition_image(www "${CMAKE_CURRENT_LIST_DIR}/data/www" FLASH_IN_PROJECT)
