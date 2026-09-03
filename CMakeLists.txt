cmake_minimum_required(VERSION 3.16.0)

# Which board this build targets. platformio.ini passes it per env via
#   board_build.cmake_extra_args = -DFLAPPY_BOARD=<name>
# and it selects three things: the pin/feature header under boards/<name>/,
# the sdkconfig defaults fragment, and which panel/touch drivers get linked.
# The default keeps a bare `idf.py build` working.
set(FLAPPY_BOARD "touch_lcd147" CACHE STRING "Target board: touch_lcd147 | lcd147")

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${FLAPPY_BOARD}/board_pins.h")
    message(FATAL_ERROR "Unknown FLAPPY_BOARD '${FLAPPY_BOARD}' -- "
                        "expected a boards/<name>/board_pins.h")
endif()

# The two boards differ in flash size, which has to be right in sdkconfig and
# not only in esptool's arguments, so the defaults are split common + per-board.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.${FLAPPY_BOARD}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flappyesp-${FLAPPY_BOARD})
