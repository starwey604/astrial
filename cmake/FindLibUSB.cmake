# SPDX-License-Identifier: MIT

# Prefer the exact active vcpkg triplet on Windows. A plain find_library()
# search can otherwise select debug/lib/libusb-1.0.lib for a Release build
# because both configurations use the same file name.
if(WIN32 AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    set(_LibUSB_vcpkg_root
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    find_path(LibUSB_INCLUDE_DIR
        NAMES libusb.h
        PATHS "${_LibUSB_vcpkg_root}/include"
        PATH_SUFFIXES libusb-1.0
        NO_DEFAULT_PATH)
    find_library(LibUSB_LIBRARY_RELEASE
        NAMES usb-1.0 libusb-1.0 libusb
        PATHS "${_LibUSB_vcpkg_root}/lib"
        NO_DEFAULT_PATH)
    find_library(LibUSB_LIBRARY_DEBUG
        NAMES usb-1.0 libusb-1.0 libusb
        PATHS "${_LibUSB_vcpkg_root}/debug/lib"
        NO_DEFAULT_PATH)
    find_file(LibUSB_RUNTIME_RELEASE
        NAMES libusb-1.0.dll usb-1.0.dll libusb.dll
        PATHS "${_LibUSB_vcpkg_root}/bin"
        NO_DEFAULT_PATH)
    find_file(LibUSB_RUNTIME_DEBUG
        NAMES libusb-1.0.dll usb-1.0.dll libusb.dll
        PATHS "${_LibUSB_vcpkg_root}/debug/bin"
        NO_DEFAULT_PATH)
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LIBUSB QUIET libusb-1.0>=1.0)
endif()

find_path(LibUSB_INCLUDE_DIR
    NAMES libusb.h
    HINTS ${PC_LIBUSB_INCLUDE_DIRS}
    PATH_SUFFIXES libusb-1.0)
find_library(LibUSB_LIBRARY_RELEASE
    NAMES usb-1.0 libusb-1.0 libusb
    HINTS ${PC_LIBUSB_LIBRARY_DIRS})

include(SelectLibraryConfigurations)
select_library_configurations(LibUSB)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibUSB
    REQUIRED_VARS LibUSB_LIBRARY LibUSB_INCLUDE_DIR
    VERSION_VAR PC_LIBUSB_VERSION)

if(LibUSB_FOUND AND NOT TARGET LibUSB::LibUSB)
    if(WIN32 AND (LibUSB_RUNTIME_RELEASE OR LibUSB_RUNTIME_DEBUG))
        add_library(LibUSB::LibUSB SHARED IMPORTED)
        set(_LibUSB_imported_configurations "")

        if(LibUSB_LIBRARY_RELEASE AND LibUSB_RUNTIME_RELEASE)
            list(APPEND _LibUSB_imported_configurations RELEASE)
            set_property(TARGET LibUSB::LibUSB PROPERTY
                IMPORTED_IMPLIB_RELEASE "${LibUSB_LIBRARY_RELEASE}")
            set_property(TARGET LibUSB::LibUSB PROPERTY
                IMPORTED_LOCATION_RELEASE "${LibUSB_RUNTIME_RELEASE}")
            set_property(TARGET LibUSB::LibUSB PROPERTY
                MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE)
            set_property(TARGET LibUSB::LibUSB PROPERTY
                MAP_IMPORTED_CONFIG_MINSIZEREL RELEASE)
        endif()

        if(LibUSB_LIBRARY_DEBUG AND LibUSB_RUNTIME_DEBUG)
            list(APPEND _LibUSB_imported_configurations DEBUG)
            set_property(TARGET LibUSB::LibUSB PROPERTY
                IMPORTED_IMPLIB_DEBUG "${LibUSB_LIBRARY_DEBUG}")
            set_property(TARGET LibUSB::LibUSB PROPERTY
                IMPORTED_LOCATION_DEBUG "${LibUSB_RUNTIME_DEBUG}")
        elseif(LibUSB_LIBRARY_RELEASE AND LibUSB_RUNTIME_RELEASE)
            set_property(TARGET LibUSB::LibUSB PROPERTY
                MAP_IMPORTED_CONFIG_DEBUG RELEASE)
        endif()

        set_property(TARGET LibUSB::LibUSB PROPERTY IMPORTED_CONFIGURATIONS
            "${_LibUSB_imported_configurations}")
        unset(_LibUSB_imported_configurations)
    else()
        add_library(LibUSB::LibUSB UNKNOWN IMPORTED)
        set_property(TARGET LibUSB::LibUSB PROPERTY
            IMPORTED_LOCATION "${LibUSB_LIBRARY}")
    endif()

    set_property(TARGET LibUSB::LibUSB PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${LibUSB_INCLUDE_DIR}")
endif()

mark_as_advanced(
    LibUSB_INCLUDE_DIR
    LibUSB_LIBRARY
    LibUSB_LIBRARY_DEBUG
    LibUSB_LIBRARY_RELEASE
    LibUSB_RUNTIME_DEBUG
    LibUSB_RUNTIME_RELEASE)

unset(_LibUSB_vcpkg_root)
