include_guard(GLOBAL)

set(PACKETIA_GRPC_PROVIDER "source" CACHE STRING "gRPC provider: source or package")
set_property(CACHE PACKETIA_GRPC_PROVIDER PROPERTY STRINGS source package)
set(PACKETIA_GRPC_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third/grpc" CACHE PATH "gRPC source checkout")
set(PACKETIA_GRPC_ROOT "" CACHE PATH "Installed gRPC prefix for the package provider")

# Keep upstream build settings local; in particular do not disable Packetia tests
# or change how other Packetia dependencies are linked.
function(packetia_configure_grpc)
    if(PACKETIA_GRPC_PROVIDER STREQUAL "source")
        if(CMAKE_VERSION VERSION_LESS 3.22)
            message(FATAL_ERROR "Building gRPC v1.85.0 from source requires CMake >= 3.22")
        endif()
        if(PACKETIA_GRPC_ROOT)
            message(FATAL_ERROR "PACKETIA_GRPC_ROOT requires -DPACKETIA_GRPC_PROVIDER=package")
        endif()
        foreach(file
            CMakeLists.txt
            third_party/abseil-cpp/CMakeLists.txt
            third_party/cares/cares/CMakeLists.txt
            third_party/protobuf/CMakeLists.txt
            third_party/protobuf/third_party/utf8_range/CMakeLists.txt
            third_party/re2/CMakeLists.txt
            third_party/zlib/CMakeLists.txt
            third_party/grpc-proto/grpc/health/v1/health.proto)
            if(NOT EXISTS "${PACKETIA_GRPC_SOURCE_DIR}/${file}")
                message(FATAL_ERROR
                    "Incomplete gRPC source: missing ${PACKETIA_GRPC_SOURCE_DIR}/${file}. "
                    "Restore the vendored gRPC v1.85.0 sources; see third/grpc/PACKETIA.md. "
                    "Alternatively use -DPACKETIA_GRPC_PROVIDER=package with PACKETIA_GRPC_ROOT.")
            endif()
        endforeach()

        # Honor scoped normal variables even in dependencies with older policies.
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
        set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
        set(BUILD_SHARED_LIBS OFF)
        set(gRPC_INSTALL OFF)
        set(gRPC_BUILD_TESTS OFF)
        set(gRPC_BUILD_CODEGEN ON)
        set(gRPC_DOWNLOAD_ARCHIVES OFF)
        set(gRPC_BUILD_GRPC_CPP_PLUGIN ON)
        foreach(language CSHARP NODE OBJECTIVE_C PHP PYTHON RUBY)
            set(gRPC_BUILD_GRPC_${language}_PLUGIN OFF)
        endforeach()
        # Packetia already links OpenSSL. Use the same implementation in gRPC.
        set(gRPC_SSL_PROVIDER package)
        foreach(dependency ABSL CARES PROTOBUF RE2 ZLIB)
            set(gRPC_${dependency}_PROVIDER module)
        endforeach()
        set(ABSL_BUILD_TESTING OFF)
        set(ABSL_ENABLE_INSTALL OFF)
        set(CARES_STATIC ON)
        set(CARES_SHARED OFF)
        set(CARES_INSTALL OFF)
        set(protobuf_BUILD_TESTS OFF)
        set(protobuf_BUILD_SHARED_LIBS OFF)
        set(protobuf_BUILD_PROTOC_BINARIES ON)
        set(protobuf_INSTALL OFF)
        set(protobuf_LOCAL_DEPENDENCIES_ONLY ON)
        set(RE2_BUILD_TESTING OFF)
        set(ZLIB_BUILD_EXAMPLES OFF)
        # zlib's configure step renames zconf.h in its source tree. Configure a
        # build-local copy so vendored sources stay intact (including read-only checkouts).
        set(ZLIB_ROOT_DIR "${CMAKE_BINARY_DIR}/third/grpc-zlib-src")
        file(COPY "${PACKETIA_GRPC_SOURCE_DIR}/third_party/zlib/" DESTINATION "${ZLIB_ROOT_DIR}")
        set(RENAME_ZCONF ON)

        add_subdirectory("${PACKETIA_GRPC_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/third/grpc" EXCLUDE_FROM_ALL)
        add_library(gRPC::grpc++ ALIAS grpc++)
        add_executable(gRPC::grpc_cpp_plugin ALIAS grpc_cpp_plugin)
        message(STATUS "Packetia gRPC: source (${PACKETIA_GRPC_SOURCE_DIR}), system OpenSSL")
    elseif(PACKETIA_GRPC_PROVIDER STREQUAL "package")
        # Drop cached package paths when switching prefixes in a build directory.
        if(NOT "${_PACKETIA_GRPC_PREVIOUS_ROOT}" STREQUAL "${PACKETIA_GRPC_ROOT}")
            foreach(package gRPC Protobuf absl c-ares re2 utf8_range)
                unset(${package}_DIR CACHE)
            endforeach()
        endif()
        set(_PACKETIA_GRPC_PREVIOUS_ROOT "${PACKETIA_GRPC_ROOT}" CACHE INTERNAL "Previous gRPC prefix")
        if(PACKETIA_GRPC_ROOT)
            list(PREPEND CMAKE_PREFIX_PATH "${PACKETIA_GRPC_ROOT}")
            find_package(Protobuf CONFIG REQUIRED PATHS "${PACKETIA_GRPC_ROOT}" NO_DEFAULT_PATH)
            find_package(gRPC CONFIG REQUIRED PATHS "${PACKETIA_GRPC_ROOT}" NO_DEFAULT_PATH)
        else()
            find_package(Protobuf CONFIG REQUIRED)
            find_package(gRPC CONFIG REQUIRED)
        endif()
        message(STATUS "Packetia gRPC: package ${gRPC_VERSION} (${gRPC_DIR})")
    else()
        message(FATAL_ERROR "PACKETIA_GRPC_PROVIDER must be source or package")
    endif()

    foreach(required gRPC::grpc++ gRPC::grpc_cpp_plugin protobuf::libprotobuf protobuf::protoc)
        if(NOT TARGET ${required})
            message(FATAL_ERROR "gRPC installation is missing target ${required}; install the C++ libraries and code generation tools together")
        endif()
    endforeach()
    add_library(packetia_grpc INTERFACE)
    target_compile_features(packetia_grpc INTERFACE cxx_std_17)
    target_link_libraries(packetia_grpc INTERFACE gRPC::grpc++ protobuf::libprotobuf)
endfunction()

packetia_configure_grpc()
