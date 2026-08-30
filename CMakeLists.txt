cmake_minimum_required(VERSION 3.22)
project(astrial)

include(CTest)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_EXAMPLE "Build example" ON)
option(ASTRIAL_IO_URING "Try enabling io_uring backend for Astrial library" ON)
option(ASTRIAL_BUILD_USB "Build the optional libusb bulk transport" OFF)

add_subdirectory(3rdparty)

add_library(astrial
        src/astrial.cpp
        src/astrial/Serial.cpp
        src/astrial/SerialBuilder.cpp
        src/astrial/Types.cpp
        src/astrial/ports.cpp
        src/astrial/detail.cpp
)

set(ASTRIAL_READ_BUFFER_SIZE "4096" CACHE STRING "The default size of astrial serial backend read buffer")
set(ASTRIAL_WRITE_BUFFER_SIZE "4096" CACHE STRING "The default size of astrial serial backend write buffer")

target_include_directories(astrial PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(astrial PUBLIC cxx_std_20)

if (WIN32)
    target_compile_options(astrial PRIVATE /utf-8)
endif ()

target_link_libraries(astrial PUBLIC tl-expected readerwriterqueue PRIVATE asio_standalone)

if (WIN32)
    target_link_libraries(astrial PRIVATE setupapi cfgmgr32)
endif ()

if (APPLE)
    find_library(ASTRIAL_IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(ASTRIAL_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(astrial PRIVATE
            ${ASTRIAL_IOKIT_FRAMEWORK}
            ${ASTRIAL_COREFOUNDATION_FRAMEWORK}
    )
endif ()

if (UNIX AND ASTRIAL_IO_URING)
    include(UseIoUring)
endif ()

target_compile_definitions(astrial PUBLIC ASTRIAL_READ_BUFFER_LENGTH=${ASTRIAL_READ_BUFFER_SIZE})
target_compile_definitions(astrial PUBLIC ASTRIAL_WRITE_BUFFER_LENGTH=${ASTRIAL_WRITE_BUFFER_SIZE})

if (ASTRIAL_BUILD_USB)
    find_package(LibUSB 1.0 REQUIRED)
    add_library(astrial_usb src/astrial/Usb.cpp)
    add_library(astrial::usb ALIAS astrial_usb)
    target_include_directories(astrial_usb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(astrial_usb PUBLIC cxx_std_20)
    target_compile_definitions(astrial_usb PUBLIC ASTRIAL_HAS_USB=1)
    target_link_libraries(astrial_usb PUBLIC tl-expected PRIVATE LibUSB::LibUSB)
    if (WIN32)
        target_compile_options(astrial_usb PRIVATE /utf-8)
    endif ()
endif ()

if (BUILD_EXAMPLE)
    add_subdirectory(examples)
endif ()

if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()
