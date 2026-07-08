cmake_minimum_required(VERSION 3.22)
project(astrial)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_subdirectory(3rdparty)

add_library(astrial ${CMAKE_CURRENT_SOURCE_DIR}/src/astrail.cpp)

target_include_directories(astrial PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(astrial PUBLIC cxx_std_20)

target_link_libraries(astrial PRIVATE asio_standalone)

add_subdirectory(examples)