add_executable(test_weak_network_manual
    test_bandwidth_framework.cpp
    test_downlink_bandwidth.cpp
    test_metric_sampling.cpp
    test_weak_network_manual.cpp
    test_send_side_transport.cpp
    test_bwe_rate_control.cpp
    test_loss_recovery.cpp
    test_twcc_feedback.cpp
    test_receive_quality_monitor.cpp
)

target_link_libraries(test_weak_network_manual
  PRIVATE
    media
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(test_weak_network_manual PRIVATE
    ${CMAKE_SOURCE_DIR}/media/quality
    ${CMAKE_SOURCE_DIR}/media/quality/bwe
    ${CMAKE_SOURCE_DIR}/protocol/Rtcp
)

add_test(NAME test_weak_network_manual COMMAND test_weak_network_manual)
