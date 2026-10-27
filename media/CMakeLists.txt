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

add_library(packetia_media_core STATIC core/MediaStream.cpp core/MediaStreamRegistry.cpp)
target_include_directories(packetia_media_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/track
    ${CMAKE_CURRENT_SOURCE_DIR}/MediaFrame)
target_compile_features(packetia_media_core PUBLIC cxx_std_17)
target_link_libraries(packetia_media_core PUBLIC Threads::Threads packetia_media_types)
target_link_libraries(rtsp PUBLIC packetia_media_core)

add_library(packetia_media_rtp STATIC rtp/RtpTrackBinding.cpp)
target_include_directories(packetia_media_rtp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/rtp
    ${CMAKE_CURRENT_SOURCE_DIR}/track)
target_compile_features(packetia_media_rtp PUBLIC cxx_std_17)
target_link_libraries(packetia_media_rtp PUBLIC packetia_media_types)

add_library(packetia_sdp_media_adapter STATIC rtp/SdpTrackBinding.cpp)
target_include_directories(packetia_sdp_media_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/rtp
    ${CMAKE_SOURCE_DIR}/src/sdp)
target_link_libraries(packetia_sdp_media_adapter PUBLIC packetia_media_rtp Interleaved LOGGER)
target_link_libraries(rtp PUBLIC packetia_sdp_media_adapter)

add_library(packetia_media_stats STATIC stats/MediaStatsCore.cpp stats/MetricsCalculator.cpp)
target_include_directories(packetia_media_stats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stats)
target_link_libraries(packetia_media_stats PUBLIC packetia_media_types Threads::Threads)

add_library(media STATIC
    core/EventBus.cpp
    core/EncodedFrameRouter.h
    core/EncodedFrameRouter.cpp
    endpoint/SfuEndpoint.h
    endpoint/SfuEndpoint.cpp
    endpoint/SfuEndpointTracks.cpp
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
    track/TrackMetadata.h
    track/MediaTrack.h
    track/MediaTrackFwd.h
    track/MediaStreamTrack.h
)

target_include_directories(media
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/MediaFrame
    ${CMAKE_CURRENT_SOURCE_DIR}/track
    ${CMAKE_CURRENT_SOURCE_DIR}/rtp
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
        packetia_media_core
        packetia_sdp_media_adapter
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

if(BUILD_TESTING)
    add_executable(test_media_parameters test/test_media_parameters.cpp test/test_event_bus.cpp)
    target_link_libraries(test_media_parameters PRIVATE packetia_media_core packetia_media_rtp
        packetia_media_stats GTest::gtest_main)
    add_test(NAME media_parameters COMMAND test_media_parameters)
    set_tests_properties(media_parameters PROPERTIES TIMEOUT 10)
endif()
