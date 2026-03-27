cmake_minimum_required(VERSION 3.16)

add_library(media STATIC
    endpoint/MediaEndpoint.cpp
    endpoint/MediaSession.cpp
)

target_include_directories(media
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(media PUBLIC cxx_std_17)

target_link_libraries(media
    PUBLIC
        Common
        utils
        protocol
        network
        log
)