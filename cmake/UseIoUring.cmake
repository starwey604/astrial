if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Target Linux Kernel: ${CMAKE_SYSTEM_VERSION}")

    if (CMAKE_SYSTEM_VERSION VERSION_GREATER_EQUAL "5.15")
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(URING liburing)
        endif()
        if (NOT URING_FOUND)
            find_library(URING_LIBRARY uring)
        endif()

        if (URING_FOUND OR URING_LIBRARY)
            message(STATUS "Kernel version is >= 5.15 and liburing found, enabling io_uring support.")
            target_compile_definitions(asio_standalone INTERFACE
                    ASIO_HAS_IO_URING
                    ASIO_DISABLE_EPOLL
            )
            target_link_libraries(asio_standalone INTERFACE uring)
        else ()
            message(STATUS "Kernel version is >= 5.15 but liburing not found, falling back to epoll.")
        endif()
    else ()
        message(STATUS "Kernel version is < 5.15, falling back to epoll.")
    endif ()
endif ()