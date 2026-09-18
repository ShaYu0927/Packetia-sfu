add_executable(test_datagram_transport
    test_datagram_protocol_classifier.cpp
    test_rtmp_transport.cpp
    test_webrtc_transport.cpp
    test_network_lifecycle.cpp
)

target_link_libraries(test_datagram_transport
    PRIVATE
        -Wl,--start-group
        network
        WebRtc
        Rtmp
        utils
        LOGGER
        -Wl,--end-group
        GTest::gtest
        GTest::gtest_main
)

add_test(NAME test_datagram_transport COMMAND test_datagram_transport)
set_tests_properties(test_datagram_transport PROPERTIES TIMEOUT 30)

add_executable(test_webrtc_session test_webrtc_session.cpp)
target_link_libraries(test_webrtc_session PRIVATE WebRtc Interleaved)
add_test(NAME test_webrtc_session COMMAND test_webrtc_session)
set_tests_properties(test_webrtc_session PROPERTIES TIMEOUT 30)
