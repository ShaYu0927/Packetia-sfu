cmake_minimum_required(VERSION 3.16)

# Keep the lifecycle controller independent of concrete network/media services.
add_library(server STATIC
    ServerLauncher.cpp
    ServerConfig.cpp
)

target_include_directories(server
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(server PUBLIC cxx_std_17)
target_link_libraries(server PUBLIC LOGGER)
