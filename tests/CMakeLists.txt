cmake_minimum_required(VERSION 3.16)
project(h0urg1ass_tests CXX)

# Host-side tests for the hardware-independent core. These build and run on a
# workstation with no board attached -- which is the point: the timer model is
# the one component whose correctness a user can check against a wall clock, so
# it is worth being able to test it in a fraction of a second.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(h0urg1ass_tests
    test_timer_model.cpp
)

target_include_directories(h0urg1ass_tests PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../firmware/src
    ${CMAKE_CURRENT_LIST_DIR}/../third_party/1bit-display/tests/doctest
)

if(NOT MSVC)
    target_compile_options(h0urg1ass_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME timer_model COMMAND h0urg1ass_tests)
