add_executable(test_weak_network_manual
    test_weak_network_manual.cpp
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
