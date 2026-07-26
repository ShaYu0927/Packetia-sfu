add_executable(test_rtp_input
    test_rtp_input.cpp
    test_sdp_track_binding.cpp
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
