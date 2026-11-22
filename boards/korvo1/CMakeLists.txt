# The player on the ESP32-S31-Korvo-1: scripts/build-korvo.sh builds it into build/player.bin.
# The board port (the korvo1 component) and PRISM (the prism component) come from prism-esp32.
cmake_minimum_required(VERSION 3.22)

set(PRISM_ESP32 "${CMAKE_CURRENT_LIST_DIR}/../../subprojects/prism-esp32")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(EXTRA_COMPONENT_DIRS "${PRISM_ESP32}/boards/korvo1" "${PRISM_ESP32}/components/prism")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(player)
