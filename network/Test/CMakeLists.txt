get_filename_component(PACKETIA_SDP_TEST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_executable(test_sdp_parser test_sdp_parser.cpp
    "${PACKETIA_SDP_TEST_ROOT}/src/sdp/Sdp.cpp"
    "${PACKETIA_SDP_TEST_ROOT}/src/sdp/SdpMode.cpp"
    "${PACKETIA_SDP_TEST_ROOT}/src/sdp/SdpUtil.cpp"
    "${PACKETIA_SDP_TEST_ROOT}/src/sdp/SdpWebRtc.cpp")
target_compile_features(test_sdp_parser PRIVATE cxx_std_17)
target_include_directories(test_sdp_parser PRIVATE
    "${PACKETIA_SDP_TEST_ROOT}/src/sdp")
add_test(NAME test_sdp_parser COMMAND test_sdp_parser)
set_tests_properties(test_sdp_parser PROPERTIES TIMEOUT 20)

add_executable(test_datagram_transport
    test_datagram_protocol_classifier.cpp
    test_rtmp_transport.cpp
    test_webrtc_transport.cpp
    test_network_lifecycle.cpp
)

target_link_libraries(test_datagram_transport
    PRIVATE
        "$<$<PLATFORM_ID:Linux>:-Wl,--start-group>"
        network
        WebRtc
        Rtmp
        utils
        LOGGER
        "$<$<PLATFORM_ID:Linux>:-Wl,--end-group>"
        GTest::gtest
        GTest::gtest_main
)

add_test(NAME test_datagram_transport COMMAND test_datagram_transport)
set_tests_properties(test_datagram_transport PROPERTIES TIMEOUT 30)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    add_subdirectory(Posix)
endif()

add_executable(test_webrtc_session test_webrtc_session.cpp)
target_link_libraries(test_webrtc_session PRIVATE WebRtc Interleaved)
add_test(NAME test_webrtc_session COMMAND test_webrtc_session)
set_tests_properties(test_webrtc_session PROPERTIES TIMEOUT 30)

add_executable(test_webrtc_sdp test_webrtc_sdp.cpp)
target_link_libraries(test_webrtc_sdp PRIVATE WebRtc)
add_test(NAME test_webrtc_sdp COMMAND test_webrtc_sdp)
set_tests_properties(test_webrtc_sdp PROPERTIES TIMEOUT 30)

add_executable(test_webrtc_codec test_webrtc_codec.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../../protocol/WebRtc/WebRtcCodec.cpp")
target_compile_features(test_webrtc_codec PRIVATE cxx_std_17)
target_include_directories(test_webrtc_codec PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../protocol/WebRtc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/sdp")
add_test(NAME test_webrtc_codec COMMAND test_webrtc_codec)
set_tests_properties(test_webrtc_codec PROPERTIES TIMEOUT 20)

if(PACKETIA_WITH_LIBWEBSOCKETS)
    add_subdirectory(../websocket/Test websocket)
endif()
