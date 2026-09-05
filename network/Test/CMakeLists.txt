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
