cmake_minimum_required(VERSION 3.16)

set(QUALITY_SRC
    quality/DownlinkBandwidthAllocator.cpp
    quality/GoogCcNetworkController.cpp
    quality/NackContext.cpp
    quality/NetworkController.cpp
    quality/NetworkQualityEstimator.cpp
    quality/NetworkStatsCollector.cpp
    quality/PacketHistory.cpp
    quality/PacketRecoveryController.cpp
    quality/RtcpBweBridge.cpp
    quality/SendSideController.cpp
    quality/TransportFeedbackAdapter.cpp
    quality/VideoRecoveryController.cpp
    quality/WeakNetController.cpp
    quality/ReceiveQualityMonitor.cpp
    quality/estimator/EwmaFilter.cpp
    quality/bwe/DelayTrendDetector.cpp
    quality/bwe/IncomingBitrateEstimator.cpp
    quality/bwe/LossBasedController.cpp
    quality/bwe/OUTrendDetect.cpp
    quality/bwe/RemoteBitrateController.cpp
    quality/bwe/TransportBweController.cpp
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

add_library(packetia_media_core STATIC
    core/EventBus.cpp
    core/MediaStream.cpp
    core/MediaStreamRegistry.cpp
    frame/EncodedFrameRouter.cpp
    frame/EncodedFrameDispatcher.cpp
    frame/EncodedFrameRouterBinding.cpp
)
target_include_directories(packetia_media_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/track
    ${CMAKE_CURRENT_SOURCE_DIR}/frame)
target_compile_features(packetia_media_core PUBLIC cxx_std_17)
target_link_libraries(packetia_media_core PUBLIC Threads::Threads packetia_media_types packetia_memory LOGGER)
target_link_libraries(rtsp PUBLIC packetia_media_core)

add_library(packetia_media_stats STATIC stats/MediaStatsCore.cpp stats/MetricsCalculator.cpp)
target_include_directories(packetia_media_stats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stats)
target_link_libraries(packetia_media_stats PUBLIC packetia_media_types Threads::Threads)

add_library(media STATIC
    frame/EncodedFrameRouter.h
    endpoint/SfuEndpointTypes.h
    endpoint/SfuEndpoint.h
    endpoint/SfuEndpoint.cpp
    endpoint/SfuEndpointTracks.cpp
    endpoint/DownlinkTrackManager.h
    endpoint/DownlinkTrackManager.cpp
    endpoint/ReceiveFeedbackHandler.h
    endpoint/ReceiveFeedbackHandler.cpp
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
    track/TrackPublicationInfo.h
    track/TrackPublication.h
    track/TrackPublicationFwd.h
    track/MediaTrackDescription.h
)

target_include_directories(media
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/frame
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
        packetia_media_core
        packetia_sdp_rtp_adapter
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
    add_executable(test_media_core
        tests/test_encoded_frame_router.cpp
        tests/test_encoded_frame_dispatcher.cpp
        tests/test_media_track_subscriptions.cpp)
    target_link_libraries(test_media_core PRIVATE packetia_media_core GTest::gtest_main)
    add_test(NAME media_core COMMAND test_media_core)
    set_tests_properties(media_core PROPERTIES TIMEOUT 15)
endif()
