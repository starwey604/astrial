# See https://discourse.cmake.org/t/how-to-fix-cmake-minimum-required-deprecation-warning/2487/2
# for more on setting the minimum required version.

cmake_minimum_required(VERSION 3.14...3.31)

add_library(readerwriterqueue INTERFACE)

target_include_directories(readerwriterqueue INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
