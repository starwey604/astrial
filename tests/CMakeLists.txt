add_executable(astrial_list_ports_smoke list_ports_smoke.cpp)
target_link_libraries(astrial_list_ports_smoke PRIVATE astrial)
add_test(NAME astrial_list_ports_smoke COMMAND astrial_list_ports_smoke)
set_tests_properties(astrial_list_ports_smoke PROPERTIES TIMEOUT 15)

if (TARGET astrial_usb)
    add_executable(astrial_usb_smoke usb_smoke.cpp)
    target_link_libraries(astrial_usb_smoke PRIVATE astrial::usb)
    add_test(NAME astrial_usb_smoke COMMAND astrial_usb_smoke)
    set_tests_properties(astrial_usb_smoke PROPERTIES TIMEOUT 15)

    if (ASTRIAL_BUILD_USB_HIL)
        add_executable(astrial_usb_lifecycle_hil usb_lifecycle_hil.cpp)
        target_link_libraries(astrial_usb_lifecycle_hil PRIVATE astrial::usb)
        target_compile_features(astrial_usb_lifecycle_hil PRIVATE cxx_std_20)
        foreach (mode latency throughput)
            add_executable(astrial_usb_${mode}_hil usb_perf_hil.cpp)
            target_link_libraries(astrial_usb_${mode}_hil PRIVATE astrial::usb)
            target_compile_features(astrial_usb_${mode}_hil PRIVATE cxx_std_20)
            if (mode STREQUAL "latency")
                target_compile_definitions(astrial_usb_${mode}_hil PRIVATE ASTRIAL_USB_LATENCY_TEST=1)
            endif ()
        endforeach ()
        add_executable(astrial_usb_latency_raw_hil usb_latency_raw_hil.cpp)
        target_link_libraries(astrial_usb_latency_raw_hil PRIVATE LibUSB::LibUSB)
        target_compile_features(astrial_usb_latency_raw_hil PRIVATE cxx_std_20)
        if (MSVC)
            target_compile_options(astrial_usb_lifecycle_hil PRIVATE /W4)
            target_compile_options(astrial_usb_latency_hil PRIVATE /W4)
            target_compile_options(astrial_usb_throughput_hil PRIVATE /W4)
            target_compile_options(astrial_usb_latency_raw_hil PRIVATE /W4)
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(astrial_usb_lifecycle_hil PRIVATE
                -Wall -Wextra -Wpedantic)
            target_compile_options(astrial_usb_latency_hil PRIVATE
                -Wall -Wextra -Wpedantic)
            target_compile_options(astrial_usb_throughput_hil PRIVATE
                -Wall -Wextra -Wpedantic)
            target_compile_options(astrial_usb_latency_raw_hil PRIVATE
                -Wall -Wextra -Wpedantic)
        endif ()
    endif ()
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    add_executable(astrial_virtual_serial_tests virtual_serial.cpp)
    target_link_libraries(astrial_virtual_serial_tests PRIVATE astrial)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(astrial_virtual_serial_tests PRIVATE util)
    endif ()
    add_test(NAME astrial_virtual_serial COMMAND astrial_virtual_serial_tests)
    set_tests_properties(astrial_virtual_serial PROPERTIES TIMEOUT 15)
endif ()

# Hardware-in-the-loop tests need a physical fixture. They are built and
# registered only when ASTRIAL_BUILD_HIL_TESTS=ON, and they exit with 77 (test
# skipped) whenever the required environment/fixture is missing.
if (ASTRIAL_BUILD_HIL_TESTS)
    add_executable(astrial_serial_hil serial_hil.cpp)
    target_link_libraries(astrial_serial_hil PRIVATE astrial)
    target_compile_features(astrial_serial_hil PRIVATE cxx_std_20)
    if (MSVC)
        target_compile_options(astrial_serial_hil PRIVATE /W4)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(astrial_serial_hil PRIVATE
            -Wall -Wextra -Wpedantic)
    endif ()
    add_test(NAME astrial_serial_hil COMMAND astrial_serial_hil)
    set_tests_properties(astrial_serial_hil PROPERTIES
        TIMEOUT 60
        SKIP_RETURN_CODE 77)
endif ()
