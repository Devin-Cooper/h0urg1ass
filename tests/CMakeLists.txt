cmake_minimum_required(VERSION 3.16)
project(h0urg1ass_tests CXX)

# Host-side tests. These build and run on a workstation with no board attached,
# which is the point: the timer model and every face can be developed and
# regression-tested without hardware in the loop.
#
# Faces are checked against braille-art baselines. Braille is a lossless 2x4 bit
# packing, so a baseline IS the framebuffer re-spelled rather than a rendering of
# it -- which means a regression shows up as a readable diff.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(ONEBIT_ROOT ${CMAKE_CURRENT_LIST_DIR}/../third_party/1bit-display)
set(FW_SRC ${CMAKE_CURRENT_LIST_DIR}/../firmware/src)

file(GLOB ONEBIT_SOURCES "${ONEBIT_ROOT}/src/*/*.cpp")

add_library(onebit_host STATIC ${ONEBIT_SOURCES})
target_include_directories(onebit_host PUBLIC ${ONEBIT_ROOT}/include ${ONEBIT_ROOT})

# The faces, built for the host. Nothing under firmware/src/faces or
# firmware/src/timer may include a pico-sdk header -- that separation is what
# makes this build possible, and this target is what enforces it.
add_library(h0_settings STATIC
    ${FW_SRC}/settings/settings.cpp
    ${FW_SRC}/settings/rows.cpp
    ${FW_SRC}/settings/settings_codec.cpp
    ${FW_SRC}/settings/settings_store.cpp
    ${FW_SRC}/settings/theme.cpp
)
target_include_directories(h0_settings PUBLIC ${FW_SRC})
target_link_libraries(h0_settings PUBLIC onebit_host)

add_library(h0_power STATIC
    ${FW_SRC}/power/battery_model.cpp
    ${FW_SRC}/power/backlight_policy.cpp
)
target_include_directories(h0_power PUBLIC ${FW_SRC})
target_link_libraries(h0_power PUBLIC h0_settings)

add_library(h0_input STATIC
    ${FW_SRC}/input/orientation.cpp
    ${FW_SRC}/app/app.cpp
    ${FW_SRC}/input/drag_column.cpp
    ${FW_SRC}/app/settings_ui.cpp
)
target_include_directories(h0_input PUBLIC ${FW_SRC})
# settings_ui.cpp needs the rows table. h0_settings must not link back to
# h0_input, or the two libraries form a cycle.
target_link_libraries(h0_input PUBLIC h0_settings)

add_library(h0_faces STATIC
    ${FW_SRC}/faces/timer_face.cpp
    ${FW_SRC}/render/raster_ops.cpp
    ${FW_SRC}/faces/setting_face.cpp
    ${FW_SRC}/faces/settings_face.cpp
    ${FW_SRC}/sand/sand_sim.cpp
    ${FW_SRC}/sand/sand_render.cpp
    ${FW_SRC}/sand/sand_vessel.cpp
)
target_include_directories(h0_faces PUBLIC ${FW_SRC})
target_link_libraries(h0_faces PUBLIC onebit_host h0_input h0_power)

add_executable(h0urg1ass_tests
    test_timer_model.cpp
    test_faces.cpp
    test_orientation.cpp
    test_app.cpp
    test_drag_column.cpp
    test_sand.cpp
    test_settings.cpp
    test_settings_codec.cpp
    test_settings_store.cpp
    test_settings_ui.cpp
    test_battery_model.cpp
    test_backlight_policy.cpp
    golden.cpp
)
target_include_directories(h0urg1ass_tests PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${ONEBIT_ROOT}/tests/doctest
)
target_link_libraries(h0urg1ass_tests PRIVATE h0_faces h0_input h0_settings)
target_compile_definitions(h0urg1ass_tests PRIVATE
    GOLDEN_DIR="${CMAKE_CURRENT_LIST_DIR}/golden"
)

if(NOT MSVC)
    target_compile_options(h0urg1ass_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(h0_faces PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(h0_input PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(h0_settings PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(h0_power PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Renders faces to .pbm for eyeballing. Separate from the test binary so the
# tests stay fast and side-effect free.
add_executable(render_faces render_faces.cpp)
target_link_libraries(render_faces PRIVATE h0_faces)

# Throwaway prototype: falling-sand hourglass, for a go/no-go look comparison
# against the shaped fill. Deliberately not part of the test suite -- it renders
# images and prints measurements, and is expected to be deleted once the
# question it exists to answer has been answered.
add_executable(sand_proto proto/sand_ca.cpp)
target_link_libraries(sand_proto PRIVATE h0_faces onebit_host)
if(NOT MSVC)
    target_compile_options(sand_proto PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Throwaway measuring instrument: finds the constants -- hole width, fill depth,
# tick rate, attractor probability -- that let the falling-sand vessel hit real
# timer durations. Prints numbers, ships nothing, and is expected to be deleted
# once those constants are pinned.
add_executable(sand_tune proto/sand_tune.cpp)
target_link_libraries(sand_tune PRIVATE h0_faces)
if(NOT MSVC)
    target_compile_options(sand_tune PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME unit COMMAND h0urg1ass_tests)
