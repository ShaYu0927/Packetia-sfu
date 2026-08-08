cmake_minimum_required(VERSION 3.16)

# The executable entry point is defined once by the root Packetia target.
# This target only publishes server-side headers to potential consumers.
add_library(server INTERFACE)

target_include_directories(server
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(server INTERFACE cxx_std_17)
