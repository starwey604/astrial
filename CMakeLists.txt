cmake_minimum_required(VERSION 3.22)
project(astrial)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    set(_ASTRIAL_BUILD_TESTS_DEFAULT "${BUILD_TESTING}")
else()
    set(_ASTRIAL_BUILD_TESTS_DEFAULT OFF)
endif()

set(_ASTRIAL_BUILD_EXAMPLES_DEFAULT "${PROJECT_IS_TOP_LEVEL}")
if(DEFINED BUILD_EXAMPLE AND NOT DEFINED ASTRIAL_BUILD_EXAMPLES)
    set(_ASTRIAL_BUILD_EXAMPLES_DEFAULT "${BUILD_EXAMPLE}")
endif()

option(ASTRIAL_BUILD_EXAMPLES "Build Astrial examples"
    "${_ASTRIAL_BUILD_EXAMPLES_DEFAULT}")
option(ASTRIAL_BUILD_TESTS "Build Astrial tests"
    "${_ASTRIAL_BUILD_TESTS_DEFAULT}")
option(ASTRIAL_ENABLE_IPO "Enable IPO for optimized Astrial configurations" ON)
option(ASTRIAL_IO_URING "Try enabling io_uring backend for Astrial library" ON)
option(ASTRIAL_BUILD_USB "Build the optional libusb bulk transport" OFF)

if(ASTRIAL_BUILD_TESTS)
    enable_testing()
endif()

set(_ASTRIAL_IPO_SUPPORTED OFF)
if(ASTRIAL_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ASTRIAL_IPO_SUPPORTED
        OUTPUT _ASTRIAL_IPO_ERROR LANGUAGES CXX)
    if(NOT _ASTRIAL_IPO_SUPPORTED)
        message(WARNING "Astrial IPO is unavailable: ${_ASTRIAL_IPO_ERROR}")
    endif()
endif()

function(_astrial_configure_ipo target)
    # Never inherit a parent project's global IPO setting into Debug builds.
    set_property(TARGET "${target}" PROPERTY
        INTERPROCEDURAL_OPTIMIZATION OFF)
    if(_ASTRIAL_IPO_SUPPORTED)
        set_property(TARGET "${target}" PROPERTY
            INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set_property(TARGET "${target}" PROPERTY
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        set_property(TARGET "${target}" PROPERTY
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
    endif()
endfunction()

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
_astrial_configure_ipo(astrial)

if (WIN32)
    target_compile_options(astrial PRIVATE /utf-8)
    target_compile_definitions(astrial PRIVATE
            _WIN32_WINNT=0x0601
            WIN32_LEAN_AND_MEAN
            NOMINMAX)
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
    _astrial_configure_ipo(astrial_usb)
    target_compile_definitions(astrial_usb PUBLIC ASTRIAL_HAS_USB=1)
    target_link_libraries(astrial_usb PUBLIC tl-expected PRIVATE LibUSB::LibUSB)
    if (WIN32)
        target_compile_options(astrial_usb PRIVATE /utf-8)
        target_compile_definitions(astrial_usb PRIVATE
                _WIN32_WINNT=0x0601
                WIN32_LEAN_AND_MEAN
                NOMINMAX)
    endif ()
endif ()

if (ASTRIAL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

if (ASTRIAL_BUILD_TESTS)
    add_subdirectory(tests)
endif ()

unset(_ASTRIAL_BUILD_EXAMPLES_DEFAULT)
unset(_ASTRIAL_BUILD_TESTS_DEFAULT)
unset(_ASTRIAL_IPO_ERROR)
unset(_ASTRIAL_IPO_SUPPORTED)
