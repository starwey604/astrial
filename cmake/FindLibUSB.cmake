# SPDX-License-Identifier: MIT

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LIBUSB QUIET libusb-1.0>=1.0)
endif()

find_path(LibUSB_INCLUDE_DIR
    NAMES libusb.h
    HINTS ${PC_LIBUSB_INCLUDE_DIRS}
    PATH_SUFFIXES libusb-1.0)
find_library(LibUSB_LIBRARY
    NAMES usb-1.0 libusb-1.0 libusb
    HINTS ${PC_LIBUSB_LIBRARY_DIRS})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibUSB
    REQUIRED_VARS LibUSB_LIBRARY LibUSB_INCLUDE_DIR
    VERSION_VAR PC_LIBUSB_VERSION)

if(LibUSB_FOUND AND NOT TARGET LibUSB::LibUSB)
    add_library(LibUSB::LibUSB UNKNOWN IMPORTED)
    set_target_properties(LibUSB::LibUSB PROPERTIES
        IMPORTED_LOCATION "${LibUSB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LibUSB_INCLUDE_DIR}")
endif()

mark_as_advanced(LibUSB_INCLUDE_DIR LibUSB_LIBRARY)
