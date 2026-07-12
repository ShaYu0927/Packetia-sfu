cmake_minimum_required(VERSION 3.16)

file(GLOB QUALITY_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/quality/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/quality/bwe/*.cpp"
)

add_library(media STATIC
    core/EventBus.cpp
    endpoint/MediaEndpoint.cpp
    endpoint/MediaSession.cpp
    track/TrackeInfo.h
    track/StreamContext.cpp
    track/StreamContext.h
    ${QUALITY_SRC}
)

target_include_directories(media
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/MediaFrame
    ${CMAKE_CURRENT_SOURCE_DIR}/track
    ${CMAKE_CURRENT_SOURCE_DIR}/quality
    ${CMAKE_CURRENT_SOURCE_DIR}/quality/bwe
    ${CMAKE_CURRENT_SOURCE_DIR}/endpoint
)

target_compile_features(media PUBLIC cxx_std_17)

target_link_libraries(media
    PUBLIC
        rtsp
        rtp
        network
        utils
        Rtcp
        LOGGER
        ROOM_LIB
        COMMON
        ICE
        Interleaved
)
