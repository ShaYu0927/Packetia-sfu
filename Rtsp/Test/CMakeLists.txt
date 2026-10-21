add_executable(test_rtp_input
    test_rtp_input.cpp
    test_sdp_track_binding.cpp
    test_rtcp_chain.cpp
    test_audio_depacketizer.cpp
    test_video_depacketizer.cpp
    test_sfu_endpoint.cpp
    test_participant.cpp
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

add_executable(test_rtp_reorder_queue test_rtp_reorder_queue.cpp)
target_include_directories(test_rtp_reorder_queue PRIVATE ${CMAKE_SOURCE_DIR}/Rtsp)
target_link_libraries(test_rtp_reorder_queue PRIVATE GTest::gtest_main)
add_test(NAME rtp_reorder_queue COMMAND test_rtp_reorder_queue)
set_tests_properties(rtp_reorder_queue PROPERTIES TIMEOUT 10)

if(UNIX)
    add_executable(rtp_weaknet_bench rtp_weaknet_bench.cpp)
    target_include_directories(rtp_weaknet_bench PRIVATE ${CMAKE_SOURCE_DIR}/Rtsp ${CMAKE_SOURCE_DIR}/third)
    target_link_libraries(rtp_weaknet_bench PRIVATE Threads::Threads)
    target_compile_options(rtp_weaknet_bench PRIVATE -O2)
    add_test(NAME rtp_weaknet_smoke COMMAND rtp_weaknet_bench
        --mode simulated --scenario video_severe --duration-ms 3000 --seed 11)
    set_tests_properties(rtp_weaknet_smoke PROPERTIES TIMEOUT 10)
    add_test(NAME rtp_resync_smoke COMMAND rtp_weaknet_bench
        --mode simulated --scenario large_gap --duration-ms 3000 --seed 11)
    set_tests_properties(rtp_resync_smoke PROPERTIES TIMEOUT 10)
endif()

add_executable(test_udp_binding test_udp_binding.cpp)
target_link_libraries(test_udp_binding PRIVATE
    "$<$<PLATFORM_ID:Linux>:-Wl,--start-group>"
    media rtsp rtp Rtcp network utils LOGGER COMMON ROOM_LIB ICE Interleaved
    "$<$<PLATFORM_ID:Linux>:-Wl,--end-group>"
    GTest::gtest GTest::gtest_main OpenSSL::Crypto pthread)
add_test(NAME test_udp_binding COMMAND test_udp_binding)
set_tests_properties(test_udp_binding PROPERTIES TIMEOUT 30)
