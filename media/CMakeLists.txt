cmake_minimum_required(VERSION 3.16)

file(GLOB QUALITY_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/quality/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/quality/bwe/*.cpp"
)

# Both RTP senders and transports use the controller. Keep it below media so
# sender-only users do not acquire a circular dependency on the endpoint layer.
add_library(media_quality STATIC ${QUALITY_SRC})
target_include_directories(media_quality PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/quality
    ${CMAKE_CURRENT_SOURCE_DIR}/quality/bwe
)
target_compile_features(media_quality PUBLIC cxx_std_17)
target_link_libraries(media_quality PUBLIC Rtcp network utils LOGGER)
target_link_libraries(rtp PUBLIC media_quality)

add_library(media STATIC
    core/EventBus.cpp
    core/EncodedFrameRouter.h
    core/EncodedFrameRouter.cpp
    endpoint/MediaEndpoint.cpp
    endpoint/SfuEndpointTracks.cpp
    endpoint/MediaSession.cpp
    transport/IMediaPacketSink.h
    transport/MediaTransportPacket.h
    transport/MediaEndpointIngress.h
    transport/MediaEndpointIngress.cpp
    transport/WebRtcMediaTransport.cpp
    transport/MediaTransportBase.h
    transport/MediaTransportBase.cpp
    transport/RtspInterleavedTransport.h
    transport/RtspInterleavedTransport.cpp
    transport/UdpMediaTransport.h
    transport/UdpMediaTransport.cpp
    track/TrackeInfo.h
    track/StreamContext.cpp
    track/StreamContext.h
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
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/transport
)

target_compile_features(media PUBLIC cxx_std_17)
target_link_libraries(ROOM_LIB PUBLIC media)

target_link_libraries(media
    PUBLIC
        media_quality
        rtsp
        rtp
        network
        utils
        Rtcp
        LOGGER
        COMMON
        ICE
        Interleaved
        WebRtc
)
