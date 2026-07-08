add_library(asio_standalone INTERFACE)

target_include_directories(asio_standalone INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(asio_standalone INTERFACE ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(asio_standalone INTERFACE Threads::Threads)
endif()