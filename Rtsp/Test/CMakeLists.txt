add_executable(test_rtp_input
    test_rtp_input.cpp
    test_sdp_track_binding.cpp
    test_rtcp_chain.cpp
    test_audio_depacketizer.cpp
    test_sfu_endpoint.cpp
    test_webrtc_media_adapter.cpp
)

target_link_libraries(test_rtp_input
  PRIVATE
    rtsp           
    rtp
    Rtcp           
    network
    utils
    LOGGER
    COMMON
    media
    ROOM_LIB
    Interleaved
    pthread
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(test_rtp_input PRIVATE
    ${CMAKE_SOURCE_DIR}/Rtsp
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/Common
)

add_test(NAME test_rtp_input COMMAND test_rtp_input)
set_tests_properties(test_rtp_input PROPERTIES TIMEOUT 30)

add_executable(test_udp_binding test_udp_binding.cpp)
target_link_libraries(test_udp_binding PRIVATE
    "$<$<PLATFORM_ID:Linux>:-Wl,--start-group>"
    media rtsp rtp Rtcp network utils LOGGER COMMON ROOM_LIB ICE Interleaved
    "$<$<PLATFORM_ID:Linux>:-Wl,--end-group>"
    GTest::gtest GTest::gtest_main OpenSSL::Crypto pthread)
add_test(NAME test_udp_binding COMMAND test_udp_binding)
set_tests_properties(test_udp_binding PROPERTIES TIMEOUT 30)
