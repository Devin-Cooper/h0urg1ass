cmake_minimum_required(VERSION 3.16)

# This MUST precede pico_sdk_import.cmake. Left unset, pico-sdk silently
# defaults to PICO_BOARD=pico (RP2040) and produces an ARMv6-M binary that will
# not run on this board -- and the failure looks like a dead board, not a build
# mistake. The vendor's own basic demo has exactly this bug.
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")

# pico2.h defaults PICO_FLASH_SIZE_BYTES to 4 MB; this board carries a discrete
# 16 MB W25Q128JVSIQ.
#
# This MUST be the CMake variable, not a compile definition, AND it MUST be set
# before pico_sdk_import.cmake is included below -- not merely before
# pico_sdk_init(). pico_sdk_import.cmake pulls in pico_pre_load_platform, which
# parses the board header (pico2.h's `pico_board_cmake_set_default(
# PICO_FLASH_SIZE_BYTES, ...)`) immediately, before project() or pico_sdk_init()
# ever run. If PICO_FLASH_SIZE_BYTES isn't already defined at that point, the
# board file's 4 MB default is taken silently and never registered as an
# override, so pico_base_headers never gets a matching -D and the code falls
# back to pico2.h's own `#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)`. The
# LINKER region reads PICO_FLASH_SIZE_BYTES later, inside pico_standard_link,
# so it looks correct either way -- only the compiled code silently keeps
# believing 4 MB. Setting the variable here, before the include, is what makes
# both the linker region and the in-code macro agree at 16 MB.
set(PICO_FLASH_SIZE_BYTES 16777216)

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
    src/board/qmi8658.cpp
    src/board/buzzer.cpp
    src/board/cst816.cpp
    src/board/flash_store.cpp
    src/faces/timer_face.cpp
    src/render/raster_ops.cpp
    src/faces/setting_face.cpp
    src/input/orientation.cpp
    src/app/app.cpp
    src/input/drag_column.cpp
    src/sand/sand_sim.cpp
    src/sand/sand_render.cpp
    src/sand/sand_vessel.cpp
    src/settings/settings.cpp
    src/settings/rows.cpp
    src/settings/settings_codec.cpp
    src/settings/settings_store.cpp
    src/settings/theme.cpp
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
    pico_flash
    hardware_flash
)

# Console over native USB. GPIO0/1 are broken out to test points, but USB-C is
# the only connection during bring-up -- and pico_stdio_usb is what makes the
# 1200-baud BOOTSEL reboot work, which is what keeps reflashing button-free.
pico_enable_stdio_usb(h0urg1ass 1)
pico_enable_stdio_uart(h0urg1ass 0)

pico_add_extra_outputs(h0urg1ass)
