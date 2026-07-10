cmake_minimum_required(VERSION 3.22)
project(astrial)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_EXAMPLE "Build example" ON)

add_subdirectory(3rdparty)

add_library(astrial
        src/astrail.cpp
        src/astrial/Serial.cpp
        src/astrial/SerialBuilder.cpp
        src/astrial/Types.cpp
        src/astrial/ports.cpp
)

set(ASTRIAL_BUFFER_SIZE "4096" CACHE STRING "The default size of astrial serial backend buffer")

target_include_directories(astrial PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(astrial PUBLIC cxx_std_20)

if (WIN32)
    target_compile_options(astrial PRIVATE /utf-8)
endif ()

target_link_libraries(astrial PUBLIC tl-expected PRIVATE asio_standalone)

if (WIN32)
    target_link_libraries(astrial PRIVATE setupapi cfgmgr32)
endif ()

if (UNIX)
    include(UseIoUring)
endif ()

target_compile_definitions(astrial PUBLIC ASTRIAL_BUFFER_LENGTH=${ASTRIAL_BUFFER_SIZE})

if (BUILD_EXAMPLE)
    add_subdirectory(examples)
endif ()