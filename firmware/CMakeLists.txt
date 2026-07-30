cmake_minimum_required(VERSION 3.16)

# This MUST precede pico_sdk_import.cmake. Left unset, pico-sdk silently
# defaults to PICO_BOARD=pico (RP2040) and produces an ARMv6-M binary that will
# not run on this board -- and the failure looks like a dead board, not a build
# mistake. The vendor's own basic demo has exactly this bug.
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(h0urg1ass C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

# The graphics library, built straight from source. There is no ESP-IDF-style
# component system for pico-sdk, so the source list is globbed.
set(ONEBIT_ROOT ${CMAKE_CURRENT_LIST_DIR}/../third_party/1bit-display)
file(GLOB ONEBIT_SOURCES "${ONEBIT_ROOT}/src/*/*.cpp")

add_executable(h0urg1ass
    src/main.cpp
    src/board/st7789_1in69.cpp
    src/faces/digits_face.cpp
    src/faces/hourglass_face.cpp
    src/faces/splitflap_face.cpp
    src/input/orientation.cpp
    ${ONEBIT_SOURCES}
)

target_include_directories(h0urg1ass PRIVATE
    src
    ${ONEBIT_ROOT}/include
    ${ONEBIT_ROOT}
)

target_link_libraries(h0urg1ass
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_pwm
    hardware_i2c
    hardware_adc
    hardware_clocks
)

# pico2.h defaults PICO_FLASH_SIZE_BYTES to 4 MB. This board carries a discrete
# 16 MB W25Q128JVSIQ.
target_compile_definitions(h0urg1ass PRIVATE
    PICO_FLASH_SIZE_BYTES=16777216
)

# Console over native USB. GPIO0/1 are broken out to test points, but USB-C is
# the only connection during bring-up -- and pico_stdio_usb is what makes the
# 1200-baud BOOTSEL reboot work, which is what keeps reflashing button-free.
pico_enable_stdio_usb(h0urg1ass 1)
pico_enable_stdio_uart(h0urg1ass 0)

pico_add_extra_outputs(h0urg1ass)
