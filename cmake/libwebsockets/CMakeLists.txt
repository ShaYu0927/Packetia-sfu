# Keep libwebsockets discovery independent of the media stack so the signaling
# server can be built and tested without FFmpeg or the network target.
if(TARGET packetia_libwebsockets)
    return()
endif()

# OpenSSL is a public header dependency in OpenSSL-enabled lws builds. Some
# installations (including Homebrew) omit it from their public pkg-config flags.
function(packetia_lws_link_openssl target)
    foreach(include_dir IN LISTS ARGN)
        if(EXISTS "${include_dir}/lws_config.h")
            file(STRINGS "${include_dir}/lws_config.h" openssl_enabled
                REGEX "^#[ \t]*define[ \t]+LWS_OPENSSL_SUPPORT([ \t]|$)")
            if(openssl_enabled)
                find_package(OpenSSL REQUIRED)
                target_link_libraries(${target} INTERFACE OpenSSL::SSL)
                return()
            endif()
        endif()
    endforeach()
endfunction()

set(PACKETIA_LIBWEBSOCKETS_ROOT "" CACHE PATH "Root of a libwebsockets installation")
if(NOT PACKETIA_LIBWEBSOCKETS_ROOT AND DEFINED ENV{LIBWEBSOCKETS_ROOT})
    set(PACKETIA_LIBWEBSOCKETS_ROOT "$ENV{LIBWEBSOCKETS_ROOT}")
endif()
if(NOT "${_PACKETIA_LWS_PREVIOUS_ROOT}" STREQUAL "${PACKETIA_LIBWEBSOCKETS_ROOT}")
    unset(PACKETIA_LWS_INCLUDE_DIR CACHE)
    unset(PACKETIA_LWS_LIBRARY CACHE)
endif()
set(_PACKETIA_LWS_PREVIOUS_ROOT "${PACKETIA_LIBWEBSOCKETS_ROOT}" CACHE INTERNAL "Last libwebsockets root used for discovery")

if(PACKETIA_LIBWEBSOCKETS_ROOT)
    find_path(PACKETIA_LWS_INCLUDE_DIR libwebsockets.h
        PATHS "${PACKETIA_LIBWEBSOCKETS_ROOT}/include" NO_DEFAULT_PATH)
    find_library(PACKETIA_LWS_LIBRARY NAMES websockets libwebsockets
        PATHS "${PACKETIA_LIBWEBSOCKETS_ROOT}/lib" "${PACKETIA_LIBWEBSOCKETS_ROOT}/lib64"
        NO_DEFAULT_PATH)
    if(NOT PACKETIA_LWS_INCLUDE_DIR OR NOT PACKETIA_LWS_LIBRARY)
        message(FATAL_ERROR "PACKETIA_LIBWEBSOCKETS_ROOT must contain include/libwebsockets.h and a libwebsockets library")
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PACKETIA_LWS QUIET IMPORTED_TARGET libwebsockets)
    endif()
    if(TARGET PkgConfig::PACKETIA_LWS)
        add_library(packetia_libwebsockets INTERFACE)
        target_link_libraries(packetia_libwebsockets INTERFACE PkgConfig::PACKETIA_LWS)
        packetia_lws_link_openssl(packetia_libwebsockets ${PACKETIA_LWS_INCLUDE_DIRS})
        return()
    endif()

    find_path(PACKETIA_LWS_INCLUDE_DIR libwebsockets.h)
    find_library(PACKETIA_LWS_LIBRARY NAMES websockets libwebsockets)
    if(NOT PACKETIA_LWS_INCLUDE_DIR OR NOT PACKETIA_LWS_LIBRARY)
        if(APPLE)
            message(FATAL_ERROR
                "Install native macOS libwebsockets (brew install libwebsockets) or set PACKETIA_LIBWEBSOCKETS_ROOT. The bundled third/libwebsocket libraries target Linux.")
        endif()
        get_filename_component(_packetia_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
        set(_packetia_lws_bundled "${_packetia_root}/third/libwebsocket")
        # A header/library pair must come from the same installation.
        unset(PACKETIA_LWS_INCLUDE_DIR CACHE)
        unset(PACKETIA_LWS_LIBRARY CACHE)
        find_path(PACKETIA_LWS_INCLUDE_DIR libwebsockets.h
            PATHS "${_packetia_lws_bundled}/include" NO_DEFAULT_PATH)
        find_library(PACKETIA_LWS_LIBRARY NAMES websockets libwebsockets
            PATHS "${_packetia_lws_bundled}/lib" "${_packetia_lws_bundled}/lib64"
            NO_DEFAULT_PATH)
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PacketiaLibwebsockets
    REQUIRED_VARS PACKETIA_LWS_INCLUDE_DIR PACKETIA_LWS_LIBRARY)

add_library(packetia_libwebsockets UNKNOWN IMPORTED GLOBAL)
set_target_properties(packetia_libwebsockets PROPERTIES
    IMPORTED_LOCATION "${PACKETIA_LWS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PACKETIA_LWS_INCLUDE_DIR}")
packetia_lws_link_openssl(packetia_libwebsockets "${PACKETIA_LWS_INCLUDE_DIR}")
# Extra dependencies are useful for custom static libwebsockets builds. Shared
# installations and pkg-config generally carry their own dependency metadata.
set(PACKETIA_LIBWEBSOCKETS_EXTRA_LIBRARIES "" CACHE STRING "Additional libraries required by a custom static libwebsockets build")
if(PACKETIA_LIBWEBSOCKETS_EXTRA_LIBRARIES)
    set_property(TARGET packetia_libwebsockets APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "${PACKETIA_LIBWEBSOCKETS_EXTRA_LIBRARIES}")
endif()
mark_as_advanced(PACKETIA_LWS_INCLUDE_DIR PACKETIA_LWS_LIBRARY)
