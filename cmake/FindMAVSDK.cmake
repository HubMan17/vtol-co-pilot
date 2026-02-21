# FindMAVSDK.cmake
# Finds the MAVSDK library (prebuilt from GitHub releases, vcpkg, or system)
#
# Sets:
#   MAVSDK_FOUND
#   MAVSDK::mavsdk target

if(NOT TARGET MAVSDK::mavsdk)
    # 1. Try CMake config mode (vcpkg, system install)
    find_package(MAVSDK CONFIG QUIET)

    if(NOT MAVSDK_FOUND)
        # 2. Try prebuilt in third_party/mavsdk/
        set(_MAVSDK_ROOT "${CMAKE_SOURCE_DIR}/third_party/mavsdk")

        if(EXISTS "${_MAVSDK_ROOT}/include/mavsdk/mavsdk.h")
            # Prefer Release, fall back to lib/ root
            if(EXISTS "${_MAVSDK_ROOT}/Release/lib/mavsdk.lib")
                set(_MAVSDK_LIB "${_MAVSDK_ROOT}/Release/lib/mavsdk.lib")
                set(_MAVSDK_DLL "${_MAVSDK_ROOT}/Release/bin/mavsdk.dll")
            elseif(EXISTS "${_MAVSDK_ROOT}/lib/mavsdk.lib")
                set(_MAVSDK_LIB "${_MAVSDK_ROOT}/lib/mavsdk.lib")
            endif()

            if(_MAVSDK_LIB)
                add_library(MAVSDK::mavsdk SHARED IMPORTED)
                set_target_properties(MAVSDK::mavsdk PROPERTIES
                    IMPORTED_IMPLIB "${_MAVSDK_LIB}"
                    INTERFACE_INCLUDE_DIRECTORIES "${_MAVSDK_ROOT}/include;${_MAVSDK_ROOT}/include/mavsdk"
                )
                if(_MAVSDK_DLL)
                    set_target_properties(MAVSDK::mavsdk PROPERTIES
                        IMPORTED_LOCATION "${_MAVSDK_DLL}"
                    )
                endif()
                set(MAVSDK_FOUND TRUE)
                message(STATUS "Found MAVSDK (prebuilt): ${_MAVSDK_LIB}")
            endif()
        endif()
    endif()

    if(NOT MAVSDK_FOUND)
        # 3. Fallback: pkg-config
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(MAVSDK IMPORTED_TARGET mavsdk)
            if(MAVSDK_FOUND)
                add_library(MAVSDK::mavsdk ALIAS PkgConfig::MAVSDK)
            endif()
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MAVSDK
    REQUIRED_VARS MAVSDK_FOUND
    FAIL_MESSAGE "MAVSDK not found. Place prebuilt in third_party/mavsdk/ or install via vcpkg."
)
